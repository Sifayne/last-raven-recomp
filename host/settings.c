/* Armored Core's own options: the table and the few rules psprecomp's
 * settings mechanism asks a pack for (psprecomp/host/settings.h). Shared by
 * Last Raven, AC3 Portable and Silent Line. */
#include "settings.h"

#define C(k,lbl,pg,hlp,d_,ch_,lb_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=lb_, __VA_ARGS__}
#define N(k,lbl,pg,hlp,d_,lo,hi,st,sp,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=PSP_OPTION_NUMBER,.dflt=d_,.min=lo,.max=hi,.step=st,.special=sp, __VA_ARGS__}
#define S(k,lbl,pg,hlp,type_,d_,lo,hi,st,sp,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=type_,.dflt=d_,.min=lo,.max=hi,.step=st,.special=sp, __VA_ARGS__}
const psp_option_def lr_options[LR_OPTION_COUNT - PSP_PLAYER_OPTIONS] = {
    C(ASPECT,"Aspect ratio","Graphics","Original keeps the PSP-shaped view. Match window expands the 3D view on wider windows; the HUD remains centered. Requires OpenGL.","native","native|window","Original|Match window",.flags=PSP_OPTION_NEEDS_GL),
    C(HIGH_FPS,"Higher FPS","Graphics","Smooth mission rendering between the game's original simulation ticks. Menus and movies keep their original timing.","0","0|1","Off|On"),
    S(FPS_CAP,"FPS cap","Graphics","Maximum mission rendering rate when Higher FPS is on. Enter a custom limit, or unlimited to follow the display's refresh with vertical sync. The game keeps its original simulation speed.",PSP_OPTION_INTEGER,"60",30,1000,1,"unlimited",.special_label="Unlimited",.format="%.0f FPS",
      .stops="30|60|90|120|144|165|240|360|1000|unlimited"),
    C(INPUT,"Control scheme","Controls","Classic uses the game's controls. Modern improves one-stick response. Dual separates movement from right-stick and mouse look.","classic","classic|modern|dual","Classic|Modern one-stick|Dual stick / mouse look"),
    C(GAMEPAD,"Controller layout","Controls","Modern: LT boost, RT right weapon, LB left weapon/event, RB switch, L3 extension, R3 OB/EO, A inside, B view reset, Y purge. Menu buttons remain conventional. A title without native replacements always uses the classic buttons.","auto","auto|classic|modern","Follow control scheme|Classic PSP buttons|Modern action buttons"),
    C(KEYS,"Keyboard layout","Controls","WASD moves; Space boosts; left mouse fires right weapon; right/middle fires left weapon; Q switches. Enter is Start, Backspace Select. Uses the game's default key assignment.","classic","classic|wasd","Classic|WASD",.flags=PSP_OPTION_LIVE),
    C(MOUSE,"Mouse capture","Controls","Captures the pointer at launch. Escape releases it; click to recapture. Mouse look requires the Dual control scheme.","0","0|1","Off|On"),
    N(MOUSE_SENS,"Mouse sensitivity","Controls","Multiplier for mouse look. 1.0 is 0.001 radians per mouse count. Requires Dual controls and mouse capture.","1",0.001,1000,0.1,NULL,.format="%.2fx"),
    N(MOVE_DEADZONE,"Movement deadzone","Controls","Ignore small movement-stick deflections. Entry uses an additional 3% to prevent jitter. Applies to enhanced control schemes.","0.10",0,0.50,0.01,NULL,.format="%.0f%%",.scale=100),
    N(LOOK_DEADZONE,"Look deadzone","Controls","Ignore small look-stick deflections. Lower values are more responsive but may reveal stick drift.","0.08",0,0.50,0.01,NULL,.format="%.0f%%",.scale=100),
    N(STICK_OUTER_DEADZONE,"Outer stick deadzone","Controls","Treat the outer part of the stick as full deflection, helping worn sticks reach the full range.","0.02",0,0.20,0.01,NULL,.format="%.0f%%",.scale=100),
    N(LOOK_EXPO,"Look response curve","Controls","0 is linear. Higher values provide finer aiming near the center while retaining the same maximum turn rate.","0.60",0,1,0.05,NULL),
    N(CAMERA_LAG,"Camera smoothing","Controls","Game preserves the original behavior. 0 follows immediately; larger values follow more slowly. Ignored in Classic controls.","game",0,0.99,0.05,"game"),
};
#undef C
#undef N
#undef S

/* Modern buttons follow the control scheme unless chosen outright. The
 * pointer is captured whenever MOUSE asks, whatever the scheme; mouse look
 * itself needs Dual, which the replacements check (print_notes says so). */
static int resolve(psp_settings *s, char *error) {
    (void)error;
    s->input = (int)s->number[LR_INPUT];
    s->gamepad = s->number[LR_GAMEPAD] ? s->number[LR_GAMEPAD] == 2 : s->input != 0;
    s->mouse = s->number[LR_MOUSE] != 0;
    return 0;
}

static void print_notes(const psp_settings *s, FILE *out) {
    if (s->number[LR_MOUSE] && s->number[LR_INPUT] != 2)
        fprintf(out, "note: mouse capture is enabled, but mouse look requires INPUT=dual\n");
}

/* The schema psprecomp's settings calls read (psprecomp/host/settings.h):
 * the section [pack last-raven], named for the app this pack had before
 * psprecomp's. A new player gets dual-stick controls; the player's own
 * defaults add the window and Match window rendering. */
const psp_settings_schema psp_title_settings = {
    .title = "Armored Core", .id = "last-raven",
    .options = lr_options, .count = LR_OPTION_COUNT - PSP_PLAYER_OPTIONS,
    .play_defaults = "INPUT=dual",
    .gl_error = "Match window resolution/aspect requires OpenGL; choose Automatic or OpenGL",
    .resolve = resolve, .print_notes = print_notes,
};
