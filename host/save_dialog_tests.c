/* Exercise the real SDL event loop and GL owner handoff on a disposable card. */
#include "present.h"
#include "render_gl.h"
#include "save_dialog.h"
#include "settings.h"
#include "psprecomp/hle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint32_t param=0x08810000, names=0x08811000, data=0x08812000;
static psp_savedata_view view;
static uint32_t pixels[SAVE_DIALOG_W*SAVE_DIALOG_H];
static uint64_t revision;
static const psp_render_backend *backend;
static char root[]="/tmp/last-raven-savedata-ui-XXXXXX";
static uint32_t call(uint32_t nid,uint32_t arg) {
    psp_cpu.r[PSP_REG_A0]=arg; psp_hle_call(nid); return psp_cpu.r[PSP_REG_V0];
}
static void string(uint32_t addr,const char *s) {
    do { psp_write8(addr++,(unsigned char)*s); } while (*s++);
}
static void request(int mode,int circle) {
    memset(psp_mem_ptr(param,1536),0,1536);
    psp_write32(param,1536); psp_write32(param+8,circle?0:1); psp_write32(param+48,mode);
    string(param+60,"UITEST002"); string(param+76,"SLOT00"); string(param+100,"DATA.BIN");
    psp_write32(param+96,names);
    for (int i=0;i<6;i++) { char n[20]; snprintf(n,sizeof n,"SLOT%02d",i); string(names+i*20,n); }
    psp_write8(names+120,0);
    string(param+128,"Savedata fixture");
    string(param+256,"Save Data 01");
    string(param+384,"Player: Test player\nPlay time: 12:34:56\nProgress: 30％");
    string(data,"independent progress"); psp_write32(param+116,data);
    psp_write32(param+120,128); psp_write32(param+124,21);
    assert(call(0x50C4CD57,param)==0);
    assert(call(0x8874DBE0,0)==1); assert(call(0x8874DBE0,0)==2);
    psp_savedata_snapshot(&view);
}
static void tick(void) {
    assert(call(0xD4B95FFB,1)==0); psp_savedata_snapshot(&view); SDL_Delay(25);
}
static void settle(void) { for (int i=0;i<8;i++) tick(); }
static void key(SDL_Keycode k) {
    SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_KEYDOWN; e.key.state=SDL_PRESSED;
    e.key.keysym.sym=k; e.key.keysym.scancode=SDL_GetScancodeFromKey(k);
    assert(SDL_PushEvent(&e)==1);
    e.type=SDL_KEYUP; e.key.state=SDL_RELEASED; assert(SDL_PushEvent(&e)==1);
    settle();
}
static void finish(void) {
    assert(call(0x8874DBE0,0)==3); assert(call(0x9790B33C,0)==0);
    assert(call(0x8874DBE0,0)==4); assert(call(0x8874DBE0,0)==0);
    SDL_Delay(100);
}
static void capture(const char *dir,const char *name) {
    assert(save_dialog_copy_pixels(pixels,&revision));
    SDL_Surface *s=SDL_CreateRGBSurfaceWithFormatFrom(pixels,SAVE_DIALOG_W,SAVE_DIALOG_H,
        32,SAVE_DIALOG_W*4,SDL_PIXELFORMAT_RGBA32); assert(s);
    char path[1024]; snprintf(path,sizeof path,"%s/%s.bmp",dir,name);
    assert(!SDL_SaveBMP(s,path)); SDL_FreeSurface(s);
}
static void scene(uint32_t color) {
    psp_blend_state blend={.write_colour=1,.write_alpha=1};
    psp_tex_state tex={0};
    backend->set_target(0x04000000,512,3); backend->set_scissor(0,0,479,271);
    backend->set_texture(&tex); backend->set_depth(0,1,0); backend->set_blend(&blend);
    psp_vertex v[2]={{.rgba=color,.inv_w=1,.tex_q=1,.fog=255},
        {.x=480*16,.y=272*16,.rgba=color,.inv_w=1,.tex_q=1,.fog=255}};
    backend->draw(PSP_PRIM_SPRITES,v,2); backend->present();
}
static void integration(const char *renderer,const char *out) {
    lr_settings settings; char error[LR_ERROR_SIZE]; lr_settings_defaults(&settings);
    assert(!lr_settings_resolve(&settings,error)); lr_settings_use(&settings);
    int gl=!strcmp(renderer,"gl");
    if (gl) {
        backend=render_gl_backend(); assert(!psp_render_register(backend));
        assert(!psp_render_select("gl")); present_want_gl();
    }
    assert(!present_start());
    if (gl) { assert(!backend->init(480,272)); scene(0xFF453120); }
    request(5,0); settle(); assert(view.active && view.count==6 && view.selected==0);
    capture(out,"empty-slots");
    for (int i=0;i<5;i++) key(SDLK_DOWN);
    assert(view.selected==5); capture(out,"scrolled-slots");
    key(SDLK_RETURN); assert(view.stage==PSP_SAVEDATA_DONE && view.result==0);
    capture(out,"saved"); key(SDLK_RETURN); assert(!view.active); finish();
    request(5,0); settle();
    for (int i=0;i<5;i++) key(SDLK_DOWN);
    key(SDLK_RETURN); assert(view.stage==PSP_SAVEDATA_CONFIRM); capture(out,"overwrite");
    key(SDLK_ESCAPE); assert(view.stage==PSP_SAVEDATA_LIST);
    key(SDLK_ESCAPE); assert(!view.active && view.result==1); finish();
    request(4,1); settle(); assert(view.count==1); capture(out,"load-slots");
    key(SDLK_z); assert(!view.active && view.result==1); finish(); /* Cross now cancels. */
    request(4,1); settle(); memset(psp_mem_ptr(data,128),0,128);
    key(SDLK_x); assert(view.stage==PSP_SAVEDATA_DONE && !view.result);
    assert(!strcmp(psp_mem_ptr(data,128),"independent progress"));
    key(SDLK_RETURN); finish();
    if (gl) {
        scene(0xFF1267AB); /* GL state must survive the overlay. */
        int w,h; unsigned char *p=render_gl_capture(0x04000000,&w,&h); assert(p);
        uint32_t value; memcpy(&value,p+((size_t)20*w+20)*4,4);
        assert(value==0xFF1267AB); free(p);
    }
    /* Modal keyboard confirmation must never become the game's START bit. */
    psp_cpu.r[PSP_REG_A1]=1; assert(call(0x3A622550,0x08818000)==1);
    assert(psp_read32(0x08818004)==0);
    /* Closing the window while a save list is open cancels the request and
     * writes nothing: no stuck modal, no implicit save. */
    request(5,0); settle(); assert(view.active && view.stage==PSP_SAVEDATA_LIST);
    SDL_Event quit; memset(&quit,0,sizeof quit); quit.type=SDL_QUIT; assert(SDL_PushEvent(&quit)==1); SDL_Delay(200);
    settle(); assert(!view.active && psp_read32(param+28)==1); finish();
    char slot0[512]; snprintf(slot0,sizeof slot0,"%s/ms/PSP/SAVEDATA/UITEST002SLOT00/DATA.BIN",root);
    assert(!fopen(slot0,"rb"));
}
/* No usable font: the dialog reports why and stays out, and the runtime then
 * cancels interactive requests instead of saving without confirmation. */
static void nofont(void) {
    setenv("PSPRECOMP_UI_FONT","/nonexistent/no-such-font.ttf",1);
    assert(!SDL_Init(SDL_INIT_VIDEO));
    assert(save_dialog_init()!=0 && save_dialog_error());
    request(5,0); for (int i=0;i<4;i++) tick();
    assert(!view.active && psp_read32(param+28)==1); finish();
    char slot0[512]; snprintf(slot0,sizeof slot0,"%s/ms/PSP/SAVEDATA/UITEST002SLOT00/DATA.BIN",root);
    assert(!fopen(slot0,"rb"));
    save_dialog_shutdown(); SDL_Quit();
}
/* Same UI with a virtual gamepad verifies opening-button suppression, physical
 * A/B semantics, focus changes, and stick repeat without game-specific maps. */
static void controller(const char *out) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    assert(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER));
    SDL_Window *win=SDL_CreateWindow("Savedata input fixture",0,0,960,544,0); assert(win);
    assert(!save_dialog_init());
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0); assert(index>=0);
    SDL_GameController *pad=SDL_GameControllerOpen(index); assert(pad);
    SDL_PumpEvents();
    SDL_Joystick *joy=SDL_GameControllerGetJoystick(pad); SDL_JoystickID id=SDL_JoystickInstanceID(joy);
    assert(!SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768));
    assert(!SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768));
    assert(!SDL_JoystickSetVirtualButton(joy,SDL_CONTROLLER_BUTTON_A,1)); SDL_JoystickUpdate();
    assert(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A));
    request(5,0); save_dialog_update(pad);
    SDL_Event e; memset(&e,0,sizeof e); e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.which=id;
    e.cbutton.button=SDL_CONTROLLER_BUTTON_A; assert(save_dialog_event(&e,id)); tick();
    assert(view.stage==PSP_SAVEDATA_LIST); /* Held opening confirm cannot save. */
    assert(!SDL_JoystickSetVirtualButton(joy,SDL_CONTROLLER_BUTTON_A,0)); SDL_JoystickUpdate();
    save_dialog_update(pad);
    assert(!SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_LEFTY,24000)); SDL_JoystickUpdate();
    save_dialog_update(pad); tick(); assert(view.selected==1);
    /* The held stick repeats on the dialog's own clock (350 ms, then 110). */
    for (int i=0;i<40 && view.selected!=2;i++) { SDL_Delay(25); save_dialog_update(pad); tick(); }
    assert(view.selected==2);
    SDL_JoystickSetVirtualAxis(joy,SDL_CONTROLLER_AXIS_LEFTY,0); SDL_JoystickUpdate(); save_dialog_update(pad);
    e.type=SDL_WINDOWEVENT; e.window.event=SDL_WINDOWEVENT_FOCUS_LOST; save_dialog_event(&e,id);
    e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.which=id; e.cbutton.button=SDL_CONTROLLER_BUTTON_A;
    save_dialog_event(&e,id); tick(); assert(view.stage==PSP_SAVEDATA_LIST);
    e.type=SDL_WINDOWEVENT; e.window.event=SDL_WINDOWEVENT_FOCUS_GAINED; save_dialog_event(&e,id);
    save_dialog_update(pad);
    e.type=SDL_CONTROLLERBUTTONDOWN; e.cbutton.which=id; e.cbutton.button=SDL_CONTROLLER_BUTTON_A;
    save_dialog_event(&e,id); tick(); assert(view.stage==PSP_SAVEDATA_DONE);
    save_dialog_update(pad); capture(out,"controller-saved");
    save_dialog_event(&e,id); tick(); finish(); save_dialog_update(pad);
    SDL_GameControllerClose(pad); SDL_JoystickDetachVirtual(index);
    save_dialog_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
}
int main(int argc,char **argv) {
    assert(argc==3);
    assert(mkdtemp(root));
    assert(!psp_mem_init()); psp_cpu_reset(); psp_hle_init(); psp_io_set_root(root);
    if (!strcmp(argv[1],"controller")) controller(argv[2]);
    else if (!strcmp(argv[1],"nofont")) nofont();
    else integration(argv[1],argv[2]);
    assert(!psp_mem_bad_access);
    printf("savedata UI: %s passed, disposable card %s\n",argv[1],root);
    return 0;
}
