/* The controller. One command per line; '#' starts a comment. Commands
 * that let the machine run ("run", "until ...") end when their condition
 * holds or their frame budget is used up; every stop is reported as one
 * JSON line on standard output. With the script on standard input
 * (--script -) a driver program can hold a conversation: write a command,
 * read lines until the one with "ev":"stopped" or "ev":"done".
 */
#include "control.h"
#include "machine.h"
#include "observe.h"
#include "hw.h"
#include "tables.h"
#include "view.h"
#include "net.h"
#include "hle.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

input_state inputs;

static FILE *script;
static int script_is_stdin;
static int line_no;
static int load_failed;                 /* the last "load" did not work: see execute() */
static char load_failed_text[256];
int control_failed;                     /* a command failed in a way that ends a script: the exit status says so */

enum { W_NONE, W_FRAMES, W_MODE, W_FN, W_MEM, W_INSNS, W_GAMEFRAME, W_CALL };
static struct {
    int kind;
    u32 until_frame;        /* budget: stop at this virtual frame whatever happens */
    int mode_main, mode_sub;
    routine *fn;
    u32 addr, value;
    int size, op;
    u64 insns;
    char text[128];
} wait;

/* switches held for a number of frames, then released */
#define HOLD_MAX 32
static struct hold {
    int group;              /* 0, 1: player; 2: system; 3: coin */
    u32 bits;
    u32 release_frame;
} holds[HOLD_MAX];
static int hold_n;

static const char *const help_text =
    "commands (one per line, # starts a comment):\n"
    "  run N                      let N vertical blanks pass\n"
    "  until mode M.S [max N]     run until the game is in main mode M, sub mode S (numbers, or the\n"
    "                             game's names with _ for spaces: ADVERTISE.TITLE_DISP,\n"
    "                             GAME_MODE.GAME_DISP); S may be left out\n"
    "  until fn NAME [max N]      run until the routine is entered\n"
    "  until mem ADDR OP VALUE [SIZE] [max N]   OP is == != < > ; checked once a frame\n"
    "  until gameframe N [max N]  run until game_frame_count reaches N\n"
    "  press WHO BUTTONS [N]      hold buttons for N frames (default 6), then let go\n"
    "  hold WHO BUTTONS           hold until released\n"
    "  release WHO BUTTONS | release all\n"
    "      WHO: p1 p2 sys; BUTTONS: up down left right p k g e start (p1/p2), test service (sys),\n"
    "      joined by '+': press p1 p+k 4\n"
    "  coin [1|2] [N]             close the coin switch of chute 1 or 2 for N frames (default 6)\n"
    "  poke ADDR VALUE [SIZE]     write a 1, 2 or 4 (default) byte value\n"
    "  pokef ADDR VALUE           write a float\n"
    "  peek ADDR [SIZE] [COUNT]   read values\n"
    "  ADDR is a named global (cpu_on_off), name+offset (robs+0x40), a number, or @rN: the\n"
    "      address register N holds (@r4+8 at a routine's entry: 8 bytes into its first argument)\n"
    "  break NAME | unbreak NAME  stop whenever the routine is entered\n"
    "  trace NAME | untrace NAME  log every call of the routine (and print it)\n"
    "  watch ADDR [LEN] [r|w|rw] [stop]   report (or stop on) accesses; unwatch removes all\n"
    "  save FILE | load FILE      snapshot of the whole machine\n"
    "  jvs fault ack V [N] | status V [N] | silent [N] | off   the I/O board answers badly for N frames\n"
    "  sound stall N | off                   the sound driver takes no request for N frames\n"
    "  backup save FILE | backup load FILE   the battery-backed RAM and the settings EEPROM to and from a file\n"
    "  backup damage sram|eeprom OFFSET      turn one byte over (one of the game's two copies then fails its check)\n"
    "  sandbox begin | sandbox end   what is done in between is thrown away: the machine is put back\n"
    "  call NAME [ARG ...] [irq] [max N]   run a routine of the image now, in a sandbox, until it returns (at\n"
    "                             most N frames, default 4); ARG: a number, a float (1.5f), an address by name,\n"
    "                             buf:0 ... buf:7 (8 KiB of scratch memory each), str:TEXT (\\s for a space);\n"
    "                             irq: with interrupts allowed (a routine that waits for the hardware)\n"
    "  snap [*]NAME LEN [for PATTERN]   keep LEN bytes at NAME (*: at what NAME points to) with every full\n"
    "                             record of the routines PATTERN names (a name, or a glob over file/name)\n"
    "  frameline NAME [SIZE]      one more global in the call log's frame lines\n"
    "  reg rN|pr VALUE            set a register (at a routine's entry r4-r7 are its arguments)\n"
    "  hash                       a fingerprint of main RAM and the registers (to compare runs)\n"
    "  status                     where the game is\n"
    "  where                      registers and the call chain\n"
    "  report                     write the reports now\n"
    "  picture FILE               draw the next frame the game renders into a PNG file\n"
    "  card units on|off          the cabinet has the two card units (before the first run; or --cards)\n"
    "  card p1|p2 insert FILE|blank [NUMBER]   a card goes in: a 128-byte image, or a card never used\n"
    "  card p1|p2 eject | pull [after N] | save FILE | status | delay N|default\n"
    "  card p1|p2 fault off | error CODE [REPLIES] | silent [FRAMES] | refuse [REPLIES]\n"
    "  net on|off                 the DIMM board's network side, with a stand-in for the servers behind it\n"
    "                             (before the first run; or --net). No real connection is ever made\n"
    "  net dns NAME ADDR|none|fail [delay MS]      net host ADDR accept|refuse|unreachable|silent [delay MS]\n"
    "  net reply KEY [once|times N] [status CODE REASON] [pack none|stored|fixed|dynamic|bad]\n"
    "            [frame length|chunked|close] [fail drop|hang|reset|garbage|cut] [delay MS] [piece N] [body TEXT]\n"
    "            KEY: poweron score power_on start end message select; net reply KEY default\n"
    "  net clock +SECONDS | net keepalive MS | net load FILE | net status   (README.md has the rest)\n"
    "  echo TEXT\n"
    "  quit\n";

int control_init(const char *path)
{
    memset(&inputs, 0, sizeof inputs);
    memset(&wait, 0, sizeof wait);
    hold_n = 0;
    script = NULL;
    snapshot_register("inputs", &inputs, sizeof inputs);
    snapshot_register("holds", holds, sizeof holds);
    snapshot_register("hold_n", &hold_n, sizeof hold_n);
    if (!path)
        return 0;
    if (!strcmp(path, "-")) {
        script = stdin;
        script_is_stdin = 1;
        setvbuf(stdout, NULL, _IOLBF, 0);
        return 0;
    }
    script = fopen(path, "r");
    if (!script) {
        perror(path);
        return -1;
    }
    return 0;
}

static void say_json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    }
    putchar('"');
}

static void say_state(sh4 *c, const char *ev, const char *reason)
{
    char mode[160];
    printf("{\"ev\":\"%s\",\"reason\":", ev);
    say_json_str(reason ? reason : "");
    printf(",\"frame\":%u,\"game_frame\":%u,\"vblank_count\":%u,\"mode\":\"%d.%d\",\"mode_name\":", machine_frame,
           game_frames(), game_vblanks(), game_mode_main(), game_mode_sub());
    say_json_str(game_mode_name(game_mode_main(), game_mode_sub(), mode, sizeof mode));
    printf(",\"pc\":\"%08x\",\"at\":", c->pc);
    say_json_str(addr_text(c->pc));
    printf(",\"insns\":%llu,\"seen\":%d}\n", (unsigned long long)c->insns, obs_seen_count());
    fflush(stdout);
}

u32 control_resolve(const char *text)
{
    char name[128];
    u32 off = 0;
    snprintf(name, sizeof name, "%s", text);
    char *plus = strchr(name, '+');
    if (plus) {
        *plus = 0;
        off = (u32)strtoul(plus + 1, NULL, 0);
    }
    if (isdigit((unsigned char)name[0]))
        return (u32)strtoul(name, NULL, 0) + off;
    /* @r4: the address a register holds (at a routine's entry: what a pointer argument points at) */
    if (name[0] == '@' && name[1] == 'r' && isdigit((unsigned char)name[2]) && atoi(name + 2) < 16)
        return cpu.r[atoi(name + 2)] + off;
    u32 a = global_addr(name);
    if (!a) {
        routine *r = routine_by_name(name);
        if (r)
            a = r->addr;
    }
    return a ? a + off : 0;
}

static int parse_buttons(int group, const char *text, u32 *bits)
{
    static const struct {
        const char *name;
        u32 bit;
    } pl[] = { { "up", BTN_UP }, { "down", BTN_DOWN }, { "left", BTN_LEFT }, { "right", BTN_RIGHT },
               { "p", BTN_P }, { "k", BTN_K }, { "g", BTN_G }, { "e", BTN_E }, { "start", BTN_START } },
      sy[] = { { "test", SYS_TEST }, { "service", SYS_SERVICE } };
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    *bits = 0;
    for (char *tok = strtok(buf, "+,"); tok; tok = strtok(NULL, "+,")) {
        int found = 0;
        for (char *p = tok; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        if (group < 2) {
            for (size_t i = 0; i < sizeof pl / sizeof pl[0]; i++)
                if (!strcmp(tok, pl[i].name)) {
                    *bits |= pl[i].bit;
                    found = 1;
                }
        } else {
            for (size_t i = 0; i < sizeof sy / sizeof sy[0]; i++)
                if (!strcmp(tok, sy[i].name)) {
                    *bits |= sy[i].bit;
                    found = 1;
                }
        }
        if (!found)
            return -1;
    }
    return 0;
}

static int parse_who(const char *text)
{
    if (!strcasecmp(text, "p1")) return 0;
    if (!strcasecmp(text, "p2")) return 1;
    if (!strcasecmp(text, "sys")) return 2;
    return -1;
}

static void set_bits(int group, u32 bits, int on)
{
    u32 *p = group < 2 ? &inputs.player[group] : group == 2 ? &inputs.sys : NULL;
    if (group == 3) {
        for (int ch = 0; ch < 2; ch++) {
            if (!(bits & (1u << ch)))
                continue;
            if (on && !inputs.coin_switch[ch])
                inputs.coins[ch]++;
            inputs.coin_switch[ch] = (u32)on;
        }
        return;
    }
    if (on)
        *p |= bits;
    else
        *p &= ~bits;
}

static void hold_for(int group, u32 bits, u32 frames)
{
    set_bits(group, bits, 1);
    if (hold_n < HOLD_MAX) {
        holds[hold_n].group = group;
        holds[hold_n].bits = bits;
        holds[hold_n].release_frame = machine_frame + frames;
        hold_n++;
    }
}

static int parse_mode(const char *text, int *m, int *s)
{
    *s = -1;
    if (isdigit((unsigned char)text[0])) {
        *m = atoi(text);
        const char *dot = strchr(text, '.');
        if (dot)
            *s = atoi(dot + 1);
        return 0;
    }
    char name[160];
    for (int mm = 0; mm < 8; mm++) {
        for (int ss = -1; ss < 16; ss++) {
            game_mode_name(mm, ss < 0 ? 99 : ss, name, sizeof name);
            if (ss < 0) {
                char *dot = strrchr(name, '.');
                if (dot)
                    *dot = 0;
            }
            /* the game's names have spaces ("GAME MODE"); a script writes them with underscores */
            for (char *q = name; *q; q++)
                if (*q == ' ')
                    *q = '_';
            if (!strcasecmp(name, text)) {
                *m = mm;
                *s = ss;
                return 0;
            }
        }
    }
    return -1;
}

static u32 take_max(char **tok, int n, u32 dflt)
{
    for (int i = 0; i + 1 < n; i++)
        if (!strcmp(tok[i], "max"))
            return (u32)strtoul(tok[i + 1], NULL, 0);
    return dflt;
}

static void err(const char *msg, const char *arg)
{
    printf("{\"ev\":\"error\",\"line\":%d,\"text\":", line_no);
    char buf[256];
    snprintf(buf, sizeof buf, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
    say_json_str(buf);
    printf("}\n");
    fflush(stdout);
}

/* ---- sandboxes and harness calls ----
 *
 * "sandbox begin" takes a copy of the whole machine into memory; what the
 * script does then (calls of routines, pokes, even frames of the game)
 * happens for real, and "sandbox end" puts the copy back: the game goes on
 * as if nothing had been done. "call NAME ARGS" runs one routine of the
 * image with arguments placed by its prototype (the ABI: integers and
 * pointers in r4-r7 and then on the stack, floats in fr4-fr11) until it
 * returns, inside the open sandbox or, when none is open, in one of its
 * own. The observer keeps everything that runs in a sandbox apart from the
 * game's own run (observe.c): such records show what the code does when it
 * is called, not that the game calls it. */

static struct {
    int on, own;                /* own: made by a "call", closed when the call ends */
    u8 *data;
    size_t size;
    u32 id;
    int exc_depth;
    u32 str_off;
} sbx;
static u32 sbx_ids;
static struct {
    int active;
    routine *r;
    sh4 before;                 /* the processor as it was, put back after the call (but for time) */
    u64 insns0;
    u32 frame0;
} callst;

static int sandbox_begin(void)
{
    if (sbx.on)
        return -1;
    if (snapshot_save_mem(&sbx.data, &sbx.size))
        return -1;
    sbx.on = 1;
    sbx.own = 0;
    sbx.id = ++sbx_ids;
    sbx.exc_depth = disp_exc_depth;
    sbx.str_off = 0;
    obs_sandbox_begin(sbx.id);
    return 0;
}

static void sandbox_end(void)
{
    if (!sbx.on)
        return;
    obs_sandbox_end();
    if (snapshot_load_mem(sbx.data, sbx.size))
        fprintf(stderr, "control: %s\n", snapshot_error);       /* (cannot happen: this process wrote it) */
    disp_exc_depth = sbx.exc_depth;
    free(sbx.data);
    sbx.data = NULL;
    sbx.on = 0;
    memset(&wait, 0, sizeof wait);
}

/* one argument of "call": a number, a float (1.5f, or anything with a '.'), an address by name
 * (as everywhere), buf:N[+OFFSET], str:TEXT. *is_float says how the text was meant. */
static int call_arg(const char *text, u32 *value, int *is_float)
{
    *is_float = 0;
    if (!strncmp(text, "buf:", 4)) {
        u32 n = (u32)strtoul(text + 4, NULL, 0);
        const char *plus = strchr(text, '+');
        if (n > 7)
            return -1;
        *value = CONTROL_SCRATCH + n * 0x2000u + (plus ? (u32)strtoul(plus + 1, NULL, 0) : 0);
        return 0;
    }
    if (!strncmp(text, "str:", 4)) {
        /* the text into the string area, NUL at its end; \s is a space, \n a line end */
        u32 a = CONTROL_SCRATCH + 0x10000u + sbx.str_off;
        u32 n = 0;
        for (const char *p = text + 4; *p && sbx.str_off + n + 2 < 0x10000u; p++) {
            char ch = *p;
            if (ch == '\\' && p[1] == 's') ch = ' ', p++;
            else if (ch == '\\' && p[1] == 'n') ch = '\n', p++;
            g_w8(a + n++, (u8)ch);
        }
        g_w8(a + n++, 0);
        sbx.str_off = (sbx.str_off + n + 3) & ~3u;
        *value = a;
        return 0;
    }
    size_t len = strlen(text);
    int numeric = isdigit((unsigned char)text[0]) || ((text[0] == '-' || text[0] == '.') && text[1]);
    if (numeric && !(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) &&
        (strchr(text, '.') || text[len - 1] == 'f')) {
        float f = strtof(text, NULL);
        memcpy(value, &f, 4);
        *is_float = 1;
        return 0;
    }
    if (text[0] == '-' && isdigit((unsigned char)text[1])) {
        *value = (u32)strtol(text, NULL, 0);
        return 0;
    }
    if (isdigit((unsigned char)text[0])) {
        *value = (u32)strtoul(text, NULL, 0);
        return 0;
    }
    *value = control_resolve(text);
    return *value ? 0 : -1;
}

/* starts a routine with the arguments in tok[]; 0 when the machine has to run now */
static int call_start(sh4 *c, routine *r, char **tok, int n, u32 budget, int with_irq, char *msg, size_t msg_n)
{
    /* the classes of the parameters, from the prototype (tools/gen_tables.py: one word per
     * parameter, a class letter and a size; ">" starts the result) */
    char cls[16];
    u32 csize[16];
    int ncls = 0;
    const char *spec = prog_routines[r - routines].spec;    /* recomp (test mode): the program's table */
    for (const char *p = spec; p && *p;) {
        while (*p == ' ')
            p++;
        if (!*p || *p == '>')
            break;
        if (ncls < 16) {
            cls[ncls] = *p;
            csize[ncls] = (u32)strtoul(p + 1, NULL, 10);
            ncls++;
        }
        while (*p && *p != ' ')
            p++;
    }
    u32 ir[4], fr[8], stk[16];
    int ireg = 0, freg = 0, nstk = 0;
    for (int i = 0; i < n; i++) {
        u32 v;
        int is_float;
        if (call_arg(tok[i], &v, &is_float)) {
            snprintf(msg, msg_n, "call: cannot make an argument of %s", tok[i]);
            return -1;
        }
        char k = i < ncls ? cls[i] : is_float ? 'f' : 'i';
        if (k == 'f' || k == 'd') {
            if (!is_float) {            /* a whole number given for a float parameter */
                float f = (float)(s32)v;
                memcpy(&v, &f, 4);
            }
            if (k == 'd') {
                /* a double: the pair of an even register, high word first */
                float f;
                memcpy(&f, &v, 4);
                double d = (double)f;
                u64 bits;
                memcpy(&bits, &d, 8);
                freg = (freg + 1) & ~1;
                if (freg <= 6) {
                    fr[freg++] = (u32)(bits >> 32);
                    fr[freg++] = (u32)bits;
                } else if (nstk + 2 <= 16) {
                    stk[nstk++] = (u32)bits;
                    stk[nstk++] = (u32)(bits >> 32);
                }
            } else if (freg < 8) {
                fr[freg++] = v;
            } else if (nstk < 16) {
                stk[nstk++] = v;
            }
        } else if (k == 's') {
            /* a structure by value: its bytes are taken from where the argument points */
            u32 words = (csize[i] + 3) / 4;
            if (!words)
                words = 1;
            if (!mem_ptr(v) || words > 8) {
                snprintf(msg, msg_n, "call: parameter %d is a structure by value: give the address of its bytes", i + 1);
                return -1;
            }
            if (ireg + (int)words <= 4) {
                for (u32 w = 0; w < words; w++)
                    ir[ireg++] = g_r32(v + 4 * w);
            } else {
                for (u32 w = 0; w < words && nstk < 16; w++)
                    stk[nstk++] = g_r32(v + 4 * w);
            }
        } else if (ireg < 4) {
            ir[ireg++] = v;
        } else if (nstk < 16) {
            stk[nstk++] = v;
        }
    }
    if (!sbx.on) {
        if (sandbox_begin()) {
            snprintf(msg, msg_n, "call: no memory for the sandbox");
            return -1;
        }
        sbx.own = 1;
    }
    callst.active = 1;
    callst.r = r;
    callst.before = *c;
    callst.insns0 = c->insns;
    callst.frame0 = machine_frame;
    for (int i = 0; i < ireg; i++)
        c->r[4 + i] = ir[i];
    for (int i = 0; i < freg; i++)
        c->fr[4 + i].u = fr[i];
    u32 sp = ((c->r[15] - 0x200u) & ~7u) - 4u * (u32)nstk;
    for (int i = 0; i < nstk; i++)
        g_w32(sp + 4u * (u32)i, stk[i]);
    c->r[15] = sp;
    c->pr = SH4_RETURN_MAGIC;
    c->pc = r->addr;
    c->stop = 0;
    /* interrupts are held off for the call (the one that is waiting at this frame's end would
     * otherwise run inside it), unless the script says the routine needs them ("irq": one that
     * waits for the hardware to answer) */
    if (!with_irq)
        sh4_set_sr(c, sh4_get_sr(c) | SR_IMASK);
    obs_harness_call(r);
    disp_enter(c, r->addr, DISP_CALL);
    wait.kind = W_CALL;
    wait.until_frame = machine_frame + budget;
    snprintf(wait.text, sizeof wait.text, "the return of %s", r->name);
    return 0;
}

/* the call is over (it returned, its budget ran out, or the machine halted in it) */
static void call_finish(sh4 *c, int code)
{
    const char *ended = code == STOP_RETURN ? "returned" : code == STOP_BREAK ? "no return within its budget" : "halted";
    printf("{\"ev\":\"call\",\"name\":\"%s\",\"ended\":\"%s\"", callst.r->name, ended);
    if (code != STOP_RETURN && code != STOP_BREAK) {
        printf(",\"reason\":");
        say_json_str(machine_stop_text);
    }
    float f0 = c->fr[0].f;
    printf(",\"r0\":%u,\"r1\":%u,\"fr0b\":%u,\"fr1b\":%u,\"fpul\":%u,\"fr0\":", c->r[0], c->r[1], c->fr[0].u, c->fr[1].u,
           c->fpul);
    if (f0 == f0 && f0 - f0 == 0.0f)
        printf("%.9g", (double)f0);
    else
        printf("null");
    printf(",\"insns\":%llu,\"frames\":%u,\"sandbox\":%u}\n", (unsigned long long)(c->insns - callst.insns0),
           machine_frame - callst.frame0, sbx.id);
    fflush(stdout);
    callst.active = 0;
    wait.kind = W_NONE;
    if (sbx.own) {
        sandbox_end();
    } else {
        /* inside the script's own sandbox: the processor as before the call, so that the
         * script can go on (the memory keeps what the routine did; time has passed) */
        u64 cycles = c->cycles, insns = c->insns;
        *c = callst.before;
        c->cycles = cycles;
        c->insns = insns;
        c->next_event = 0;
        c->stop = 0;
    }
}

/* returns 1 when the machine has to run now, 0 to go on with the next
 * command, -1 to end */
static int execute(sh4 *c, char *line)
{
    char *tok[16];
    int n = 0;
    for (char *p = line, quoted = 0; *p; p++) {      /* a comment starts at a # outside quotes */
        if (*p == '"')
            quoted = !quoted;
        else if (*p == '#' && !quoted) {
            *p = 0;
            break;
        }
    }
    char echo_copy[512];
    snprintf(echo_copy, sizeof echo_copy, "%s", line);
    /* words; one in double quotes may hold spaces (a file name) */
    for (char *p = line; *p && n < 16;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    if (n == 0)
        return 0;
    const char *cmd = tok[0];

    if (load_failed && (!strcmp(cmd, "run") || !strcmp(cmd, "frames") || !strcmp(cmd, "until"))) {
        say_state(c, "halted", load_failed_text);
        return 0;
    }
    if (!strcmp(cmd, "run") || !strcmp(cmd, "frames")) {
        wait.kind = W_FRAMES;
        wait.until_frame = machine_frame + (n > 1 ? (u32)strtoul(tok[1], NULL, 0) : 1);
        snprintf(wait.text, sizeof wait.text, "ran %s frames", n > 1 ? tok[1] : "1");
        return 1;
    }
    if (!strcmp(cmd, "until") && n >= 3) {
        wait.until_frame = machine_frame + take_max(tok, n, 36000);
        if (!strcmp(tok[1], "mode")) {
            if (parse_mode(tok[2], &wait.mode_main, &wait.mode_sub)) {
                err("unknown mode", tok[2]);
                return 0;
            }
            wait.kind = W_MODE;
            snprintf(wait.text, sizeof wait.text, "mode %s", tok[2]);
            return 1;
        }
        if (!strcmp(tok[1], "fn")) {
            wait.fn = routine_by_name(tok[2]);
            if (!wait.fn) {
                err("unknown routine", tok[2]);
                return 0;
            }
            wait.kind = W_FN;
            disp_set_flag(wait.fn, DF_BREAK, 1);
            snprintf(wait.text, sizeof wait.text, "routine %s entered", tok[2]);
            return 1;
        }
        if (!strcmp(tok[1], "mem") && n >= 5) {
            wait.addr = control_resolve(tok[2]);
            if (!wait.addr) {
                err("unknown address", tok[2]);
                return 0;
            }
            wait.op = !strcmp(tok[3], "==") ? 0 : !strcmp(tok[3], "!=") ? 1 : !strcmp(tok[3], "<") ? 2 : 3;
            wait.value = (u32)strtoul(tok[4], NULL, 0);
            wait.size = (n > 5 && isdigit((unsigned char)tok[5][0])) ? atoi(tok[5]) : 4;
            wait.kind = W_MEM;
            snprintf(wait.text, sizeof wait.text, "%s %s %s", tok[2], tok[3], tok[4]);
            return 1;
        }
        if (!strcmp(tok[1], "gameframe")) {
            wait.value = (u32)strtoul(tok[2], NULL, 0);
            wait.kind = W_GAMEFRAME;
            snprintf(wait.text, sizeof wait.text, "game frame %s", tok[2]);
            return 1;
        }
        if (!strcmp(tok[1], "insns")) {
            wait.insns = strtoull(tok[2], NULL, 0);
            wait.kind = W_INSNS;
            snprintf(wait.text, sizeof wait.text, "%s instructions", tok[2]);
            return 1;
        }
        err("unknown condition", tok[1]);
        return 0;
    }
    if ((!strcmp(cmd, "press") || !strcmp(cmd, "hold") || !strcmp(cmd, "release")) && n >= 2) {
        if (!strcmp(cmd, "release") && !strcmp(tok[1], "all")) {
            inputs.player[0] = inputs.player[1] = inputs.sys = 0;
            inputs.coin_switch[0] = inputs.coin_switch[1] = 0;
            hold_n = 0;
            return 0;
        }
        int who = parse_who(tok[1]);
        u32 bits;
        if (who < 0 || n < 3 || parse_buttons(who, tok[2], &bits)) {
            err("bad buttons", n >= 3 ? tok[2] : tok[1]);
            return 0;
        }
        if (!strcmp(cmd, "press"))
            hold_for(who, bits, n > 3 ? (u32)strtoul(tok[3], NULL, 0) : 6);
        else
            set_bits(who, bits, !strcmp(cmd, "hold"));
        return 0;
    }
    if (!strcmp(cmd, "coin")) {
        int chute = n > 1 ? atoi(tok[1]) : 1;
        hold_for(3, chute == 2 ? 2 : 1, n > 2 ? (u32)strtoul(tok[2], NULL, 0) : 6);
        return 0;
    }
    if ((!strcmp(cmd, "poke") || !strcmp(cmd, "pokef")) && n >= 3) {
        u32 a = control_resolve(tok[1]);
        if (!a || !mem_ptr(a)) {
            err("unknown address", tok[1]);
            return 0;
        }
        if (!strcmp(cmd, "pokef")) {
            float v = strtof(tok[2], NULL);
            u32 bits;
            memcpy(&bits, &v, 4);
            g_w32(a, bits);
            obs_poke(a, bits, 4, tok[1]);
        } else {
            u32 v = (u32)strtoul(tok[2], NULL, 0);
            int size = n > 3 ? atoi(tok[3]) : 4;
            if (size == 1) g_w8(a, v);
            else if (size == 2) g_w16(a, v);
            else g_w32(a, v);
            obs_poke(a, v, size, tok[1]);       /* the log says what the controller changed */
        }
        return 0;
    }
    if (!strcmp(cmd, "peek") && n >= 2) {
        u32 a = control_resolve(tok[1]);
        int size = n > 2 ? atoi(tok[2]) : 4, count = n > 3 ? atoi(tok[3]) : 1;
        if (!a || !mem_ptr(a)) {
            err("unknown address", tok[1]);
            return 0;
        }
        printf("{\"ev\":\"peek\",\"addr\":\"%08x\",\"name\":", a);
        say_json_str(tok[1]);
        printf(",\"size\":%d,\"values\":[", size);
        for (int i = 0; i < count; i++) {
            u32 v = size == 1 ? g_r8(a + (u32)i) : size == 2 ? g_r16(a + 2 * (u32)i) : g_r32(a + 4 * (u32)i);
            printf("%s%u", i ? "," : "", v);
        }
        printf("]}\n");
        fflush(stdout);
        return 0;
    }
    if ((!strcmp(cmd, "break") || !strcmp(cmd, "unbreak") || !strcmp(cmd, "trace") || !strcmp(cmd, "untrace")) &&
        n >= 2) {
        routine *r = routine_by_name(tok[1]);
        if (!r) {
            err("unknown routine", tok[1]);
            return 0;
        }
        if (cmd[0] == 'b' || !strcmp(cmd, "unbreak"))
            disp_set_flag(r, DF_BREAK, cmd[0] == 'b');
        else {
            disp_set_flag(r, DF_TRACE, cmd[0] == 't');
            if (cmd[0] == 't')
                disp_set_flag(r, DF_LOG, 1);
        }
        return 0;
    }
    if (!strcmp(cmd, "watch") && n >= 2) {
        u32 a = control_resolve(tok[1]);
        u32 len = 4;
        int rd = 0, wr = 1, stop = 0;
        for (int i = 2; i < n; i++) {
            if (isdigit((unsigned char)tok[i][0])) len = (u32)strtoul(tok[i], NULL, 0);
            else if (!strcmp(tok[i], "r")) { rd = 1; wr = 0; }
            else if (!strcmp(tok[i], "w")) { rd = 0; wr = 1; }
            else if (!strcmp(tok[i], "rw")) { rd = 1; wr = 1; }
            else if (!strcmp(tok[i], "stop")) stop = 1;
        }
        if (!a || mem_watch_add(a, len, rd, wr, stop, tok[1]))
            err("cannot watch", tok[1]);
        return 0;
    }
    if (!strcmp(cmd, "unwatch")) {
        mem_watch_clear();
        return 0;
    }
    if (!strcmp(cmd, "backup") && n >= 3) {
        /* The battery-backed RAM (64 KiB at 0x00200000) and the settings EEPROM (128 bytes), which start
         * blank in every run: "backup save FILE" writes both to a file, "backup load FILE" (before the
         * first run) starts the machine with what a file holds, "backup damage sram|eeprom OFFSET" turns
         * one byte over, so that one of the game's two copies of its data fails its check. */
        const u32 ram = 0xa0200000u, ram_size = 0x10000u;
        u8 *eep = hw_maple_eeprom();
        if (!strcmp(tok[1], "save") || !strcmp(tok[1], "load")) {
            int save = tok[1][0] == 's';
            FILE *f = fopen(tok[2], save ? "wb" : "rb");
            static u8 buf[0x10000];
            int ok = f != NULL;
            if (ok && save) {
                g_read(ram, buf, ram_size);
                ok = fwrite(buf, 1, ram_size, f) == ram_size && fwrite(eep, 1, 0x80, f) == 0x80;
            } else if (ok) {
                u8 e2[0x80];
                ok = fread(buf, 1, ram_size, f) == ram_size && fread(e2, 1, 0x80, f) == 0x80;
                if (ok) {
                    g_write(ram, buf, ram_size);
                    memcpy(eep, e2, 0x80);
                    obs_note("the controller loaded the backup RAM and the settings EEPROM from a file");
                }
            }
            if (f)
                fclose(f);
            if (!ok)
                err(save ? "backup not saved" : "backup not loaded (a file of 65,664 bytes, as \"backup save\" writes)", tok[2]);
        } else if (!strcmp(tok[1], "damage") && n >= 4) {
            u32 off = (u32)strtoul(tok[3], NULL, 0);
            char note[120];
            if (!strcmp(tok[2], "sram") && off < ram_size)
                g_w8(ram + off, g_r8(ram + off) ^ 0xff);
            else if (!strcmp(tok[2], "eeprom") && off < 0x80)
                eep[off] ^= 0xff;
            else
                err("backup damage sram|eeprom OFFSET", tok[3]);
            snprintf(note, sizeof note, "the controller turned over byte 0x%x of the %s", off, tok[2]);
            obs_note(note);
        } else {
            err("backup save FILE | load FILE | damage sram|eeprom OFFSET", NULL);
        }
        return 0;
    }
    if (!strcmp(cmd, "sandbox") && n >= 2) {
        if (!strcmp(tok[1], "begin")) {
            if (sandbox_begin())
                err("sandbox", sbx.on ? "one is open already" : "no memory");
        } else if (!strcmp(tok[1], "end")) {
            if (!sbx.on)
                err("sandbox", "none is open");
            sandbox_end();
        } else {
            err("sandbox", "begin or end");
        }
        return 0;
    }
    if (!strcmp(cmd, "call") && n >= 2) {
        routine *r = routine_by_name(tok[1]);
        char msg[200];
        int nargs = n - 2;
        u32 budget = 4;
        int with_irq = 0;
        for (int again = 0; again < 2; again++) {
            if (nargs >= 2 && !strcmp(tok[2 + nargs - 2], "max")) {
                budget = (u32)strtoul(tok[2 + nargs - 1], NULL, 0);
                nargs -= 2;
            }
            if (nargs >= 1 && !strcmp(tok[2 + nargs - 1], "irq")) {
                with_irq = 1;
                nargs -= 1;
            }
        }
        if (!r) {
            err("unknown routine", tok[1]);
            return 0;
        }
        if (callst.active || call_start(c, r, tok + 2, nargs, budget ? budget : 1, with_irq, msg, sizeof msg)) {
            err(callst.active ? "call: one is running" : msg, NULL);
            return 0;
        }
        return 1;
    }
    if (!strcmp(cmd, "snap") && n >= 3) {
        /* snap [*]NAME LEN [for PATTERN]: the range goes with every full record of the routines the
         * pattern names ("*" in NAME's place: the range the pointer at NAME points at) */
        int deref = tok[1][0] == '*';
        u32 a = control_resolve(tok[1] + deref);
        const char *pattern = n >= 5 && !strcmp(tok[3], "for") ? tok[4] : "*";
        if (!a || obs_snap_add(tok[1], a, deref, (u32)strtoul(tok[2], NULL, 0), pattern))
            err("cannot snap", tok[1]);
        return 0;
    }
    if (!strcmp(cmd, "frameline") && n >= 2) {
        if (!obs_frame_global(tok[1], n > 2 ? atoi(tok[2]) : 4))
            err("frameline: unknown global (or the list is full)", tok[1]);
        return 0;
    }
    if ((!strcmp(cmd, "save") || !strcmp(cmd, "load")) && sbx.on) {
        err(cmd, "not inside a sandbox");
        return 0;
    }
    if (!strcmp(cmd, "save") && n >= 2) {
        if (snapshot_save(tok[1]))
            err("snapshot not saved", tok[1]);
        return 0;
    }
    if (!strcmp(cmd, "load") && n >= 2) {
        if (snapshot_load(tok[1])) {
            /* The machine is as it was before the command (the load is all
             * or nothing), but the script after this line was written for
             * the state in the file: it must not run on. A script from a
             * file ends here; a driver on the pipe gets "halted" for every
             * command that would let the machine run, until a load works. */
            err("snapshot not loaded", snapshot_error);
            load_failed = 1;
            control_failed = 1;
            snprintf(load_failed_text, sizeof load_failed_text, "snapshot not loaded: %.200s", snapshot_error);
            say_state(c, "halted", load_failed_text);
            return script_is_stdin ? 0 : -1;
        }
        load_failed = 0;
        control_failed = 0;
        obs_machine_jumped("the controller loaded a snapshot");
        return 0;
    }
    if (!strcmp(cmd, "reg") && n >= 3) {
        u32 v = (u32)strtoul(tok[2], NULL, 0);
        if (tok[1][0] == 'r' && isdigit((unsigned char)tok[1][1]) && atoi(tok[1] + 1) < 16)
            c->r[atoi(tok[1] + 1)] = v;
        else if (!strcmp(tok[1], "pr"))
            c->pr = v;
        else
            err("unknown register", tok[1]);
        obs_poke(0, v, 0, tok[1]);              /* a register set by the controller */
        return 0;
    }
    if (!strcmp(cmd, "hash")) {
        /* a fingerprint of the machine: main RAM and the processor's registers */
        u64 h = 1469598103934665603ull;
        const u8 *ram = mem_ptr(RAM_BASE);
        for (u32 i = 0; i < RAM_SIZE; i += 4)
            h = (h ^ *(const u32 *)(ram + i)) * 1099511628211ull;
        for (int i = 0; i < 16; i++)
            h = (h ^ c->r[i]) * 1099511628211ull;
        h = (h ^ c->pc) * 1099511628211ull;
        h = (h ^ c->pr) * 1099511628211ull;
        printf("{\"ev\":\"hash\",\"value\":\"%016llx\",\"frame\":%u,\"insns\":%llu}\n", (unsigned long long)h,
               machine_frame, (unsigned long long)c->insns);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(cmd, "status")) {
        say_state(c, "status", "");
        return 0;
    }
    if (!strcmp(cmd, "where")) {
        machine_where(c, stderr);
        machine_backtrace(c, stderr);
        return 0;
    }
    if (!strcmp(cmd, "report")) {
        write_reports();
        return 0;
    }
    if (!strcmp(cmd, "picture") && n >= 2) {
        view_picture(tok[1]);           /* the next frame the game renders, as a PNG (view.c) */
        return 0;
    }
    if (!strcmp(cmd, "card")) {
        char msg[400];
        if (hw_card_command(tok + 1, n - 1, msg, sizeof msg))   /* the card units (hw_card.c) */
            err(msg, NULL);
        return 0;
    }
    if (!strcmp(cmd, "jvs") || !strcmp(cmd, "sound")) {
        char msg[400];
        if (cmd[0] == 'j' ? hw_maple_command(tok + 1, n - 1, msg, sizeof msg)    /* the I/O board (hw_maple.c) */
                          : hw_sound_command(tok + 1, n - 1, msg, sizeof msg))   /* the sound driver (hw_sound.c) */
            err(msg, NULL);
        return 0;
    }
    if (!strcmp(cmd, "net")) {
        char msg[400];
        if (hw_net_command(c, tok + 1, n - 1, msg, sizeof msg))  /* the network board (hw_dimm_net.c, net_server.c) */
            err(msg, NULL);
        return 0;
    }
    if (!strcmp(cmd, "echo")) {
        const char *p = echo_copy;
        while (*p && isspace((unsigned char)*p)) p++;
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        printf("{\"ev\":\"echo\",\"text\":");
        char buf[256];
        snprintf(buf, sizeof buf, "%s", p);
        buf[strcspn(buf, "\r\n")] = 0;
        say_json_str(buf);
        printf("}\n");
        fflush(stdout);
        return 0;
    }
    if (!strcmp(cmd, "help")) {
        fputs(help_text, stderr);
        return 0;
    }
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit"))
        return -1;
    err("unknown command", cmd);
    return 0;
}

int control_next(sh4 *c)
{
    char line[512];
    if (!script) {
        /* no script: one run to the limits */
        static int started;
        if (started)
            return 0;
        started = 1;
        wait.kind = W_FRAMES;
        wait.until_frame = cfg.max_frames ? cfg.max_frames : 0xffffffffu;
        snprintf(wait.text, sizeof wait.text, "frame limit");
        return 1;
    }
    while (fgets(line, sizeof line, script)) {
        line_no++;
        int r = execute(c, line);
        if (r > 0)
            return 1;
        if (r < 0)
            return 0;
    }
    return 0;
}

int control_stopped(sh4 *c, int code)
{
    if (callst.active) {
        call_finish(c, code);
        return 1;
    }
    if (code == STOP_BREAK) {
        say_state(c, "stopped", machine_stop_text);
        return 1;
    }
    /* anything else ends a scripted run; a driver on standard input can
     * still look around (peek, where, report) and must then quit */
    char reason[640];
    snprintf(reason, sizeof reason, "%s: %s", sh4_stop_name(code), machine_stop_text);
    say_state(c, "halted", reason);
    return script_is_stdin;
}

static void wait_done(sh4 *c, const char *how)
{
    if (wait.kind == W_FN && wait.fn)
        disp_set_flag(wait.fn, DF_BREAK, 0);
    wait.kind = W_NONE;
    machine_stop(c, STOP_BREAK, "%s", how);
}

void control_frame(sh4 *c)
{
    for (int i = 0; i < hold_n;) {
        if (machine_frame >= holds[i].release_frame) {
            set_bits(holds[i].group, holds[i].bits, 0);
            holds[i] = holds[--hold_n];
        } else {
            i++;
        }
    }
    if (wait.kind == W_NONE || c->stop)
        return;
    char text[200];
    switch (wait.kind) {
    case W_FRAMES:
        if (machine_frame >= wait.until_frame)
            wait_done(c, wait.text);
        return;
    case W_MODE:
        if (game_mode_main() == wait.mode_main && (wait.mode_sub < 0 || game_mode_sub() == wait.mode_sub)) {
            snprintf(text, sizeof text, "reached %s", wait.text);
            wait_done(c, text);
            return;
        }
        break;
    case W_MEM: {
        u32 v = wait.size == 1 ? g_r8(wait.addr) : wait.size == 2 ? g_r16(wait.addr) : g_r32(wait.addr);
        int hit = wait.op == 0 ? v == wait.value : wait.op == 1 ? v != wait.value :
                  wait.op == 2 ? v < wait.value : v > wait.value;
        if (hit) {
            snprintf(text, sizeof text, "condition %s holds (value %u)", wait.text, v);
            wait_done(c, text);
            return;
        }
        break;
    }
    case W_GAMEFRAME:
        if (game_frames() >= wait.value) {
            snprintf(text, sizeof text, "reached %s", wait.text);
            wait_done(c, text);
            return;
        }
        break;
    case W_INSNS:
        if (c->insns >= wait.insns) {
            snprintf(text, sizeof text, "reached %s", wait.text);
            wait_done(c, text);
            return;
        }
        break;
    default:
        break;
    }
    if (machine_frame >= wait.until_frame) {
        snprintf(text, sizeof text, "gave up waiting for %s (frame budget used)", wait.text);
        wait_done(c, text);
    }
}

void control_break(sh4 *c, routine *r)
{
    char text[200];
    if (wait.kind == W_FN && wait.fn == r) {
        snprintf(text, sizeof text, "%s entered from %s", r->name, addr_text(c->pr));
        wait_done(c, text);
        return;
    }
    snprintf(text, sizeof text, "breakpoint: %s entered from %s", r->name, addr_text(c->pr));
    machine_stop(c, STOP_BREAK, "%s", text);
}

void control_watch_hit(sh4 *c, mem_watch *w, u32 addr, u32 val, int size, int is_write)
{
    printf("{\"ev\":\"watch\",\"name\":");
    say_json_str(w->name);
    printf(",\"addr\":\"%08x\",\"access\":\"%s\",\"size\":%d,\"value\":%u,\"by\":", addr, is_write ? "write" : "read",
           size, val);
    say_json_str(addr_text(c->pc - 2));
    printf(",\"frame\":%u,\"insns\":%llu}\n", machine_frame, (unsigned long long)c->insns);
    if (w->stop)
        machine_stop(c, STOP_BREAK, "watchpoint %s", w->name);
}

void control_mode_changed(sh4 *c, int main_mode, int sub_mode)
{
    (void)c;
    if (cfg.quiet)
        return;
    char name[160];
    fprintf(stderr, "[mode] vb %-6u game frame %-6u -> %d.%d %s\n", machine_frame, game_frames(), main_mode,
            sub_mode, game_mode_name(main_mode, sub_mode, name, sizeof name));
}
