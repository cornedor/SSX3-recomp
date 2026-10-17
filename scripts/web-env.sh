# Source before building for the web: Emscripten + cmake/ninja.
source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
if [ -d "$HOME/.local/ssx3-sysroot/usr/bin" ]; then
    export PATH="$PATH:$HOME/.local/ssx3-sysroot/usr/bin"
    export LD_LIBRARY_PATH="$HOME/.local/ssx3-sysroot/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
