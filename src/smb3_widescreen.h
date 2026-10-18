#pragma once
#include <stdbool.h>

/* SMB3 widescreen compositor (cyc_render.h). Presentation only: the machine
 * keeps drawing its 256x240 picture; nothing here writes guest state. */

/* NES_VIDEO_STOCK (off) .. NES_VIDEO_FIT. Applied at once. */
void smb3_ws_set_mode(int mode);
int  smb3_ws_mode(void);
/* Install the compositor (call from power_on, after the cartridge is up). */
void smb3_ws_power_on(void);
/* Per machine frame (host frame_end): the object pass for the wide view. */
void smb3_ws_frame_end(void);
/* Enemy lifetime across the wide view (true, default) or original 4:3. */
void smb3_ws_objects_set_wide(bool wide);
bool smb3_ws_objects_wide(void);
/* --widescreen-check: compare the rebuilt background against the PPU's own
 * on every composed frame and report mismatches on stderr. */
void smb3_ws_set_check(bool on);
