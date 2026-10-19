/* The launcher's Mods screen: Quality of life (mods/preloaded/.../qol/manifest.toml).
 *
 * No slowdown: nesrecomp's MMC3 extra-scanline enhancement (cyc_core.h) adds
 * blank lines before NMI, so the game's frame work always finishes in time;
 * the window still presents at 60 Hz and audio pauses for those lines.
 * Mods activate before the cartridge powers on, so the selection is applied
 * from the game's power_on (smb3_qol_power_on). */
#include <stdio.h>

#include "cyc_core.h"
#include "mod_runtime.h"
#include "smb3_qol.h"

#define NO_SLOWDOWN_LINES 128

static unsigned s_extra_lines;

static void reset_qol(void) { s_extra_lines = 0; }
static void activate_no_slowdown(void) { s_extra_lines = NO_SLOWDOWN_LINES; }

void smb3_qol_power_on(void) {
    if (!s_extra_lines) { cyc_set_extra_scanlines(0); return; }
    if (!cyc_extra_scanlines_supported() || !cyc_set_extra_scanlines(s_extra_lines))
        fprintf(stderr, "[Mods] No slowdown: extra scanlines not available on this board\n");
}

NES_MOD_CONSTRUCTOR(register_qol_plugin) {
    if (!nes_mod_register_reset_callback(reset_qol) ||
        !nes_mod_register_activation_plugin("super-mario-bros-3.no-slowdown", activate_no_slowdown))
        fprintf(stderr, "[Mods] Failed to register the SMB3 quality-of-life plugin\n");
}
