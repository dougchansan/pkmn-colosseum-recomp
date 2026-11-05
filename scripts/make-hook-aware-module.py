"""Make the generated module "hook-aware" so it can run with the normal call
depth AND keep every mod hook firing natively (no JIT detour).

Contract (checked by the runtime via the exported dolrecomp_hook_aware_calls):
  * Control never reaches a hooked entry H except through the chassis, whose
    dispatch (dolrecomp_call -> ppc_host_call) runs the entry hooks.
      - cross-chunk direct calls whose target is exactly H return to the chassis
      - same-chunk `goto label_H` (other than the chunk's entry switch) does too
  * A function with return hooks never returns locally: its `blr`
    (goto return_dispatch_*) and any branch out of its body return to the
    chassis, which dispatches the return address and fires the pending return.
Everything else stays a direct C call / goto.

Every rerouted spot is conditional on a per-hook live flag
(dolrecomp_hook_live[i], all 1 as built): the runtime clears the flags of hooks
no loaded mod uses, so they cost one byte test and the calls stay direct. One
module can then carry every mod's hooks at the speed of the loaded ones.

The module also exports the hooked addresses (dolrecomp_hook_aware_addresses,
zero-terminated, in live-flag order). The runtime checks the loaded mods' hooks
against it and runs the module like any other (hooked chunks through the
fallback) if a mod hooks an address the module was not built for, since a
direct call would skip it.

Usage (scripts/setup-unix.sh does this for the release modules):
  make-hook-aware-module.py --src <dolrecomp generated dir> --dst <out dir>
      --functions patches/GC6E01-functions.csv --mods mods [--hook ADDR[:r] ...]
--mods takes every RECOMP_HOOK / RECOMP_HOOK_RETURN / RECOMP_PATCH address in
the mod sources; --symbols (a decomp symbols.txt) can replace --functions.
"""
import argparse, bisect, re, shutil
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--src', type=Path, required=True)
ap.add_argument('--dst', type=Path, required=True)
ap.add_argument('--functions', type=Path, help='CSV: address,size (hex) per guest function')
ap.add_argument('--symbols', type=Path, help='decomp symbols.txt instead of --functions')
ap.add_argument('--mods', type=Path, action='append', default=[],
                help='mod source directory: hook every address its mods hook or patch')
ap.add_argument('--hook', action='append', default=[], help='ADDR[:r]  (:r = has return hooks)')
ap.add_argument('--idle-continuation', action='append', default=[], metavar='ADDR',
                help='return address of a call in the game\'s idle loop: that call goes '
                     'through the dispatcher so the runtime sees the loop and skips it '
                     '(GC6E01: 800FEB9C, the task scheduler)')
ap.add_argument('--idle-loop', action='append', default=[], metavar='START:END',
                help='the idle loop the runtime watches (its configured idle pc range): '
                     'branches into it keep returning to the dispatcher '
                     '(GC6E01: 800D31E0:800D321C)')
a = ap.parse_args()
if not a.functions and not a.symbols:
    ap.error('--functions or --symbols is required')

sizes = {}
if a.functions:
    for line in a.functions.read_text().splitlines():
        m = re.match(r'\s*(?:0x)?([0-9A-Fa-f]{8})\s*,\s*(?:0x)?([0-9A-Fa-f]+)', line)
        if m:
            sizes[int(m.group(1), 16)] = int(m.group(2), 16)
else:
    for line in a.symbols.read_text(errors='replace').splitlines():
        m = re.match(r'\s*\S+\s*=\s*\.\w+:0x([0-9A-Fa-f]{8});.*type:function.*size:0x([0-9A-Fa-f]+)', line)
        if m:
            sizes[int(m.group(1), 16)] = int(m.group(2), 16)

requested = {}
for h in a.hook:
    addr, _, flag = h.partition(':')
    requested[int(addr, 16)] = requested.get(int(addr, 16), False) or flag == 'r'
mod_hook = re.compile(r'RECOMP_(HOOK_RETURN|HOOK|FORCE_PATCH|PATCH)\(\s*0x([0-9A-Fa-f]{8})u?\s*,')
for directory in a.mods:
    for source in sorted(directory.glob('*.c')):
        if "hook-scan: skip" in source.read_text(errors="replace"):
            continue  # not built for this DOL; see the marker comment
        for kind, addr in mod_hook.findall(source.read_text(errors='replace')):
            addr = int(addr, 16)
            requested[addr] = requested.get(addr, False) or kind == 'HOOK_RETURN'

hooks = {}
for addr, ret in requested.items():
    if addr not in sizes:
        raise SystemExit(f'no function starts at {addr:08X} (hook or patch address)')
    hooks[addr] = (addr + sizes[addr], ret)
print('hooks:', ', '.join(f'{h:08X}-{e:08X}{" ret" if r else ""}' for h, (e, r) in sorted(hooks.items())))

if a.dst.exists():
    shutil.rmtree(a.dst)
shutil.copytree(a.src, a.dst)

call_site = re.compile(r'(ctx->pc = 0x([0-9A-F]{8})u;\n(\s+))if \(dolrecomp_call_enter\(\)\) \{\n(\s+)void func_([0-9A-F]{8})\(CPUState\* ctx\);')
goto_label = re.compile(r'goto label_([0-9A-F]{8});')
label_line = re.compile(r'^label_([0-9A-F]{8}):')
insn_comment = re.compile(r'\s*// ([0-9A-F]{8}): (\S+)')
call_mnemonic = re.compile(r'(bl|bla|bcl|bcla)$')
branch_mnemonic = re.compile(r'(b|ba|bc|bca)$')
stats = dict(sites=0, sites_routed=0, entry_gotos=0, returns=0, exits=0)
order = {h: i for i, h in enumerate(sorted(hooks))}


def live(h):
    return f'dolrecomp_hook_live[{order[h]}]'


def in_hook(addr):
    for h, (end, ret) in hooks.items():
        if h <= addr < end:
            return h, end, ret
    return None


# Cross-chunk tail jumps. A constant branch (b/bc, no link) into another chunk
# returned to the dispatcher, which only dispatched the target: a function
# that straddles a chunk boundary paid two dispatcher round trips per loop
# iteration. Instead the branch tail-calls the target chunk's function
# (musttail: no C stack growth), while the loop cycle budget lasts, unless the
# target is a live hooked entry, leaves a return-hooked function (its return
# hook would be skipped) or enters the idle loop the runtime watches. Without
# musttail (MSVC) the macro is empty and the branch returns as before.
chunk_starts = sorted(int(m.group(1), 16) for m in
                      (re.match(r'chunk_\d+_\w+?_([0-9A-F]{8})\.c$', p.name)
                       for p in (a.dst / 'chunks').glob('chunk_*.c')) if m)
idle_loops = [tuple(int(x, 16) for x in r.split(':')) for r in a.idle_loop]
TAIL_MACRO = ('#if defined(__has_attribute)\n#if __has_attribute(musttail)\n'
              '#define DOLRECOMP_TAIL_JUMP(f) { void f(CPUState* ctx); '
              '__attribute__((musttail)) return f(ctx); }\n#endif\n#endif\n'
              '#ifndef DOLRECOMP_TAIL_JUMP\n#define DOLRECOMP_TAIL_JUMP(f) {}\n#endif\n'
              # Whether an indirect call's target is a live hooked entry: one
              # bucket load for nearly every target (the table is in the marker).
              'extern const unsigned short dolrecomp_hook_bucket[256];\n'
              'extern const unsigned int dolrecomp_hook_aware_addresses[];\n'
              'int dolrecomp_hook_live_slow(unsigned int address);\n'
              'static inline int dolrecomp_hook_live_at(unsigned int address) {\n'
              '    unsigned short e = dolrecomp_hook_bucket[(address >> 2) & 255u];\n'
              '    if (!e) return 0;\n'
              '    if (e != 0xFFFFu) return dolrecomp_hook_aware_addresses[e - 1] == address && '
              'dolrecomp_hook_live[e - 1];\n'
              '    return dolrecomp_hook_live_slow(address);\n'
              '}\n')
pc_return = re.compile(r'(\s*)ctx->pc = 0x([0-9A-F]{8})u;$')
lr_set = re.compile(r'(\s*)ctx->lr = 0x([0-9A-F]{8})u;$')
indirect_call_mnemonic = re.compile(r'(bctrl|bcctrl|blrl|bclrl)$')


# Data cache ops (dcbf/dcbst/dcbi). The runtime runs them through its
# instruction fallback, which only invalidates any compiled code at the line
# (the SMC guard) and steps pc; the dispatcher round trip after it was what
# they cost - per 32-byte line of DCFlushRange, over half of all dispatches in
# battle. When the line is outside the module's code (no chunk can have been
# marked for re-verification), nothing was raised and pc stepped normally,
# execution continues at the next label.
cache_op_mnemonic = re.compile(r'(dcbf|dcbst|dcbi)$')
fallback_call = re.compile(r'(\s*)ppc_fallback_instruction\(ctx, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u\);$')
code_start = chunk_starts[0] if chunk_starts else 0
code_end = (chunk_starts[-1] + 0x4000) if chunk_starts else 0


def cache_ops(text):
    lines = text.split('\n')
    labels = set(re.findall(r'^label_([0-9A-F]{8}):', text, re.M))
    out = []
    in_main = False
    mnemonic = ''
    for i, line in enumerate(lines):
        if line.startswith('void func_'):
            in_main = True
        elif line.startswith('static void loop_'):
            in_main = False
        im = insn_comment.match(line)
        if im:
            mnemonic = im.group(2)
        out.append(line)
        fm = fallback_call.match(line)
        if not (in_main and fm and cache_op_mnemonic.match(mnemonic) and i + 1 < len(lines)
                and lines[i + 1].strip() == 'return;'):
            continue
        raw = int(fm.group(2), 16)
        nxt = f'{int(fm.group(3), 16) + 4:08X}'
        if nxt not in labels:
            continue
        ra, rb = (raw >> 16) & 31, (raw >> 11) & 31
        ea = f'ctx->gpr[{rb}]' if ra == 0 else f'(ctx->gpr[{ra}] + ctx->gpr[{rb}])'
        ind = fm.group(1)
        out.append(f'{ind}{{ u32 phys = {ea} & 0x01FFFFFFu; if (!ctx->exception && ctx->pc == 0x{nxt}u && '
                   f'(phys < 0x{code_start & 0x01FFFFFF:08X}u || phys >= 0x{code_end & 0x01FFFFFF:08X}u)) '
                   f'goto label_{nxt}; }}')
        stats['cache_ops'] = stats.get('cache_ops', 0) + 1
    return '\n'.join(out)


# Indirect calls (bctrl/blrl: callbacks, function pointers). They returned to
# the dispatcher twice, to enter the target and to come back. They now call
# the target's chunk function nested, as a direct cross-chunk call does, and
# continue at the return label - unless the target is a live hooked entry
# (its hooks fire in the dispatcher), no compiled function covers it, the
# call depth is exhausted, or the return label is in another chunk; then the
# original exit runs.
def indirect_calls(text):
    lines = text.split('\n')
    labels = set(re.findall(r'^label_([0-9A-F]{8}):', text, re.M))
    out = []
    in_main = False
    mnemonic = ''
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line.startswith('void func_'):
            in_main = True
        elif line.startswith('static void loop_'):
            in_main = False
        im = insn_comment.match(line)
        if im:
            mnemonic = im.group(2)
        out.append(line)
        lm = lr_set.match(line)
        if (in_main and lm and indirect_call_mnemonic.match(mnemonic) and i < len(lines)
                and lines[i].strip() == 'ctx->pc = target;' and lm.group(2) in labels):
            ind = lm.group(1)
            ret = lm.group(2)
            out.append(lines[i])  # ctx->pc = target;
            i += 1
            out.append(f'{ind}if (!dolrecomp_hook_live_at(target) && dolrecomp_call_enter()) {{')
            out.append(f'{ind}    DolRecompFunction fn = dolrecomp_find_original(target);')
            out.append(f'{ind}    if (fn) {{ fn(ctx); dolrecomp_call_leave(); '
                       f'if (ctx->pc == 0x{ret}u) goto label_{ret}; return; }}')
            out.append(f'{ind}    dolrecomp_call_leave();')
            out.append(f'{ind}}}')
            stats['indirect_calls'] = stats.get('indirect_calls', 0) + 1
    return '\n'.join(out)


def chunk_of(addr):
    i = bisect.bisect_right(chunk_starts, addr) - 1
    if i < 0 or (i == len(chunk_starts) - 1 and addr >= chunk_starts[i] + 0x4000):
        return None
    return chunk_starts[i]


def tail_jumps(path, text):
    m = re.match(r'chunk_\d+_\w+?_([0-9A-F]{8})\.c$', path.name)
    if not m:
        return text
    here = int(m.group(1), 16)
    lines = text.split('\n')
    out = []
    in_main = False
    mnemonic = ''
    current = None
    for i, line in enumerate(lines):
        if line.startswith('void func_'):
            in_main = True
        elif line.startswith('static void loop_'):
            in_main = False
        im = insn_comment.match(line)
        if im:
            current = int(im.group(1), 16)
            mnemonic = im.group(2)
        pm = pc_return.match(line)
        if (in_main and pm and i + 1 < len(lines) and lines[i + 1].strip() == 'return;'
                and branch_mnemonic.match(mnemonic)
                and 'DOLRECOMP_C_LOOP_CYCLE_BUDGET' not in (out[-1] if out else '')):
            tgt = int(pm.group(2), 16)
            target_chunk = chunk_of(tgt)
            region = in_hook(current) if current is not None else None
            leaves_returning = region is not None and region[2] and not (region[0] <= tgt < region[1])
            if (target_chunk is not None and target_chunk != here and not leaves_returning
                    and not any(s <= tgt < e for s, e in idle_loops)):
                cond = '(ctx->downcount > -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)'
                if tgt in hooks:
                    cond += f' && !{live(tgt)}'
                out.append(line)
                out.append(f'{pm.group(1)}if ({cond}) DOLRECOMP_TAIL_JUMP(func_{target_chunk:08X})')
                stats['tail_jumps'] = stats.get('tail_jumps', 0) + 1
                continue
        out.append(line)
    return '\n'.join(out)


for f in sorted((a.dst / 'chunks').glob('*.c')):
    text = f.read_text()
    orig = text

    def site(m):
        stats['sites'] += 1
        if int(m.group(2), 16) in hooks:
            stats['sites_routed'] += 1
            tgt = int(m.group(2), 16)
            return (f"{m.group(1)}if (!{live(tgt)} && dolrecomp_call_enter()) {{\n"
                    f"{m.group(4)}void func_{m.group(5)}(CPUState* ctx);")
        return m.group(0)

    text = call_site.sub(site, text)

    # Idle-loop calls: back through the dispatcher at their return address,
    # where the runtime's idle detection looks (otherwise the CPU thread spins
    # through the game's idle time natively).
    for cont in a.idle_continuation:
        text, n = re.subn(
            r'(ctx->lr = 0x%08Xu;\n\s+ctx->pc = 0x[0-9A-F]{8}u;\n\s+)if \(dolrecomp_call_enter\(\)\)'
            % int(cont, 16), r'\1if (0) /* idle-detect continuation */', text)
        stats['idle_continuations'] = stats.get('idle_continuations', 0) + n

    if any(f"label_{h:08X}:" in text for h in hooks) or any(f"goto label_{h:08X};" in text for h in hooks):
        out = []
        in_main = False
        current = None
        entry_switch_done = False
        call_insn = False
        for line in text.split('\n'):
            if line.startswith('void func_'):
                in_main = True; current = None; entry_switch_done = False
            elif line.startswith('static void loop_'):
                in_main = False
            lm = label_line.match(line)
            if lm:
                current = int(lm.group(1), 16)
                entry_switch_done = True
            im = insn_comment.match(line)
            if im:
                call_insn = bool(call_mnemonic.match(im.group(2)))
            if in_main:
                region = in_hook(current) if current is not None else None
                # Same-chunk jumps to a hooked entry must go through the chassis.
                def entry(m):
                    tgt = int(m.group(1), 16)
                    if tgt in hooks and not (line.lstrip().startswith('case ') and not entry_switch_done):
                        if region and region[0] == tgt and False:
                            return m.group(0)
                        stats['entry_gotos'] += 1
                        return (f'{{ if ({live(tgt)}) {{ ctx->pc = 0x{tgt:08X}u; return; }} '
                                f'goto label_{tgt:08X} /*ha*/; }}')
                    return m.group(0)
                # A hooked function's own internal back-edges to its entry are
                # not new entries; only rewrite gotos from outside its body.
                if region is None or not any(int(x, 16) == region[0] for x in goto_label.findall(line)):
                    line = goto_label.sub(entry, line)
                if region is not None:
                    h, end, ret = region
                    if ret and 'goto return_dispatch_' in line:
                        line = re.sub(r'goto (return_dispatch_[0-9A-F]{8});',
                                      lambda r: f'{{ if ({live(h)}) return; goto {r.group(1)} /*ha*/; }}',
                                      line)
                        stats['returns'] += 1

                    def exit_(m):
                        tgt = int(m.group(1), 16)
                        # A call's callee returns into this body, so the
                        # return hook still fires at the function's own blr.
                        if call_insn:
                            return m.group(0)
                        if ret and not (h <= tgt < end):
                            stats['exits'] += 1
                            return (f'{{ if ({live(h)}) {{ ctx->pc = 0x{tgt:08X}u; return; }} '
                                    f'goto label_{tgt:08X} /*ha*/; }}')
                        return m.group(0)
                    line = goto_label.sub(exit_, line)
            out.append(line)
        text = '\n'.join(out)
    text = cache_ops(indirect_calls(tail_jumps(f, text)))
    if text != orig:
        f.write_text('extern unsigned char dolrecomp_hook_live[];\n' + TAIL_MACRO + text)

# Exported on every platform: dllexport on Windows; on Linux / macOS default
# visibility, and the module template's module.exports lists them.
addresses = ', '.join(f'0x{h:08X}u' for h in sorted(hooks))
# dolrecomp_hook_live_at's table: bucket (address >> 2) & 255 holds the hook's
# index + 1, 0 for no hook, 0xFFFF when two hooks share it (then a scan).
buckets = [0] * 256
for i, h in enumerate(sorted(hooks)):
    b = (h >> 2) & 255
    buckets[b] = i + 1 if buckets[b] == 0 else 0xFFFF
bucket_rows = ',\n'.join('    ' + ', '.join(f'0x{v:04X}u' for v in buckets[r:r + 16])
                         for r in range(0, 256, 16))
(a.dst / 'chunks' / 'zz_hook_aware_marker.c').write_text(
    '/* Marks this module as hook-aware and lists the hooked addresses; see\n'
    '   make-hook-aware-module.py. */\n'
    '#ifdef _WIN32\n'
    '#define HOOK_AWARE_EXPORT __declspec(dllexport)\n'
    '#else\n'
    '#define HOOK_AWARE_EXPORT __attribute__((visibility("default")))\n'
    '#endif\n'
    'HOOK_AWARE_EXPORT const int dolrecomp_hook_aware_calls = 1;\n'
    f'HOOK_AWARE_EXPORT const unsigned int dolrecomp_hook_aware_addresses[] = {{{addresses}, 0u}};\n'
    '/* Per hook, in address order: 1 = a loaded mod uses it (go through the\n'
    '   dispatcher). The runtime clears unused ones; all 1 is always correct. */\n'
    f'HOOK_AWARE_EXPORT unsigned char dolrecomp_hook_live[{max(len(hooks), 1)}] = '
    f'{{{", ".join("1" for _ in hooks) or "0"}}};\n'
    '/* Indirect calls\' hooked-entry check (dolrecomp_hook_live_at). */\n'
    f'const unsigned short dolrecomp_hook_bucket[256] = {{\n{bucket_rows}\n}};\n'
    'int dolrecomp_hook_live_slow(unsigned int address) {\n'
    f'    for (unsigned int i = 0; i < {len(hooks)}u; ++i)\n'
    '        if (dolrecomp_hook_aware_addresses[i] == address) return dolrecomp_hook_live[i];\n'
    '    return 0;\n'
    '}\n')
print(stats)
