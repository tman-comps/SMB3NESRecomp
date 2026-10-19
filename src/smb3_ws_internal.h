#pragma once
/* Shared between the widescreen compositor and its object pass. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;      /* a horizontal level the compositor extends */
    int  width;       /* presented width */
    int  native_world;/* world X of native column 0 */
    int  view_left;   /* world X of canvas column 0 */
    int  extra_top;   /* lines shown above the native picture (modern HUD) */
} Smb3WsView;

/* Start a picture's sprites: draw them in priority order afterwards (first
 * opaque pixel wins, as on the PPU). */
void smb3_ws_sprites_begin(int width);
/* One sprite (8x8 or 8x16 as the frame used), through the game's own CHR
 * bank selections; `opaque` (width x 240) decides behind-background pixels. */
void smb3_ws_draw_sprite(uint32_t *out, int width, int height, int x, int y, uint8_t tile, uint8_t attr,
                         const uint8_t *opaque);

void smb3_ws_objects_reset(void);
void smb3_ws_objects_frame_end(const Smb3WsView *view);
/* dy: canvas lines the playfield is moved down by (modern HUD). */
void smb3_ws_objects_draw(uint32_t *out, int width, int height, int view_left, int dy, const uint8_t *opaque);
void smb3_ws_objects_set_wide(bool wide);
bool smb3_ws_objects_wide(void);
