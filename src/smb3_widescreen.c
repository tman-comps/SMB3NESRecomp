/*
 * SMB3 widescreen compositor.
 *
 * SMB3 decompresses a whole horizontal level into cartridge WRAM (Tile_Mem,
 * $6000: 15 screens x 27 rows x 16 columns of 16x16 metatiles), so the
 * columns beyond the original 256 already exist in guest memory. This draws
 * them with the game's own metatile layout (TileLayout_ByTileset) and its
 * background CHR bank selections (PatTable_BankSel), at the scroll the PPU
 * actually rendered each line with.
 *
 * The native picture stays authoritative for columns 8-247 of the playfield
 * (sprites, priority, anything the game drew into the nametables). The left
 * 8 columns (masked by the game) and the right 8 (SMB3's attribute-wrap
 * colours) take the rebuilt background plus OAM sprites. The status bar is
 * centred with its edge colours extended. Anything that is not a horizontal
 * level -- title, map, vertical levels -- is left to the centred 4:3 picture.
 *
 * Addresses: Southbird's SMB3 disassembly (smb3-disassembly/), verified
 * against the USA Rev 1 ROM at power-on; a mismatch disables the mode.
 */
#include "smb3_widescreen.h"
#include "smb3_ws_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include "hw_internal.h"

/* CPU RAM */
#define RAM_HORZ_SCROLL_HI   0x0012
#define RAM_LEVEL_WIDTH      0x0022 /* screens - 1 */
#define RAM_HORZ_SCROLL      0x00FD
#define RAM_LEVEL_7VERTICAL  0x03EF
#define RAM_UPDATE_SELECT    0x0100 /* $20 title/ending, $80 vertical, $C0 normal */
#define RAM_PATTABLE_BANKSEL 0x0719 /* [0] BG $0000, [1] BG $0800 (1K bank numbers) */
#define RAM_LEVEL_TILESET    0x070A /* 0 = world map */

/* Cartridge WRAM ($6000-) */
#define TILE_MEM_SCREEN 0x1B0
#define TILE_MEM_ROWS   27
#define TILE_MEM_SCREENS 15

/* Fixed $8000 bank (PRG bank 30) */
#define ROM_PAGE_C000_BY_TILESET  0x83D6
#define ROM_PAGE_A000_BY_TILESET  0x83E9
#define ROM_TILELAYOUT_BY_TILESET 0x99F7
#define TILESET_COUNT 19

static int  s_mode = NES_VIDEO_STOCK;
static bool s_rom_ok, s_check, s_raw, s_modern_hud;
static uint8_t  s_opaque[CYC_VIDEO_MAX_WIDTH * 240];
static uint8_t  s_pixels[256][8][8]; /* decoded BG tiles: 2-bit colour */
static uint64_t s_frames, s_check_px, s_check_bad;

static const uint8_t PAGE_A000_EXPECTED[TILESET_COUNT] = {
    11, 15, 21, 16, 17, 19, 18, 18, 18, 20, 23, 19, 17, 19, 13, 26, 26, 26, 9,
};

static uint8_t ram(uint16_t a) { return cyc_mod_peek(a); }

/* A byte of the fixed $8000 bank, from the image: the mapper only puts bank
 * 30 there once the game has set it up. */
static uint8_t rom30(uint16_t a) { return hw_cart.prg[30 * 0x2000 + (a & 0x1FFF)]; }

static bool verify_rom(void) {
    if (!hw_cart.prg || !hw_cart.chr || hw_cart.prg_len < 0x40000 || !hw_cart.chr_len) return false;
    for (int i = 0; i < TILESET_COUNT; i++)
        if (rom30((uint16_t)(ROM_PAGE_A000_BY_TILESET + i)) != PAGE_A000_EXPECTED[i]) return false;
    /* TS1 (plains) layout pointer is $A000 */
    return rom30(ROM_TILELAYOUT_BY_TILESET + 2) == 0x00 && rom30(ROM_TILELAYOUT_BY_TILESET + 3) == 0xA0;
}

/* The tileset's metatile -> 8x8 table: four 256-byte arrays UL, LL, UR, LR. */
static const uint8_t *tile_layout(int tileset) {
    uint16_t at = (uint16_t)(ROM_TILELAYOUT_BY_TILESET + tileset * 2);
    uint16_t ptr = (uint16_t)(rom30(at) | (rom30((uint16_t)(at + 1)) << 8));
    int bank = ptr >= 0xC000 ? rom30((uint16_t)(ROM_PAGE_C000_BY_TILESET + tileset))
                             : rom30((uint16_t)(ROM_PAGE_A000_BY_TILESET + tileset));
    /* The map's page entry (11) is its runtime code; its layout, used while
     * the map loads, is Tile_Layout_TS0 at the start of PRG012. */
    if (tileset == 0) bank = 12;
    uint32_t off = (uint32_t)bank * 0x2000u + (ptr & 0x1FFFu);
    if (off + 1024 > hw_cart.prg_len) return NULL;
    return hw_cart.prg + off;
}

/* A pattern byte through the game's own bank selections (PatTable_BankSel:
 * 2K BG banks at $0000/$0800, 1K sprite banks at $1000-$1C00). The status
 * bar's IRQ swaps the PPU's live banks late in the frame; these are the
 * playfield's. */
static uint8_t chr_byte(uint16_t addr) {
    uint32_t off;
    addr &= 0x1FFF;
    if (addr < 0x1000) off = (uint32_t)(ram((uint16_t)(RAM_PATTABLE_BANKSEL + (addr >> 11))) & 0xFE) * 0x400u + (addr & 0x7FFu);
    else off = (uint32_t)ram((uint16_t)(RAM_PATTABLE_BANKSEL + 2 + ((addr - 0x1000) >> 10))) * 0x400u + (addr & 0x3FFu);
    return hw_cart.chr[off % hw_cart.chr_len];
}

static void decode_tiles(void) {
    for (int t = 0; t < 256; t++)
        for (int r = 0; r < 8; r++) {
            uint8_t lo = chr_byte((uint16_t)(t * 16 + r)), hi = chr_byte((uint16_t)(t * 16 + r + 8));
            for (int c = 0; c < 8; c++)
                s_pixels[t][r][c] = (uint8_t)(((lo >> (7 - c)) & 1) | (((hi >> (7 - c)) & 1) << 1));
        }
}

/* Sprite pixels already decided this picture. The PPU lets the first opaque
 * sprite pixel (lowest OAM index) win even when that sprite is behind the
 * background; SMB3 relies on it, masking a Piranha's stem with behind-BG
 * sprites over the pipe. Sprites are therefore drawn in priority order. */
static uint8_t s_claim[CYC_VIDEO_MAX_WIDTH * 240];

void smb3_ws_sprites_begin(int width) { memset(s_claim, 0, (size_t)width * 240); }

/* An 8x8 block of sprite pixels from pattern `pat`, `rows_from` lines down. */
static void sprite_tile(uint32_t *out, int width, int height, int x, int y, uint16_t pat, int rows_from, int palette,
                        bool hflip, bool vflip, bool behind, const uint8_t *opaque) {
    for (int r = 0; r < 8; r++) {
        int sy = y + rows_from + r;
        if (sy < 0 || sy >= height) continue;
        int pr = vflip ? 7 - r : r;
        uint8_t lo = chr_byte((uint16_t)(pat + pr)), hi = chr_byte((uint16_t)(pat + pr + 8));
        for (int c = 0; c < 8; c++) {
            int sx = x + c, i = sy * width + sx;
            if (sx < 0 || sx >= width || s_claim[i]) continue;
            int b = hflip ? c : 7 - c;
            int p = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
            if (!p) continue;
            s_claim[i] = 1;
            if (!(behind && opaque[i])) out[i] = cyc_render_color(16 + palette * 4 + p);
        }
    }
}

void smb3_ws_draw_sprite(uint32_t *out, int width, int height, int x, int y, uint8_t tile, uint8_t attr,
                         const uint8_t *opaque) {
    bool hflip = attr & 0x40, vflip = attr & 0x80, behind = attr & 0x20;
    int pal = attr & 3;
    if (cyc_render_line_sprite16(100)) {
        uint16_t base = (uint16_t)(((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16);
        sprite_tile(out, width, height, x, y, vflip ? base + 16 : base, 0, pal, hflip, vflip, behind, opaque);
        sprite_tile(out, width, height, x, y, vflip ? base : base + 16, 8, pal, hflip, vflip, behind, opaque);
    } else {
        sprite_tile(out, width, height, x, y, (uint16_t)(cyc_render_line_sprite_table(100) + tile * 16), 0, pal,
                    hflip, vflip, behind, opaque);
    }
}

/* What one horizontal-level frame needs: shared by the per-frame object pass
 * and the compositor, so both agree on the view. */
typedef struct {
    int tileset, hud, cam, level_px, w0, view, x0;
    const uint8_t *layout, *tile_mem;
} LevelFrame;

static int world_x(int ppu_x, int cam);
static int hud_top(void);

static bool level_frame(LevelFrame *f, int width) {
    if (!s_rom_ok || width <= 256) return false;
    f->tileset = ram(RAM_LEVEL_TILESET);
    if (f->tileset >= TILESET_COUNT || ram(RAM_LEVEL_7VERTICAL)) return false;
    if (!(cyc_render_line_mask(100) & 0x08) || cyc_render_line_bg_table(100) != 0) return false;
    f->layout = tile_layout(f->tileset);
    size_t wram_len = 0;
    f->tile_mem = cyc_cart_ram(&wram_len);
    if (!f->layout || !f->tile_mem || wram_len < TILE_MEM_SCREEN * TILE_MEM_SCREENS) return false;
    /* The world map (Tile_Mem + $110, shown from the lower nametable) draws
     * its status bar in the same nametable, so it ends where the stored rows
     * do rather than at a scroll split. */
    f->hud = hud_top();
    for (int y = 120; y < f->hud; y++)
        if (cyc_render_line_scroll_y(y) >= TILE_MEM_ROWS * 16) { f->hud = y; break; }
    if (f->hud < 160) return false; /* not the usual playfield/status-bar split */
    f->cam = (ram(RAM_HORZ_SCROLL_HI) << 8) | ram(RAM_HORZ_SCROLL);
    f->level_px = (ram(RAM_LEVEL_WIDTH) + 1) * 256;
    if (f->level_px > TILE_MEM_SCREENS * 256) f->level_px = TILE_MEM_SCREENS * 256;
    if (f->tileset == 0 && f->level_px <= 256) return false; /* World 1's framed one-screen map */
    /* Camera: centred on the native view, held at the level's ends. */
    f->w0 = world_x(cyc_render_line_scroll_x(100), f->cam);
    f->view = f->w0 - (width - 256) / 2;
    if (f->level_px <= width) f->view = -(width - f->level_px) / 2;
    else if (f->view < 0) f->view = 0;
    else if (f->view > f->level_px - width) f->view = f->level_px - width;
    f->x0 = f->w0 - f->view;
    return true;
}

/* Unwrap a line's nametable X (mod 256: SMB3's levels are horizontally
 * mirrored) against the level camera, which may already be a frame ahead. */
static int world_x(int ppu_x, int cam) {
    return cam + (((ppu_x - cam) + 128) & 255) - 128;
}

/* First status-bar line: where the scroll stops following the playfield. */
static int hud_top(void) {
    for (int y = 120; y < 240; y++) {
        int sy0 = cyc_render_line_scroll_y(y - 1), sy1 = cyc_render_line_scroll_y(y);
        if (cyc_render_line_scroll_x(y) != cyc_render_line_scroll_x(y - 1) ||
            (sy1 != sy0 + 1 && !(sy0 == 479 && sy1 == 0)) ||
            cyc_render_line_bg_table(y) != cyc_render_line_bg_table(y - 1))
            return y;
    }
    return 240;
}

/* The status bar's white top border: its artwork starts there. */
static int hud_art_line(const uint32_t *native, int height, int hud) {
    for (int art = hud; art < height && art < hud + 8; art++) {
        int white = 0;
        for (int x = 16; x < 256; x++) white += (native[art * 256 + x] & 0xFFFFFFu) == 0xFFFFFFu;
        if (white >= 16) return art;
    }
    return hud;
}

/* ---- Modern HUD -------------------------------------------------------
 * Wonder-style: no boxes, no score or timer. The values the status bar shows
 * are cut from its live picture cell by cell (located through its nametable
 * tiles), the box fill made transparent, and drawn onto the playfield with a
 * dark outline: lives and coins top-left, the P-meter under them while it
 * charges, held item cards top-right. */
#define TILE_M0    0x74 /* Mario icon (Luigi: $76) */
#define TILE_TIMES 0xFB
#define TILE_COIN  0xEC
#define TILE_PMT_ON  0xEE
#define TILE_PMT_OFF 0xEF
#define RAM_PLAYER_POWER    0x03DD
#define RAM_INVENTORY_CARDS 0x7D9C

typedef struct { int ntrow, y0; uint8_t tiles[32]; } HudRow;

/* The nametable row shown on line y of the status bar, and the line its
 * tiles start on. */
static bool hud_row_at(int y, HudRow *r) {
    int sy = cyc_render_line_scroll_y(y), sx = cyc_render_line_scroll_x(y);
    if (sx & 7) return false;
    uint16_t base = (uint16_t)(sy >= 240 ? 0x2800 : 0x2000);
    r->ntrow = (sy % 240) / 8;
    r->y0 = y - (sy % 8);
    for (int c = 0; c < 32; c++) {
        int col = (sx / 8 + c) & 63;
        r->tiles[c] = cyc_render_nametable((uint16_t)(base + (col >= 32 ? 0x400 : 0) + r->ntrow * 32 + (col & 31)));
    }
    return true;
}

static int find_tile(const HudRow *r, uint8_t t, int from) {
    for (int c = from; c < 32; c++)
        if (r->tiles[c] == t) return c;
    return -1;
}

/* Copy native cells [col, col+cols) x [y0, y0+rows*8) to (dx, dy), the box fill
 * transparent. `tint` 0 keeps the cell's own colours; otherwise every glyph
 * pixel takes it. Drawn pixels are marked for the outline pass. */
static uint8_t s_hudmask[CYC_VIDEO_MAX_WIDTH * 240];
static void hud_cells(uint32_t *out, int width, int height, const uint32_t *native, int col, int cols, int y0,
                      int rows, int dx, int dy, uint32_t fill, uint32_t tint) {
    for (int y = 0; y < rows * 8; y++)
        for (int x = 0; x < cols * 8; x++) {
            int sx = col * 8 + x, sy = y0 + y, ox = dx + x, oy = dy + y;
            if (sx < 0 || sx >= 256 || sy < 0 || sy >= 240 || ox < 0 || ox >= width || oy < 0 || oy >= height) continue;
            uint32_t c = native[sy * 256 + sx];
            if (c == fill) continue;
            out[oy * width + ox] = tint ? tint : c;
            s_hudmask[oy * width + ox] = 1;
        }
}


/* The box fill: the most common colour of a blank ($FE) status-bar cell. */
static uint32_t hud_fill(const uint32_t *native, const HudRow *r) {
    int c = find_tile(r, 0xFE, 2);
    if (c < 0) return 0;
    uint32_t best = 0;
    int best_n = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t v = native[(r->y0 + i / 8) * 256 + c * 8 + i % 8];
        int n = 0;
        for (int j = 0; j < 64; j++) n += native[(r->y0 + j / 8) * 256 + c * 8 + j % 8] == v;
        if (n > best_n) { best_n = n; best = v; }
    }
    return best;
}

#define HUD_WHITE 0xFFFFFFFFu
#define HUD_GOLD  0xFFFFC020u
#define HUD_GRAY  0xFF808080u

static int digits_width(const HudRow *r, int col) { return r->tiles[col] == 0xFE ? 1 : 2; }

static void draw_floating_hud(uint32_t *out, int width, int height, int hud, const uint32_t *native) {
    int art = hud_art_line(native, height, hud);
    HudRow r1, r2;
    int found = 0;
    for (int y = art; y < height && found < 2; y++) {
        HudRow r;
        if (!hud_row_at(y, &r)) return;
        if (found == 0 && find_tile(&r, TILE_COIN, 0) >= 0) { r1 = r; found = 1; y = r.y0 + 7; continue; }
        if (found == 1 && r.ntrow == r1.ntrow + 1) { r2 = r; found = 2; }
    }
    if (found < 2) return;
    int cm = find_tile(&r2, TILE_M0, 0);
    if (cm < 0) cm = find_tile(&r2, TILE_M0 + 2, 0);
    int cx = find_tile(&r2, TILE_TIMES, 0), cc = find_tile(&r1, TILE_COIN, 0);
    int cp = find_tile(&r1, TILE_PMT_ON, 0);
    if (cp < 0) cp = find_tile(&r1, TILE_PMT_OFF, 0);
    if (cm < 0 || cx < 0 || cc < 0) return;
    uint32_t fill = hud_fill(native, &r1);

    memset(s_hudmask, 0, (size_t)width * (size_t)height);
    const int m = 8, top = 8;
    int x = m;
    hud_cells(out, width, height, native, cm, 2, r2.y0, 1, x, top, fill, 0);                /* M */
    x += 18;
    /* Digits are already white with a black edge; "x" and the coin are
     * one-colour glyphs, tinted. */
    hud_cells(out, width, height, native, cx, 1, r2.y0, 1, x, top, fill, HUD_WHITE);        /* x */
    x += 8;
    int lw = digits_width(&r2, cx + 1), lc = cx + 1 + (lw == 1);
    hud_cells(out, width, height, native, lc, lw, r2.y0, 1, x, top, fill, 0);               /* lives */
    x += lw * 8 + 14;
    hud_cells(out, width, height, native, cc, 1, r1.y0, 1, x, top, fill, HUD_GOLD);         /* $ */
    x += 9;
    int cw = digits_width(&r1, cc + 1), ccol = cc + 1 + (cw == 1); /* CoinH, CoinL follow the icon */
    hud_cells(out, width, height, native, ccol, cw, r1.y0, 1, x, top, fill, 0);             /* coins */
    if (cp >= 0 && ram(RAM_PLAYER_POWER)) {                                                  /* P-meter */
        bool full = true;
        for (int i = 0; i < 6; i++) {
            bool lit = r1.tiles[cp + i] == TILE_PMT_ON;
            full = full && lit;
            hud_cells(out, width, height, native, cp + i, 1, r1.y0, 1, m + i * 8, top + 12, fill,
                      lit ? HUD_WHITE : HUD_GRAY);
        }
        hud_cells(out, width, height, native, cp + 6, 2, r1.y0, 1, m + 50, top + 12, fill, full ? HUD_GOLD : HUD_GRAY);
    }

    /* Held item cards: the 16x16 picture in each of the three card boxes. */
    int cards[3], nc = 0;
    for (int i = 0; i < 3; i++)
        if (ram((uint16_t)(RAM_INVENTORY_CARDS + i))) cards[nc++] = i;
    int boxes[3], nb = 0;
    for (int c = cc + 3; c < 31 && nb < 3; c++)
        if (r1.tiles[c] == 0xA6 || r1.tiles[c] == 0xAA) boxes[nb++] = c + 1;
    for (int k = 0; k < nc && cards[k] < nb; k++) {
        int dx = width - m - (nc - k) * 18;
        hud_cells(out, width, height, native, boxes[cards[k]], 2, r1.y0, 2, dx, top, fill, 0);
    }

    /* Dark outline around everything drawn. */
    for (int y = 1; y < 48 && y < height - 1; y++)
        for (int xx = 1; xx < width - 1; xx++) {
            int i = y * width + xx;
            if (s_hudmask[i]) continue;
            if (s_hudmask[i - 1] || s_hudmask[i + 1] || s_hudmask[i - width] || s_hudmask[i + width])
                out[i] = 0xFF000000u;
        }
}

/* Status bar centred on black (its own surround colour). The line where the
 * IRQ switches to it renders backdrop only; wide, that reads as a stray band,
 * so a backdrop-only line there is black too. */
static void draw_hud(uint32_t *out, int width, int height, int hud, const uint32_t *native) {
    const uint32_t black = 0xFF000000u, backdrop = cyc_render_color(0);
    int hx0 = (width - 256) / 2;
    /* Lines between the split and the status bar's white top border are
     * blank by design; whatever the late IRQ leaked into them is dropped. */
    int art = hud_art_line(native, height, hud);
    for (int y = hud; y < art; y++)
        for (int x = 0; x < width; x++) out[y * width + x] = black;
    for (int y = art; y < height; y++) {
        uint32_t *line = out + y * width;
        const uint32_t *src = native + y * 256;
        /* The switch line can come out partly drawn (IRQ timing varies by a few
         * dots), so the first lines count as blank if they hold nothing but
         * backdrop and black. */
        /* The status bar's artwork starts at native column 16; the IRQ lands a
         * few dots into its first lines, leaking playfield pixels left of it. */
        bool blank = true;
        for (int x = 16; x < 256 && blank; x++)
            blank = src[x] == backdrop || (y < hud + 3 && (src[x] & 0xFFFFFFu) == 0);
        for (int x = 0; x < width; x++) line[x] = black;
        if (!blank) memcpy(line + hx0 + 8, src + 8, 248 * sizeof(uint32_t));
    }
}

/* Smallest period (8/16/32) with which the `span` pixels from `start` repeat. */
static int edge_period(const uint32_t *src, int start, int dir) {
    static const int periods[] = { 8, 16, 32 };
    for (int k = 0; k < 3; k++) {
        int p = periods[k];
        bool ok = true;
        for (int i = 0; i < 2 * p && ok; i++) ok = src[start + dir * i] == src[start + dir * (i + p)];
        if (ok) return p;
    }
    return 0;
}

/* Title/ending: a fixed stage with no level behind it. Rows whose edges
 * repeat (curtain, checkered floor) tile outward in phase. Elsewhere the
 * scenery at each edge (the hills, cut by the original frame) is mirrored
 * outward from the sprite-free background, up to TITLE_MIRROR pixels, which
 * stays clear of the logo and menu text; beyond that is backdrop. */
#define TITLE_MIRROR 56
/* Scenery to mirror: anything but the backdrop and the logo's blue shadow
 * fragments that touch the frame edge. */
static uint32_t title_scenery(uint32_t c, uint32_t backdrop) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return (b > r + 40 && b > g + 40) ? backdrop : c;
}
/* The "MARIO BROS." line of the logo reaches into the mirror window. */
static bool title_logo(int col, int y) { return y >= 58 && y < 112 && col >= 36 && col <= 220; }

static bool line_black(const uint32_t *src) {
    for (int x = 8; x < 256; x++)
        if (src[x] & 0xFFFFFFu) return false;
    return true;
}
static bool lines_equal(const uint32_t *a, const uint32_t *b) {
    return !memcmp(a + 8, b + 8, 248 * sizeof(uint32_t));
}
/* The stage's last rows are black below the checkered floor (overscan on a
 * CRT); wide, that reads as a bar. Continue the floor with its own vertical
 * period instead. */
static void title_floor_to_bottom(uint32_t *out, int width, int height, const uint32_t *native) {
    int last = height - 1;
    while (last > 180 && line_black(native + last * 256)) last--;
    if (last == height - 1 || last <= 180) return;
    for (int p = 8; p <= 40; p++) {
        bool ok = true;
        for (int k = 0; k < p && ok; k++) ok = lines_equal(native + (last - k) * 256, native + (last - k - p) * 256);
        if (!ok) continue;
        for (int y = last + 1; y < height; y++) memcpy(out + y * width, out + (y - p) * width, (size_t)width * 4);
        return;
    }
}

static int compose_title(uint32_t *out, int width, int height, int x0, const uint32_t *native) {
    static uint32_t bg[256 * 240];
    const uint32_t *scenery = cyc_render_background(bg) ? bg : native;
    uint32_t backdrop = cyc_render_color(0);
    for (int y = 0; y < height; y++) {
        const uint32_t *src = native + y * 256, *bgl = scenery + y * 256;
        uint32_t *line = out + y * width;
        /* Only the curtain and the floor tile outward; a hill's base repeats
         * too, but tiling it would lay a flat band across the margin. */
        bool band = y < 40 || y >= 190;
        int pl = band ? edge_period(src, 8, 1) : 0, pr = band ? edge_period(src, 255, -1) : 0;
        /* Native columns 0-7 are masked by the game; they continue the edge too. */
        int masked = (cyc_render_line_mask(y) & 0x02) ? 0 : 8;
        for (int x = 0; x < x0 + masked; x++) {
            int d = x0 + 8 - x; /* pixels left of native column 8 (1-based) */
            if (pl) line[x] = src[8 + (pl - d % pl) % pl];
            else line[x] = d <= TITLE_MIRROR && !title_logo(8 + d, y) ? title_scenery(bgl[8 + d], backdrop) : backdrop;
        }
        for (int x = masked; x < 256; x++) line[x0 + x] = src[x];
        for (int x = x0 + 256; x < width; x++) {
            int d = x - (x0 + 255); /* pixels right of native column 255 (1-based) */
            if (pr) line[x] = src[255 - (pr - d % pr) % pr];
            else line[x] = d <= TITLE_MIRROR && !title_logo(255 - d, y) ? title_scenery(bgl[255 - d], backdrop) : backdrop;
        }
    }
    title_floor_to_bottom(out, width, height, native);
    return 1;
}

static int compose(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user) {
    (void)user;
    if (s_rom_ok && ram(RAM_UPDATE_SELECT) == 0x20) return compose_title(out, width, height, native_x0, native);
    LevelFrame f;
    if (!level_frame(&f, width)) return 0;
    const uint8_t *layout = f.layout, *tile_mem = f.tile_mem;
    int hud = f.hud, cam = f.cam, level_px = f.level_px, view = f.view, x0 = f.x0;
    decode_tiles();
    cyc_render_set_native_origin(x0);

    /* Modern HUD (levels only): the playfield takes the whole picture, moved
     * down by the status bar's height, and the rows above it come from the
     * level rebuild. The status bar's panels then float in the corners. */
    int dy = (s_modern_hud && f.tileset != 0) ? height - hud : 0;
    int rows = dy ? height : hud;
    uint32_t backdrop = cyc_render_color(0);
    for (int cy = 0; cy < rows; cy++) {
        int y = cy - dy; /* native line; negative above the original picture */
        int src = y < 0 ? 0 : y;
        int wx0 = world_x(cyc_render_line_scroll_x(src), cam) - x0;
        int wy = cyc_render_line_scroll_y(src) + (y < 0 ? y : 0);
        int row = wy >> 4, py = wy & 15;
        uint32_t *line = out + cy * width;
        uint8_t *cov = s_opaque + cy * width;
        for (int x = 0; x < width; x++) {
            int wx = wx0 + x;
            uint32_t color = backdrop;
            uint8_t opaque = 0;
            if (wx >= 0 && wx < level_px && wy >= 0 && row < TILE_MEM_ROWS) {
                int scr = wx >> 8, col = (wx >> 4) & 15, px = wx & 15;
                uint8_t m = tile_mem[scr * TILE_MEM_SCREEN + row * 16 + col];
                int quad = (px >> 3) * 2 + (py >> 3); /* UL, LL, UR, LR */
                uint8_t t = layout[quad * 256 + m];
                uint8_t p = s_pixels[t][py & 7][px & 7];
                if (p) { color = cyc_render_color((m >> 6) * 4 + p); opaque = 1; }
            }
            line[x] = color;
            cov[x] = opaque;
        }
    }
    for (int y = rows; y < height; y++) memset(s_opaque + y * width, 1, (size_t)width);

    if (s_check) {
        static uint32_t bg[256 * 240];
        if (cyc_render_background(bg)) {
            for (int y = 8; y < hud; y++)
                for (int x = 8; x < 248; x++) {
                    int cx = x0 + x;
                    if (cx < 0 || cx >= width) continue;
                    s_check_px++;
                    s_check_bad += out[(y + dy) * width + cx] != bg[y * 256 + x];
                }
        }
    }

    /* The frame's own sprites in the two native edge strips the rebuilt
     * background replaced, then objects the game kept off its screen. */
    const uint8_t *oam = cyc_render_oam();
    smb3_ws_sprites_begin(width);
    for (int i = 0; i < 64; i++) {
        const uint8_t *s = oam + i * 4;
        if (s[0] >= 0xEF || (s[3] >= 8 && s[3] < 241)) continue;
        smb3_ws_draw_sprite(out, width, height, x0 + s[3], s[0] + 1 + dy, s[1], s[2], s_opaque);
    }
    smb3_ws_objects_draw(out, width, height, view, dy, s_opaque);

    /* Native playfield, minus the masked/garbled edge columns. Where the
     * native pixel is plain background (it equals the sprite-free capture),
     * the exact rebuild stands, which also drops SMB3's right-edge attribute
     * glitch; sprites and anything else the PPU drew stay native. */
    static uint32_t bgcap[256 * 240];
    bool have_bg = cyc_render_background(bgcap);
    for (int y = 0; y < hud && !s_raw; y++)
        for (int x = 8; x < 248; x++) {
            int cx = x0 + x;
            if (cx < 0 || cx >= width) continue;
            uint32_t n = native[y * 256 + x];
            if (!have_bg || n != bgcap[y * 256 + x]) out[(y + dy) * width + cx] = n;
        }
    if (dy) draw_floating_hud(out, width, height, hud, native);
    else draw_hud(out, width, height, hud, native);

    if (s_check && (++s_frames == 1 || s_frames % 60 == 0)) {
        fprintf(stderr, "[smb3-ws] check: %llu/%llu background pixels differ (hud_top %d, view %d, level %dpx)\n",
                (unsigned long long)s_check_bad, (unsigned long long)s_check_px, hud, view, level_px);
        s_check_bad = s_check_px = 0;
    }
    return 1;
}

void smb3_ws_frame_end(void) {
    Smb3WsView v = { 0 };
    LevelFrame f;
    int width = cyc_video_width();
    /* Normal horizontal play only ($C0); maps, bonus games and partitioned
     * screens keep their objects to the native screen. */
    if (s_mode != NES_VIDEO_STOCK && ram(RAM_UPDATE_SELECT) == 0xC0 && level_frame(&f, width) && f.tileset != 0) {
        v.active = true;
        v.width = width;
        v.native_world = f.w0;
        v.view_left = f.view;
    }
    smb3_ws_objects_frame_end(&v);
}

static void apply(void) {
    bool on = s_mode != NES_VIDEO_STOCK && s_rom_ok;
    cyc_render_set_compositor(on ? compose : NULL, NULL);
    cyc_video_set_mode(on ? s_mode : NES_VIDEO_STOCK);
    cyc_render_capture_background(on); /* the title's scenery is mirrored from the BG alone */
    if (!on) smb3_ws_objects_reset();
}

void smb3_ws_power_on(void) {
    smb3_ws_objects_reset();
    s_rom_ok = verify_rom();
    if (!s_rom_ok && s_mode != NES_VIDEO_STOCK)
        fprintf(stderr, "[smb3-ws] ROM tables not recognised (needs SMB3 USA Rev 1); widescreen off\n");
    apply();
}

void smb3_ws_set_mode(int mode) {
    s_mode = mode;
    apply();
}

int smb3_ws_mode(void) { return s_mode; }

void smb3_ws_set_modern_hud(bool on) { s_modern_hud = on; }
bool smb3_ws_modern_hud(void) { return s_modern_hud; }

void smb3_ws_set_check(bool on) {
    s_check = on;
    s_raw = on && getenv("SMB3_WS_RAW"); /* diagnostics: show the rebuilt view alone */
    apply();
}
