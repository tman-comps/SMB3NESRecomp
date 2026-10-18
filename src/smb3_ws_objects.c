/*
 * SMB3 widescreen: enemies across the wide view.
 *
 * 1. Lifetime. SMB3 spawns level objects 16 px past the right edge / 32 px
 *    past the left (Level_ObjectsSpawnByScroll) and deletes them 32 / 48 px
 *    out (Object_DeleteOffScreen), both measured from Horz_Scroll. A RAM read
 *    hook adds the wide margin to Horz_Scroll at exactly those instructions,
 *    so objects live across the whole picture ("wide"; "classic" keeps the
 *    original 4:3 timing). The ROM is untouched.
 *
 * 2. Pictures. The game draws only what is on its 256-pixel screen. For each
 *    object partly or wholly outside it, the object's own state routine
 *    (Object_DoStateAction) runs in an isolated call with the camera moved to
 *    put it on screen; the sprites it writes are kept at their world X and
 *    drawn by the compositor. Isolation restores the machine bit for bit.
 *
 * Addresses: smb3-disassembly assembled listing (identical to USA Rev 1).
 */
#include "smb3_ws_internal.h"

#include <string.h>

#include "cyc_mod.h"
#include "hw_internal.h"

#define RAM_TEMP_VAR2        0x0001
#define RAM_HORZ_SCROLL_HI   0x0012
#define RAM_SCROLL_LASTDIR   0x005B
#define RAM_OBJECTS_XHI      0x0076
#define RAM_OBJECTS_X        0x0091
#define RAM_SLOT_INDEX_BACKUP 0x00CD
#define RAM_HORZ_SCROLL      0x00FD
#define RAM_SPRITE_RAM       0x0200
#define RAM_OBJECTS_STATE    0x0661
#define RAM_PAGE_C000        0x071F
#define RAM_PAGE_A000        0x0720

#define PC_SPAWN_SCROLL_LO   0xB838 /* PRG005 Level_ObjectsSpawnByScroll */
#define PC_SPAWN_SCROLL_HI   0xB842
#define PC_DELETE_SCROLL_LO  0xD40F /* PRG000 Object_DeleteOffScreen */
#define PC_DELETE_SCROLL_HI  0xD41A
#define ROUTINE_CHANGE_BOTH2 0xFFBF /* PRGROM_Change_Both2 (fixed bank) */
#define ROUTINE_DO_STATE     0xCA46 /* Object_DoStateAction (PRG000), X = slot */

#define OBJECT_SLOTS 8
#define PACKET_SPRITES 24

typedef struct { int world_x, y; uint8_t tile, attr; } HostSprite;
typedef struct { int count; HostSprite spr[PACKET_SPRITES]; } Packet;

static bool s_wide_lifetime = true;
static int  s_margin_left, s_margin_right; /* pixels of view beyond the native 256 */
static Packet s_packets[OBJECT_SLOTS];

static int bank_at(uint16_t pc) { return (int)(hw_cart.prg_off[(pc >> 12) & 7] / 0x2000u); }

/* Horz_Scroll as the spawn/delete edge tests should see it. */
static uint8_t scroll_policy(uint16_t pc, uint16_t addr, uint8_t value) {
    int delta = 0;
    if ((pc == PC_SPAWN_SCROLL_LO || pc == PC_SPAWN_SCROLL_HI) && bank_at(pc) == 5)
        delta = cyc_mod_peek(RAM_SCROLL_LASTDIR) ? -s_margin_left : s_margin_right;
    else if ((pc == PC_DELETE_SCROLL_LO || pc == PC_DELETE_SCROLL_HI) && bank_at(pc) == 0)
        delta = cyc_mod_peek(RAM_TEMP_VAR2) ? -s_margin_left : s_margin_right;
    else
        return value;
    if (!delta) return value;
    int cam = (cyc_mod_peek(RAM_HORZ_SCROLL_HI) << 8) | cyc_mod_peek(RAM_HORZ_SCROLL);
    int shifted = cam + delta;
    if (addr == RAM_HORZ_SCROLL) return (uint8_t)(shifted & 0xFF);
    if (addr == RAM_HORZ_SCROLL_HI) return (uint8_t)((shifted >> 8) & 0xFF);
    return value;
}

void smb3_ws_objects_set_wide(bool wide) { s_wide_lifetime = wide; }
bool smb3_ws_objects_wide(void) { return s_wide_lifetime; }

void smb3_ws_objects_reset(void) {
    memset(s_packets, 0, sizeof s_packets);
    s_margin_left = s_margin_right = 0;
    cyc_mod_set_ram_read_hook(NULL);
}

static void capture(int slot, int world_x, int cam) {
    Packet *pk = &s_packets[slot];
    pk->count = 0;
    int shifted = world_x - 120; /* the object near mid-screen */
    if (shifted < 0) shifted = 0;
    cyc_mod_poke(RAM_HORZ_SCROLL, (uint8_t)(shifted & 0xFF));
    cyc_mod_poke(RAM_HORZ_SCROLL_HI, (uint8_t)(shifted >> 8));
    cyc_mod_poke(RAM_SLOT_INDEX_BACKUP, (uint8_t)slot); /* the object loop's X, reloaded mid-routine */
    for (int i = 0; i < 64; i++) cyc_mod_poke((uint16_t)(RAM_SPRITE_RAM + i * 4), 0xF8);
    CycModRegs r = { 0, (uint8_t)slot, 0, 0xFD, 0x24, 0 };
    cyc_mod_regs(&r);
    r.x = (uint8_t)slot;
    if (!cyc_mod_call(ROUTINE_DO_STATE, &r)) return;
    for (int i = 0; i < 64 && pk->count < PACKET_SPRITES; i++) {
        uint16_t a = (uint16_t)(RAM_SPRITE_RAM + i * 4);
        uint8_t y = cyc_mod_peek(a);
        if (y >= 0xEF) continue;
        HostSprite *s = &pk->spr[pk->count++];
        s->y = y;
        s->tile = cyc_mod_peek((uint16_t)(a + 1));
        s->attr = cyc_mod_peek((uint16_t)(a + 2));
        s->world_x = shifted + cyc_mod_peek((uint16_t)(a + 3));
    }
    (void)cam;
}

void smb3_ws_objects_frame_end(const Smb3WsView *v) {
    for (int i = 0; i < OBJECT_SLOTS; i++) s_packets[i].count = 0;
    if (!v->active) {
        s_margin_left = s_margin_right = 0;
        cyc_mod_set_ram_read_hook(NULL);
        return;
    }
    s_margin_left = v->native_world - v->view_left;
    s_margin_right = v->view_left + v->width - (v->native_world + 256);
    if (s_margin_left < 0) s_margin_left = 0;
    if (s_margin_right < 0) s_margin_right = 0;
    cyc_mod_set_ram_read_hook(s_wide_lifetime ? scroll_policy : NULL);

    int cam = (cyc_mod_peek(RAM_HORZ_SCROLL_HI) << 8) | cyc_mod_peek(RAM_HORZ_SCROLL);
    int want[OBJECT_SLOTS], n = 0;
    for (int slot = 0; slot < OBJECT_SLOTS; slot++) {
        if (!cyc_mod_peek((uint16_t)(RAM_OBJECTS_STATE + slot))) continue;
        int wx = (cyc_mod_peek((uint16_t)(RAM_OBJECTS_XHI + slot)) << 8) | cyc_mod_peek((uint16_t)(RAM_OBJECTS_X + slot));
        int sx = wx - cam;
        if (sx >= 16 && sx <= 224) continue;                  /* the game draws it whole */
        if (sx < -s_margin_left - 48 || sx > 256 + s_margin_right + 32) continue;
        want[n++] = slot;
    }
    if (!n || !cyc_mod_isolate_begin()) return;
    /* Object code expects PRG000 at $C000 and PRG005 at $A000. */
    cyc_mod_poke(RAM_PAGE_C000, 0);
    cyc_mod_poke(RAM_PAGE_A000, 5);
    CycModRegs r;
    cyc_mod_regs(&r);
    if (cyc_mod_call(ROUTINE_CHANGE_BOTH2, &r) && bank_at(ROUTINE_DO_STATE) == 0) {
        for (int i = 0; i < n; i++) {
            int slot = want[i];
            int wx = (cyc_mod_peek((uint16_t)(RAM_OBJECTS_XHI + slot)) << 8) |
                     cyc_mod_peek((uint16_t)(RAM_OBJECTS_X + slot));
            capture(slot, wx, cam);
        }
    }
    cyc_mod_isolate_end();
}

void smb3_ws_objects_draw(uint32_t *out, int width, int height, int view_left, const uint8_t *opaque) {
    for (int slot = OBJECT_SLOTS - 1; slot >= 0; slot--) {
        const Packet *pk = &s_packets[slot];
        for (int i = pk->count - 1; i >= 0; i--) {
            const HostSprite *s = &pk->spr[i];
            smb3_ws_draw_sprite(out, width, height, s->world_x - view_left, s->y + 1, s->tile, s->attr, opaque);
        }
    }
}
