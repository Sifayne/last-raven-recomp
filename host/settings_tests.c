#include "settings.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char error[LR_ERROR_SIZE];
static void putfile(const char *path, const char *text) {
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
int main(void) {
    for (int i=0;i<LR_OPTION_COUNT;i++) unsetenv(lr_options[i].env);
    lr_settings s; assert(!lr_settings_load(&s,NULL,NULL,error));
    assert(s.render==1 && !s.window && !s.gamepad);
    assert(s.width==960 && s.height==544 && s.number[LR_CAMERA_LAG]==-1);
    assert(!strcmp(s.value[LR_WINDOW_MODE],"windowed"));
    assert(!strcmp(s.value[LR_DISPLAY],"primary"));
    assert(!s.number[LR_HIGH_FPS] && s.number[LR_FPS_CAP]==60);
    const char *bad_caps[]={"0","29","1001","59.94","nan","inf","60junk"};
    for (size_t i=0;i<sizeof bad_caps/sizeof bad_caps[0];i++)
        assert(lr_settings_set(&s,LR_FPS_CAP,bad_caps[i],LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_FPS_CAP,"unlimited",LR_PRESET,error));
    assert(s.number[LR_FPS_CAP]==-1);
    assert(!lr_settings_set(&s,LR_FPS_CAP,"165",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_DISPLAY,"1.5",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_DISPLAY,"0",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_DISPLAY,"65536",LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_WINDOW_MODE,"borderless",LR_PRESET,error));
    assert(!lr_settings_resolve(&s,error));
    assert(s.window && s.realtime && s.render==1);
    assert(!s.number[LR_RESOLUTION] && !s.number[LR_ASPECT]);
    assert(lr_settings_set(&s,LR_WINDOW_MODE,"typo",LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_WINDOW_MODE,"windowed",LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_INPUT,"dual",LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_RESOLUTION,"window",LR_PRESET,error));
    assert(!lr_settings_resolve(&s,error)); assert(s.render==2 && s.window && s.gamepad);
    assert(!lr_settings_set(&s,LR_RENDER,"software",LR_PRESET,error));
    assert(lr_settings_resolve(&s,error));
    const char *bad[]={"nan","inf","-inf","1junk","0.1 ","-0.01","0.51","1e999"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++)
        assert(lr_settings_set(&s,LR_MOVE_DEADZONE,bad[i],LR_PRESET,error));
    assert(lr_settings_set(&s,LR_WINDOW_SIZE,"1920x1080oops",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_WINDOW_SIZE,"0x1080",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_WINDOW_SIZE,"999999999999999999999x1",LR_PRESET,error));
    assert(lr_settings_set(&s,LR_INPUT,"typo",LR_PRESET,error));
    assert(!lr_settings_set(&s,LR_MOUSE,"false",LR_PRESET,error));
    assert(!s.number[LR_MOUSE]);

    char dir[]="/tmp/lr-settings-test-XXXXXX"; assert(mkdtemp(dir));
    char path[256]; snprintf(path,sizeof path,"%s/settings.ini",dir);
    lr_presets p, loaded; lr_presets_defaults(&p);
    assert(!lr_settings_set(&p.presets[1].settings,LR_HIGH_FPS,"1",LR_PRESET,error));
    assert(!lr_settings_set(&p.presets[1].settings,LR_FPS_CAP,"144",LR_PRESET,error));
    assert(!lr_settings_set(&p.presets[1].settings,LR_WINDOW_MODE,"borderless",LR_PRESET,error));
    assert(!lr_settings_set(&p.presets[1].settings,LR_DISPLAY,"2",LR_PRESET,error));
    assert(!lr_presets_save(&p,path,error)); assert(!lr_presets_load(&loaded,path,error));
    assert(loaded.count==3 && loaded.selected==1);
    setenv("PSPRECOMP_FPS_CAP","240",1);
    assert(!lr_settings_load(&s,path,"Controller",error));
    assert(s.number[LR_HIGH_FPS]==1 && s.number[LR_FPS_CAP]==240 && s.source[LR_FPS_CAP]==LR_ENV);
    unsetenv("PSPRECOMP_FPS_CAP");
    for (int i=0;i<3;i++) for (int k=0;k<LR_OPTION_COUNT;k++)
        assert(!strcmp(p.presets[i].settings.value[k],loaded.presets[i].settings.value[k]));
    setenv("PSPRECOMP_WINDOW_MODE","windowed",1);
    assert(!lr_settings_load(&s,path,"Controller",error));
    assert(!s.number[LR_WINDOW_MODE] && s.source[LR_WINDOW_MODE]==LR_ENV);
    unsetenv("PSPRECOMP_WINDOW_MODE");
    assert(!lr_settings_load(&s,path,"Controller",error));
    assert(s.number[LR_WINDOW_MODE]==1 && s.source[LR_WINDOW_MODE]==LR_PRESET);
    assert(s.number[LR_DISPLAY]==2);
    setenv("PSPRECOMP_DISPLAY","primary",1);
    assert(!lr_settings_load(&s,path,"Controller",error));
    assert(s.number[LR_DISPLAY]==-1 && s.source[LR_DISPLAY]==LR_ENV);
    unsetenv("PSPRECOMP_DISPLAY");
    assert(!lr_settings_load(&s,path,"Mouse & Keyboard",error));
    assert(s.number[LR_INPUT]==2 && s.number[LR_MOUSE]==1 && s.number[LR_KEYS]==1);
    assert(lr_settings_load(&s,path,"Missing",error));
    assert(lr_settings_load(&s,NULL,"Controller",error));
    setenv("PSPRECOMP_MOUSE_SENS","2.5",1);
    assert(!lr_settings_load(&s,path,"Mouse & Keyboard",error));
    assert(s.number[LR_MOUSE_SENS]==2.5 && s.source[LR_MOUSE_SENS]==LR_ENV);
    assert(!lr_presets_save(&loaded,path,error));
    unsetenv("PSPRECOMP_MOUSE_SENS");
    assert(!lr_settings_load(&s,path,"Mouse & Keyboard",error));
    assert(s.number[LR_MOUSE_SENS]==1);
    /* A saved Controller preset never changes direct boot or replay defaults. */
    assert(!lr_settings_load(&s,NULL,NULL,error)); assert(s.number[LR_INPUT]==0 && !s.window);
    setenv("PSPRECOMP_INPUT","",1);
    assert(!lr_settings_load(&s,NULL,NULL,error)); assert(s.source[LR_INPUT]==LR_DEFAULT);
    setenv("PSPRECOMP_INPUT","invalid",1);
    lr_settings previous=s; assert(lr_settings_env(&s,error)); assert(!memcmp(&previous,&s,sizeof s));
    unsetenv("PSPRECOMP_INPUT");

    assert(!lr_presets_add(&p,"My setup",&p.presets[1].settings,error));
    assert(lr_presets_add(&p,"My setup",&s,error));
    assert(lr_presets_add(&p,"broken\nname",&s,error));
    assert(!lr_presets_save(&p,path,error));
    assert(lr_presets_save(&p,"/no-such-directory/settings.ini",error));
    assert(!lr_presets_load(&loaded,path,error)); assert(loaded.count==4);
    /* A validation failure must leave the previous file intact. */
    strcpy(p.presets[0].settings.value[LR_WINDOW_SIZE],"broken");
    assert(lr_presets_save(&p,path,error));
    assert(!lr_presets_load(&loaded,path,error)); assert(loaded.count==4);
    /* Presets saved before window mode existed keep their windowed behavior. */
    putfile(path,"version=1\nselected=Old\n[preset Old]\nWINDOW_SIZE=1280x720\n");
    assert(!lr_settings_load(&s,path,"Old",error));
    assert(!strcmp(s.value[LR_WINDOW_MODE],"windowed") && s.width==1280 && s.height==720);
    assert(!s.number[LR_HIGH_FPS] && s.number[LR_FPS_CAP]==60);
    assert(!strcmp(s.value[LR_DISPLAY],"primary"));
    const char *invalid[]={
        "version=2\nselected=A\n[preset A]\n",
        "version=1\nselected=Missing\n[preset A]\n",
        "version=1\nselected=A\n[preset A]\nINPUT=dual\nINPUT=classic\n",
        "version=1\nselected=A\n[preset A]\n[preset A]\n",
        "version=1\nselected=A\n[preset A]\nCAPTURE=oops\n",
        "version=1\nselected=A\n[preset A]\nLOOK_EXPO=NaN\n"
    };
    for (size_t i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        putfile(path,invalid[i]); lr_presets before=loaded;
        assert(lr_presets_load(&loaded,path,error)); assert(!memcmp(&before,&loaded,sizeof loaded));
    }
    unlink(path); rmdir(dir);
    puts("settings: defaults, validation, precedence, preset round trips, atomic failure and replay isolation passed");
    return 0;
}
