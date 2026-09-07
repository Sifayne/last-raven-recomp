#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C(key,label,page,help,dflt,choices,labels) \
    {#key,"PSPRECOMP_" #key,label,page,help,LR_CHOICE,dflt,choices,labels,0,0,0,NULL}
#define N(key,label,page,help,dflt,min,max,step,special) \
    {#key,"PSPRECOMP_" #key,label,page,help,LR_NUMBER,dflt,NULL,NULL,min,max,step,special}
const lr_option_def lr_options[LR_OPTION_COUNT] = {
    C(RESOLUTION,"Rendering resolution","Graphics","Original preserves the PSP resolution. Match window renders at the window's physical pixel size and requires OpenGL.","psp","psp|window","Original (480x272)|Match window"),
    C(ASPECT,"Aspect ratio","Graphics","Original keeps the PSP-shaped view. Match window expands the 3D view on wider windows; the HUD remains centered. Requires OpenGL.","native","native|window","Original|Match window"),
    {"WINDOW_SIZE","PSPRECOMP_WINDOW_SIZE","Window size","Graphics","Starting size in logical pixels, WIDTHxHEIGHT, in Windowed mode. Windowed fullscreen uses the desktop size instead.",LR_SIZE,"960x544",NULL,NULL,1,16384,0,NULL},
    C(WINDOW_MODE,"Window mode","Graphics","Windowed fullscreen fills the display without borders at the current desktop resolution. Rendering resolution and aspect ratio remain separate settings.","windowed","windowed|borderless","Windowed|Windowed fullscreen"),
    {"DISPLAY","PSPRECOMP_DISPLAY","Start on display","Graphics","Choose the screen for the game in either window mode. If the saved screen is unavailable, the primary display is used. Screen numbers follow the current display order.",LR_INTEGER,"primary",NULL,NULL,1,65535,1,"primary"},
    C(INPUT,"Control scheme","Controls","Classic uses the game's controls. Modern improves one-stick response. Dual separates movement from right-stick and mouse look.","classic","classic|modern|dual","Classic|Modern one-stick|Dual stick / mouse look"),
    C(GAMEPAD,"Controller layout","Controls","Modern: LT boost, RT right weapon, LB left weapon/event, RB switch, L3 extension, R3 OB/EO, A inside, B view reset, Y purge. Menu buttons remain conventional.","auto","auto|classic|modern","Follow control scheme|Classic PSP buttons|Modern action buttons"),
    C(KEYS,"Keyboard layout","Controls","WASD moves; Space boosts; left mouse fires right weapon; right/middle fires left weapon; Q switches. Enter is Start, Backspace Select. Uses the game's default key assignment.","classic","classic|wasd","Classic|WASD"),
    C(MOUSE,"Mouse capture","Controls","Captures the pointer at launch. Escape releases it; click to recapture. Mouse look requires the Dual control scheme.","0","0|1","Off|On"),
    N(MOUSE_SENS,"Mouse sensitivity","Controls","Multiplier for mouse look. 1.0 is 0.001 radians per mouse count. Requires Dual controls and mouse capture.","1",0.001,1000,0.1,NULL),
    N(MOVE_DEADZONE,"Movement deadzone","Controls","Ignore small movement-stick deflections. Entry uses an additional 3% to prevent jitter. Applies to enhanced control schemes.","0.10",0,0.50,0.01,NULL),
    N(LOOK_DEADZONE,"Look deadzone","Controls","Ignore small look-stick deflections. Lower values are more responsive but may reveal stick drift.","0.08",0,0.50,0.01,NULL),
    N(STICK_OUTER_DEADZONE,"Outer stick deadzone","Controls","Treat the outer part of the stick as full deflection, helping worn sticks reach the full range.","0.02",0,0.20,0.01,NULL),
    N(LOOK_EXPO,"Look response curve","Controls","0 is linear. Higher values provide finer aiming near the center while retaining the same maximum turn rate.","0.60",0,1,0.05,NULL),
    N(CAMERA_LAG,"Camera smoothing","Controls","Game preserves the original behavior. 0 follows immediately; larger values follow more slowly. Ignored in Classic controls.","game",0,0.99,0.05,"game"),
    C(RENDER,"Renderer","Advanced","Automatic selects OpenGL for enhanced resolution/aspect, otherwise software. Software is the reference renderer. Null is for diagnostics and is not offered in the launcher.","auto","auto|software|gl|null","Automatic|Software|OpenGL|Null (diagnostic)"),
    N(AUDIO_LEAD_MS,"Audio buffer lead","Advanced","Milliseconds queued ahead. Auto uses two of the channel's buffers. Smaller buffers can reduce latency but may underrun.","auto",0,4000,5,"auto"),
    N(AUDIO_PREROLL_MS,"Audio preroll","Advanced","Milliseconds buffered before playback starts. Auto preserves the 4096-frame default (about 93 ms).","auto",0,4000,5,"auto"),
    C(MPEG_DECODE,"Intro movie decoding","Advanced","Decode the intro movie. Requires a build with OpenH264. Play presets enable this when supported.","0","0|1","Off|On"),
    C(WINDOW,"Window","Launch","A window also enables real-time pacing. OpenGL always requires a window.","0","0|1","Off|On"),
    C(REALTIME,"Real-time pacing","Launch","Headless real-time pacing. Windowed play always uses real time.","0","0|1","Off|On"),
};
#undef C
#undef N

static int fail(char *error, const char *key, const char *message) {
    snprintf(error, LR_ERROR_SIZE, "%s: %s", key, message);
    return -1;
}

static int token(const char *list, int index, char *out, size_t size) {
    if (!list || index < 0) return 0;
    while (index-- > 0) { list = strchr(list, '|'); if (!list) return 0; list++; }
    size_t n = strcspn(list, "|");
    if (n >= size) return 0;
    memcpy(out, list, n); out[n] = 0;
    return 1;
}

int lr_settings_set(lr_settings *s, int id, const char *value,
                    enum lr_source source, char *error) {
    if (id < 0 || id >= LR_OPTION_COUNT) return fail(error,"settings","unknown option");
    const lr_option_def *d = &lr_options[id];
    if (!value || !*value || strlen(value) >= LR_VALUE_SIZE)
        return fail(error,d->key,"missing or excessively long value");
    double n = 0;
    int w = 0, h = 0;
    char canonical[LR_VALUE_SIZE];
    if (d->type == LR_CHOICE) {
        char part[LR_VALUE_SIZE]; int found = 0;
        /* Explicit conventional booleans; empty environment values are unset. */
        if (!strcmp(d->choices,"0|1")) {
            if (!strcmp(value,"true") || !strcmp(value,"on")) value = "1";
            if (!strcmp(value,"false") || !strcmp(value,"off")) value = "0";
        }
        for (int i=0; token(d->choices,i,part,sizeof part); i++) {
            if (!strcmp(part,value)) { n=i; found=1; break; }
        }
        if (!found) {
            snprintf(error,LR_ERROR_SIZE,"%s: expected %s, got '%s'",d->key,d->choices,value);
            return -1;
        }
        snprintf(canonical,sizeof canonical,"%s",value);
    } else if (d->type == LR_SIZE) {
        char *end; errno=0;
        long ww=strtol(value,&end,10);
        if (errno || end==value || *end!='x') return fail(error,d->key,"expected WIDTHxHEIGHT");
        const char *tail=end+1; long hh=strtol(tail,&end,10);
        if (errno || end==tail || *end || ww<1 || hh<1 || ww>16384 || hh>16384)
            return fail(error,d->key,"dimensions must be whole numbers from 1 to 16384");
        w=(int)ww; h=(int)hh;
        snprintf(canonical,sizeof canonical,"%dx%d",w,h);
    } else if (d->special && !strcmp(value,d->special)) {
        n=-1; snprintf(canonical,sizeof canonical,"%s",value);
    } else {
        char *end; errno=0; n=strtod(value,&end);
        if (errno || end==value || *end || !isfinite(n) || n<d->min || n>d->max) {
            snprintf(error,LR_ERROR_SIZE,"%s: expected a finite number from %g to %g%s%s",
                     d->key,d->min,d->max,d->special?", or ":"",d->special?d->special:"");
            return -1;
        }
        if (d->type==LR_INTEGER && floor(n)!=n)
            return fail(error,d->key,"expected a whole number");
        snprintf(canonical,sizeof canonical,"%.9g",n);
    }
    strcpy(s->value[id],canonical); s->number[id]=n; s->source[id]=source;
    if (d->type==LR_SIZE) { s->width=w; s->height=h; }
    return 0;
}

void lr_settings_defaults(lr_settings *s) {
    memset(s,0,sizeof *s); char error[LR_ERROR_SIZE];
    for (int i=0;i<LR_OPTION_COUNT;i++)
        if (lr_settings_set(s,i,lr_options[i].dflt,LR_DEFAULT,error)) abort();
    lr_settings_resolve(s,error);
}

int lr_settings_env(lr_settings *s, char *error) {
    lr_settings next=*s;
    for (int i=0;i<LR_OPTION_COUNT;i++) {
        const char *v=getenv(lr_options[i].env);
        if (v && *v && lr_settings_set(&next,i,v,LR_ENV,error)) return -1;
    }
    *s=next; return 0;
}

int lr_settings_resolve(lr_settings *s, char *error) {
    const int enhanced=s->number[LR_RESOLUTION] || s->number[LR_ASPECT];
    s->render=(int)s->number[LR_RENDER];
    if (!s->render) s->render=enhanced?2:1; /* software=1, gl=2, null=3 */
    if (enhanced && s->render!=2)
        return fail(error,"RENDER","Match window resolution/aspect requires OpenGL; choose Automatic or OpenGL");
    s->gamepad=s->number[LR_GAMEPAD] ? s->number[LR_GAMEPAD]==2 : s->number[LR_INPUT]!=0;
    s->window=s->number[LR_WINDOW]!=0 || s->number[LR_WINDOW_MODE]!=0 || s->render==2;
    s->realtime=s->window || s->number[LR_REALTIME]!=0;
    return 0;
}

void lr_option_label(const lr_settings *s, int id, char *out, size_t size) {
    const lr_option_def *d=&lr_options[id];
    if (d->type==LR_CHOICE && token(d->labels,(int)s->number[id],out,size)) return;
    if (id==LR_DISPLAY) {
        if (s->number[id]<0) snprintf(out,size,"Primary display");
        else snprintf(out,size,"Display %.0f",s->number[id]);
        return;
    }
    if (id==LR_MOVE_DEADZONE || id==LR_LOOK_DEADZONE || id==LR_STICK_OUTER_DEADZONE)
        snprintf(out,size,"%.0f%%",s->number[id]*100);
    else if (id==LR_MOUSE_SENS) snprintf(out,size,"%.2fx",s->number[id]);
    else snprintf(out,size,"%s",s->value[id]);
}

void lr_settings_print(const lr_settings *s, FILE *out) {
    for (int i=0;i<LR_OPTION_COUNT;i++) {
        fprintf(out,"%-24s = %-12s [%s%s]\n",lr_options[i].key,s->value[i],
                s->source[i]==LR_ENV?"environment: ":s->source[i]==LR_PRESET?"preset":
                s->source[i]==LR_COMMAND_LINE?"command line":"default",
                s->source[i]==LR_ENV?lr_options[i].env:"");
    }
    fprintf(out,"effective: renderer=%s gamepad=%s window=%d realtime=%d\n",
            s->render==2?"gl":s->render==3?"null":"software",
            s->gamepad?"modern":"classic",s->window,s->realtime);
    if (s->number[LR_MOUSE] && s->number[LR_INPUT]!=2)
        fprintf(out,"note: mouse capture is enabled, but mouse look requires INPUT=dual\n");
}

static lr_settings installed, fallback;
static const lr_settings *active;
static pthread_once_t fallback_once=PTHREAD_ONCE_INIT;
static void default_environment(void) {
    char error[LR_ERROR_SIZE]; lr_settings_defaults(&fallback);
    if (lr_settings_env(&fallback,error) || lr_settings_resolve(&fallback,error)) {
        fprintf(stderr,"settings: %s\n",error); exit(2);
    }
}
void lr_settings_use(const lr_settings *s) { installed=*s; active=&installed; }
const lr_settings *lr_settings_current(void) {
    if (active) return active;
    pthread_once(&fallback_once,default_environment); return &fallback;
}

int lr_presets_find(const lr_presets *p, const char *name) {
    for (int i=0;i<p->count;i++) if (!strcmp(p->presets[i].name,name)) return i;
    return -1;
}
int lr_presets_name_valid(const char *name) {
    size_t n=strlen(name);
    if (!n || n>=LR_NAME_SIZE || isspace((unsigned char)name[0]) || isspace((unsigned char)name[n-1])) return 0;
    for (const unsigned char *c=(const unsigned char *)name;*c;c++)
        if (*c<32 || *c==127 || strchr("[]=;#",*c)) return 0;
    return 1;
}
int lr_presets_add(lr_presets *p, const char *name, const lr_settings *s, char *error) {
    if (!lr_presets_name_valid(name)) return fail(error,"preset name","use 1-63 characters without brackets, =, ; or #, or leading/trailing spaces");
    if (p->count>=LR_MAX_PRESETS) return fail(error,"presets","at most 32 presets are supported");
    if (lr_presets_find(p,name)>=0) return fail(error,"preset name","already exists");
    lr_preset *v=&p->presets[p->count++]; strcpy(v->name,name); v->settings=*s;
    return 0;
}
void lr_presets_defaults(lr_presets *p) {
    memset(p,0,sizeof *p); lr_settings s; char error[LR_ERROR_SIZE]; lr_settings_defaults(&s);
    lr_settings_set(&s,LR_WINDOW,"1",LR_PRESET,error);
    lr_settings_set(&s,LR_RENDER,"gl",LR_PRESET,error);
    lr_settings_set(&s,LR_MPEG_DECODE,"1",LR_PRESET,error);
    lr_presets_add(p,"Classic",&s,error);
    lr_settings_set(&s,LR_INPUT,"dual",LR_PRESET,error);
    lr_settings_set(&s,LR_RESOLUTION,"window",LR_PRESET,error);
    lr_presets_add(p,"Controller",&s,error);
    lr_settings_set(&s,LR_KEYS,"wasd",LR_PRESET,error);
    lr_settings_set(&s,LR_MOUSE,"1",LR_PRESET,error);
    lr_presets_add(p,"Mouse & Keyboard",&s,error);
    p->selected=1;
    for (int i=0;i<p->count;i++) lr_settings_resolve(&p->presets[i].settings,error);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n=strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n]=0;
    return s;
}
int lr_presets_load(lr_presets *p, const char *path, char *error) {
    FILE *f=fopen(path,"r");
    if (!f) return fail(error,path,strerror(errno));
    lr_presets next={0}; char line[512], selected[LR_NAME_SIZE]="", why[LR_ERROR_SIZE]="";
    int at=-1, version=0, lineno=0, bad=0, seen[LR_MAX_PRESETS][LR_OPTION_COUNT]={{0}};
    while (fgets(line,sizeof line,f)) {
        lineno++;
        if (!strchr(line,'\n') && !feof(f)) { strcpy(why,"line too long"); bad=1; break; }
        char *v=trim(line);
        if (!*v || *v=='#' || *v==';') continue;
        if (*v=='[') {
            size_t n=strlen(v);
            if (strncmp(v,"[preset ",8) || n<10 || v[n-1]!=']') { strcpy(why,"expected [preset Name]"); bad=1; break; }
            v[n-1]=0; lr_settings s; lr_settings_defaults(&s);
            if (lr_presets_add(&next,v+8,&s,why)) { bad=1; break; }
            at=next.count-1; continue;
        }
        char *eq=strchr(v,'=');
        if (!eq) { strcpy(why,"expected key=value"); bad=1; break; }
        *eq=0; char *key=trim(v); v=trim(eq+1);
        if (at<0) {
            if (!strcmp(key,"version") && !version && !strcmp(v,"1")) version=1;
            else if (!strcmp(key,"selected") && !*selected && lr_presets_name_valid(v)) strcpy(selected,v);
            else { strcpy(why,"expected version=1 and selected=Name once, before preset sections"); bad=1; break; }
        } else {
            int id=0; while (id<LR_OPTION_COUNT && strcmp(key,lr_options[id].key)) id++;
            if (id==LR_OPTION_COUNT) { snprintf(why,sizeof why,"unknown option '%s'",key); bad=1; break; }
            if (seen[at][id]++) { snprintf(why,sizeof why,"duplicate option '%s'",key); bad=1; break; }
            if (lr_settings_set(&next.presets[at].settings,id,v,LR_PRESET,why)) { bad=1; break; }
        }
    }
    if (ferror(f)) { snprintf(why,sizeof why,"read failed: %s",strerror(errno)); bad=1; }
    fclose(f);
    if (bad) { snprintf(error,LR_ERROR_SIZE,"%s:%d: %.300s",path,lineno,why); return -1; }
    if (!version || !next.count || (next.selected=lr_presets_find(&next,selected))<0)
        return fail(error,path,"requires version=1, at least one preset and an existing selected preset");
    *p=next; return 0;
}

int lr_presets_save(const lr_presets *p, const char *path, char *error) {
    if (p->count<1 || p->count>LR_MAX_PRESETS || p->selected<0 || p->selected>=p->count)
        return fail(error,"presets","invalid selection");
    for (int i=0;i<p->count;i++) {
        if (!lr_presets_name_valid(p->presets[i].name) || lr_presets_find(p,p->presets[i].name)!=i)
            return fail(error,"presets","invalid or duplicate name");
        lr_settings check; lr_settings_defaults(&check);
        for (int k=0;k<LR_OPTION_COUNT;k++)
            if (lr_settings_set(&check,k,p->presets[i].settings.value[k],LR_PRESET,error)) return -1;
        if (lr_settings_resolve(&check,error)) return -1;
    }
    size_t n=strlen(path)+16; char *tmp=malloc(n);
    if (!tmp) return fail(error,path,"out of memory");
    snprintf(tmp,n,"%s.tmp.XXXXXX",path);
    int fd=mkstemp(tmp);
    if (fd<0) { free(tmp); return fail(error,path,strerror(errno)); }
    FILE *f=fdopen(fd,"w");
    if (!f) { int e=errno; close(fd); unlink(tmp); free(tmp); return fail(error,path,strerror(e)); }
    fprintf(f,"# Last Raven player settings. Environment overrides are never saved.\nversion=1\nselected=%s\n",p->presets[p->selected].name);
    for (int i=0;i<p->count;i++) {
        fprintf(f,"\n[preset %s]\n",p->presets[i].name);
        for (int k=0;k<LR_OPTION_COUNT;k++) fprintf(f,"%s=%s\n",lr_options[k].key,p->presets[i].settings.value[k]);
    }
    int bad=ferror(f), saved_errno=errno;
    if (fflush(f) || fsync(fd)) { bad=1; saved_errno=errno; }
    if (fclose(f)) { bad=1; saved_errno=errno; }
    if (!bad && rename(tmp,path)) { bad=1; saved_errno=errno; }
    if (bad) { unlink(tmp); fail(error,path,strerror(saved_errno)); }
    free(tmp); return bad?-1:0;
}

int lr_settings_load(lr_settings *s, const char *path, const char *preset, char *error) {
    lr_settings next; lr_settings_defaults(&next);
    if (preset && !path) return fail(error,"--preset","requires --config");
    if (path) {
        lr_presets p;
        if (lr_presets_load(&p,path,error)) return -1;
        int at=preset?lr_presets_find(&p,preset):p.selected;
        if (at<0) return fail(error,preset,"preset does not exist");
        next=p.presets[at].settings;
    }
    if (lr_settings_env(&next,error) || lr_settings_resolve(&next,error)) return -1;
    *s=next; return 0;
}
