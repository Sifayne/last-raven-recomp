/* What psprecomp's launcher shows of Armored Core besides its settings
 * (psprecomp/host/launcher.h); the pages come from host/settings.c. Its
 * earlier name is the one this game's own launcher kept settings under,
 * which psprecomp's brings in once. */
#include <psprecomp/host/launcher.h>
#include <stddef.h>

static const char *const titles[] = {"ac3p", "acsl", "aclr", NULL};
static const char *const names[] = {"Armored Core 3 Portable", "Armored Core: Silent Line",
                                    "Armored Core: Last Raven", NULL};

const psp_launcher psp_launcher_info = {
    .name = "Armored Core",
    .earlier = "Last Raven",
    .about =
        "The Armored Core pack -- its host code, controls and higher frame rates for "
        "Armored Core 3 Portable, Silent Line and Last Raven -- is MIT licensed. "
        "Original game code and assets belong to their rights holders.",
    .titles = titles,
    .names = names,
};
