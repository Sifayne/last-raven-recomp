/* Native replacements for the ac3p module (Armored Core 3 Portable).
 *
 * host/replace-ac3p.txt names the functions; this file defines them. The
 * shape of a replacement and the rules it has to follow are in
 * host/replacements.c, Last Raven's. */
#include <stdint.h>

#include "psprecomp/mem.h"
#include "ac3p_funcs.h"          /* psp_func_*, the __orig originals, r_* aliases */

/* Is a sortie live? The word at 0x00490B90 is the head of the mission's unit
 * objects (four of 0x180 bytes: the player's is the last), the list the
 * per-frame record builder 000FDFA8 walks. Null on the title and option
 * screens and in the corporate menu; set from the sortie's opening cutscene
 * on and constant through play. Found by scripts/ram-gate.py over
 * PSPRECOMP_RAMSNAP images of scenarios/ac3p/probe-mission.pad (null at polls
 * 3000 and 5500, set at 6500 and 7500..8900), 8 Sep.
 *
 * The first gate tried was the player's slot in the builder's record array
 * (0x002C4180): it is set only some seconds into control -- the builder
 * skips a unit until a flag at +142 is raised -- so the first seconds of a
 * sortie ran on the game's own converter and then switched (Sif, 8 Sep).
 * Re-sourcing the stick during the cutscene, where input is ignored, costs
 * nothing; a gate that opens late costs the feel of every sortie's start. */
enum { AC3P_UNIT_LIST = 0x00490B90u };
static int unit_list_live(void) { return psp_read32(AC3P_UNIT_LIST) != 0; }

/* The stick converter: this title's twin of Last Raven's psp_func_00279A50,
 * found by instruction fingerprint (scripts/fn-twins.py). The yaw integrator
 * and the player's AC: see host/ac3_controls.h. The hold at +7108 is the only
 * gate the integrator itself has; the turn state's handler is only reached in
 * a sortie. */
#define AC3_STICK_CONVERTER 001ECE84
#define AC3_YAW_INTEGRATOR  00106B3C
#define AC3_PLAYER_AC       0x0046EED0u
static int yaw_gate(uint32_t ac, int *held) {
    *held = (int16_t)psp_read16(ac + 7108) != 0;
    return 0;
}

/* The look controller reads its pad inline: the held word at +180 and the
 * pressed word at +184 against the key-assign masks at +1876 (up) and +1878
 * (down). It has no state gate of its own; the player's per-frame update
 * 00100450 decides when it runs. */
#define AC3_PITCH_CONTROLLER 00104FC4
#define AC3_LOOK_PARAMS      0x0025DEE0u
static int pitch_gate(uint32_t ac) { (void)ac; return 0; }
static int look_keys(uint32_t ac) {
    const uint32_t keys  = psp_read16(ac + 180) | psp_read16(ac + 184);
    const uint32_t masks = psp_read16(ac + 1876) | psp_read16(ac + 1878);
    return (keys & masks) != 0;
}

/* The walk's two push assemblers and what they call; see host/ac3_controls.h.
 * The animation state the travel is read from is the pointer at ac+3164, the
 * parts block at ac+1924, the start-up ramp the float at ac+3060. */
#define AC3_WALK_PUSH_A 0010D5A4
#define AC3_WALK_PUSH_B 0010DF98
#define AC3_ANIM_TRAVEL 000DC628
#define AC3_YAW_MATRIX  0021A790
#define AC3_PUSH        00106EF8
#define AC3_ROT_TABLE   0x00268800u
enum { AC3_AC_ANIM = 3164, AC3_AC_PARTS = 1924, AC3_AC_RAMP = 3060 };

/* The modern pad: the PSP converter (Last Raven's 00279A10 to the
 * instruction) strips and remembers the carrier bits; the AC's per-frame pad
 * copy, psp_func_00100894(ac, word), then gets the actions' own masks from
 * the key-assign row at ac+1856 ORed into the held word at +180 and the
 * pressed word at +184; see host/ac3_controls.h. This title has no pad
 * object to match, so the player check is the AC itself. */
#define AC3_PAD_MAPPER 001ECE44
#define AC3_PAD_COPY   00100894
static int player_pad_is(uint32_t pad) { return pad == AC3_PLAYER_AC; }
static uint32_t action_mask(uint32_t ac, uint32_t action) {
    return action < 16 ? psp_read16(ac + 1856 + 2 * action) : 0;
}
/* The animation updater, for the walk cycle's cadence; see host/ac3_controls.h. */
#define AC3_ANIM_UPDATE 000DD29C

/* The per-frame camera update and the camera table, for the chase camera's
 * lag; see host/ac3_controls.h. */
#define AC3_CAMERA_UPDATE 0011B440
#define AC3_CAMERA_TABLE  0x00497800u

/* The projection builder, for the adaptive aspect; see host/ac3_controls.h. */
#define AC3_PERSPECTIVE 001CC4DC
#include "ac3_controls.h"

/* What the shared host may offer this title (psprecomp/host/title.h):
 * - the adaptive aspect: there is a native camera replacement here. Without
 *   it the GL backend would spread a scene the camera never widened;
 * - the modern controller layout: this host reads the modern pad's carrier
 *   bits (the converter and the pad copy above). */
const psp_title psp_title_info = {
    .name = "Armored Core 3 Portable",
    .capabilities = PSP_TITLE_MODERN_CONTROLS | PSP_TITLE_ADAPTIVE_ASPECT,
    .keys_wasd_help = AC_KEYS_WASD_HELP,
    .gamepad_modern_help = AC_GAMEPAD_MODERN_HELP,
};

/* Mission rendering at independent FPS: US PSN NPUH10023 1.01. */
#define FPS_MISSION_LOOP 000E0F10
#define FPS_PACER        001CD1C4
#define FPS_BUILD_JOINTS 00104EEC
#define FPS_REBUILD_AC   00108940
#define FPS_UPDATE_VIEWS 000E07F0
#define FPS_PUBLISH_VIEW 0000D5DC
#define FPS_STOP_QUERY   0017BC48
#define FPS_VIEW_CONTEXT 0x0024D1C0u
#define FPS_CAMERA_MODE  0x0046A240u
#define FPS_PAUSE_BYTE   0x0046A22Fu
#define FPS_AC_COUNT     0x0048A1D0u
#define FPS_AC_STRIDE    11968u
#define FPS_AC_MAX       9u      /* the count word itself sits 9.3 strides past AC[0] */
#define FPS_TICK         0x00268780u
#define FPS_TIMER        0x0048BA54u
#include "fps_ac3.h"
