#!/bin/bash
# Play SSX 3 on Linux (Wayland or X11): starts build-linux/ssx3_recomp from
# the project root (it opens game/default.xbe relative to the working
# directory) with the keyboard as player 1 on top of any controller.
#
#   Keyboard (game window focused): Enter START, Backspace BACK, Z X A S =
#   A B X Y, Q E white/black, 1 3 triggers, arrows d-pad, numpad 8/4/2/6
#   left stick, I J K L right stick, F3 FPS counter, F11 / Alt+Enter
#   fullscreen.
#
# On a Wayland session the window is native Wayland (RECOMP_WAYLAND=0: X11
# through Xwayland). The remaster look is on (RECOMP_REMASTER=0: the plain
# one; each part has its own flag, see src/main.c). Any RECOMP_* variable
# set in the environment wins.
#   tools/play.sh diag   also log frame times and GPU cost per frame
# Logs: play_stdout.txt / play_stderr.txt.
cd "$(dirname "$0")/.." || exit 1
export RECOMP_KEYBOARD=${RECOMP_KEYBOARD:-1}
if [ "$1" = diag ]; then
    export RECOMP_FRAME_STATS=1 RECOMP_VKR_STATS=1 RECOMP_VKR_GPUPROF=25 RECOMP_FPS_OVERLAY=1
fi
exec build-linux/ssx3_recomp > play_stdout.txt 2> play_stderr.txt
