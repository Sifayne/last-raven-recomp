/* Armored Core's player options: the table and the few rules psprecomp's
 * settings mechanism asks a title for (psprecomp/host/settings.h). Shared by
 * Last Raven, AC3 Portable and Silent Line, whose presets are shared too. */
#include "settings.h"

#define C(k,lbl,pg,hlp,d_,ch_,lb_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=lb_, __VA_ARGS__}
#define N(k,lbl,pg,hlp,d_,lo,hi,st,sp,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=PSP_OPTION_NUMBER,.dflt=d_,.min=lo,.max=hi,.step=st,.special=sp, __VA_ARGS__}
#define S(k,lbl,pg,hlp,type_,d_,lo,hi,st,sp,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=type_,.dflt=d_,.min=lo,.max=hi,.step=st,.special=sp, __VA_ARGS__}
const psp_option_def lr_options[LR_OPTION_COUNT] = {
    C(RESOLUTION,"Rendering resolution","Graphics","Original preserves the PSP resolution. Match window renders at the window's physical pixel size and requires OpenGL.","psp","psp|window","Original (480x272)|Match window",.flags=PSP_OPTION_NEEDS_GL),
    C(ASPECT,"Aspect ratio","Graphics","Original keeps the PSP-shaped view. Match window expands the 3D view on wider windows; the HUD remains centered. Requires OpenGL.","native","native|window","Original|Match window",.flags=PSP_OPTION_NEEDS_GL),
    S(WINDOW_SIZE,"Window size","Graphics","Starting size in logical pixels, WIDTHxHEIGHT, in Windowed mode. Windowed fullscreen uses the desktop size instead.",PSP_OPTION_SIZE,"960x544",1,16384,0,NULL),
    C(WINDOW_MODE,"Window mode","Graphics","Windowed fullscreen fills the display without borders at the current desktop resolution. Rendering resolution and aspect ratio remain separate settings.","windowed","windowed|borderless","Windowed|Windowed fullscreen"),
    S(DISPLAY,"Start on display","Graphics","Choose the screen for the game in either window mode. If the saved screen is unavailable, the primary display is used. Screen numbers follow the current display order.",PSP_OPTION_INTEGER,"primary",1,65535,1,"primary",.special_label="Primary display",.format="Display %.0f"),
    C(INPUT,"Control scheme","Controls","Classic uses the game's controls. Modern improves one-stick response. Dual separates movement from right-stick and mouse look.","classic","classic|modern|dual","Classic|Modern one-stick|Dual stick / mouse look"),
    C(GAMEPAD,"Controller layout","Controls","Modern: LT boost, RT right weapon, LB left weapon/event, RB switch, L3 extension, R3 OB/EO, A inside, B view reset, Y purge. Menu buttons remain conventional. A title without native replacements always uses the classic buttons.","auto","auto|classic|modern","Follow control scheme|Classic PSP buttons|Modern action buttons"),
    C(KEYS,"Keyboard layout","Controls","WASD moves; Space boosts; left mouse fires right weapon; right/middle fires left weapon; Q switches. Enter is Start, Backspace Select. Uses the game's default key assignment.","classic","classic|wasd","Classic|WASD"),
    C(MOUSE,"Mouse capture","Controls","Captures the pointer at launch. Escape releases it; click to recapture. Mouse look requires the Dual control scheme.","0","0|1","Off|On"),
    N(MOUSE_SENS,"Mouse sensitivity","Controls","Multiplier for mouse look. 1.0 is 0.001 radians per mouse count. Requires Dual controls and mouse capture.","1",0.001,1000,0.1,NULL,.format="%.2fx"),
    N(MOVE_DEADZONE,"Movement deadzone","Controls","Ignore small movement-stick deflections. Entry uses an additional 3% to prevent jitter. Applies to enhanced control schemes.","0.10",0,0.50,0.01,NULL,.format="%.0f%%",.scale=100),
    N(LOOK_DEADZONE,"Look deadzone","Controls","Ignore small look-stick deflections. Lower values are more responsive but may reveal stick drift.","0.08",0,0.50,0.01,NULL,.format="%.0f%%",.scale=100),
    N(STICK_OUTER_DEADZONE,"Outer stick deadzone","Controls","Treat the outer part of the stick as full deflection, helping worn sticks reach the full range.","0.02",0,0.20,0.01,NULL,.format="%.0f%%",.scale=100),
    N(LOOK_EXPO,"Look response curve","Controls","0 is linear. Higher values provide finer aiming near the center while retaining the same maximum turn rate.","0.60",0,1,0.05,NULL),
    N(CAMERA_LAG,"Camera smoothing","Controls","Game preserves the original behavior. 0 follows immediately; larger values follow more slowly. Ignored in Classic controls.","game",0,0.99,0.05,"game"),
    C(RENDER,"Renderer","Advanced","Automatic selects OpenGL for enhanced resolution/aspect, otherwise software. Software is the reference renderer. Null is for diagnostics and is not offered in the launcher.","auto","auto|software|gl|null","Automatic|Software|OpenGL|Null (diagnostic)"),
    N(AUDIO_LEAD_MS,"Audio buffer lead","Advanced","Milliseconds queued ahead. Auto uses two of the channel's buffers. Smaller buffers can reduce latency but may underrun.","auto",0,4000,5,"auto"),
    N(AUDIO_PREROLL_MS,"Audio preroll","Advanced","Milliseconds buffered before playback starts. Auto preserves the 4096-frame default (about 93 ms).","auto",0,4000,5,"auto"),
    C(MPEG_DECODE,"Intro movie decoding","Advanced","Decode the intro movie. Requires a build with OpenH264. Play presets enable this when supported.","0","0|1","Off|On"),
    C(WINDOW,"Window","Launch","A window also enables real-time pacing. OpenGL always requires a window.","0","0|1","Off|On"),
    C(REALTIME,"Real-time pacing","Launch","Headless real-time pacing. Windowed play always uses real time.","0","0|1","Off|On"),
    C(HIGH_FPS,"Higher FPS","Graphics","Smooth mission rendering between the game's original simulation ticks. Menus and movies keep their original timing.","0","0|1","Off|On"),
    S(FPS_CAP,"FPS cap","Graphics","Maximum mission rendering rate when Higher FPS is on. Enter a custom limit, or unlimited to follow the display's refresh with vertical sync. The game keeps its original simulation speed.",PSP_OPTION_INTEGER,"60",30,1000,1,"unlimited",.special_label="Unlimited",.format="%.0f FPS"),
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

static const psp_preset_def presets[] = {
    {"Classic", "WINDOW=1 RENDER=gl MPEG_DECODE=1"},
    {"Controller", "WINDOW=1 RENDER=gl MPEG_DECODE=1 INPUT=dual RESOLUTION=window"},
    {"Mouse & Keyboard", "WINDOW=1 RENDER=gl MPEG_DECODE=1 INPUT=dual RESOLUTION=window KEYS=wasd MOUSE=1"},
};

/* The schema psprecomp's settings calls read (psprecomp/host/settings.h). */
const psp_settings_schema psp_title_settings = {
    .title = "Armored Core",
    .options = lr_options, .count = LR_OPTION_COUNT,
    .presets = presets, .preset_count = 3, .preset_selected = 1,
    .gl_error = "Match window resolution/aspect requires OpenGL; choose Automatic or OpenGL",
    .resolve = resolve, .print_notes = print_notes,
};
