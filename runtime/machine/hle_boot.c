/* What the BIOS leaves behind and offers, without the BIOS.
 *
 * On the real board the BIOS loads the game, leaves values in the first
 * 128 KiB of RAM, and keeps a block of service routines in its ROM at
 * 0xa0060000 that the game's library copies to 0x0c018000 and calls through
 * a table of addresses (sdk/libam/nlasrvdt.c: bios_helpers_copy and the
 * bios_* wrappers). Here:
 *
 *   - a synthetic ROM holds the signature the game looks for
 *     (bios_signature_check compares against its own copy, which is where
 *     the bytes come from) and a service table whose entries point at stub
 *     addresses; the game's own copy routine and wrappers run unchanged;
 *   - a call that lands on a stub address is served by the host
 *     (hle_outside below). The services are written from how the game's
 *     library uses them: the argument lists and the meaning of the results
 *     are in the decompilation's notes on each bios_* wrapper and in
 *     sdk/libam/nlam.c (credit_snapshot, credit_try_spend, credit_event_pop,
 *     coin_config_unpack, board_config_validate).
 *
 * The credit service is a simple one of our own: every coin is one credit
 * (the coin rates and bonus adders of the real service are not modelled),
 * the service switch adds a credit, free play is honored.
 */
#include "hle.h"
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "observe.h"
#include "tables.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

#define ROM_SIGNATURE  0x001ffd00u      /* physical; the game reads 0xa01ffd00 */
#define ROM_SERVICES   0x00060000u      /* physical; the game reads 0xa0060000 */
#define RAM_SERVICES   0x0c018000u      /* where the game copies the block */
#define STUB_OFFSET    0x100u           /* stubs start here inside the block */
#define SERVICE_COUNT  0x14u            /* table entries 0x00 to 0x4c */
#define RATE_TABLE     0x0c018400u      /* inside the copied block: answer of service 0x14 */
#define VECTOR_BASE    0x8c000000u      /* VBR as the BIOS leaves it: the game never sets it */
#define FIRMWARE_NOTE  0x0c004000u      /* a firmware routine the game's interrupt entry calls */

void (*hle_program)(sh4 *c, int program);     /* recomp (test mode): see service 0x44 */

static struct {
    u32 last_service_switch;
    u32 service_calls[32];
    u32 exit_requested;
    u32 firmware_notes;
} bios;

/* ---- the credit service ---- */

enum { CS_SELECT = 0, CS_CREDITS = 4, CS_EVENT_BUF = 0x74 / 4, CS_EVENT_TAIL = 0xb4 / 4, CS_EVENT_HEAD = 0xb8 / 4 };
enum { CF_MAX_CREDIT = 0, CF_SEPARATE = 2, CF_FREE_PLAY = 3, CF_COIN_SETTING = 4, CF_PRICE = 5, CF_RATE = 13 };

static void credit_event(u32 state, u32 what)
{
    u32 tail = g_r32(state + 4 * CS_EVENT_TAIL) & 15;
    g_w32(state + 4 * CS_EVENT_BUF + 4 * tail, what);
    g_w32(state + 4 * CS_EVENT_TAIL, (tail + 1) & 15);
}

static void credit_refresh(u32 settings, u32 state, u32 status)
{
    u32 separate = g_r32(settings + 4 * CF_SEPARATE);
    u32 free_play = g_r32(settings + 4 * CF_FREE_PLAY);
    for (u32 p = 0; p < 4; p++) {
        u32 k = separate ? (p & 1) : 0;
        u32 credits = g_r32(state + 4 * (CS_CREDITS + k));
        u32 sel = g_r32(state + 4 * (CS_SELECT + p)) & 7;
        u32 price = g_r32(settings + 4 * (CF_PRICE + sel));
        if (price == 0)
            price = 1;
        g_w8(status + 4 * p + 0, credits > 255 ? 255 : credits);
        g_w8(status + 4 * p + 1, 0);
        g_w8(status + 4 * p + 2, 0);
        g_w8(status + 4 * p + 3, (free_play == 1 || credits >= price) ? 2 : credits ? 1 : 0);
    }
}

static u32 coin_config_fix(u32 settings)
{
    u32 bad = 0;
    for (u32 i = 0; i < 8; i++) {
        u32 a = settings + 4 * (CF_PRICE + i);
        if (g_r32(a) == 0 || g_r32(a) > 9) {
            g_w32(a, 1);
            bad = 1;
        }
    }
    for (u32 i = 0; i < 4; i++) {
        u32 a = settings + 4 * (CF_RATE + i);
        if (g_r32(a) == 0 || g_r32(a) > 9) {
            g_w32(a, 1);
            bad = 1;
        }
    }
    if (g_r32(settings + 4 * CF_SEPARATE) > 1) {
        g_w32(settings + 4 * CF_SEPARATE, 0);
        bad = 1;
    }
    if (g_r32(settings + 4 * CF_COIN_SETTING) > 27) {
        g_w32(settings + 4 * CF_COIN_SETTING, 0);
        bad = 1;
    }
    /* setting 26 (the 27th) is free play */
    g_w32(settings + 4 * CF_FREE_PLAY, g_r32(settings + 4 * CF_COIN_SETTING) == 26);
    return bad;
}

static void service_do(sh4 *c, u32 offset);

static void service(sh4 *c, u32 offset)
{
    /* what a service hands back in the game's memory is this model's
     * answer: a planted value (mem.h), flagged in the call log when read */
    mem_planting++;
    service_do(c, offset);
    mem_planting--;
}

static void service_do(sh4 *c, u32 offset)
{
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    bios.service_calls[offset / 4]++;
    switch (offset) {
    case 0x08:      /* the game's warning_init asks; above zero means "shown before" */
        c->r[0] = 1;
        return;
    case 0x10:      /* coin configuration check: non-zero when it had to be repaired */
        c->r[0] = coin_config_fix(a0);
        return;
    case 0x14:      /* the four coin rates of a coin setting, as bytes */
        g_w32(RATE_TABLE, 0x01010101);
        c->r[0] = RATE_TABLE;
        return;
    case 0x18:      /* credit service start: (settings, state, ledger, status) */
        for (u32 i = 0; i < 0xbc; i += 4)
            g_w32(a1 + i, 0);
        bios.last_service_switch = 0;
        credit_refresh(a0, a1, a3);
        return;
    case 0x1c:      /* ledger check: (value, ledger); 0 = nothing to rewrite */
        c->r[0] = 0;
        return;
    case 0x20:      /* credit refresh: (settings, state, status) */
        credit_refresh(a0, a1, a2);
        return;
    case 0x24: {    /* one frame: (settings, state, status, service switches, coins, ledger) */
        u32 coins = arg_stack(c, 0);
        u32 separate = g_r32(a0 + 4 * CF_SEPARATE);
        u32 max = g_r32(a0 + 4 * CF_MAX_CREDIT);
        u32 changed = 0;
        if (max == 0)
            max = 9;
        for (u32 i = 0; i < 2; i++) {
            u32 add = g_r32(coins + 4 * i) ? 1 : 0;
            u32 bit = 0x10u << i;
            if ((a3 & bit) && !(bios.last_service_switch & bit))
                add++;                          /* the service switch gives a credit */
            if (!add)
                continue;
            u32 k = separate ? i : 0;
            u32 credits = g_r32(a1 + 4 * (CS_CREDITS + k)) + add;
            if (credits > max)
                credits = max;
            g_w32(a1 + 4 * (CS_CREDITS + k), credits);
            credit_event(a1, 1);
            changed = 1;
        }
        bios.last_service_switch = a3;
        credit_refresh(a0, a1, a2);
        c->r[0] = changed;
        return;
    }
    case 0x28:      /* board settings: country, two flags, DIP switch 1 */
        g_w32(a0 + 0, (u32)cfg.country);
        g_w32(a0 + 4, 1);
        g_w32(a0 + 8, 0);
        g_w32(a0 + 12, 1);
        return;
    case 0x2c:      /* coin settings: 1 coin 1 credit, both chutes, shared credits */
        g_w32(a0 + 4 * CF_MAX_CREDIT, 9);
        g_w32(a0 + 4, 0);
        g_w32(a0 + 4 * CF_SEPARATE, 0);
        g_w32(a0 + 4 * CF_FREE_PLAY, 0);
        g_w32(a0 + 4 * CF_COIN_SETTING, 0);
        for (u32 i = 0; i < 8; i++)
            g_w32(a0 + 4 * (CF_PRICE + i), 1);
        for (u32 i = 0; i < 4; i++)
            g_w32(a0 + 4 * (CF_RATE + i), 1);
        return;
    case 0x34:      /* (the backup RAM's bank flag): libam calls it after every credit start
                     * (bios_helper_34, with sram_read_bank_flag's result). What the BIOS does
                     * with the value is not in the game; nothing is done and 0 comes back */
        c->r[0] = 0;
        return;
    case 0x44:      /* leave the game for the BIOS test menu */
        bios.exit_requested = 1;
        /* recomp (test mode): the runtime starts the other program, as the
         * BIOS would after its menu: the game's test program (test.bin) from
         * the game, the game again when the test program's EXIT calls it.
         * hle_program does not return */
        if (hle_program)
            hle_program(c, disp_program == PROGRAM_TEST ? PROGRAM_MAIN : PROGRAM_TEST);
        machine_stop(c, STOP_FATAL, "the game asked the BIOS for its test menu (service 0x44, from %s)",
                     addr_text(c->pr));
        return;
    default:
        if (!cfg.quiet)
            fprintf(stderr, "[bios] service 0x%02x has no model: called from %s with %08x %08x %08x %08x; "
                    "answered 0\n", offset, addr_text(c->pr), a0, a1, a2, a3);
        c->r[0] = 0;
        return;
    }
}

int hle_outside(sh4 *c, u32 target, int kind)
{
    u32 t = target & 0x1fffffffu;
    if (t == FIRMWARE_NOTE && kind != DISP_RETURN) {
        /* the routine the game's interrupt entry calls with (0x200, 2) and
         * its exit with (0x200, 3); what the real one does with them is not
         * known, and nothing reads a result */
        bios.firmware_notes++;
        c->pc = c->pr;
        return 1;
    }
    if (t >= RAM_SERVICES + STUB_OFFSET && t < RAM_SERVICES + STUB_OFFSET + 8 * SERVICE_COUNT && !(t & 7) &&
        kind != DISP_RETURN) {
        service(c, (t - RAM_SERVICES - STUB_OFFSET) / 2);
        c->pc = c->pr;
        return 1;
    }
    if (hw_net_outside(c, target, kind))        /* services 0x58 and 0x60, when the network board is on */
        return 1;
    return 0;
}

/* The vector area. The game never loads VBR: it uses the one the BIOS
 * left, fills the handler table at VBR + 0x1c0 to VBR + 0x3ff
 * (irq_hook_insert) and ends every interrupt by jumping to the address in
 * VBR (context_save_dispatch). The code at the vectors is the BIOS's; the
 * pieces below are ours, the least that makes that protocol work.
 *
 * What the hand-over has to be follows from the game's own entry and exit
 * code (source/asm of the decompilation: exception_entry,
 * irq_entry_via_firmware, context_save_entry, context_save_dispatch):
 *
 *   - the entries take over "with the registers of whatever was running":
 *     they have no register to spare, so they push r1 and park r0 in a
 *     pool word to get two, and context_save_dispatch then stores r0-r14,
 *     PR, GBR, VBR, MACH, MACL, FPSCR, FPUL and both FPU banks as it found
 *     them and loads them back before it leaves. Nothing is saved before
 *     the entry runs, so whatever the vector code changes on the way in is
 *     lost to the frame: the vector code must arrive with every register
 *     as the event left it;
 *   - the only far jump that keeps every register is the one the game's
 *     own exit uses to go to the code at VBR ("stc vbr,r0; jmp @r0" with
 *     "mov.l @r15+,r0" in the delay slot): the jump takes its target from
 *     r0 before the slot gives r0 back. So: push r0, load the entry's
 *     address, jump, pop r0 in the slot. One word below the interrupted
 *     stack pointer is written, where the entry's own pushes land next;
 *   - the exit arrives at VBR + 0 with the whole frame loaded back and SPC
 *     and SSR as the event set them: a return from exception is all that
 *     is left to do.
 *
 * The events run in register bank 1 (the processor sets SR.RB on entry and
 * the game's code never clears it): the r0-r7 the frame holds are that
 * bank's, and the interrupted program's own r0-r7, in bank 0, are not
 * touched by anything. The stubs used to load the entry's address into r0
 * and leave it there: bank 1's r0, so a program in bank 0 never saw it, but
 * the "r0" the game parks and stores in its frame was the stub's value,
 * and a program running in bank 1 lost its r0 at every interrupt. */
void hle_vectors_write(void)
{
    routine *exc = routine_by_name("exception_entry"), *irq = routine_by_name("irq_entry_via_firmware");
    /* recomp (test mode): the test program's exception entry, the same code
     * as main.bin's exception_entry (sdk/sy/exception_vectors.c), is named in
     * the decompilation's map since decomp 794d491f; a map without the name
     * gets it 0x44 bytes before irq_entry_via_firmware, where it is in both
     * programs */
    u32 exc_addr = exc ? exc->addr : irq ? irq->addr - 0x44 : 0;
    static const u16 keep_all[] = { 0x2f06 /* mov.l r0,@-r15 */, 0xd001 /* mov.l @(4,pc),r0 */, 0x402b /* jmp @r0 */,
                                    0x60f6 /* mov.l @r15+,r0 (the delay slot) */ };
    static const u16 old_stub[] = { 0xd001 /* mov.l @(4,pc),r0 */, 0x402b /* jmp @r0 */, 0x0009 /* nop */, 0x0009 };
    const u16 *stub = (sh4_diag & DIAG_OLD_VECTORS) ? old_stub : keep_all;
    low_ram_w16(VECTOR_BASE + 0, 0x002b);               /* rte */
    low_ram_w16(VECTOR_BASE + 2, 0x0009);               /* nop */
    for (u32 v = 0x100; v <= 0x600; v += v == 0x100 ? 0x300 : 0x200) {
        for (u32 i = 0; i < 4; i++)
            low_ram_w16(VECTOR_BASE + v + 2 * i, stub[i]);
        low_ram_w32(VECTOR_BASE + v + 8, v == 0x600 ? (irq ? irq->addr : 0) : exc_addr);     /* recomp: exc_addr */
    }
    low_ram_w16(FIRMWARE_NOTE, 0x000b);                 /* rts */
    low_ram_w16(FIRMWARE_NOTE + 2, 0x0009);             /* nop */
}

/* The DIMM board's word: bit 30 and no bit 31 (the "option" media_board_probe
 * takes from the top two bits: 1), the first module's size code 2 in bits
 * 16-19 (256 MiB: dimm_board_info_fetch), and in the low 12 bits the size
 * of what is loaded in MiB, which media_board_probe moves up by 20 into the
 * media size word: 0x0f0 for the 240 MiB image (the decompilation's own
 * record of the board shows 0x...00f0 there). Bit 29: the board has a
 * network side. */
u32 hle_board_id_word(int with_network)
{
    u32 w = 0x40020000u | (with_network ? 0x20000000u : 0);
    if (!(sh4_diag & DIAG_OLD_BOARD_WORD))
        w |= (hw_cart_image_size() >> 20) & 0xfffu;
    return w;
}

void hle_boot_install(sh4 *c)
{
    static int registered;
    int again = registered;     /* recomp (test mode): this runs again for the other program */
    /* the ROM the game looks into, and the battery-backed RAM; recomp (test
     * mode): once, the first time */
    if (!registered) {
        snapshot_register("bios", &bios, sizeof bios);
        mem_add_ram("bios-rom", 0x00000000u, 0x00200000u);
        mem_add_ram("backup-ram", 0x00200000u, 0x00010000u);
    }
    registered = 1;

    /* the signature: byte i of the game's own copy, less i mod 8 */
    u32 sig = global_addr("bios_signature");
    for (u32 i = 0; sig && i < 0x70; i++)
        g_w8(0xa0000000u + ROM_SIGNATURE + i, g_r8(sig + i) - (i & 7));

    /* the service block: a table of addresses, then a return for each */
    u32 rom = 0xa0000000u + ROM_SERVICES;
    for (u32 i = 0; i < SERVICE_COUNT; i++) {
        g_w32(rom + 4 * i, RAM_SERVICES + STUB_OFFSET + 8 * i);
        g_w16(rom + STUB_OFFSET + 8 * i, 0x000b);       /* rts */
        g_w16(rom + STUB_OFFSET + 8 * i + 2, 0x0009);   /* nop */
    }

    hle_vectors_write();
    c->vbr = VECTOR_BASE;
    /* the processor as the BIOS hands it over: privileged, the first
     * register bank, exceptions allowed, interrupts masked (_start sets
     * the rest itself). The game never writes the bank bit; its handlers
     * expect bank 1 while an event is served (int_break_handler reads "the
     * other bank" as the interrupted r0-r7), which fits a program in bank
     * 0. With the vector code above either bank would do: the interrupted
     * r0-r7 come back in both (tests/test_interp.c). */
    sh4_set_sr(c, SR_MD | SR_IMASK);

    /* recomp (test mode): for the other program the words below are already
     * there (the machine is back in its power-on state, rt_native.c), and
     * they are the BIOS's and the board's, at the same addresses for both
     * programs: the test program's decompilation names fewer of them */
    if (again)
        return;

    /* what the BIOS leaves in low RAM */
    u32 a;
    if ((a = global_addr("boot_country")) != 0)
        low_ram_w32(a, (u32)cfg.country);
    if ((a = global_addr("bios_rtc_seconds")) != 0)
        low_ram_w32(a, hw.rtc_base);
    /* the words the DIMM board keeps there: a board with one 256 MiB
     * module, and its firmware version in the top two bytes of the reply
     * word (the game's alab_dimm_firmware_check wants major 3, minor 0x17
     * or later; with anything less game_boot ends in
     * dimm_firmware_error_halt and the game never reaches its main loop) */
    for (u32 w = 0x0c01fc00u; w < 0x0c01fd00u; w += 4)
        low_ram_w32(w, 0);                              /* the rest of the board's words: nothing to tell */
    if ((a = global_addr("dimm_board_id_word")) != 0)
        low_ram_w32(a, hle_board_id_word(0));
    if ((a = global_addr("dimm_reply_word")) != 0)
        low_ram_w32(a, 0x03170000u);
    if ((a = global_addr("dimm_comms_mode")) != 0)
        low_ram_w32(a, 0);
    for (u32 i = 0; i < 16; i += 4) {
        if ((a = global_addr("board_serial_1")) != 0)
            low_ram_w32(a + i, 0x30303030u);            /* "0000" */
        if ((a = global_addr("board_serial_2")) != 0)
            low_ram_w32(a + i, 0x30303030u);
    }
}
