/* The pre-launch UI owns its SDL window. No game, guest clock or GL backend
 * runs here; the child boot process receives an explicit preset on launch. */
#include "settings.h"
#include "psprecomp/hle.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { UI_W=1120, UI_H=800, VISIBLE_ROWS=6 };
enum { NEW=100, DUPLICATE, RENAME, DELETE, RESET, SAVE, PLAY, CANCEL,
       MODAL_OK, MODAL_CANCEL, PRESET_BASE=200, PAGE_BASE=300 };
enum { MODAL_NONE, MODAL_NEW, MODAL_DUPLICATE, MODAL_RENAME, MODAL_VALUE,
       MODAL_DELETE, MODAL_RESET, MODAL_CANCEL_DIRTY };
typedef struct { SDL_Rect rect; int id; } hit;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *small, *body, *heading;
    SDL_GameController *pad;
    lr_presets book;
    lr_settings effective;
    char path[4096];
    const char *boot, *module, *iso;
    int page, scroll, selected_row, focus, dirty, running, valid, movie_available;
    int load_failed; /* A malformed file must never be overwritten by defaults. */
    char status[LR_ERROR_SIZE], validation[LR_ERROR_SIZE];
    hit hits[96]; int hit_count;
    int modal, edit_id, select_text;
    char edit[LR_VALUE_SIZE], modal_error[LR_ERROR_SIZE];
    pid_t child; int child_error_fd;
    char child_error[2048]; size_t child_error_len;
    Uint32 stick_repeat;
} launcher;

static const SDL_Color BG={16,22,29,255}, PANEL={24,32,42,255},
    ROW={30,40,52,255}, BORDER={49,64,79,255}, TEXT={230,237,240,255},
    MUTED={151,169,181,255}, ACCENT={239,184,90,255}, GOOD={110,208,191,255};

static void box(launcher *a, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(a->renderer,c.r,c.g,c.b,c.a);
    SDL_RenderFillRect(a->renderer,&r);
}
static void outline(launcher *a, SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(a->renderer,c.r,c.g,c.b,c.a);
    SDL_RenderDrawRect(a->renderer,&r);
}
static void label(launcher *a, TTF_Font *font, int x,int y,int width,const char *s,SDL_Color c) {
    if (!s || !*s) return;
    SDL_Surface *surface=TTF_RenderUTF8_Blended_Wrapped(font,s,c,(Uint32)width);
    if (!surface) return;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(a->renderer,surface);
    if (texture) {
        SDL_Rect dst={x,y,surface->w,surface->h};
        SDL_RenderCopy(a->renderer,texture,NULL,&dst); SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}
static void add_hit(launcher *a,int id,SDL_Rect rect) {
    if (a->hit_count<(int)(sizeof a->hits/sizeof a->hits[0])) a->hits[a->hit_count++]=(hit){rect,id};
}
static void button(launcher *a,int id,int x,int y,int w,int h,const char *s,int primary) {
    SDL_Rect r={x,y,w,h}; box(a,r,primary?ACCENT:ROW);
    outline(a,r,a->focus==id?ACCENT:BORDER);
    label(a,a->body,x+12,y+(h-24)/2,w-24,s,primary?BG:TEXT);
    add_hit(a,id,r);
}
static lr_settings *editing(launcher *a) { return &a->book.presets[a->book.selected].settings; }
static int overridden(int id) { const char *v=getenv(lr_options[id].env); return v && *v; }
static int unavailable(launcher *a,int id) { return id==LR_MPEG_DECODE && !a->movie_available; }
static void display_label(const lr_settings *s,char *out,size_t size) {
    lr_option_label(s,LR_DISPLAY,out,size);
    int screen=(int)s->number[LR_DISPLAY];
    if (screen<0) return;
    if (screen>SDL_GetNumVideoDisplays()) {
        snprintf(out,size,"Display %d (unavailable)",screen);
        return;
    }
    const char *name=SDL_GetDisplayName(screen-1);
    if (name && *name) snprintf(out,size,"%d: %s",screen,name);
}
static void refresh(launcher *a) {
    a->effective=*editing(a); a->validation[0]=0;
    a->valid=!lr_settings_env(&a->effective,a->validation) &&
             !lr_settings_resolve(&a->effective,a->validation);
    if (a->valid && !a->movie_available && a->effective.number[LR_MPEG_DECODE]) {
        strcpy(a->validation,"Intro decoding is unavailable in this build. Turn it off or remove its environment override.");
        a->valid=0;
    }
    if (a->effective.number[LR_WINDOW_MODE]) {
        if (a->focus==LR_WINDOW_SIZE) a->focus=LR_WINDOW_MODE;
        if (a->selected_row==LR_WINDOW_SIZE) a->selected_row=LR_WINDOW_MODE;
    }
}
static int rows(launcher *a,int *ids) {
    const char *pages[]={"Graphics","Controls","Advanced"}; int n=0;
    for (int i=0;i<LR_OPTION_COUNT;i++) {
        if (i==LR_WINDOW_SIZE && a->effective.number[LR_WINDOW_MODE]) continue;
        if (!strcmp(lr_options[i].page,pages[a->page])) ids[n++]=i;
    }
    return n;
}
static void show_row(launcher *a,int id) {
    int ids[LR_OPTION_COUNT], n=rows(a,ids);
    for (int k=0;k<n;k++) if (ids[k]==id) {
        a->selected_row=id;
        if (k<a->scroll) a->scroll=k;
        if (k>=a->scroll+VISIBLE_ROWS) a->scroll=k-VISIBLE_ROWS+1;
        break;
    }
}
static void open_modal(launcher *a,int kind,const char *text) {
    a->modal=kind; a->modal_error[0]=0; a->focus=MODAL_OK;
    snprintf(a->edit,sizeof a->edit,"%s",text?text:""); a->select_text=1;
    if (kind<=MODAL_VALUE) SDL_StartTextInput();
}
static void close_modal(launcher *a) {
    SDL_StopTextInput(); a->modal=MODAL_NONE; a->focus=a->selected_row;
}

static void draw(launcher *a) {
    refresh(a); a->hit_count=0;
    box(a,(SDL_Rect){0,0,UI_W,UI_H},BG);
    label(a,a->small,28,22,800,"ARMORED CORE  /  PC SETTINGS",ACCENT);
    label(a,a->heading,26,48,700,"LAST RAVEN",TEXT);
    label(a,a->body,28,94,1000,"Choose a setup. Make it yours. Launch when you're ready.",MUTED);
    box(a,(SDL_Rect){28,144,242,544},PANEL);
    label(a,a->small,44,160,210,"SAVED PRESETS",MUTED);
    int start=a->book.selected>7?a->book.selected-7:0;
    for (int k=start;k<a->book.count && k<start+8;k++) {
        int y=192+(k-start)*43; SDL_Rect r={40,y,218,39};
        box(a,r,k==a->book.selected?ROW:PANEL);
        if (k==a->book.selected) box(a,(SDL_Rect){40,y,3,39},ACCENT);
        if (a->focus==PRESET_BASE+k) outline(a,r,ACCENT);
        SDL_RenderSetClipRect(a->renderer,&r);
        label(a,a->body,52,y+7,900,a->book.presets[k].name,k==a->book.selected?TEXT:MUTED);
        SDL_RenderSetClipRect(a->renderer,NULL);
        add_hit(a,PRESET_BASE+k,r);
    }
    if (a->book.count>8) label(a,a->small,44,540,210,"Scroll here for more presets",MUTED);
    button(a,NEW,40,580,103,38,"New",0); button(a,DUPLICATE,151,580,107,38,"Duplicate",0);
    button(a,RENAME,40,626,103,38,"Rename",0); button(a,DELETE,151,626,107,38,"Delete",0);
    const char *pages[]={"Graphics","Controls","Advanced"};
    for (int k=0;k<3;k++) {
        button(a,PAGE_BASE+k,294+k*180,144,170,44,pages[k],0);
        if (k==a->page) box(a,(SDL_Rect){294+k*180,185,170,3},ACCENT);
    }
    int ids[LR_OPTION_COUNT], n=rows(a,ids);
    if (a->scroll>n-VISIBLE_ROWS) a->scroll=n>VISIBLE_ROWS?n-VISIBLE_ROWS:0;
    if (a->scroll<0) a->scroll=0;
    for (int k=a->scroll;k<n && k<a->scroll+VISIBLE_ROWS;k++) {
        int id=ids[k],y=204+(k-a->scroll)*56;
        SDL_Rect r={294,y,798,50}; box(a,r,ROW);
        if (a->focus==id) outline(a,r,ACCENT);
        int locked=overridden(id), missing=unavailable(a,id);
        int disabled=missing && !editing(a)->number[id];
        label(a,a->body,310,y+5,300,lr_options[id].label,locked||disabled?MUTED:TEXT);
        label(a,a->small,310,y+29,360,locked?"Environment override":missing?
              (disabled?"Not available in this build":"Decoder unavailable; switch off"):"Applies on next launch",locked?ACCENT:MUTED);
        char value[LR_VALUE_SIZE]; lr_option_label(&a->effective,id,value,sizeof value);
        if (id==LR_DISPLAY) display_label(&a->effective,value,sizeof value);
        SDL_Rect clip={662,y,330,50}; SDL_RenderSetClipRect(a->renderer,&clip);
        label(a,a->body,668,y+13,600,value,locked?ACCENT:TEXT);
        SDL_RenderSetClipRect(a->renderer,NULL);
        add_hit(a,id,r);
        if (!locked && !disabled) {
            /* These hits deliberately follow the row hit, so the small
             * controls win hit testing without adding keyboard focus stops. */
            label(a,a->body,1001,y+12,35,"-",MUTED);
            label(a,a->body,1050,y+12,35,"+",MUTED);
            add_hit(a,400+id,(SDL_Rect){986,y,48,50});
            add_hit(a,500+id,(SDL_Rect){1034,y,58,50});
        }
    }
    if (n>VISIBLE_ROWS) {
        char range[80]; snprintf(range,sizeof range,"%d-%d of %d  /  Scroll or use Up / Down",a->scroll+1,
                                a->scroll+VISIBLE_ROWS<n?a->scroll+VISIBLE_ROWS:n,n);
        label(a,a->small,310,546,650,range,MUTED);
    }
    box(a,(SDL_Rect){294,580,798,108},PANEL);
    int id=a->selected_row;
    if (id>=0 && id<LR_OPTION_COUNT) {
        char info[640];
        snprintf(info,sizeof info,"%s%s%s",lr_options[id].help,
                 overridden(id)?"  Locked for this run by ":"",overridden(id)?lr_options[id].env:"");
        if (id==LR_DISPLAY) {
            int screen=(int)a->effective.number[id];
            if (screen<0) screen=1;
            const char *name=screen<=SDL_GetNumVideoDisplays()?SDL_GetDisplayName(screen-1):NULL;
            if (name) {
                size_t used=strlen(info);
                snprintf(info+used,sizeof info-used,"\nDisplay %d: %s",screen,name);
            }
        }
        label(a,a->small,310,594,762,info,MUTED);
    }
    const char *status=a->load_failed?a->status:!a->valid?a->validation:*a->status?a->status:
                       a->dirty?"Unsaved changes":"Presets are ready. Changes take effect when you launch.";
    SDL_Rect status_clip={28,695,1064,30}; SDL_RenderSetClipRect(a->renderer,&status_clip);
    label(a,a->small,28,700,1064,status,!a->valid||a->load_failed?ACCENT:GOOD);
    SDL_RenderSetClipRect(a->renderer,NULL);
    button(a,RESET,28,737,155,43,"Reset preset",0);
    button(a,CANCEL,700,737,110,43,"Cancel",0);
    button(a,SAVE,822,737,100,43,"Save",0);
    button(a,PLAY,934,737,158,43,"Save & Play",1);
    label(a,a->small,205,745,465,"Tab: focus   Arrows: adjust   Enter: edit\nController: D-pad / A / B   Bumpers: pages",MUTED);

    if (a->modal) {
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
        box(a,(SDL_Rect){0,0,UI_W,UI_H},(SDL_Color){0,0,0,190});
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        a->hit_count=0;
        box(a,(SDL_Rect){270,239,580,320},PANEL); outline(a,(SDL_Rect){270,239,580,320},BORDER);
        const char *titles[]={"","New preset","Duplicate preset","Rename preset","Edit value",
                              "Delete preset?","Reset this preset?","Discard unsaved changes?"};
        label(a,a->heading,296,258,530,titles[a->modal],TEXT);
        if (a->modal<=MODAL_VALUE) {
            char hint[256];
            if (a->modal==MODAL_VALUE) {
                const lr_option_def *d=&lr_options[a->edit_id];
                if (d->type==LR_SIZE) snprintf(hint,sizeof hint,"%s: WIDTHxHEIGHT",d->label);
                else snprintf(hint,sizeof hint,"%s: %g to %g%s%s",d->label,d->min,d->max,
                              d->special?", or ":"",d->special?d->special:"");
            } else strcpy(hint,"Preset name (up to 63 UTF-8 bytes)");
            label(a,a->small,296,307,530,hint,MUTED);
            SDL_Rect entry={296,340,528,50}; box(a,entry,ROW); outline(a,entry,ACCENT);
            SDL_RenderSetClipRect(a->renderer,&entry);
            label(a,a->body,308,352,1500,a->edit,a->select_text?ACCENT:TEXT);
            SDL_RenderSetClipRect(a->renderer,NULL);
            label(a,a->small,296,402,526,a->modal_error,MUTED);
        } else {
            const char *explain=a->modal==MODAL_DELETE?"The preset will be removed from this session. Save to make the deletion permanent.":
                a->modal==MODAL_RESET?"Restore this preset to the game's original graphics and controls. Save to keep the change.":
                "Your unsaved preset edits will be discarded. The saved file will stay as it was.";
            label(a,a->body,296,321,526,explain,MUTED);
            label(a,a->small,296,419,526,a->modal_error,ACCENT);
        }
        button(a,MODAL_CANCEL,576,495,114,42,"Cancel",0);
        button(a,MODAL_OK,702,495,122,42,a->modal<=MODAL_VALUE?"Accept":"Confirm",1);
    }
}

static int save(launcher *a) {
    if (a->load_failed) return -1;
    if (lr_presets_save(&a->book,a->path,a->status)) return -1;
    a->dirty=0; snprintf(a->status,sizeof a->status,"Saved preset: %.63s",a->book.presets[a->book.selected].name);
    return 0;
}
static void changed(launcher *a) { a->dirty=1; a->status[0]=0; refresh(a); }

static void adjust(launcher *a,int id,int direction) {
    if (id<0 || id>=LR_OPTION_COUNT) return;
    a->focus=id; show_row(a,id);
    if (overridden(id)) return;
    lr_settings *s=editing(a); const lr_option_def *d=&lr_options[id]; char value[LR_VALUE_SIZE];
    if (unavailable(a,id)) {
        /* A preset from a decoder-equipped build can still be repaired here. */
        if (s->number[id] && !lr_settings_set(s,id,"0",LR_PRESET,a->status)) changed(a);
        return;
    }
    if (id==LR_DISPLAY) {
        int count=SDL_GetNumVideoDisplays();
        if (count<0) count=0;
        int pick=(int)s->number[id];
        if (pick<0 || pick>count) pick=0; /* Primary or disconnected. */
        pick=(pick+direction+count+1)%(count+1);
        if (!pick) snprintf(value,sizeof value,"primary");
        else snprintf(value,sizeof value,"%d",pick);
    } else if (d->type==LR_CHOICE) {
        int count=1; for (const char *v=d->choices;*v;v++) if (*v=='|') count++;
        if (id==LR_RENDER) count--; /* Null is useful in files/CLI, not for play. */
        int pick=((int)s->number[id]+direction+count)%count;
        const char *v=d->choices; while (pick--) v=strchr(v,'|')+1;
        size_t n=strcspn(v,"|"); memcpy(value,v,n); value[n]=0;
    } else if (d->type==LR_SIZE) {
        const char *sizes[]={"960x544","1280x720","1600x900","1920x1080","2560x1440","3840x2160"};
        int pick=0; for (int i=0;i<6;i++) if (!strcmp(sizes[i],s->value[id])) pick=i;
        snprintf(value,sizeof value,"%s",sizes[(pick+direction+6)%6]);
    } else {
        double n=s->number[id];
        if (n<0) n=direction>0?d->min:d->max;
        else n=round((n+direction*d->step)/d->step)*d->step;
        if (n<d->min && d->special) snprintf(value,sizeof value,"%s",d->special);
        else snprintf(value,sizeof value,"%.9g",fmin(d->max,fmax(d->min,n)));
    }
    if (!lr_settings_set(s,id,value,LR_PRESET,a->status)) changed(a);
}

static void modal_accept(launcher *a) {
    int kind=a->modal;
    if (kind==MODAL_NEW || kind==MODAL_DUPLICATE) {
        lr_settings s;
        if (kind==MODAL_DUPLICATE) s=*editing(a);
        else {
            lr_settings_defaults(&s);
            lr_settings_set(&s,LR_WINDOW,"1",LR_PRESET,a->modal_error);
            lr_settings_set(&s,LR_RENDER,"gl",LR_PRESET,a->modal_error);
            lr_settings_set(&s,LR_MPEG_DECODE,a->movie_available?"1":"0",LR_PRESET,a->modal_error);
        }
        if (lr_presets_add(&a->book,a->edit,&s,a->modal_error)) return;
        a->book.selected=a->book.count-1;
    } else if (kind==MODAL_RENAME) {
        int existing=lr_presets_find(&a->book,a->edit);
        if (!lr_presets_name_valid(a->edit) || (existing>=0 && existing!=a->book.selected)) {
            strcpy(a->modal_error,"Use a unique name without brackets, =, ; or #."); return;
        }
        strcpy(a->book.presets[a->book.selected].name,a->edit);
    } else if (kind==MODAL_VALUE) {
        if (lr_settings_set(editing(a),a->edit_id,a->edit,LR_PRESET,a->modal_error)) return;
    } else if (kind==MODAL_DELETE) {
        if (a->book.count==1) { strcpy(a->modal_error,"Keep at least one preset."); return; }
        int at=a->book.selected;
        memmove(a->book.presets+at,a->book.presets+at+1,(size_t)(a->book.count-at-1)*sizeof(lr_preset));
        a->book.count--; if (at>=a->book.count) a->book.selected=a->book.count-1;
    } else if (kind==MODAL_RESET) {
        lr_settings_defaults(editing(a));
        lr_settings_set(editing(a),LR_WINDOW,"1",LR_PRESET,a->modal_error);
    } else if (kind==MODAL_CANCEL_DIRTY) { a->running=0; close_modal(a); return; }
    changed(a); close_modal(a);
}

static void launch_game(launcher *a) {
    refresh(a);
    if (!a->valid || a->load_failed) return;
    if (!a->boot || !a->module) { strcpy(a->status,"Launch needs --boot EXECUTABLE and --module ELF. Settings can still be saved."); return; }
    if (access(a->boot,X_OK) || access(a->module,R_OK) || (a->iso && access(a->iso,R_OK))) {
        snprintf(a->status,sizeof a->status,"Cannot access game executable, module or disc: %s",strerror(errno)); return;
    }
    if (a->effective.render==3) { strcpy(a->status,"The null renderer is for diagnostics. Select OpenGL or software before playing."); return; }
    if (save(a)) return;
    int pipes[2];
    if (pipe(pipes)) { snprintf(a->status,sizeof a->status,"Cannot launch: %s",strerror(errno)); return; }
    fflush(NULL);
    pid_t child=fork();
    if (child<0) { close(pipes[0]); close(pipes[1]); snprintf(a->status,sizeof a->status,"Cannot launch: %s",strerror(errno)); return; }
    if (!child) {
        close(pipes[0]); dup2(pipes[1],STDERR_FILENO); close(pipes[1]);
        const char *args[12]; int n=0;
        args[n++]=a->boot; args[n++]=a->module; if (a->iso) args[n++]=a->iso;
        args[n++]="--config"; args[n++]=a->path;
        args[n++]="--preset"; args[n++]=a->book.presets[a->book.selected].name;
        args[n++]="--window"; args[n]=NULL;
        execv(a->boot,(char *const *)args);
        /* Only async-signal-safe operations between fork and exec: SDL may
         * have other threads with libc locks held at the fork boundary. */
        const char message[]="Could not execute the game host. Check its path and permissions.\n";
        write(STDERR_FILENO,message,sizeof message-1); _exit(127);
    }
    close(pipes[1]); fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    a->child=child; a->child_error_fd=pipes[0]; a->child_error_len=0; a->child_error[0]=0;
    SDL_HideWindow(a->window);
}

static void activate(launcher *a,int id) {
    if (a->modal) {
        if (id==MODAL_OK) modal_accept(a);
        else if (id==MODAL_CANCEL) close_modal(a);
        return;
    }
    if (id>=500 && id<500+LR_OPTION_COUNT) { adjust(a,id-500,1); return; }
    if (id>=400 && id<400+LR_OPTION_COUNT) { adjust(a,id-400,-1); return; }
    if (id>=PAGE_BASE && id<PAGE_BASE+3) {
        a->page=id-PAGE_BASE; a->scroll=0;
        int ids[LR_OPTION_COUNT]; rows(a,ids); a->focus=a->selected_row=ids[0]; return;
    }
    if (id>=PRESET_BASE && id<PRESET_BASE+a->book.count) {
        if (a->book.selected!=id-PRESET_BASE) { a->book.selected=id-PRESET_BASE; changed(a); }
        a->focus=id; return;
    }
    if (id>=0 && id<LR_OPTION_COUNT) {
        a->selected_row=a->focus=id;
        if (overridden(id) || (unavailable(a,id) && !editing(a)->number[id])) return;
        if (lr_options[id].type==LR_CHOICE || id==LR_DISPLAY) adjust(a,id,1);
        else { a->edit_id=id; open_modal(a,MODAL_VALUE,editing(a)->value[id]); }
        return;
    }
    char name[LR_NAME_SIZE];
    switch (id) {
    case NEW: case DUPLICATE:
        for (int n=1;;n++) {
            snprintf(name,sizeof name,"%s %d",id==NEW?"Preset":"Copy",n);
            if (lr_presets_find(&a->book,name)<0) break;
        }
        open_modal(a,id==NEW?MODAL_NEW:MODAL_DUPLICATE,name); break;
    case RENAME: open_modal(a,MODAL_RENAME,a->book.presets[a->book.selected].name); break;
    case DELETE: open_modal(a,MODAL_DELETE,NULL); break;
    case RESET: open_modal(a,MODAL_RESET,NULL); break;
    case SAVE: save(a); break;
    case PLAY: launch_game(a); break;
    case CANCEL:
        if (a->dirty) open_modal(a,MODAL_CANCEL_DIRTY,NULL); else a->running=0;
        break;
    }
}

static void focus_next(launcher *a,int direction) {
    int ids[96],n=0;
    if (a->modal) { a->focus=a->focus==MODAL_OK?MODAL_CANCEL:MODAL_OK; return; }
    /* Include every option, including offscreen rows, so navigation can
     * scroll them into view. Small +/- mouse targets are not tab stops. */
    for (int i=0;i<a->book.count;i++) ids[n++]=PRESET_BASE+i;
    ids[n++]=NEW; ids[n++]=DUPLICATE; ids[n++]=RENAME; ids[n++]=DELETE;
    for (int i=0;i<3;i++) ids[n++]=PAGE_BASE+i;
    int option_ids[LR_OPTION_COUNT],count=rows(a,option_ids);
    for (int i=0;i<count;i++) ids[n++]=option_ids[i];
    ids[n++]=RESET; ids[n++]=CANCEL; ids[n++]=SAVE; ids[n++]=PLAY;
    int at=0; for (int i=0;i<n;i++) if (ids[i]==a->focus) at=i;
    a->focus=ids[(at+direction+n)%n]; show_row(a,a->focus);
}

static void key(launcher *a,SDL_Keycode k,SDL_Keymod mod) {
    if (k==SDLK_ESCAPE) { if (a->modal) close_modal(a); else activate(a,CANCEL); return; }
    if (k==SDLK_TAB) { focus_next(a,(mod & KMOD_SHIFT)?-1:1); return; }
    if (k==SDLK_RETURN || k==SDLK_KP_ENTER) { activate(a,a->focus); return; }
    if (a->modal) {
        if (a->modal<=MODAL_VALUE && k==SDLK_BACKSPACE) {
            size_t n=strlen(a->edit);
            if (a->select_text) a->edit[0]=0;
            else if (n) { do { n--; } while (n && (a->edit[n]&0xc0)==0x80); a->edit[n]=0; }
            a->select_text=0;
        }
        if (k==SDLK_a && (mod & KMOD_CTRL)) a->select_text=1;
        if (k==SDLK_LEFT || k==SDLK_RIGHT) focus_next(a,1);
        return;
    }
    if (k==SDLK_UP || k==SDLK_DOWN) { focus_next(a,k==SDLK_UP?-1:1); return; }
    if (k==SDLK_LEFT || k==SDLK_RIGHT) {
        if (a->focus>=0 && a->focus<LR_OPTION_COUNT) adjust(a,a->focus,k==SDLK_LEFT?-1:1);
        else focus_next(a,k==SDLK_LEFT?-1:1);
    }
}

static void controller_open(launcher *a) {
    if (a->pad && !SDL_GameControllerGetAttached(a->pad)) { SDL_GameControllerClose(a->pad); a->pad=NULL; }
    if (!a->pad) for (int i=0;i<SDL_NumJoysticks();i++) if (SDL_IsGameController(i)) {
        a->pad=SDL_GameControllerOpen(i); if (a->pad) break;
    }
}
static void event(launcher *a,const SDL_Event *e) {
    if (e->type==SDL_CONTROLLERDEVICEADDED || e->type==SDL_CONTROLLERDEVICEREMOVED) controller_open(a);
    if (a->child) return;
    if (e->type==SDL_QUIT) { activate(a,CANCEL); return; }
    if (e->type==SDL_KEYDOWN) key(a,e->key.keysym.sym,(SDL_Keymod)e->key.keysym.mod);
    if (e->type==SDL_TEXTINPUT && a->modal && a->modal<=MODAL_VALUE) {
        if (a->select_text) a->edit[0]=0;
        a->select_text=0;
        size_t n=strlen(a->edit), add=strlen(e->text.text);
        size_t max=a->modal==MODAL_VALUE?LR_VALUE_SIZE:LR_NAME_SIZE;
        if (n+add<max) memcpy(a->edit+n,e->text.text,add+1);
    }
    if (e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) {
        for (int i=a->hit_count-1;i>=0;i--) {
            SDL_Point p={e->button.x,e->button.y};
            if (SDL_PointInRect(&p,&a->hits[i].rect)) { a->focus=a->hits[i].id; activate(a,a->focus); break; }
        }
    }
    if (e->type==SDL_MOUSEWHEEL && !a->modal) {
        int x,y; SDL_GetMouseState(&x,&y);
        float lx,ly; SDL_RenderWindowToLogical(a->renderer,x,y,&lx,&ly);
        int delta=e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-e->wheel.y:e->wheel.y;
        if (lx<280) {
            int at=a->book.selected-delta;
            if (at<0) at=0;
            if (at>=a->book.count) at=a->book.count-1;
            activate(a,PRESET_BASE+at);
        } else a->scroll-=delta;
    }
    if (e->type==SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: key(a,SDLK_RETURN,0); break;
        case SDL_CONTROLLER_BUTTON_B: key(a,SDLK_ESCAPE,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: key(a,SDLK_UP,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key(a,SDLK_DOWN,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key(a,SDLK_LEFT,0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key(a,SDLK_RIGHT,0); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (!a->modal) activate(a,PAGE_BASE+(a->page+(e->cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER?2:1))%3);
            break;
        case SDL_CONTROLLER_BUTTON_START: if (!a->modal) activate(a,PLAY); break;
        }
    }
}

static void poll_child(launcher *a) {
    if (!a->child) return;
    char buf[512]; ssize_t got;
    while ((got=read(a->child_error_fd,buf,sizeof buf))>0) {
        fwrite(buf,1,(size_t)got,stderr);
        size_t keep=a->child_error_len;
        if (keep+(size_t)got>=sizeof a->child_error) {
            size_t discard=keep+(size_t)got-sizeof a->child_error+1;
            memmove(a->child_error,a->child_error+discard,keep-discard); keep-=discard;
        }
        memcpy(a->child_error+keep,buf,(size_t)got); a->child_error_len=keep+(size_t)got;
        a->child_error[a->child_error_len]=0;
    }
    int status; pid_t done=waitpid(a->child,&status,WNOHANG);
    if (done<=0) return;
    close(a->child_error_fd); a->child=0;
    if (WIFEXITED(status) && WEXITSTATUS(status)==0) { a->running=0; return; }
    SDL_ShowWindow(a->window); SDL_RaiseWindow(a->window);
    snprintf(a->status,sizeof a->status,"Game stopped (%s %d). See terminal output.",
             WIFEXITED(status)?"exit":"signal",WIFEXITED(status)?WEXITSTATUS(status):WTERMSIG(status));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Last Raven could not start",
                             *a->child_error?a->child_error:a->status,a->window);
}

static int fonts(launcher *a,const char *requested) {
    const char *paths[]={requested,"/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf","C:/Windows/Fonts/segoeui.ttf"};
    for (size_t i=0;i<sizeof paths/sizeof paths[0];i++) {
        if (!paths[i]) continue;
        a->body=TTF_OpenFont(paths[i],17);
        if (a->body) {
            a->small=TTF_OpenFont(paths[i],13); a->heading=TTF_OpenFont(paths[i],28);
            return a->small && a->heading?0:-1;
        }
        if (requested) break;
    }
    return -1;
}

int main(int argc,char **argv) {
    launcher a={0}; a.running=1; a.focus=a.selected_row=LR_RESOLUTION;
    const char *preset=NULL,*font=NULL;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--help")) {
            puts("launcher [--config FILE] [--preset NAME] [--boot EXECUTABLE --module ELF [--iso DISC]] [--font TTF]\n"
                 "Keyboard: Tab, arrows, Enter, Escape. Controller: D-pad, A/B, bumpers, Start."); return 0;
        }
        const char *option=argv[i];
        if (++i==argc) { fprintf(stderr,"%s needs a value\n",option); return 2; }
        if (!strcmp(option,"--config")) {
            if (strlen(argv[i])>=sizeof a.path) { fprintf(stderr,"config path too long\n"); return 2; }
            strcpy(a.path,argv[i]);
        } else if (!strcmp(option,"--preset")) preset=argv[i];
        else if (!strcmp(option,"--boot")) a.boot=argv[i];
        else if (!strcmp(option,"--module")) a.module=argv[i];
        else if (!strcmp(option,"--iso")) a.iso=argv[i];
        else if (!strcmp(option,"--font")) font=argv[i];
        else { fprintf(stderr,"unknown option: %s\n",option); return 2; }
    }
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER) || TTF_Init()) {
        fprintf(stderr,"launcher: %s\n",SDL_GetError()); return 1;
    }
    if (!*a.path) {
        char *dir=SDL_GetPrefPath("","Last Raven");
        if (!dir || snprintf(a.path,sizeof a.path,"%ssettings.ini",dir)>=(int)sizeof a.path) {
            fprintf(stderr,"cannot find preferences directory: %s\n",SDL_GetError()); SDL_free(dir); return 1;
        }
        SDL_free(dir);
    }
    a.movie_available=psp_mpeg_decoding_available();
    if (access(a.path,F_OK)==0 || errno!=ENOENT) {
        if (lr_presets_load(&a.book,a.path,a.status)) a.load_failed=1;
    } else a.dirty=1;
    if (!a.book.count) {
        lr_presets_defaults(&a.book);
        if (!a.movie_available) for (int i=0;i<a.book.count;i++)
            lr_settings_set(&a.book.presets[i].settings,LR_MPEG_DECODE,"0",LR_PRESET,a.validation);
    }
    if (preset) {
        int at=lr_presets_find(&a.book,preset);
        if (at<0) { fprintf(stderr,"preset does not exist: %s\n",preset); return 2; }
        if (a.book.selected!=at) a.dirty=1;
        a.book.selected=at;
    }
    a.window=SDL_CreateWindow("Last Raven - Settings",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
                              UI_W,UI_H,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!a.window) { fprintf(stderr,"launcher: %s\n",SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(a.window,840,600);
    a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!a.renderer) a.renderer=SDL_CreateRenderer(a.window,-1,SDL_RENDERER_SOFTWARE);
    if (!a.renderer || fonts(&a,font)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Settings unavailable",
            "Could not create the renderer or load a system font. Try --font /path/to/font.ttf.",a.window);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
    SDL_RenderSetLogicalSize(a.renderer,UI_W,UI_H);
    controller_open(&a);
    fprintf(stderr,"launcher: preferences %s\n",a.path);
    if (a.load_failed) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Cannot read saved presets",a.status,a.window);
    while (a.running) {
        if (!a.child) { draw(&a); SDL_RenderPresent(a.renderer); }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e,30)) { event(&a,&e); while (SDL_PollEvent(&e)) event(&a,&e); }
        if (a.pad && !a.child && !a.modal) {
            int x=SDL_GameControllerGetAxis(a.pad,SDL_CONTROLLER_AXIS_LEFTX);
            int y=SDL_GameControllerGetAxis(a.pad,SDL_CONTROLLER_AXIS_LEFTY);
            Uint32 now=SDL_GetTicks();
            if ((abs(x)>18000 || abs(y)>18000) && SDL_TICKS_PASSED(now,a.stick_repeat)) {
                key(&a,abs(y)>abs(x)?(y<0?SDLK_UP:SDLK_DOWN):(x<0?SDLK_LEFT:SDLK_RIGHT),0);
                a.stick_repeat=now+180;
            }
        }
        poll_child(&a);
    }
    if (a.pad) SDL_GameControllerClose(a.pad);
    TTF_CloseFont(a.body); TTF_CloseFont(a.small); TTF_CloseFont(a.heading);
    SDL_DestroyRenderer(a.renderer); SDL_DestroyWindow(a.window); TTF_Quit(); SDL_Quit();
    return 0;
}
