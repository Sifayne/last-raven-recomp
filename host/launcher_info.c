/* What psprecomp's launcher shows of Armored Core besides its settings
 * (psprecomp/host/launcher.h); the pages come from host/settings.c. */
#include <psprecomp/host/launcher.h>
#include <stddef.h>

static const char *const titles[] = {"ac3p", "acsl", "aclr", NULL};

const psp_launcher psp_launcher_info = {
    .name = "Last Raven",
    .id = "last-raven",
    .about =
        "Armored Core PC runtime and launcher: MIT license.\n\n"
        "Audio uses FFmpeg libraries, copyright the FFmpeg contributors,\n"
        "under the GNU LGPL version 2.1 or later. https://ffmpeg.org/\n\n"
        "See licenses/ffmpeg/ for the license and notices, and the\n"
        "source/ package alongside the release for matching FFmpeg\n"
        "source and build instructions. Compatible modified shared\n"
        "libraries may be substituted.\n\n"
        "Original game code and assets belong to their rights holders.",
    .titles = titles,
    .new_preset = "WINDOW=1 RENDER=gl MPEG_DECODE=1",
    .reset_preset = "WINDOW=1",
};
