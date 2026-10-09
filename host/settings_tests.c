#include "settings.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char error[PSP_SETTINGS_ERROR];
static void putfile(const char *path, const char *text) {
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static int contains(const char *path, const char *text) {
    FILE *f=fopen(path,"r"); assert(f);
    static char buf[16384]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    return strstr(buf,text)!=NULL;
}
int main(void) {
    assert(psp_settings_count()==LR_OPTION_COUNT && psp_settings_find("INPUT")==LR_INPUT);
    for (int i=0;i<LR_OPTION_COUNT;i++) unsetenv(psp_settings_option(i)->env);
    psp_settings s; assert(!psp_settings_load(&s,NULL,error));
    assert(s.render==1 && !s.window && !s.gamepad);
    assert(s.width==960 && s.height==544 && s.number[LR_CAMERA_LAG]==-1);
    assert(!strcmp(s.value[LR_WINDOW_MODE],"windowed"));
    assert(!strcmp(s.value[LR_DISPLAY],"primary"));
    assert(!s.number[LR_HIGH_FPS] && s.number[LR_FPS_CAP]==60);
    const char *bad_caps[]={"0","29","1001","59.94","nan","inf","60junk"};
    for (size_t i=0;i<sizeof bad_caps/sizeof bad_caps[0];i++)
        assert(psp_settings_set(&s,LR_FPS_CAP,bad_caps[i],PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_FPS_CAP,"unlimited",PSP_SOURCE_FILE,error));
    assert(s.number[LR_FPS_CAP]==-1);
    assert(!psp_settings_set(&s,LR_FPS_CAP,"165",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_DISPLAY,"1.5",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_DISPLAY,"0",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_DISPLAY,"65536",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_WINDOW_MODE,"borderless",PSP_SOURCE_FILE,error));
    assert(!psp_settings_resolve(&s,error));
    assert(s.window && s.realtime && s.render==1);
    assert(!s.number[LR_RESOLUTION] && !s.number[LR_ASPECT]);
    assert(psp_settings_set(&s,LR_WINDOW_MODE,"typo",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_WINDOW_MODE,"windowed",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_INPUT,"dual",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_RESOLUTION,"window",PSP_SOURCE_FILE,error));
    assert(!psp_settings_resolve(&s,error)); assert(s.render==2 && s.window && s.gamepad);
    assert(!psp_settings_set(&s,LR_RENDER,"software",PSP_SOURCE_FILE,error));
    assert(psp_settings_resolve(&s,error));
    const char *bad[]={"nan","inf","-inf","1junk","0.1 ","-0.01","0.51","1e999"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++)
        assert(psp_settings_set(&s,LR_MOVE_DEADZONE,bad[i],PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_WINDOW_SIZE,"1920x1080oops",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_WINDOW_SIZE,"0x1080",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_WINDOW_SIZE,"999999999999999999999x1",PSP_SOURCE_FILE,error));
    assert(psp_settings_set(&s,LR_INPUT,"typo",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&s,LR_MOUSE,"false",PSP_SOURCE_FILE,error));
    assert(!s.number[LR_MOUSE]);

    char dir[]="/tmp/lr-settings-test-XXXXXX"; assert(mkdtemp(dir));
    char path[256]; snprintf(path,sizeof path,"%s/settings.ini",dir);
    /* A new player's settings: dual-stick controls, over the player's own
     * window and Match window rendering. */
    psp_settings play; psp_settings_play_defaults(&play);
    assert(play.number[LR_INPUT]==2 && play.number[LR_WINDOW]==1 && play.number[LR_RESOLUTION]==1);
    assert(play.render==2 && play.gamepad && play.number[LR_MPEG_DECODE]==1);
    assert(!psp_settings_set(&play,LR_HIGH_FPS,"1",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&play,LR_FPS_CAP,"144",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&play,LR_WINDOW_MODE,"borderless",PSP_SOURCE_FILE,error));
    assert(!psp_settings_set(&play,LR_DISPLAY,"2",PSP_SOURCE_FILE,error));
    psp_settings_file *f=psp_settings_file_new(); assert(f);
    assert(!psp_settings_file_put(f,&play,error) && !psp_settings_file_write(f,path,error));
    psp_settings_file_free(f);
    assert(contains(path,"[player]\nRESOLUTION=window\nWINDOW_MODE=borderless\n"));
    assert(contains(path,"\n[pack last-raven]\nASPECT=native\nHIGH_FPS=1\nFPS_CAP=144\nINPUT=dual\n"));
    setenv("PSPRECOMP_FPS_CAP","240",1);
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_HIGH_FPS]==1 && s.number[LR_FPS_CAP]==240 && s.source[LR_FPS_CAP]==PSP_SOURCE_ENV);
    unsetenv("PSPRECOMP_FPS_CAP");
    assert(!psp_settings_load(&s,path,error));
    for (int k=0;k<LR_OPTION_COUNT;k++) assert(!strcmp(s.value[k],play.value[k]));
    setenv("PSPRECOMP_WINDOW_MODE","windowed",1);
    assert(!psp_settings_load(&s,path,error));
    assert(!s.number[LR_WINDOW_MODE] && s.source[LR_WINDOW_MODE]==PSP_SOURCE_ENV);
    unsetenv("PSPRECOMP_WINDOW_MODE");
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_WINDOW_MODE]==1 && s.source[LR_WINDOW_MODE]==PSP_SOURCE_FILE);
    assert(s.number[LR_DISPLAY]==2);
    setenv("PSPRECOMP_DISPLAY","primary",1);
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_DISPLAY]==-1 && s.source[LR_DISPLAY]==PSP_SOURCE_ENV);
    unsetenv("PSPRECOMP_DISPLAY");
    /* What the in-game menu writes back keeps what the environment set out. */
    setenv("PSPRECOMP_MOUSE_SENS","2.5",1);
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_MOUSE_SENS]==2.5 && s.source[LR_MOUSE_SENS]==PSP_SOURCE_ENV);
    assert(!psp_settings_set(&s,LR_KEYS,"wasd",PSP_SOURCE_FILE,error));
    assert(!psp_settings_save_origin(&s,error));
    unsetenv("PSPRECOMP_MOUSE_SENS");
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_MOUSE_SENS]==1 && s.number[LR_KEYS]==1);
    /* Saved settings never change direct boot or replay defaults. */
    assert(!psp_settings_load(&s,NULL,error)); assert(s.number[LR_INPUT]==0 && !s.window);
    setenv("PSPRECOMP_INPUT","",1);
    assert(!psp_settings_load(&s,NULL,error)); assert(s.source[LR_INPUT]==PSP_SOURCE_DEFAULT);
    setenv("PSPRECOMP_INPUT","invalid",1);
    psp_settings previous=s; assert(psp_settings_env(&s,error)); assert(!memcmp(&previous,&s,sizeof s));
    unsetenv("PSPRECOMP_INPUT");

    /* The presets this pack's own launcher wrote: the selected one is the
     * settings, split between [player] and [pack last-raven]; the others
     * stay in the file. */
    putfile(path,"version=1\nselected=Controller\ngame=aclr\n"
                 "[preset Classic]\nWINDOW=1\nRENDER=gl\nMPEG_DECODE=1\n"
                 "[preset Controller]\nWINDOW=1\nRENDER=gl\nMPEG_DECODE=1\nINPUT=dual\nRESOLUTION=window\nbind.pad.boost=lefttrigger\n"
                 "[preset Mouse & Keyboard]\nWINDOW=1\nRENDER=gl\nINPUT=dual\nKEYS=wasd\nMOUSE=1\n");
    assert(!psp_settings_load(&s,path,error));
    assert(s.number[LR_INPUT]==2 && s.number[LR_RESOLUTION]==1 && s.render==2 && s.bind_count==1);
    assert(!psp_settings_save_origin(&s,error));
    assert(contains(path,"version=2\ngame=aclr\n\n[player]\nWINDOW=1\nRENDER=gl\nMPEG_DECODE=1\nRESOLUTION=window\n"));
    assert(contains(path,"[pack last-raven]\nINPUT=dual\nASPECT=native\n") && contains(path,"bind.pad.boost=lefttrigger\n"));
    assert(contains(path,"\n[preset Classic]\nWINDOW=1\n") && contains(path,"\n[preset Mouse & Keyboard]\nWINDOW=1\n"));
    assert(!contains(path,"[preset Controller]"));
    /* Presets saved before window mode existed keep their windowed behavior. */
    putfile(path,"version=1\nselected=Old\n[preset Old]\nWINDOW_SIZE=1280x720\n");
    assert(!psp_settings_load(&s,path,error));
    assert(!strcmp(s.value[LR_WINDOW_MODE],"windowed") && s.width==1280 && s.height==720);
    assert(!s.number[LR_HIGH_FPS] && s.number[LR_FPS_CAP]==60);
    assert(!strcmp(s.value[LR_DISPLAY],"primary"));
    const char *invalid[]={
        "version=2\n[pack last-raven]\nINPUT=dual\nINPUT=classic\n",
        "version=2\n[pack last-raven]\nCAPTURE=oops\n",
        "version=2\n[pack last-raven]\nLOOK_EXPO=NaN\n",
        "version=2\n[pack last-raven]\nRENDER=gl\n",
        "version=2\n[player]\nINPUT=dual\n",
        "version=1\nselected=Missing\n[preset A]\n",
    };
    for (size_t i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        putfile(path,invalid[i]); psp_settings before=s;
        assert(psp_settings_load(&s,path,error)); assert(!memcmp(&before,&s,sizeof s));
    }
    unlink(path); rmdir(dir);
    puts("settings: defaults, validation, precedence, the file's sections, earlier presets and replay isolation passed");
    return 0;
}
