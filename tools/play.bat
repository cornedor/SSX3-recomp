@echo off
rem Play SSX 3: start the recompiled build from the project root (it opens
rem game\default.xbe relative to the working directory), with the keyboard
rem enabled as player 1 on top of any XInput controller.
rem
rem   Keyboard (the game window must have focus):
rem     Enter      START          Backspace  BACK
rem     Z X A S    A B X Y        Q E        white / black
rem     1 3        left / right trigger
rem     arrows     d-pad          numpad 8/4/2/6   left stick
rem     I J K L    right stick    Shift / Ctrl     stick clicks
rem     F3         FPS counter      F11 / Alt+Enter  fullscreen
rem     F4         first-person shooter mode (mouse aims, left button fires)
rem     F5         18-speed gearbox: hold G + mouse = stick, Space = clutch,
rem                R = range, F = splitter
rem   An Xbox/XInput controller works too and needs no setting.
rem
rem Logs go to play_stdout.txt / play_stderr.txt (console output slows it);
rem earlier runs are kept in logs\ (tools\archive_logs.ps1).
setlocal
cd /d "%~dp0.."
if not defined RECOMP_KEYBOARD set RECOMP_KEYBOARD=1
rem tools\play.bat diag: also log frame times, renderer and guest-CPU stats
rem the threads' stacks whenever a frame takes over 50 ms in game, and the
rem most expensive GPU draws of any frame over 25 ms of GPU time
rem (python tools\slowstacks.py play_stderr.txt names them).
if /i "%1"=="diag" (
    set RECOMP_FRAME_STATS=1
    set RECOMP_SLOW_STACKS=50
    set RECOMP_VKR_STATS=1
    set RECOMP_GUESTCPU_STATS=1
    set RECOMP_APU_LEVEL=1
    set RECOMP_VKR_GPUPROF=25
    set RECOMP_FPS_OVERLAY=1
)
rem tools\play.bat fps: start in the first-person shooter mode (src/fps_mode.c).
if /i "%1"=="fps" set RECOMP_FPS=1
powershell -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0archive_logs.ps1'; Save-PreviousLogs '%CD%' 'play_'"
start "SSX 3" /b build\ssx3_recomp.exe > play_stdout.txt 2> play_stderr.txt
