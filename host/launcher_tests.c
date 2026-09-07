/* Exercise the actual event handlers and render the real screen. This fixture
 * uses a temporary preferences directory and a harmless child in place of a
 * game, so Save & Play can be verified without guest data or user settings. */
#define main launcher_entry
#include "launcher.c"
#undef main
#include <assert.h>
#include <pthread.h>
#include <sys/stat.h>

static void *close_startup(void *unused) {
    (void)unused;
    SDL_Delay(500);
    SDL_Event e={0}; e.type=SDL_QUIT;
    assert(SDL_PushEvent(&e)==1);
    return NULL;
}

static void press(launcher *a,SDL_Keycode sym) {
    SDL_Event e={0}; e.type=SDL_KEYDOWN; e.key.keysym.sym=sym; event(a,&e);
}
static void pad_press(launcher *a,Uint8 button) {
    SDL_Event e={0}; e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.button=button; event(a,&e);
}
static void click(launcher *a,int x,int y) {
    SDL_Event e={0}; e.type=SDL_MOUSEBUTTONDOWN; e.button.button=SDL_BUTTON_LEFT;
    e.button.x=x; e.button.y=y; event(a,&e);
}
static void type(launcher *a,const char *s) {
    SDL_Event e={0}; e.type=SDL_TEXTINPUT; snprintf(e.text.text,sizeof e.text.text,"%s",s); event(a,&e);
}
static void shot(launcher *a,const char *dir,const char *name) {
    draw(a);
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,UI_W,UI_H,32,SDL_PIXELFORMAT_ARGB8888);
    assert(surface); assert(!SDL_RenderReadPixels(a->renderer,NULL,surface->format->format,surface->pixels,surface->pitch));
    char path[4096]; snprintf(path,sizeof path,"%s/%s.bmp",dir,name);
    assert(!SDL_SaveBMP(surface,path)); SDL_FreeSurface(surface); SDL_RenderPresent(a->renderer);
}
int main(int argc,char **argv) {
    assert(argc==2);
    for (int k=0;k<LR_OPTION_COUNT;k++) unsetenv(lr_options[k].env);
    launcher a={0}; a.running=1; a.focus=a.selected_row=LR_RESOLUTION; a.movie_available=1;
    snprintf(a.path,sizeof a.path,"%s/presets with spaces.ini",argv[1]);
    assert(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)); assert(!TTF_Init());
    a.window=SDL_CreateWindow("Settings UI checks",0,0,UI_W,UI_H,SDL_WINDOW_HIDDEN);
    assert(a.window); a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_SOFTWARE); assert(a.renderer);
    SDL_RenderSetLogicalSize(a.renderer,UI_W,UI_H); assert(!fonts(&a,NULL));
    lr_presets_defaults(&a.book); shot(&a,argv[1],"graphics");
    /* Mouse navigation and adjustment. */
    click(&a,500,166); assert(a.page==1); draw(&a);
    a.focus=LR_MOUSE_SENS; press(&a,SDLK_RIGHT);
    assert(fabs(editing(&a)->number[LR_MOUSE_SENS]-1.1)<1e-6);
    shot(&a,argv[1],"controls");
    /* Controller navigation scrolls to rows that were initially offscreen. */
    a.focus=LR_LOOK_DEADZONE; pad_press(&a,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    assert(a.focus==LR_STICK_OUTER_DEADZONE && a.scroll>0);
    pad_press(&a,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); assert(a.page==2);
    shot(&a,argv[1],"advanced");
    /* Override rows are locked; even Reset never writes the override value. */
    setenv("PSPRECOMP_MOUSE_SENS","3",1); refresh(&a);
    double saved=editing(&a)->number[LR_MOUSE_SENS];
    adjust(&a,LR_MOUSE_SENS,1); assert(editing(&a)->number[LR_MOUSE_SENS]==saved);
    assert(a.effective.number[LR_MOUSE_SENS]==3);
    activate(&a,PAGE_BASE+1); a.focus=a.selected_row=LR_MOUSE_SENS;
    shot(&a,argv[1],"override");
    assert(!save(&a)); lr_presets disk; char error[LR_ERROR_SIZE];
    assert(!lr_presets_load(&disk,a.path,error));
    assert(disk.presets[disk.selected].settings.number[LR_MOUSE_SENS]==saved);
    unsetenv("PSPRECOMP_MOUSE_SENS");
    activate(&a,DUPLICATE); type(&a,"My mouse setup"); press(&a,SDLK_RETURN);
    assert(a.book.count==4 && !strcmp(a.book.presets[3].name,"My mouse setup"));
    activate(&a,RENAME); type(&a,"Desk & controller"); press(&a,SDLK_RETURN);
    assert(!strcmp(a.book.presets[3].name,"Desk & controller"));
    /* Escape cancels text edits. */
    activate(&a,RENAME); type(&a,"Do not save"); press(&a,SDLK_ESCAPE);
    assert(!strcmp(a.book.presets[3].name,"Desk & controller"));
    activate(&a,NEW); pad_press(&a,SDL_CONTROLLER_BUTTON_A); assert(a.book.count==5);
    activate(&a,DELETE); pad_press(&a,SDL_CONTROLLER_BUTTON_A); assert(a.book.count==4);
    activate(&a,RESET); press(&a,SDLK_ESCAPE); assert(editing(&a)->number[LR_INPUT]==2);
    activate(&a,RESET); press(&a,SDLK_RETURN); assert(editing(&a)->number[LR_INPUT]==0);
    /* Invalid input keeps the editor open and preserves the saved value. */
    activate(&a,LR_MOUSE_SENS); type(&a,"nan"); press(&a,SDLK_RETURN);
    assert(a.modal==MODAL_VALUE && *a.modal_error); shot(&a,argv[1],"invalid-value");
    press(&a,SDLK_ESCAPE);
    a.movie_available=0;
    lr_settings_set(editing(&a),LR_MPEG_DECODE,"1",LR_PRESET,error);
    adjust(&a,LR_MPEG_DECODE,1); assert(!editing(&a)->number[LR_MPEG_DECODE]);
    adjust(&a,LR_MPEG_DECODE,1); assert(!editing(&a)->number[LR_MPEG_DECODE]);
    a.movie_available=1;
    /* Verify a launch receives each argument intact, including spaces and '&'. */
    char child[4096],module[4096],arguments[4096];
    snprintf(child,sizeof child,"%s/fake boot",argv[1]);
    snprintf(module,sizeof module,"%s/game dump.elf",argv[1]);
    snprintf(arguments,sizeof arguments,"%s/arguments.txt",argv[1]);
    FILE *f=fopen(child,"w"); assert(f);
    fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LR_LAUNCH_TEST_ARGS\"\n",f); fclose(f); assert(!chmod(child,0700));
    f=fopen(module,"w"); assert(f); fclose(f);
    setenv("LR_LAUNCH_TEST_ARGS",arguments,1);
    a.boot=child; a.module=module;
    activate(&a,PLAY); assert(a.child>0);
    for (int i=0;i<200 && a.child;i++) { SDL_Delay(5); poll_child(&a); }
    assert(!a.child && !a.running);
    char contents[12000]={0}; f=fopen(arguments,"r"); assert(f);
    fread(contents,1,sizeof contents-1,f); fclose(f);
    assert(strstr(contents,module) && strstr(contents,a.path));
    assert(strstr(contents,"--preset\nDesk & controller\n--window\n"));
    assert(!lr_presets_load(&disk,a.path,error));
    assert(!strcmp(disk.presets[disk.selected].name,"Desk & controller"));
    /* Cancel with edits asks once; rejecting the dialog keeps edits intact. */
    a.running=1; a.dirty=1; activate(&a,CANCEL); assert(a.modal==MODAL_CANCEL_DIRTY);
    press(&a,SDLK_ESCAPE); assert(a.running && a.dirty);
    activate(&a,CANCEL); press(&a,SDLK_RETURN); assert(!a.running);
    unlink(child); unlink(module); unlink(arguments);
    TTF_CloseFont(a.body); TTF_CloseFont(a.small); TTF_CloseFont(a.heading);
    SDL_DestroyRenderer(a.renderer); SDL_DestroyWindow(a.window); TTF_Quit(); SDL_Quit();
    /* Also exercise the real CLI entry point, loading the file we just saved.
     * The event thread closes it normally, without touching user preferences. */
    pthread_t closer; assert(!pthread_create(&closer,NULL,close_startup,NULL));
    char *startup[]={"launcher","--config",a.path,NULL};
    assert(!launcher_entry(3,startup)); assert(!pthread_join(closer,NULL));
    unlink(a.path);
    puts("launcher: mouse, keyboard, controller, presets, overrides, cancel, validation and launch handoff passed");
    return 0;
}
