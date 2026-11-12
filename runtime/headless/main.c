/* vf4ft-headless: the translated game on VF4debug's machine, with no
 * window (milestone 2): the same options, scripts and closing report as
 * VF4debug, and --interp to run VF4debug's own interpreter instead, in the
 * same program, for comparison.
 *
 * recomp: this is VF4debug's src/main.c (MIT, same maintainer; see
 * runtime/machine/ORIGIN.md), changed where marked. */
#include "sh4.h"
#include "mem.h"
#include "hw.h"
#include "dispatch.h"
#include "observe.h"
#include "machine.h"
#include "control.h"
#include "hle.h"
#include "tables.h"
#include "view.h"
#include "net.h"
#include "rt_native.h"     /* recomp */
#include "aica.h"           /* recomp */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef DEFAULT_DECOMP
#define DEFAULT_DECOMP "../vf4ft-decomp"
#endif

static char path_image[1024], path_dimm[1024], path_log[1024];

static void usage(void)
{
    fputs(
        "usage: vf4ft-headless [options]\n"
        "  --interp          run VF4debug's interpreter instead of the translated code (recomp)\n"
        "  --sound MODEL     the sound board: mailbox (VF4debug's model, the default) or chip (the AICA with\n"
        "                    the game's own driver on its ARM7) (recomp)\n"
        "  --arm-budget N    with the chip: ARM7 instructions a sample (default 128) (recomp)\n"
        "  --wav FILE        with the chip: write what it plays, 44,100 Hz stereo (recomp)\n"
        "  --decomp DIR      the vf4ft-decomp checkout (default: $VF4_DECOMP, else " DEFAULT_DECOMP ")\n"
        "  --image FILE      the program image (default: DIR/work/main.bin, or $VF4_IMAGE)\n"
        "  --dimm FILE       the decrypted DIMM image with the asset files\n"
        "                    (default: DIR/work/vf41_s.dec, or $VF4_DIMM)\n"
        "  --frames N        stop after N virtual vertical blanks\n"
        "  --insns N         stop after N instructions\n"
        "  --script FILE     controller script; - reads commands from standard input\n"
        "  --log FILE        write the call log (JSON lines; README.md, \"The call log\")\n"
        "  --run NAME        run name in the log ids (default: harness)\n"
        "  --hits N          a routine's first N calls are kept whatever they show (default 3)\n"
        "  --max-hits N      calls kept per routine in all (default 56)\n"
        "  --peek N          bytes read behind a pointer of unknown size (default 64)\n"
        "  --writes N        stores a kept call may make before its write list is cut (default 8192)\n"
        "  --trace NAME      log and print every call of a routine (may be repeated)\n"
        "  --break NAME      stop when a routine is entered (may be repeated)\n"
        "  --country N       the board's region: 0 Japan (default), 1 USA, 2 export, 3 Korea, 4 Australia\n"
        "  --profile         count every call of every routine (the observer without the call log)\n"
        "  --coverage        the same: record which instructions ran\n"
        "  --out DIR         where the reports go (default: out)\n"
        "  --hw-trace        print each new hardware access as it happens (twice: every access)\n"
        "  --cards           the cabinet has its two card reader/writer units (default: none, as before)\n"
        "  --net             the DIMM board's network side answers, with a stand-in for the servers behind it\n"
        "                    (default: no answer, as before; no real connection is made either way)\n"
        "  --list-hle        print the replacements and hooks, then exit\n"
        "  -v                more to say (twice: every routine as it is first entered)\n"
        "  -q                less to say\n"
        "  --view DIR        draw what the game sends to the graphics hardware: PNG files in DIR\n"
        "  --view-every N    draw every Nth frame (default: 60 for --view alone, 2 for --live, else 1)\n"
        "  --view-from N     no picture, viewer or pacing before vertical blank N (6400 skips the boot)\n"
        "  --live            show the picture in a window as the run goes (needs ffplay)\n"
        "  --video FILE      write the picture as a video file (needs ffmpeg)\n"
        "  --view-cmd CMD    send the frames (640x480 RGB, raw) to the standard input of a command\n"
        "  --view-marks      put a cross where the game's own record says each fighter is on the screen\n"
        "  --realtime        keep the game's pace: 60 vertical blanks a second\n"
        "  --diag WORD       diagnosis only: one thing as it was before milestone 4, to lay a difference between\n"
        "                    two runs at one change's door: hostnan (a NaN result keeps the host's bits),\n"
        "                    nofpflags (no FPSCR cause and flag bits), oldvectors (the vector stubs leave the\n"
        "                    entry's address in r0), oldboardword (the DIMM board's word without the loaded size);\n"
        "                    may be repeated\n",
        stderr);
}

static void on_interrupt(int sig)
{
    (void)sig;
    if (machine_interrupted)
        _Exit(130);             /* a second Ctrl-C: now */
    machine_interrupted = 1;
}

/* recomp: --wav: the chip's output, taken at every vertical blank */
static FILE *wav_f;
static unsigned long long wav_frames;

static void wav_header(FILE *f, unsigned long long frames)
{
    unsigned bytes = (unsigned)(frames * 4);
    unsigned char h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                            0x44, 0xac, 0, 0, 0x10, 0xb1, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a' };
    unsigned riff = bytes + 36;
    for (int i = 0; i < 4; i++) {
        h[4 + i] = (unsigned char)(riff >> (8 * i));
    }
    fwrite(h, 1, 40, f);
    unsigned char b[4] = { (unsigned char)bytes, (unsigned char)(bytes >> 8), (unsigned char)(bytes >> 16),
                           (unsigned char)(bytes >> 24) };
    fwrite(b, 1, 4, f);
}

static void wav_frame(sh4 *c)
{
    (void)c;
    int16_t buf[2 * 2048];
    unsigned n;
    while ((n = aica_audio_take(buf, 2048)) > 0) {
        fwrite(buf, 4, n, wav_f);
        wav_frames += n;
    }
}

void write_reports(void)
{
    char path[1200];
    const char *dir = cfg.report_dir ? cfg.report_dir : "out";
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/hardware.tsv", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        hw_report(f, 0);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/routines.tsv", dir);
    obs_write_profile(path);
    snprintf(path, sizeof path, "%s/coverage.tsv", dir);
    obs_write_coverage(path);
    snprintf(path, sizeof path, "%s/edges.tsv", dir);
    obs_write_edges(path);
    snprintf(path, sizeof path, "%s/hle.tsv", dir);
    f = fopen(path, "w");
    if (f) {
        hle_list(f);
        fclose(f);
    }
    if (obs_log)
        fflush(obs_log);
}

int main(int argc, char **argv)
{
    const char *traces[64], *breaks[64];
    int trace_n = 0, break_n = 0, list_hle = 0, view_used;
    int interp = 0;     /* recomp */
    const char *wav_path = NULL;    /* recomp */
    memset(&cfg, 0, sizeof cfg);
    cfg.decomp = getenv("VF4_DECOMP") ? getenv("VF4_DECOMP") : DEFAULT_DECOMP;
    cfg.image = getenv("VF4_IMAGE");
    cfg.dimm = getenv("VF4_DIMM");
    cfg.run_name = "harness";
    cfg.report_dir = "out";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED() do { if (!v) { usage(); return 2; } i++; } while (0)
        if (!strcmp(a, "--decomp")) { NEED(); cfg.decomp = v; }
        else if (!strcmp(a, "--image")) { NEED(); cfg.image = v; }
        else if (!strcmp(a, "--dimm")) { NEED(); cfg.dimm = v; }
        else if (!strcmp(a, "--frames")) { NEED(); cfg.max_frames = (u32)strtoul(v, NULL, 0); }
        else if (!strcmp(a, "--insns")) { NEED(); cfg.max_insns = strtoull(v, NULL, 0); }
        else if (!strcmp(a, "--script")) { NEED(); cfg.script = v; }
        else if (!strcmp(a, "--log")) { NEED(); cfg.log_path = v; }
        else if (!strcmp(a, "--run")) { NEED(); cfg.run_name = v; }
        else if (!strcmp(a, "--hits")) { NEED(); obs_rules.hits = atoi(v); }
        else if (!strcmp(a, "--max-hits")) { NEED(); obs_rules.max_hits = atoi(v); }
        else if (!strcmp(a, "--peek")) { NEED(); obs_rules.peek = atoi(v); }
        else if (!strcmp(a, "--writes")) { NEED(); obs_rules.writes = atoi(v); }
        else if (!strcmp(a, "--trace")) { NEED(); if (trace_n < 64) traces[trace_n++] = v; }
        else if (!strcmp(a, "--break")) { NEED(); if (break_n < 64) breaks[break_n++] = v; }
        else if (!strcmp(a, "--out")) { NEED(); cfg.report_dir = v; }
        else if (!strcmp(a, "--country")) { NEED(); cfg.country = atoi(v); }
        else if (!strcmp(a, "--profile")) cfg.profile = 1;
        else if (!strcmp(a, "--coverage")) cfg.coverage = 1;
        else if (!strcmp(a, "--hw-trace")) cfg.hw_trace++;
        else if (!strcmp(a, "--cards")) hw_card_option = 1;
        else if (!strcmp(a, "--net")) hw_net_option = 1;
        else if (!strcmp(a, "--list-hle")) list_hle = 1;
        else if (!strcmp(a, "--interp")) interp = 1;      /* recomp */
        else if (!strcmp(a, "--sound")) {                 /* recomp */
            NEED();
            if (!strcmp(v, "chip")) hw_sound_chip = 1;
            else if (!strcmp(v, "mailbox")) hw_sound_chip = 0;
            else { usage(); return 2; }
        }
        else if (!strcmp(a, "--arm-budget")) { NEED(); hw_sound_arm_budget = (unsigned)strtoul(v, NULL, 0); }   /* recomp */
        else if (!strcmp(a, "--wav")) { NEED(); wav_path = v; }      /* recomp */
        else if (!strcmp(a, "--diag")) {
            NEED();
            if (!strcmp(v, "hostnan")) sh4_diag |= DIAG_HOST_NAN;
            else if (!strcmp(v, "nofpflags")) sh4_diag |= DIAG_NO_FP_FLAGS;
            else if (!strcmp(v, "oldvectors")) sh4_diag |= DIAG_OLD_VECTORS;
            else if (!strcmp(v, "oldboardword")) sh4_diag |= DIAG_OLD_BOARD_WORD;
            else { usage(); return 2; }
        }
        else if (!strcmp(a, "-v")) cfg.verbose++;
        else if (!strcmp(a, "-q")) cfg.quiet = 1;
        else if ((view_used = view_option(a, v)) != 0) { if (view_used < 0) { usage(); return 2; } i += view_used - 1; }
        else { usage(); return 2; }
    }
    if (!cfg.image) {
        snprintf(path_image, sizeof path_image, "%s/work/main.bin", cfg.decomp);
        cfg.image = path_image;
    }
    if (!cfg.dimm) {
        snprintf(path_dimm, sizeof path_dimm, "%s/work/vf41_s.dec", cfg.decomp);
        cfg.dimm = path_dimm;
    }
    (void)path_log;

    if (mem_init() || mem_add_ram("main-ram", RAM_BASE, RAM_SIZE) ||
        mem_add_ram("harness-scratch", CONTROL_SCRATCH, CONTROL_SCRATCH_SIZE))
        return 1;
    u32 loaded = 0;
    if (mem_load_file(cfg.image, IMAGE_BASE, 0x340000, &loaded) || loaded != 0x340000) {
        fprintf(stderr, "vf4ft-headless: %s is not the 3,407,872-byte program image (work/main.bin of the "
                "decompilation); see README.md\n", cfg.image);
        return 1;
    }
    if (disp_init())
        return 1;
    if (rt_native_names_check())      /* recomp: moved to rt_native.c */
        return 1;
    machine_init();
    hw_init(&cpu);
    if (hw_cart_init(cfg.dimm))
        return 1;
    sh4_reset(&cpu, IMAGE_BASE);
    if (control_init(cfg.script))
        return 1;
    if (hle_init(&cpu))
        return 1;
    hw_net_boot(&cpu);
    if (view_init())
        return 1;
    mem_guards();
    if (list_hle) {
        hle_list(stdout);
        return 0;
    }
    /* recomp: the observer watches the interpreter's every instruction */
    if (!interp && (cfg.coverage || cfg.profile || cfg.log_path || trace_n)) {
        fprintf(stderr, "vf4ft-headless: --log, --profile, --coverage and --trace need --interp\n");
        return 2;
    }
    if (!interp && rt_native_init(cfg.image))
        return 1;
    if (wav_path) {                     /* recomp */
        if (!hw_sound_chip) {
            fprintf(stderr, "vf4ft-headless: --wav needs --sound chip\n");
            return 2;
        }
        wav_f = fopen(wav_path, "wb");
        if (!wav_f) {
            perror(wav_path);
            return 1;
        }
        wav_header(wav_f, 0);
        machine_host_frame = wav_frame;
    }
    if ((cfg.coverage || cfg.profile) && obs_enable())
        return 1;
    if (cfg.log_path && obs_open(cfg.log_path, cfg.run_name))
        return 1;
    for (int i = 0; i < trace_n; i++) {
        routine *r = routine_by_name(traces[i]);
        if (!r) {
            fprintf(stderr, "vf4ft-headless: no routine named %s\n", traces[i]);
            return 2;
        }
        disp_set_flag(r, DF_TRACE | DF_LOG, 1);
    }
    for (int i = 0; i < break_n; i++) {
        routine *r = routine_by_name(breaks[i]);
        if (!r) {
            fprintf(stderr, "vf4ft-headless: no routine named %s\n", breaks[i]);
            return 2;
        }
        disp_set_flag(r, DF_BREAK, 1);
    }

    signal(SIGINT, on_interrupt);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int code = STOP_NONE;
    if (!interp) {
        code = rt_native_run(&cpu);         /* recomp */
    } else {
        while (control_next(&cpu)) {
            code = machine_run(&cpu);
            if (!control_stopped(&cpu, code) || machine_interrupted)
                break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (!cfg.quiet) {
        char mode[160];
        fprintf(stderr, "\nvf4ft-headless (%s): stopped: %s%s%s\n", interp ? "interpreter" : "translated",
                sh4_stop_name(code), machine_stop_text[0] ? ": " : "", machine_stop_text);
        if (!interp)                        /* recomp */
            fprintf(stderr, "  translated: %llu slow blocks, %llu instructions interpreted, %llu exceptions, "
                    "%llu calls outside translated code\n"
                    "  changed code: %llu stores into code pages, %llu blocks changed\n", rt_stats.slow_blocks,
                    rt_stats.interp_insns, rt_stats.exceptions, rt_stats.outside_calls, rt_stats.code_page_stores,
                    rt_stats.changed_blocks);
        machine_where(&cpu, stderr);
        if (code != STOP_BREAK && code != STOP_LIMIT)
            machine_backtrace(&cpu, stderr);
        hw_gfx_stats(stderr);
        hw_cart_stats(stderr);
        hw_sound_stats(stderr);
        hw_net_stats(stderr);
        hw_card_stats(stderr);
        view_stats(stderr);
        if (sh4_nan_results || sh4_fpu_traps || sh4_diag)
            fprintf(stderr, "  FPU: %llu results made the processor's NaN (7FBFFFFF), %llu exceptions trapped%s\n",
                    (unsigned long long)sh4_nan_results, (unsigned long long)sh4_fpu_traps,
                    sh4_diag ? " (--diag in use: not a faithful run)" : "");
        if (hw_sandbox_unmodelled)
            fprintf(stderr, "  %d hardware accesses without a model inside sandboxes (routines the harness called)\n",
                    hw_sandbox_unmodelled);
        fprintf(stderr, "  %d of %d routines entered; %d hardware accesses without a model (see %s/hardware.tsv)\n",
                obs_seen_count(), routine_count, hw_unmodelled_count(), cfg.report_dir);
        fprintf(stderr, "  %.2f s, %.1f million instructions a second; last mode %s\n", secs,
                secs > 0 ? (double)cpu.insns / secs / 1e6 : 0.0,
                game_mode_name(game_mode_main(), game_mode_sub(), mode, sizeof mode));
    }
    if (wav_f) {                        /* recomp */
        wav_frame(&cpu);
        fseek(wav_f, 0, SEEK_SET);
        wav_header(wav_f, wav_frames);
        fclose(wav_f);
    }
    view_close();
    write_reports();
    obs_close();
    return (code == STOP_BREAK || code == STOP_LIMIT || code == STOP_NONE) && !control_failed ? 0 : 3;
}
