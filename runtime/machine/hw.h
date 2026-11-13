/* Minimal hardware models: only what the game's own code still touches
 * after the library replacements, each one listed in docs/hle.md. */
#ifndef VF4_HW_H
#define VF4_HW_H

#include "sh4.h"
#include <stdio.h>

/* bits of the system chip's normal interrupt status */
#define IRQ_RENDER_END_VIDEO  0x00000001u
#define IRQ_RENDER_END_DEPTH    0x00000002u
#define IRQ_RENDER_END_TEXTURE    0x00000004u
#define IRQ_VBLANK_IN     0x00000008u
#define IRQ_VBLANK_OUT    0x00000010u
#define IRQ_HBLANK     0x00000020u
#define IRQ_LIST_END_OPAQUE   0x00000080u
#define IRQ_LIST_END_OPAQUE_VOLUME 0x00000100u
#define IRQ_LIST_END_TRANSLUCENT    0x00000200u
#define IRQ_LIST_END_TRANSLUCENT_VOLUME 0x00000400u
#define IRQ_VIDEO_DMA_END       0x00000800u
#define IRQ_MAPLE_DMA_END     0x00001000u
#define IRQ_CART_DMA_END        0x00004000u
#define IRQ_LIST_END_PUNCH_THROUGH    0x00200000u

typedef struct hw_state {
    /* Holly interrupt controller */
    u32 irq_normal, irq_external, irq_error;
    u32 mask2_normal, mask2_external, mask2_error;
    u32 mask4_normal, mask4_external, mask4_error;
    u32 mask6_normal, mask6_external, mask6_error;
    /* SH-4 */
    u32 intevt, expevt, tra;
    u32 ccr, qacr[2];
    u32 ipra, iprb, iprc, icr;
    struct {
        u32 tcor, tcnt, tcr;
        u64 synced;          /* cycle at which tcnt was valid */
        int running;
    } tmu[3];
    u32 tstr, tocr;
    /* generic "remember what was written" store for registers with no model */
    struct {
        u32 addr, val, used;
    } reg[4096];
    u32 reg_count;
    u32 rtc_base;            /* seconds the AICA clock shows at cycle 0 */
} hw_state;

extern hw_state hw;

void hw_init(sh4 *c);
void hw_raise(sh4 *c, u32 normal_bits);
void hw_raise_ext(sh4 *c, u32 external_bits);
/* the highest interrupt level waiting and its INTEVT code; 0 when none */
int hw_irq_pending(sh4 *c, u32 *intevt);
void hw_irq_update(sh4 *c);
u32 hw_tmu_count(sh4 *c, int ch);

/* writes by the harness into the BIOS's part of RAM (marks the words as
 * provided, see hw.c) */
int hw_maple_command(char **tok, int n, char *msg, size_t msg_len);   /* the controller's "jvs" */
int hw_sound_command(char **tok, int n, char *msg, size_t msg_len);   /* the controller's "sound" */
u8 *hw_maple_eeprom(void);           /* the settings EEPROM model's 128 bytes */
extern int hw_sandbox_unmodelled;   /* accesses without a model made inside sandboxes (harness calls) */
void low_ram_w32(u32 addr, u32 v);
void low_ram_w16(u32 addr, u32 v);

/* the Maple bus, the I/O bridge and the JVS board (hw_maple.c) */
void hw_maple_init(void);
int hw_maple_read(sh4 *c, u32 p, u32 *out);
int hw_maple_write(sh4 *c, u32 p, u32 v);

/* the two card reader/writer units on Maple ports 1 and 2 (hw_card.c) */
extern int hw_card_option;      /* --cards: the cabinet has the units */
void hw_card_init(void);
int hw_card_frame(int unit, u32 pattern, u32 cmd, u32 to, u32 from, const u8 *data, int nbytes, u8 *reply);
int hw_card_command(char **tok, int n, char *msg, size_t msg_len);
void hw_card_stats(FILE *f);
int hw_card_put(int unit, const u8 *card);     /* recomp (M6): a card goes in (NULL: never used); 0 or -1 */
int hw_card_get(int unit, u8 *card);           /* recomp (M6): the card in the unit, 128 bytes; 0 or -1 */

/* the cartridge port and the DIMM image behind it (hw_cart.c) */
int hw_cart_init(const char *dimm_image_path);
int hw_cart_init_memory(u8 *image, u32 size);   /* recomp (M5): an image already in memory, decrypted */
int hw_cart_read(sh4 *c, u32 p, u32 *out);
int hw_cart_write(sh4 *c, u32 p, u32 v);
void hw_cart_stats(FILE *f);
u32 hw_cart_image_size(void);         /* bytes of the DIMM image (0: none) */
const u8 *hw_cart_image(void);        /* recomp (test mode): its bytes, NULL without one */

/* the sound board (hw_sound.c) */
void hw_sound_init(void);
int hw_sound_read(sh4 *c, u32 p, u32 *out);
int hw_sound_write(sh4 *c, u32 p, u32 v);
void hw_sound_stats(FILE *f);
/* recomp: 1 = the sound chip with the game's own driver on its ARM7
 * (runtime/sound, docs/design.md section 16); 0 = VF4debug's mailbox model.
 * Set before hw_init. hw_sound_arm_budget: ARM instructions a sample (0: the default) */
extern int hw_sound_chip;
extern unsigned hw_sound_arm_budget;

/* the rendering chips and the ELAN (hw_gfx.c) */
void hw_gfx_init(void);
int hw_gfx_read(sh4 *c, u32 p, u32 *out);
int hw_gfx_write(sh4 *c, u32 p, u32 v);
int hw_gfx_sq(sh4 *c, u32 dst, const u32 *words);
int hw_gfx_irq_level(void);
int hw_dmac_read(u32 a, u32 *out);
int hw_dmac_write(u32 a, u32 v);
void hw_gfx_frame(sh4 *c);
void hw_gfx_stats(FILE *f);
/* where the display list segments the ELAN is told to run arrive
 * (address and length in guest memory); nothing is attached in milestone 1 */
typedef void (*gfx_list_sink_fn)(sh4 *c, u32 addr, u32 len);
extern gfx_list_sink_fn gfx_list_sink;

/* the report of hardware accesses: which routine touched what */
void hw_report(FILE *f, int unmodelled_only);
int hw_unmodelled_count(void);

/* where display lists arrive: 32-byte store-queue blocks sent outside RAM
 * (the tile accelerator at 0x10000000, the ELAN at 0x0a000000 and so on).
 * Milestone 1 counts them; a decoder attaches here later. */
typedef void (*gfx_sink_fn)(sh4 *c, u32 dst, const u32 *words);
extern gfx_sink_fn gfx_sink;
extern u64 gfx_sq_blocks, gfx_sq_blocks_frame;

#endif
