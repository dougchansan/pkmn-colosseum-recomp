/* GX sync census: the synchronisation contract between the game and the GPU.

   Records, per VI retrace (a "frame" here: one field, 1/60 s), what the
   game waits on:
   - interrupts the guest handles: __OSDispatchInterrupt, the PE finish and
     token handlers, the CP FIFO handler and the VI retrace handler;
   - the GX and VI sync calls (GXSetDrawDone, GXWaitDrawDone, GXDrawDone,
     GXSetDrawSync, VIWaitForRetrace, the callback setters) by caller;
   - every OSSleepThread / OSWakeupThread pair, by queue, sleeping function
     and waking function, with the host and guest time slept; and
     OSSuspendThread / OSResumeThread pairs the same way (FIFO overflow);
   - CPU reads and writes of the CP, PE, VI and PI registers (0xCC000000 to
     0xCC003FFF) and of the EFB (0xC8000000), by register and by the guest
     instruction that made them, with the values of the status registers.

   MMIO is seen by wrapping the CPU state's external read/write callbacks:
   the generated code calls them for every access outside RAM and stores the
   accessing instruction's address in pc first. A hook cannot change the CPU
   state (the host restores it after each hook), so a small thread installs
   the wrappers once the first hook has shown where the state lives, and puts
   them back if a hook's restore raced with it.

   Two modes, because a mod hook makes the module run the 16 KiB chunk that
   holds the hooked function in Dolphin's interpreter (unless the module was
   built hook-aware for that address), and the interpreter's loads and stores
   do not go through the CPU state's callbacks:
   - MODERNGEKKO_GX_SYNC_MODE=hooks (default): the guest hooks above. Only
     calls that reach the dispatcher fire them (indirect calls, calls out of
     interpreted chunks); direct calls between native functions do not. The
     interrupt handlers (called through __OSDispatchInterrupt's table) and
     OSSleepThread from the SDK fire reliably. MMIO is seen only where it
     runs natively.
   - MODERNGEKKO_GX_SYNC_MODE=mmio: no hooks in those chunks, so the OS, VI
     and GX code all runs natively and every CP/PE/VI/PI access is seen.
     Interrupts are counted from the cause and mask __OSDispatchInterrupt
     reads (PI INTSR & INTMR), and a VI cause ticks the frame.

   Enabled by MODERNGEKKO_GX_SYNC=<path>; the file is rewritten every 600
   frames and when the mod unloads. tools/gx-census/sync_report.py reads it. */

#include "moderngekko/mod_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* ---- guest functions (GC6E01; names from the decomp and gx_sdk_names.json) */
#define TARGETS(X)                                                      \
    X(OS_DISPATCH_INTERRUPT, 0x8009E414u, "__OSDispatchInterrupt")      \
    X(PE_FINISH_HANDLER, 0x800B90E8u, "GXFinishInterruptHandler")       \
    X(PE_TOKEN_HANDLER, 0x800B901Cu, "GXTokenInterruptHandler")         \
    X(CP_HANDLER, 0x800B6FE0u, "GXCPInterruptHandler")                  \
    X(VI_RETRACE_HANDLER, 0x800A85DCu, "__VIRetraceHandler")            \
    X(GX_SET_DRAW_DONE, 0x800B8D10u, "GXSetDrawDone")                   \
    X(GX_WAIT_DRAW_DONE, 0x800B8DA8u, "GXWaitDrawDone")                 \
    X(GX_DRAW_DONE, 0x800B8DF4u, "GXDrawDone")                          \
    X(GX_SET_DRAW_SYNC, 0x800B8C58u, "GXSetDrawSync")                   \
    X(GX_SET_DRAW_SYNC_CB, 0x800B8FD8u, "GXSetDrawSyncCallback")        \
    X(GX_SET_DRAW_DONE_CB, 0x800B90A4u, "GXSetDrawDoneCallback")        \
    X(GX_FLUSH, 0x800B8920u, "GXFlush")                                 \
    X(GX_PIX_MODE_SYNC, 0x800B8E74u, "GXPixModeSync")                   \
    X(GX_ABORT, 0x800B897Cu, "__GXAbort")                               \
    X(GX_FN_800B8AE8, 0x800B8AE8u, "fn_800B8AE8")                       \
    X(GX_SET_MISC, 0x800B888Cu, "GXSetMisc")                            \
    X(GX_COPY_DISP, 0x800B9E88u, "GXCopyDisp")                          \
    X(GX_INIT_FIFO_LIMITS, 0x800B71F0u, "GXInitFifoLimits")             \
    X(GX_SET_CPU_FIFO, 0x800B71FCu, "GXSetCPUFifo")                     \
    X(GX_SET_GP_FIFO, 0x800B730Cu, "GXSetGPFifo")                       \
    X(GX_FN_800B7484, 0x800B7484u, "fn_800B7484")                       \
    X(GX_FIFO_READ_ENABLE, 0x800B7514u, "__GXFifoReadEnable")           \
    X(GX_FIFO_READ_DISABLE, 0x800B7538u, "__GXFifoReadDisable")         \
    X(GX_FIFO_LINK, 0x800B7558u, "__GXFifoLink")                        \
    X(GX_WRITE_FIFO_INT_ENABLE, 0x800B7594u, "__GXWriteFifoIntEnable")  \
    X(GX_WRITE_FIFO_INT_RESET, 0x800B75D0u, "__GXWriteFifoIntReset")    \
    X(VI_SET_PRE_RETRACE_CB, 0x800A880Cu, "VISetPreRetraceCallback")    \
    X(VI_SET_POST_RETRACE_CB, 0x800A8850u, "VISetPostRetraceCallback")  \
    X(VI_WAIT_FOR_RETRACE, 0x800A8FE4u, "VIWaitForRetrace")             \
    X(VI_FLUSH, 0x800AA068u, "VIFlush")                                 \
    X(VI_SET_NEXT_FRAME_BUFFER, 0x800AA198u, "VISetNextFrameBuffer")    \
    X(VI_SET_BLACK, 0x800AA204u, "VISetBlack")                          \
    X(OS_SLEEP_THREAD, 0x800A238Cu, "OSSleepThread")                    \
    X(OS_WAKEUP_THREAD, 0x800A2478u, "OSWakeupThread")                  \
    X(OS_SUSPEND_THREAD, 0x800A221Cu, "OSSuspendThread")                \
    X(OS_RESUME_THREAD, 0x800A1F94u, "OSResumeThread")                  \
    X(OS_MASK_INTERRUPTS, 0x8009E304u, "__OSMaskInterrupts")            \
    X(OS_UNMASK_INTERRUPTS, 0x8009E38Cu, "__OSUnmaskInterrupts")        \
    X(GAPP_DRAW_SYNC_CB, 0x80101B34u, "fn_80101B34")                    \
    X(GAPP_SET_DRAW_SYNC, 0x80101D5Cu, "fn_80101D5C")                   \
    /* HSD's VI layer (hsd/hsd_video): the frame's EFB->XFB copy and the  \
       callbacks it registers (tools/gx-census/sync_callers.py). */        \
    X(HSD_COPY_XFB_ASYNC, 0x801BF8A0u, "HSD_VICopyXFBASync")            \
    X(HSD_DRAW_DONE_CB, 0x801BFCB0u, "HSD_VIGXDrawDoneCB")              \
    X(HSD_POST_RETRACE_CB, 0x801BFD10u, "HSD_VIPostRetraceCB")          \
    X(HSD_PRE_RETRACE_CB, 0x801BFF18u, "HSD_VIPreRetraceCB")            \
    X(THP_POST_RETRACE_CB, 0x801E3A50u, "fn_801E3A50")

#define ENUM_TARGET(id, address, name) T_##id,
enum { TARGETS(ENUM_TARGET) TARGET_COUNT };

typedef struct Target
{
    uint32_t address;
    const char* name;
} Target;

#define DECLARE_TARGET(id, address, name) {address, name},
static const Target targets[] = {TARGETS(DECLARE_TARGET)};

/* GXWaitDrawDone, VIWaitForRetrace and GXCPInterruptHandler bodies: a
   sleep whose caller lies in one of them is a draw-done, retrace or FIFO
   overflow wait. */
#define IN_RANGE(lr, start, size) ((lr) >= (start) && (lr) < (start) + (size))

/* ---- per-frame counters --------------------------------------------- */
#define FRAME_COUNTERS(X)                                                 \
    X(irq_dispatch) X(irq_pe_finish) X(irq_pe_token) X(irq_cp) X(irq_vi)  \
    X(set_draw_done) X(wait_draw_done) X(draw_done) X(draw_done_slept)    \
    X(set_draw_sync) X(draw_sync_cb) X(vi_wait) X(vi_wait_slept)          \
    X(copy_disp) X(gx_flush) X(sleeps) X(wakeups) X(suspends) X(resumes)  \
    X(cp_read) X(cp_write) X(pe_read) X(pe_write) X(vi_read) X(vi_write)  \
    X(pi_read) X(pi_write) X(efb_read) X(efb_write) X(other_mmio)         \
    X(xfb_copy) X(cb_draw_done) X(cb_pre_retrace) X(cb_post_retrace)      \
    X(cb_thp_retrace)                                                     \
    X(wait_draw_done_us) X(wait_retrace_us) X(wait_other_us)

#define ENUM_COUNTER(name) C_##name,
enum { FRAME_COUNTERS(ENUM_COUNTER) COUNTER_COUNT };
#define NAME_COUNTER(name) #name,
static const char* const counter_names[] = {FRAME_COUNTERS(NAME_COUNTER)};

/* Counts per frame go into buckets 0..15 exactly, then log2 buckets; the
   microsecond counters use log2 buckets throughout (bucket b: < 2^b us). */
#define HIST_BUCKETS 40
static uint64_t frame_now[COUNTER_COUNT];
static uint64_t totals[COUNTER_COUNT];
static uint64_t histogram[COUNTER_COUNT][HIST_BUCKETS];
static uint64_t frames;

static int bucket_of(uint64_t v, int is_us)
{
    if (!is_us && v < 16)
        return (int)v;
    int b = 0;
    while (v) {
        v >>= 1;
        ++b;
    }
    if (!is_us)
        b += 11; /* 16..31 -> 16 */
    return b < HIST_BUCKETS ? b : HIST_BUCKETS - 1;
}

/* ---- callers per target, and argument values ------------------------- */
#define MAX_CALLERS 32
typedef struct Caller
{
    uint32_t lr;
    uint64_t count;
} Caller;
static uint64_t calls[TARGET_COUNT];
static uint64_t other_callers[TARGET_COUNT];
static Caller callers[TARGET_COUNT][MAX_CALLERS];

#define MAX_ARGS 1024
typedef struct Arg
{
    uint32_t target, lr, r3, r4;
    uint64_t count, first_frame, last_frame;
} Arg;
static Arg args[MAX_ARGS];
static uint64_t args_dropped;

/* ---- waits ------------------------------------------------------------ */
typedef struct WaitStat
{
    uint32_t kind; /* 0 sleep/wakeup on a queue, 1 suspend/resume, 2 latency */
    uint32_t object; /* queue, thread, or latency id */
    uint32_t waiter_lr; /* who slept (lr of OSSleepThread) */
    uint32_t waker_lr; /* who woke it (lr of OSWakeupThread) */
    uint64_t count;
    double us_total, us_max;
    uint64_t ticks_total, ticks_max;
    uint64_t hist[HIST_BUCKETS];
} WaitStat;
#define MAX_WAITS 512
static WaitStat waits[MAX_WAITS];
static uint64_t waits_dropped;

typedef struct Pending
{
    uint32_t object, lr;
    double t;
    uint64_t tb;
} Pending;
#define MAX_PENDING 64
static Pending sleepers[MAX_PENDING];
static Pending suspended[MAX_PENDING];

/* Latency ids for kind 2. */
enum {
    LAT_SET_DRAW_DONE_TO_FINISH = 1, /* GXSetDrawDone -> finish interrupt */
    LAT_WAIT_TO_FINISH = 2,          /* GXWaitDrawDone entry -> finish interrupt */
    LAT_SET_DRAW_SYNC_TO_TOKEN = 3,  /* GXSetDrawSync -> token interrupt */
    LAT_VI_WAIT_TO_RETRACE = 4,      /* VIWaitForRetrace entry -> retrace interrupt */
    LAT_RETRACE_TO_RETRACE = 5,      /* retrace interrupt interval */
    LAT_FRAME = 6,                   /* VIFlush interval */
};
static double t_set_draw_done = -1, t_wait_draw_done = -1, t_set_draw_sync = -1,
              t_vi_wait = -1, t_retrace = -1, t_frame = -1;
static uint64_t tb_set_draw_done, tb_wait_draw_done, tb_set_draw_sync, tb_vi_wait,
    tb_retrace, tb_frame;

/* ---- MMIO -------------------------------------------------------------- */
typedef struct Access
{
    uint32_t reg; /* physical address; 0x08000000 for any EFB access */
    uint32_t pc;
    uint8_t write, size;
    uint64_t count;
} Access;
#define MAX_ACCESS 4096
static Access accesses[MAX_ACCESS];
static uint64_t accesses_dropped;

typedef struct Value
{
    uint32_t reg, value;
    uint8_t write;
    uint64_t count;
} Value;
#define MAX_VALUES 2048
static Value values[MAX_VALUES];
static uint64_t values_dropped;
static uint64_t gather_pipe_writes;

/* ---- timeline: cumulative counters every TIMELINE_EVERY frames -------- */
#define TIMELINE_EVERY 60
#define MAX_TIMELINE 20000
typedef struct Row
{
    double wall;
    uint64_t frame;
    uint64_t c[COUNTER_COUNT];
} Row;
static Row* timeline;
static uint32_t timeline_rows;

static const char* output_path;
static int mmio_mode;
static uint32_t last_intsr;
static void end_frame(void);
static double now_us(void);

static double now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
#endif
}

static double wall_seconds(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- recording -------------------------------------------------------- */

static void count_caller(int target, uint32_t lr)
{
    ++calls[target];
    for (int c = 0; c < MAX_CALLERS; ++c) {
        Caller* k = &callers[target][c];
        if (k->count && k->lr != lr)
            continue;
        k->lr = lr;
        ++k->count;
        return;
    }
    ++other_callers[target];
}

static void record_arg(int target, uint32_t lr, uint32_t r3, uint32_t r4)
{
    uint32_t h = ((uint32_t)target * 2654435761u) ^ (lr * 40503u) ^ (r3 * 2246822519u) ^ r4;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        Arg* a = &args[(h + probe) % MAX_ARGS];
        if (a->count && (a->target != (uint32_t)target || a->lr != lr || a->r3 != r3 ||
                         a->r4 != r4))
            continue;
        if (!a->count) {
            a->target = target;
            a->lr = lr;
            a->r3 = r3;
            a->r4 = r4;
            a->first_frame = frames;
        }
        a->last_frame = frames;
        ++a->count;
        return;
    }
    ++args_dropped;
}

static void record_wait(uint32_t kind, uint32_t object, uint32_t waiter, uint32_t waker,
                        double us, uint64_t ticks)
{
    if (us < 0)
        us = 0;
    uint32_t h = (kind * 2654435761u) ^ (object * 40503u) ^ (waiter * 2246822519u) ^ waker;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        WaitStat* w = &waits[(h + probe) % MAX_WAITS];
        if (w->count && (w->kind != kind || w->object != object || w->waiter_lr != waiter ||
                         w->waker_lr != waker))
            continue;
        if (!w->count) {
            w->kind = kind;
            w->object = object;
            w->waiter_lr = waiter;
            w->waker_lr = waker;
        }
        ++w->count;
        w->us_total += us;
        if (us > w->us_max)
            w->us_max = us;
        w->ticks_total += ticks;
        if (ticks > w->ticks_max)
            w->ticks_max = ticks;
        ++w->hist[bucket_of((uint64_t)us, 1)];
        return;
    }
    ++waits_dropped;
}

static void latency(uint32_t id, double* since, uint64_t* tb_since, double t, uint64_t tb)
{
    if (*since >= 0)
        record_wait(2, id, 0, 0, t - *since, tb >= *tb_since ? tb - *tb_since : 0);
    *since = -1;
}

static void record_access(uint32_t reg, uint32_t pc, int write, uint8_t size)
{
    uint32_t h = (reg * 2654435761u) ^ (pc * 40503u) ^ (uint32_t)write;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        Access* a = &accesses[(h + probe) % MAX_ACCESS];
        if (a->count && (a->reg != reg || a->pc != pc || a->write != write || a->size != size))
            continue;
        if (!a->count) {
            a->reg = reg;
            a->pc = pc;
            a->write = (uint8_t)write;
            a->size = size;
        }
        ++a->count;
        return;
    }
    ++accesses_dropped;
}

static void record_value(uint32_t reg, uint32_t value, int write)
{
    uint32_t h = (reg * 2654435761u) ^ (value * 40503u) ^ (uint32_t)write;
    for (uint32_t probe = 0; probe < 32; ++probe) {
        Value* v = &values[(h + probe) % MAX_VALUES];
        if (v->count && (v->reg != reg || v->value != value || v->write != write))
            continue;
        if (!v->count) {
            v->reg = reg;
            v->value = value;
            v->write = (uint8_t)write;
        }
        ++v->count;
        return;
    }
    ++values_dropped;
}

static void mmio(CPUState* cpu, uint32_t ea, uint64_t value, uint8_t size, int write)
{
    uint32_t phys = ea & 0x1FFFFFFFu;
    if ((phys & 0x1FFFF000u) == 0x0C008000u) {
        ++gather_pipe_writes;
        return;
    }
    int counter;
    uint32_t reg = phys;
    if (phys >= 0x08000000u && phys < 0x0C000000u) {
        counter = write ? C_efb_write : C_efb_read;
        reg = 0x08000000u;
    } else if (phys >= 0x0C000000u && phys < 0x0C001000u) {
        counter = write ? C_cp_write : C_cp_read;
    } else if (phys >= 0x0C001000u && phys < 0x0C002000u) {
        counter = write ? C_pe_write : C_pe_read;
    } else if (phys >= 0x0C002000u && phys < 0x0C003000u) {
        counter = write ? C_vi_write : C_vi_read;
    } else if (phys >= 0x0C003000u && phys < 0x0C004000u) {
        counter = write ? C_pi_write : C_pi_read;
    } else {
        ++frame_now[C_other_mmio];
        return;
    }
    ++frame_now[counter];
    record_access(reg, cpu->pc, write, size);
    /* __OSDispatchInterrupt reads the cause, then the mask: their AND is
       what it dispatches. */
    if (mmio_mode && !write && cpu->pc >= 0x8009E414u && cpu->pc < 0x8009E758u) {
        if (reg == 0x0C003000u) {
            last_intsr = (uint32_t)value;
        } else if (reg == 0x0C003004u) {
            const uint32_t cause = last_intsr & (uint32_t)value;
            ++frame_now[C_irq_dispatch];
            if (cause & (1u << 9))
                ++frame_now[C_irq_pe_token];
            if (cause & (1u << 10))
                ++frame_now[C_irq_pe_finish];
            if (cause & (1u << 11))
                ++frame_now[C_irq_cp];
            if (cause & (1u << 8)) {
                ++frame_now[C_irq_vi];
                end_frame();
            }
        }
    }
    /* Status and control values: CP, PE and PI registers (VI's timing
       registers would flood the table). */
    if (reg >= 0x0C000000u && reg < 0x0C004000u && (reg < 0x0C002000u || reg >= 0x0C003000u))
        record_value(reg, (uint32_t)value, write);
}

/* ---- wrapped external access ---------------------------------------- */
static PPCExternalRead original_read;
static PPCExternalWrite original_write;
static CPUState* volatile guest_cpu;
static volatile int installer_stop;
static volatile int wrapped_seen; /* the wrappers were installed at some point */

static uint64_t wrapped_read(CPUState* cpu, uint32_t ea, uint8_t size)
{
    uint64_t value = original_read(cpu, ea, size);
    mmio(cpu, ea, value, size, 0);
    return value;
}

static void wrapped_write(CPUState* cpu, uint32_t ea, uint64_t value, uint8_t size)
{
    mmio(cpu, ea, value, size, 1);
    original_write(cpu, ea, value, size);
}

static void install_wrappers(void)
{
    CPUState* cpu = guest_cpu;
    if (!cpu)
        return;
    if (!original_read && cpu->external_read && cpu->external_read != wrapped_read)
        original_read = cpu->external_read;
    if (!original_write && cpu->external_write && cpu->external_write != wrapped_write)
        original_write = cpu->external_write;
    if (original_read && cpu->external_read == original_read)
        cpu->external_read = wrapped_read;
    if (cpu->external_read == wrapped_read)
        wrapped_seen = 1;
    if (original_write && cpu->external_write == original_write)
        cpu->external_write = wrapped_write;
}

#ifdef _WIN32
static HANDLE installer;
static DWORD WINAPI installer_main(LPVOID unused)
{
    (void)unused;
    while (!installer_stop) {
        install_wrappers();
        Sleep(50);
    }
    return 0;
}
#else
static pthread_t installer;
static int installer_started;
static void* installer_main(void* unused)
{
    (void)unused;
    while (!installer_stop) {
        install_wrappers();
        usleep(50000);
    }
    return NULL;
}
#endif

/* ---- output -------------------------------------------------------------- */

static void write_counters(FILE* f, const uint64_t* c)
{
    fprintf(f, "[");
    for (int i = 0; i < COUNTER_COUNT; ++i)
        fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)c[i]);
    fprintf(f, "]");
}

static void flush(void)
{
    if (!output_path)
        return;
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s.tmp", output_path);
    FILE* f = fopen(temp, "w");
    if (!f)
        return;
    fprintf(f, "{\"version\": 2, \"mode\": \"%s\", \"frames\": %llu, \"wrapped_mmio\": %s,\n",
            mmio_mode ? "mmio" : "hooks", (unsigned long long)frames,
            wrapped_seen ? "true" : "false");
    fprintf(f, "\"gather_pipe_writes\": %llu, \"args_dropped\": %llu, \"waits_dropped\": %llu, "
               "\"accesses_dropped\": %llu, \"values_dropped\": %llu,\n",
            (unsigned long long)gather_pipe_writes, (unsigned long long)args_dropped,
            (unsigned long long)waits_dropped, (unsigned long long)accesses_dropped,
            (unsigned long long)values_dropped);
    fprintf(f, "\"counters\": [");
    for (int i = 0; i < COUNTER_COUNT; ++i)
        fprintf(f, "%s\"%s\"", i ? ", " : "", counter_names[i]);
    fprintf(f, "],\n\"totals\": ");
    write_counters(f, totals);
    fprintf(f, ",\n\"histograms\": [");
    for (int i = 0; i < COUNTER_COUNT; ++i) {
        fprintf(f, "%s\n [", i ? "," : "");
        for (int b = 0; b < HIST_BUCKETS; ++b)
            fprintf(f, "%s%llu", b ? "," : "", (unsigned long long)histogram[i][b]);
        fprintf(f, "]");
    }
    fprintf(f, "],\n\"targets\": [");
    int first = 1;
    for (int t = 0; t < TARGET_COUNT; ++t) {
        if (!calls[t])
            continue;
        fprintf(f, "%s\n {\"address\": \"%08X\", \"name\": \"%s\", \"calls\": %llu, "
                   "\"other_callers\": %llu, \"callers\": {",
                first ? "" : ",", targets[t].address, targets[t].name,
                (unsigned long long)calls[t], (unsigned long long)other_callers[t]);
        first = 0;
        for (int c = 0; c < MAX_CALLERS && callers[t][c].count; ++c)
            fprintf(f, "%s\"%08X\": %llu", c ? ", " : "", callers[t][c].lr,
                    (unsigned long long)callers[t][c].count);
        fprintf(f, "}}");
    }
    fprintf(f, "\n],\n\"args\": [");
    first = 1;
    for (int i = 0; i < MAX_ARGS; ++i) {
        const Arg* a = &args[i];
        if (!a->count)
            continue;
        fprintf(f, "%s\n [\"%s\", \"%08X\", \"%08X\", \"%08X\", %llu, %llu, %llu]",
                first ? "" : ",", targets[a->target].name, a->lr, a->r3, a->r4,
                (unsigned long long)a->count, (unsigned long long)a->first_frame,
                (unsigned long long)a->last_frame);
        first = 0;
    }
    fprintf(f, "\n],\n\"waits\": [");
    first = 1;
    for (int i = 0; i < MAX_WAITS; ++i) {
        const WaitStat* w = &waits[i];
        if (!w->count)
            continue;
        fprintf(f, "%s\n {\"kind\": %u, \"object\": \"%08X\", \"waiter\": \"%08X\", "
                   "\"waker\": \"%08X\", \"count\": %llu, \"us_total\": %.1f, \"us_max\": %.1f, "
                   "\"ticks_total\": %llu, \"ticks_max\": %llu, \"hist\": [",
                first ? "" : ",", w->kind, w->object, w->waiter_lr, w->waker_lr,
                (unsigned long long)w->count, w->us_total, w->us_max,
                (unsigned long long)w->ticks_total, (unsigned long long)w->ticks_max);
        first = 0;
        for (int b = 0; b < HIST_BUCKETS; ++b)
            fprintf(f, "%s%llu", b ? "," : "", (unsigned long long)w->hist[b]);
        fprintf(f, "]}");
    }
    fprintf(f, "\n],\n\"mmio\": [");
    first = 1;
    for (int i = 0; i < MAX_ACCESS; ++i) {
        const Access* a = &accesses[i];
        if (!a->count)
            continue;
        fprintf(f, "%s\n [\"%08X\", \"%08X\", %u, %u, %llu]", first ? "" : ",", a->reg, a->pc,
                a->write, a->size, (unsigned long long)a->count);
        first = 0;
    }
    fprintf(f, "\n],\n\"values\": [");
    first = 1;
    for (int i = 0; i < MAX_VALUES; ++i) {
        const Value* v = &values[i];
        if (!v->count)
            continue;
        fprintf(f, "%s\n [\"%08X\", \"%08X\", %u, %llu]", first ? "" : ",", v->reg, v->value,
                v->write, (unsigned long long)v->count);
        first = 0;
    }
    fprintf(f, "\n],\n\"timeline_every\": %d, \"timeline\": [", TIMELINE_EVERY);
    for (uint32_t r = 0; r < timeline_rows; ++r) {
        fprintf(f, "%s\n [%.3f, %llu, ", r ? "," : "", timeline[r].wall,
                (unsigned long long)timeline[r].frame);
        write_counters(f, timeline[r].c);
        fprintf(f, "]");
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    remove(output_path);
    rename(temp, output_path);
}

static void end_frame(void)
{
    for (int i = 0; i < COUNTER_COUNT; ++i) {
        const int is_us = i >= C_wait_draw_done_us;
        ++histogram[i][bucket_of(frame_now[i], is_us)];
        totals[i] += frame_now[i];
        frame_now[i] = 0;
    }
    ++frames;
    if (frames % TIMELINE_EVERY == 0 && timeline && timeline_rows < MAX_TIMELINE) {
        Row* r = &timeline[timeline_rows++];
        r->wall = wall_seconds();
        r->frame = frames;
        memcpy(r->c, totals, sizeof(totals));
    }
    if (frames % 600 == 0)
        flush();
}

/* ---- hooks --------------------------------------------------------------- */

static void pend(Pending* list, uint32_t object, uint32_t lr, double t, uint64_t tb)
{
    for (int i = 0; i < MAX_PENDING; ++i) {
        if (list[i].object == 0 || list[i].object == object) {
            list[i].object = object;
            list[i].lr = lr;
            list[i].t = t;
            list[i].tb = tb;
            return;
        }
    }
}

/* GXDrawDone sleeps on the finish queue itself rather than calling
   GXWaitDrawDone. */
#define IN_DRAW_DONE_WAIT(lr) (IN_RANGE(lr, 0x800B8DA8u, 0x4Cu) || IN_RANGE(lr, 0x800B8DF4u, 0x80u))

static int wait_counter(uint32_t lr)
{
    if (IN_DRAW_DONE_WAIT(lr))
        return C_wait_draw_done_us;
    if (IN_RANGE(lr, 0x800A8FE4u, 0x54u))
        return C_wait_retrace_us;
    return C_wait_other_us;
}

/* A sleep on the finish queue or the retrace queue ends when its interrupt
   handler runs: the handler's own OSWakeupThread call may be a direct native
   call that no hook sees. */
static void close_sleepers(int draw_done, uint32_t waker)
{
    const double t = now_us();
    const uint64_t tb = guest_cpu ? guest_cpu->timebase : 0;
    for (int i = 0; i < MAX_PENDING; ++i) {
        Pending* p = &sleepers[i];
        if (!p->object)
            continue;
        const int kind = wait_counter(p->lr);
        if (kind != (draw_done ? C_wait_draw_done_us : C_wait_retrace_us))
            continue;
        const double us = t - p->t;
        record_wait(0, p->object, p->lr, waker, us, tb - p->tb);
        frame_now[kind] += (uint64_t)(us > 0 ? us : 0);
        p->object = 0;
    }
}

static void hit(int target, CPUState* s)
{
    if (!output_path)
        return;
    if (!guest_cpu)
        guest_cpu = s;
    const double t = now_us();
    const uint64_t tb = s->timebase;
    const uint32_t lr = s->lr, r3 = s->gpr[3], r4 = s->gpr[4];
    count_caller(target, lr);
    switch (target) {
    case T_OS_DISPATCH_INTERRUPT:
        ++frame_now[C_irq_dispatch];
        break;
    case T_PE_FINISH_HANDLER:
        ++frame_now[C_irq_pe_finish];
        close_sleepers(1, targets[T_PE_FINISH_HANDLER].address);
        latency(LAT_SET_DRAW_DONE_TO_FINISH, &t_set_draw_done, &tb_set_draw_done, t, tb);
        latency(LAT_WAIT_TO_FINISH, &t_wait_draw_done, &tb_wait_draw_done, t, tb);
        break;
    case T_PE_TOKEN_HANDLER:
        ++frame_now[C_irq_pe_token];
        latency(LAT_SET_DRAW_SYNC_TO_TOKEN, &t_set_draw_sync, &tb_set_draw_sync, t, tb);
        break;
    case T_CP_HANDLER:
        ++frame_now[C_irq_cp];
        break;
    case T_VI_RETRACE_HANDLER:
        ++frame_now[C_irq_vi];
        close_sleepers(0, targets[T_VI_RETRACE_HANDLER].address);
        latency(LAT_VI_WAIT_TO_RETRACE, &t_vi_wait, &tb_vi_wait, t, tb);
        if (t_retrace >= 0)
            record_wait(2, LAT_RETRACE_TO_RETRACE, 0, 0, t - t_retrace, tb - tb_retrace);
        t_retrace = t;
        tb_retrace = tb;
        end_frame();
        break;
    case T_GX_SET_DRAW_DONE:
        ++frame_now[C_set_draw_done];
        t_set_draw_done = t;
        tb_set_draw_done = tb;
        break;
    case T_GX_WAIT_DRAW_DONE:
        ++frame_now[C_wait_draw_done];
        /* Only while a draw-done is outstanding; otherwise the next
           frame's finish would be paired with this call. */
        if (t_set_draw_done >= 0) {
            t_wait_draw_done = t;
            tb_wait_draw_done = tb;
        }
        break;
    case T_GX_DRAW_DONE:
        ++frame_now[C_draw_done];
        break;
    case T_GX_SET_DRAW_SYNC:
        ++frame_now[C_set_draw_sync];
        t_set_draw_sync = t;
        tb_set_draw_sync = tb;
        record_arg(target, lr, r3, 0);
        break;
    case T_HSD_COPY_XFB_ASYNC:
        ++frame_now[C_xfb_copy];
        break;
    case T_HSD_DRAW_DONE_CB:
        ++frame_now[C_cb_draw_done];
        break;
    case T_HSD_PRE_RETRACE_CB:
        ++frame_now[C_cb_pre_retrace];
        break;
    case T_HSD_POST_RETRACE_CB:
        ++frame_now[C_cb_post_retrace];
        break;
    case T_THP_POST_RETRACE_CB:
        ++frame_now[C_cb_thp_retrace];
        break;
    case T_GAPP_DRAW_SYNC_CB:
        ++frame_now[C_draw_sync_cb];
        record_arg(target, lr, r3, 0);
        break;
    case T_VI_WAIT_FOR_RETRACE:
        ++frame_now[C_vi_wait];
        t_vi_wait = t;
        tb_vi_wait = tb;
        break;
    case T_VI_FLUSH:
        if (t_frame >= 0)
            record_wait(2, LAT_FRAME, 0, 0, t - t_frame, tb - tb_frame);
        t_frame = t;
        tb_frame = tb;
        break;
    case T_GX_COPY_DISP:
        ++frame_now[C_copy_disp];
        break;
    case T_GX_FLUSH:
        ++frame_now[C_gx_flush];
        break;
    case T_OS_SLEEP_THREAD:
        ++frame_now[C_sleeps];
        if (IN_DRAW_DONE_WAIT(lr))
            ++frame_now[C_draw_done_slept];
        if (IN_RANGE(lr, 0x800A8FE4u, 0x54u))
            ++frame_now[C_vi_wait_slept];
        /* One thread per queue at a time is enough here: each queue the
           game sleeps on has one waiter. */
        pend(sleepers, r3, lr, t, tb);
        record_arg(target, lr, r3, 0);
        break;
    case T_OS_WAKEUP_THREAD:
        ++frame_now[C_wakeups];
        for (int i = 0; i < MAX_PENDING; ++i) {
            Pending* p = &sleepers[i];
            if (p->object != r3)
                continue;
            const double us = t - p->t;
            record_wait(0, r3, p->lr, lr, us, tb - p->tb);
            frame_now[wait_counter(p->lr)] += (uint64_t)(us > 0 ? us : 0);
            p->object = 0;
        }
        break;
    case T_OS_SUSPEND_THREAD:
        ++frame_now[C_suspends];
        pend(suspended, r3, lr, t, tb);
        record_arg(target, lr, r3, 0);
        break;
    case T_OS_RESUME_THREAD:
        ++frame_now[C_resumes];
        for (int i = 0; i < MAX_PENDING; ++i) {
            Pending* p = &suspended[i];
            if (p->object != r3)
                continue;
            record_wait(1, r3, p->lr, lr, t - p->t, tb - p->tb);
            p->object = 0;
        }
        record_arg(target, lr, r3, 0);
        break;
    case T_GX_SET_DRAW_SYNC_CB:
    case T_GX_SET_DRAW_DONE_CB:
    case T_VI_SET_PRE_RETRACE_CB:
    case T_VI_SET_POST_RETRACE_CB:
    case T_OS_MASK_INTERRUPTS:
    case T_OS_UNMASK_INTERRUPTS:
    case T_GX_SET_MISC:
    case T_GX_INIT_FIFO_LIMITS:
    case T_GX_SET_CPU_FIFO:
    case T_GX_SET_GP_FIFO:
    case T_GX_PIX_MODE_SYNC:
    case T_GX_ABORT:
    case T_GX_FN_800B8AE8:
    case T_GX_FN_800B7484:
    case T_GX_FIFO_READ_ENABLE:
    case T_GX_FIFO_READ_DISABLE:
    case T_GX_FIFO_LINK:
    case T_GX_WRITE_FIFO_INT_ENABLE:
    case T_GX_WRITE_FIFO_INT_RESET:
    case T_VI_SET_BLACK:
    case T_GAPP_SET_DRAW_SYNC:
        record_arg(target, lr, r3, r4);
        break;
    default:
        break;
    }
}

#define DEFINE_HOOK(id, address, name) \
    static void hook_##id(CPUState* s) { hit(T_##id, s); }
TARGETS(DEFINE_HOOK)

#define LIST_HOOK(id, address, name) RECOMP_HOOK(address, hook_##id),
static const ModernGekkoModHook hooks[] = {TARGETS(LIST_HOOK)};

/* mmio mode: one hook, at an address the release module is built hook-aware
   for (PSMTXIdentity; patches/GC6E01-module-hooks.txt), only to learn where
   the CPU state lives. */
static void hook_cpu_only(CPUState* s)
{
    if (output_path && !guest_cpu)
        guest_cpu = s;
}
static const ModernGekkoModHook mmio_hooks[] = {RECOMP_HOOK(0x800A2D38u, hook_cpu_only)};

static void on_load(const ModernGekkoModHostApi* host)
{
    (void)host;
    output_path = getenv("MODERNGEKKO_GX_SYNC");
    if (output_path && !*output_path)
        output_path = NULL;
    fprintf(stderr, "[gx-sync] %s (%s mode)\n",
            output_path ? output_path : "off (set MODERNGEKKO_GX_SYNC)",
            mmio_mode ? "mmio" : "hooks");
    if (!output_path)
        return;
    timeline = (Row*)calloc(MAX_TIMELINE, sizeof(Row));
#ifdef _WIN32
    installer = CreateThread(NULL, 0, installer_main, NULL, 0, NULL);
#else
    installer_started = pthread_create(&installer, NULL, installer_main, NULL) == 0;
#endif
}

static void on_unload(void)
{
    installer_stop = 1;
#ifdef _WIN32
    if (installer) {
        WaitForSingleObject(installer, 5000);
        CloseHandle(installer);
        installer = NULL;
    }
#else
    if (installer_started)
        pthread_join(installer, NULL);
#endif
    /* The mod may be unloaded while the state lives on: put the host's
       callbacks back. */
    CPUState* cpu = guest_cpu;
    if (cpu) {
        if (original_read && cpu->external_read == wrapped_read)
            cpu->external_read = original_read;
        if (original_write && cpu->external_write == wrapped_write)
            cpu->external_write = original_write;
    }
    flush();
}

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    NULL,
    0,
    NULL,
    0,
    hooks,
    sizeof(hooks) / sizeof(hooks[0]),
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    on_load,
    on_unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc* moderngekko_get_mod(void)
{
    const char* mode = getenv("MODERNGEKKO_GX_SYNC_MODE");
    mmio_mode = mode && strcmp(mode, "mmio") == 0;
    if (mmio_mode) {
        descriptor.hooks = mmio_hooks;
        descriptor.num_hooks = sizeof(mmio_hooks) / sizeof(mmio_hooks[0]);
    }
    return &descriptor;
}
