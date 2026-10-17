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
import argparse, re, shutil
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
stats = dict(sites=0, sites_routed=0, entry_gotos=0, returns=0, exits=0)
order = {h: i for i, h in enumerate(sorted(hooks))}


def live(h):
    return f'dolrecomp_hook_live[{order[h]}]'


def in_hook(addr):
    for h, (end, ret) in hooks.items():
        if h <= addr < end:
            return h, end, ret
    return None


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
        for line in text.split('\n'):
            if line.startswith('void func_'):
                in_main = True; current = None; entry_switch_done = False
            elif line.startswith('static void loop_'):
                in_main = False
            lm = label_line.match(line)
            if lm:
                current = int(lm.group(1), 16)
                entry_switch_done = True
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
                        if ret and not (h <= tgt < end):
                            stats['exits'] += 1
                            return (f'{{ if ({live(h)}) {{ ctx->pc = 0x{tgt:08X}u; return; }} '
                                    f'goto label_{tgt:08X} /*ha*/; }}')
                        return m.group(0)
                    line = goto_label.sub(exit_, line)
            out.append(line)
        text = '\n'.join(out)
    if text != orig:
        f.write_text('extern unsigned char dolrecomp_hook_live[];\n' + text)

# Exported on every platform: dllexport on Windows; on Linux / macOS default
# visibility, and the module template's module.exports lists them.
addresses = ', '.join(f'0x{h:08X}u' for h in sorted(hooks))
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
    f'{{{", ".join("1" for _ in hooks) or "0"}}};\n')
print(stats)
