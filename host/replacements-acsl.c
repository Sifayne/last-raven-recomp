/* Native replacements for the acsl module (Armored Core: Silent Line Portable).
 *
 * host/replace-acsl.txt names the functions; this file defines them. The
 * shape of a replacement and the rules it has to follow are in
 * host/replacements.c, Last Raven's. */
#include <stdint.h>

#include "psprecomp/mem.h"
#include "acsl_funcs.h"          /* psp_func_*, the __orig originals, r_* aliases */

/* Is a sortie live? AC3 Portable's gate (host/replacements-ac3p.c) is the
 * head of the mission's unit-object list, which its per-frame record builder
 * 000FDFA8 loads from 0x00490B90; that builder's twin here, 000E2464 (0.94 by
 * instruction sequence, the same prologue), loads its list head from
 * 0x0047A7D4. Null in every menu snapshot (title, option screen, name entry,
 * prologue, assembly) and set from the sortie on -- and, measured on Sif's
 * recording, still set through the mission's results and the menus after
 * them, which is why in_play (host/ac3_controls.h) also wants the player's
 * update to be running. */
enum { ACSL_UNIT_LIST = 0x0047A7D4u };
static int unit_list_live(void) { return psp_read32(ACSL_UNIT_LIST) != 0; }

/* The stick converter: this title's twin of Last Raven's psp_func_00279A50,
 * found by instruction fingerprint (scripts/fn-twins.py). The yaw integrator
 * is the one the turn handler's twin (000006F4) calls; its gate is Last
 * Raven's -- the movement-state object at ac+8888 with byte +340 == -1, and
 * the hold flag s16 +32 of what the getter psp_func_0000AF40 returns. The
 * player's AC is the first of the record builder's array (0x00459B20,
 * stride 8896), as AC3P's is -- confirmed on 9 Sep against Sif's recording
 * of the first mission (scenarios/acsl/mission.pad): the replacements' logs
 * name it and only it. */
#define AC3_STICK_CONVERTER 0020CE84
#define AC3_YAW_INTEGRATOR  00009D80
#define AC3_PLAYER_AC       0x00459B20u
static int yaw_gate(uint32_t ac, int *held) {
    const uint32_t state = psp_read32(ac + 8888);
    if (!state || (int8_t)psp_read8(state + 340) != -1) return 1;
    r_a0 = ac;
    psp_func_0000AF40();
    *held = r_v0 ? (int16_t)psp_read16(r_v0 + 32) != 0 : 0;
    return 0;
}

/* The look controller here is Last Raven's shape (AC3P's reads its pad
 * inline): the same movement-state gate as the yaw's, then actions 11 and 10
 * through the pad helpers -- psp_func_000130D8(pad, action) for a press this
 * frame, which clears the lockout, and psp_func_000130B8 for a hold, which
 * ramps the rate -- where pad = *(*(ac+8892)+4). Both are asked here, so a
 * frame the game's own buttons would act on is left to them whole. The six
 * constants are Last Raven's values at 0x00275B60. The AC offsets of the
 * state, the yaw and the velocity are AC3P's; the sortie that would confirm
 * them on this title were confirmed on 9 Sep by a recorded sortie. */
#define AC3_PITCH_CONTROLLER 000081A4
#define AC3_LOOK_PARAMS      0x00275B60u
static int pitch_gate(uint32_t ac) {
    const uint32_t state = psp_read32(ac + 8888);
    return !state || (int8_t)psp_read8(state + 340) != -1;
}
static int pad_action(uint32_t ac, uint32_t action, void (*ask)(void)) {
    r_a0 = psp_read32(psp_read32(ac + 8892) + 4);
    r_a1 = action;
    ask();
    return r_v0 != 0;
}
static int look_keys(uint32_t ac) {
    return pad_action(ac, 10, psp_func_000130B8) || pad_action(ac, 11, psp_func_000130B8) ||
           pad_action(ac, 10, psp_func_000130D8) || pad_action(ac, 11, psp_func_000130D8);
}

/* The walk's two push assemblers (identical to AC3P's by fingerprint, as are
 * every helper they call) and this title's offsets: the animation state
 * pointer at ac+3104, the parts block at ac+1860, the ramp at ac+3024. */
#define AC3_WALK_PUSH_A 000193D4
#define AC3_WALK_PUSH_B 00019E24
#define AC3_ANIM_TRAVEL 000992D8
#define AC3_YAW_MATRIX  0023D72C
#define AC3_PUSH        00009500
#define AC3_ROT_TABLE   0x0027B190u
enum { AC3_AC_ANIM = 3104, AC3_AC_PARTS = 1860, AC3_AC_RAMP = 3024 };

/* The modern pad, Last Raven's way: the PSP converter (0020CE44) strips and
 * remembers the carrier bits, and the game's two action queries -- held
 * psp_func_000130B8(pad, action), pad+0 against the row at
 * (*(pad+28))+1792+2*action, and pressed 000130D8, the same on pad+4 -- are
 * answered semantically for the player's pad, *(*(ac+8892)+4); see
 * host/ac3_controls.h. */
#define AC3_PAD_MAPPER     0020CE44
#define AC3_ACTION_HELD    000130B8
#define AC3_ACTION_PRESSED 000130D8
static int player_pad_is(uint32_t pad) {
    const uint32_t holder = psp_read32(AC3_PLAYER_AC + 8892);
    return holder && pad && psp_read32(holder + 4) == pad;
}
/* The animation updater, for the walk cycle's cadence; see host/ac3_controls.h. */
#define AC3_ANIM_UPDATE 00099E38

/* The per-frame camera update and the camera table, for the chase camera's
 * lag; see host/ac3_controls.h. */
#define AC3_CAMERA_UPDATE 00025268
#define AC3_CAMERA_TABLE  0x00474100u

/* The projection builder, for the adaptive aspect; see host/ac3_controls.h. */
#define AC3_PERSPECTIVE 001EA920
#include "ac3_controls.h"

/* The adaptive aspect has a native camera replacement here, so present.c may
 * offer "Match window". Without it the GL backend would spread a scene the
 * camera never widened. */
const int lr_adaptive_aspect_available = 1;

/* This host reads the modern pad's carrier bits (the converter and the two
 * queries above), so present.c may offer that layout. */
const int lr_modern_controls_available = 1;

/* Mission rendering at independent FPS: US PSN NPUH10025 1.00. */
#define FPS_MISSION_LOOP 000914B0
#define FPS_PACER        001EB608
#define FPS_BUILD_JOINTS 000023C4
#define FPS_REBUILD_AC   0000CB54
#define FPS_UPDATE_VIEWS 0009039C
#define FPS_PUBLISH_VIEW 000EDBB0
#define FPS_STOP_QUERY   000D4BF4
#define FPS_VIEW_CONTEXT 0x0028B6B0u
#define FPS_CAMERA_MODE  0x00459940u
#define FPS_PAUSE_BYTE   0x00459930u
#define FPS_AC_COUNT     0x00459B10u
#define FPS_AC_STRIDE    8896u
#define FPS_TICK         0x00280DF0u
#define FPS_TIMER        0x00498200u
#include "fps_ac3.h"
