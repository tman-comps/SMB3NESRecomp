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

#define RAM_PLAYER_YHI       0x0087
#define RAM_OBJECTS_YHI      0x0088
#define RAM_PLAYER_Y         0x00A2
#define RAM_OBJECTS_Y        0x00A3
#define RAM_VERT_SCROLL_HI   0x0542 /* Level_VertScrollH */
#define RAM_VERT_SCROLL      0x0543 /* Level_VertScroll */
#define ROUTINE_PLAYER_DRAW  0xCE9F /* Player_Draw (PRG029 at $C000) */
#define BANK_PLAYER_DRAW     29

#define OBJECT_SLOTS 8
#define PLAYER_PACKET OBJECT_SLOTS
#define PACKET_SPRITES 24

typedef struct { int world_x, world_y; uint8_t tile, attr; } HostSprite;
typedef struct { int count; HostSprite spr[PACKET_SPRITES]; } Packet;

static bool s_wide_lifetime = true;
static int  s_margin_left, s_margin_right; /* pixels of view beyond the native 256 */
static int  s_extra_top;                   /* lines shown above the native picture */
static int  s_vscroll;                     /* Level_VertScroll this frame */
static Packet s_packets[OBJECT_SLOTS + 1]; /* objects, then the player */

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

static int peek16(uint16_t hi, uint16_t lo) { return (cyc_mod_peek(hi) << 8) | cyc_mod_peek(lo); }

/* Move the camera so world (cx, cy) is on the native screen: horizontally near
 * its middle when cx >= 0, vertically 64 lines down when cy >= 0. Returns the
 * camera actually set. */
static void place_camera(int cx, int cy, int *hcam, int *vcam) {
    *hcam = peek16(RAM_HORZ_SCROLL_HI, RAM_HORZ_SCROLL);
    *vcam = s_vscroll;
    if (cx >= 0) {
        *hcam = cx - 120 < 0 ? 0 : cx - 120;
        cyc_mod_poke(RAM_HORZ_SCROLL, (uint8_t)(*hcam & 0xFF));
        cyc_mod_poke(RAM_HORZ_SCROLL_HI, (uint8_t)(*hcam >> 8));
    }
    if (cy >= 0) {
        *vcam = cy - 64 < 0 ? 0 : cy - 64;
        cyc_mod_poke(RAM_VERT_SCROLL, (uint8_t)(*vcam & 0xFF));
        cyc_mod_poke(RAM_VERT_SCROLL_HI, (uint8_t)(*vcam >> 8));
    }
}

static void read_sprites(Packet *pk, int hcam, int vcam) {
    pk->count = 0;
    for (int i = 0; i < 64 && pk->count < PACKET_SPRITES; i++) {
        uint16_t a = (uint16_t)(RAM_SPRITE_RAM + i * 4);
        uint8_t y = cyc_mod_peek(a);
        if (y >= 0xEF) continue;
        HostSprite *s = &pk->spr[pk->count++];
        s->world_y = vcam + y;
        s->tile = cyc_mod_peek((uint16_t)(a + 1));
        s->attr = cyc_mod_peek((uint16_t)(a + 2));
        s->world_x = hcam + cyc_mod_peek((uint16_t)(a + 3));
    }
}

static void clear_sprites(void) {
    for (int i = 0; i < 64; i++) cyc_mod_poke((uint16_t)(RAM_SPRITE_RAM + i * 4), 0xF8);
}

static void capture(int slot, int cx, int cy) {
    int hcam, vcam;
    place_camera(cx, cy, &hcam, &vcam);
    cyc_mod_poke(RAM_SLOT_INDEX_BACKUP, (uint8_t)slot); /* the object loop's X, reloaded mid-routine */
    clear_sprites();
    CycModRegs r;
    cyc_mod_regs(&r);
    r.x = (uint8_t)slot;
    if (cyc_mod_call(ROUTINE_DO_STATE, &r)) read_sprites(&s_packets[slot], hcam, vcam);
}

/* The player above the native picture (modern HUD): Player_Draw with the
 * vertical camera raised. */
static void capture_player(int py) {
    cyc_mod_poke(RAM_PAGE_C000, BANK_PLAYER_DRAW);
    CycModRegs r;
    cyc_mod_regs(&r);
    if (!cyc_mod_call(ROUTINE_CHANGE_BOTH2, &r) || bank_at(ROUTINE_PLAYER_DRAW) != BANK_PLAYER_DRAW) return;
    int hcam, vcam;
    place_camera(-1, py, &hcam, &vcam);
    clear_sprites();
    cyc_mod_regs(&r);
    if (cyc_mod_call(ROUTINE_PLAYER_DRAW, &r)) read_sprites(&s_packets[PLAYER_PACKET], hcam, vcam);
}

void smb3_ws_objects_frame_end(const Smb3WsView *v) {
    for (int i = 0; i <= OBJECT_SLOTS; i++) s_packets[i].count = 0;
    if (!v->active) {
        s_margin_left = s_margin_right = s_extra_top = 0;
        cyc_mod_set_ram_read_hook(NULL);
        return;
    }
    s_margin_left = v->native_world - v->view_left;
    s_margin_right = v->view_left + v->width - (v->native_world + 256);
    if (s_margin_left < 0) s_margin_left = 0;
    if (s_margin_right < 0) s_margin_right = 0;
    s_extra_top = v->extra_top;
    s_vscroll = peek16(RAM_VERT_SCROLL_HI, RAM_VERT_SCROLL);
    cyc_mod_set_ram_read_hook(s_wide_lifetime ? scroll_policy : NULL);

    /* Objects the game keeps off its screen: beside it (wide margins) or above
     * it (the modern HUD's taller view). Each gets the camera moved onto it
     * along whichever axis it is off. */
    int cam = peek16(RAM_HORZ_SCROLL_HI, RAM_HORZ_SCROLL);
    int want[OBJECT_SLOTS], wcx[OBJECT_SLOTS], wcy[OBJECT_SLOTS], n = 0;
    for (int slot = 0; slot < OBJECT_SLOTS; slot++) {
        if (!cyc_mod_peek((uint16_t)(RAM_OBJECTS_STATE + slot))) continue;
        int wx = peek16((uint16_t)(RAM_OBJECTS_XHI + slot), (uint16_t)(RAM_OBJECTS_X + slot));
        int wy = peek16((uint16_t)(RAM_OBJECTS_YHI + slot), (uint16_t)(RAM_OBJECTS_Y + slot));
        int sx = wx - cam, sy = wy - s_vscroll;
        bool off_x = sx < 16 || sx > 224, off_y = s_extra_top && sy < 16;
        if (!off_x && !off_y) continue;                        /* the game draws it whole */
        if (sx < -s_margin_left - 48 || sx > 256 + s_margin_right + 32) continue;
        if (sy < -s_extra_top - 32) continue;
        want[n] = slot;
        wcx[n] = off_x ? wx : -1;
        wcy[n] = off_y ? wy : -1;
        n++;
    }
    int py = peek16(RAM_PLAYER_YHI, RAM_PLAYER_Y);
    bool player_up = s_extra_top && py - s_vscroll < 16 && py - s_vscroll > -s_extra_top - 48;
    if ((!n && !player_up) || !cyc_mod_isolate_begin()) return;
    /* Object code expects PRG000 at $C000 and PRG005 at $A000. */
    cyc_mod_poke(RAM_PAGE_C000, 0);
    cyc_mod_poke(RAM_PAGE_A000, 5);
    CycModRegs r;
    cyc_mod_regs(&r);
    if (cyc_mod_call(ROUTINE_CHANGE_BOTH2, &r) && bank_at(ROUTINE_DO_STATE) == 0) {
        for (int i = 0; i < n; i++) {
            capture(want[i], wcx[i], wcy[i]);
            /* restore the cameras for the next object */
            cyc_mod_poke(RAM_HORZ_SCROLL, (uint8_t)(cam & 0xFF));
            cyc_mod_poke(RAM_HORZ_SCROLL_HI, (uint8_t)(cam >> 8));
            cyc_mod_poke(RAM_VERT_SCROLL, (uint8_t)(s_vscroll & 0xFF));
            cyc_mod_poke(RAM_VERT_SCROLL_HI, (uint8_t)(s_vscroll >> 8));
        }
    }
    if (player_up) capture_player(py);
    cyc_mod_isolate_end();
}

void smb3_ws_objects_draw(uint32_t *out, int width, int height, int view_left, int dy, const uint8_t *opaque) {
    /* Priority order: the player first (it is OAM's first entries in SMB3),
     * then each object in its own Sprite_RAM order. */
    for (int k = 0; k <= OBJECT_SLOTS; k++) {
        const Packet *pk = &s_packets[k == 0 ? PLAYER_PACKET : k - 1];
        for (int i = 0; i < pk->count; i++) {
            const HostSprite *s = &pk->spr[i];
            smb3_ws_draw_sprite(out, width, height, s->world_x - view_left, s->world_y - s_vscroll + 1 + dy, s->tile,
                                s->attr, opaque);
        }
    }
}
