/* psprecomp's launcher (src/host/launcher.c, found on the include path) with
 * Armored Core's pack linked in (launcher_one.c): every page drawn and saved
 * as a BMP to look at, and a session -- the pack's settings changed and
 * saved to its own section, and Save and play running a harmless child in
 * place of a game, from the title's save folder, with the preferences file.
 * A folder under the first argument stands in for the user's. */
#include "settings.h"
#include <SDL2/SDL.h>
#define main launcher_entry
#include "launcher.c"
#undef main
#include <assert.h>

/* The host's own pads stay out: a press on one could close the launcher (B
 * goes back) mid-check. SDL keeps HIDAPI devices closed and passes only
 * VID/PID 0. Override priority, because a same-named environment variable
 * would win over SDL_SetHint; SDL_Quit clears hints. */
static void ignore_host_controllers(void) {
    assert(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE));
    assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000", SDL_HINT_OVERRIDE));
}

static void shot(launcher *a, const char *dir, const char *name) {
    /* Twice: ImGui sizes a new window or popup on its first frame. */
    render(a); render(a);
    int w = 0, h = 0;
    assert(!SDL_GetRendererOutputSize(a->renderer, &w, &h));
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    assert(surface);
    assert(!SDL_RenderReadPixels(a->renderer, NULL, surface->format->format, surface->pixels, surface->pitch));
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    for (char *c = path + strlen(dir) + 1; *c; c++) if (*c == ' ' || *c == '&') *c = '-';
    assert(!SDL_SaveBMP(surface, path));
    SDL_FreeSurface(surface);
    SDL_RenderPresent(a->renderer);
}

static int contains(const char *path, const char *text) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    static char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, text) != NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    for (int k = 0; k < LR_OPTION_COUNT; k++) unsetenv(psp_settings_option(k)->env);
    char home[3000], config[3100], data[3100], module[3100], settings[3200];
    snprintf(home, sizeof home, "%s/home", argv[1]);
    snprintf(config, sizeof config, "%s/config", home);
    snprintf(data, sizeof data, "%s/data", home);
    assert(!make_directories(config) && !make_directories(data));
    setenv("XDG_CONFIG_HOME", config, 1); setenv("XDG_DATA_HOME", data, 1);
    unsetenv("PSPRECOMP_DATA_ROOT");
    snprintf(settings, sizeof settings, "%s/psprecomp/settings.ini", config);
    unlink(settings);
    snprintf(module, sizeof module, "%s/module.elf", home);
    FILE *m = fopen(module, "w"); assert(m); fclose(m);
    ignore_host_controllers();
    assert(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER));

    /* The three titles, with a harmless child for a boot host. */
    char specs[3][4000];
    const char *slugs[] = {"aclr", "ac3p", "acsl"};
    char *args[8]; int n = 0;
    args[n++] = "launcher";
    for (int i = 0; i < 3; i++) {
        snprintf(specs[i], sizeof specs[i], "%s|%s|/bin/true|%s", slugs[i],
                 psp_launcher_info.names[i == 0 ? 2 : i - 1], module);
        args[n++] = "--game"; args[n++] = specs[i];
    }
    args[n] = NULL;
    launcher a = {0}; a.running = 1;
    int check = 0; const char *wanted = NULL;
    assert(!start(&a, n, args, &check, &wanted));
    /* The pack's titles in its order, opened on the first; a new player's
     * settings: dual-stick controls over a window and Match window. */
    assert(pack_count == 1 && a.game_count == 3 && !strcmp(a.games[0].slug, "ac3p") && a.pack == 0);
    assert(a.edit.number[LR_INPUT] == 2 && a.edit.number[LR_WINDOW] == 1 && a.edit.number[LR_RESOLUTION] == 1);
    const char *pages[PAGES_MAX];
    assert(group_pages(&a, GROUP_PACK, pages) == 2 && !strcmp(pages[0], "Graphics") && !strcmp(pages[1], "Controls"));

    /* Every page, at the Steam Deck's size. */
    assert(!open_window(&a));
    SDL_SetWindowSize(a.window, 1280, 800);
    a.group = GROUP_PLAYER; a.page = NULL;
    render(&a);
    int groups[4 * PAGES_MAX], at; const char *ring[4 * PAGES_MAX];
    const int count = page_ring(&a, groups, ring, &at);
    for (int i = 0; i < count; i++) {
        char name[160];
        snprintf(name, sizeof name, "page-%d-%s", groups[at], ring[at]);
        shot(&a, argv[1], name);
        step_page(&a, 1);
        page_ring(&a, groups, ring, &at);
    }
    a.confirm = CONFIRM_RESET; a.group = GROUP_PACK; a.page = "Controls";
    shot(&a, argv[1], "reset");
    a.confirm = CONFIRM_NONE;
    a.importer = "/bin/false";
    browser_open(&a, BROWSE_ISO);
    shot(&a, argv[1], "add-game");
    activate_back(&a);

    /* A change to each table, saved: the player's to [player], the pack's to
     * [pack last-raven]. */
    char error[PSP_SETTINGS_ERROR];
    assert(!psp_settings_set(&a.edit, LR_FPS_CAP, "144", PSP_SOURCE_FILE, error)); changed(&a);
    assert(!psp_settings_set(&a.edit, LR_WINDOW_MODE, "borderless", PSP_SOURCE_FILE, error)); changed(&a);
    shot(&a, argv[1], "unsaved");
    assert(!save(&a));
    assert(contains(settings, "[player]\nRESOLUTION=window\nWINDOW_MODE=borderless\n"));
    assert(contains(settings, "[pack last-raven]\nASPECT=native\nHIGH_FPS=0\nFPS_CAP=144\nINPUT=dual\n"));
    assert(contains(settings, "game=ac3p\n"));

    /* Save and play: the child runs from the title's save folder and exits
     * 0, which closes the launcher. */
    a.importer = NULL;
    select_game(&a, 2);
    launch_game(&a);
    assert(a.child);
    for (int i = 0; i < 500 && a.child; i++) { SDL_Delay(10); poll_child(&a); }
    assert(!a.child && !a.running);
    char saves[3200];
    snprintf(saves, sizeof saves, "%s/psprecomp/saves/aclr", data);
    struct stat st;
    assert(!stat(saves, &st) && S_ISDIR(st.st_mode));
    assert(contains(settings, "game=aclr\n"));

    close_window(&a);
    psp_settings_file_free(a.file);
    SDL_Quit();
    printf("launcher: Armored Core's pages drawn to %s, settings saved to its section, Save and play ran\n", argv[1]);
    return 0;
}
