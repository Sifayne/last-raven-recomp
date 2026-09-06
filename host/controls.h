#ifndef LAST_RAVEN_CONTROLS_H
#define LAST_RAVEN_CONTROLS_H

/* Extra physical controls carried in bits the game's PSP-button converter
 * does not consume.  They still pass through sceCtrl, and therefore through
 * the recorder/replayer, but psp_func_00279A10 strips them before handing the
 * ordinary PSP buttons to the game.  HOME (0x10000) and HOLD (0x20000) are
 * deliberately skipped: the game reads those two before the converter. */
enum {
    LR_PAD_A     = 0x00000400u,
    LR_PAD_B     = 0x00000800u,
    LR_PAD_X     = 0x00040000u,
    LR_PAD_Y     = 0x00080000u,
    LR_PAD_LB    = 0x00100000u,
    LR_PAD_RB    = 0x00200000u,
    LR_PAD_LT    = 0x00400000u,
    LR_PAD_RT    = 0x00800000u,
    LR_PAD_L3    = 0x01000000u,
    LR_PAD_R3    = 0x02000000u,
    LR_PAD_EXTRA = 0x03FC0C00u,
};

#endif
