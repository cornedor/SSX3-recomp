#!/bin/bash
# Build tools and headers for the Linux build without root (Fedora): the
# packages are downloaded with dnf and unpacked into ~/.local/ssx3-sysroot.
# With root, `sudo dnf install cmake ninja-build sdl2-compat-devel
# libepoxy-devel vulkan-loader-devel vulkan-headers glslc openssl-devel` does
# the same. Then:
#   source ~/.local/ssx3-sysroot/env.sh
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   ninja -C build-linux
set -e
R=$HOME/.local/ssx3-sysroot
T=$(mktemp -d)
mkdir -p "$R"
(cd "$T" && dnf download --resolve --setopt=cachedir="$T/cache" \
    cmake ninja-build sdl2-compat-devel libepoxy-devel vulkan-headers vulkan-loader-devel glslc rhash)
cd "$R"
for f in "$T"/*.x86_64.rpm "$T"/*.noarch.rpm; do
    case $(basename "$f") in
    *-devel-*|cmake*|ninja*|glslc*|vulkan-headers*|libglvnd*|xorg-x11-proto*|rhash*|spirv*|libshaderc*|glslang*)
        rpm2cpio "$f" | cpio -idm --quiet 2>/dev/null ;;
    esac
done
# the -devel .so links point at runtime libraries that live in /usr/lib64
cd "$R/usr/lib64"
for l in *.so; do
    t=$(readlink "$l" || true)
    [ -n "$t" ] && [ ! -e "$t" ] && [ -e "/usr/lib64/$t" ] && ln -sf "/usr/lib64/$t" "$l"
done
for f in /usr/lib64/libSDL2-2.0.so.0* /usr/lib64/libepoxy.so.0* /usr/lib64/libvulkan.so.1*; do
    ln -sf "$f" .
done
sed -i "s#^prefix=/usr#prefix=$R/usr#" pkgconfig/*.pc 2>/dev/null || true
cat > "$R/env.sh" <<E2
export PATH=$R/usr/bin:\$PATH
export LD_LIBRARY_PATH=$R/usr/lib64
export PKG_CONFIG_PATH=$R/usr/lib64/pkgconfig:$R/usr/share/pkgconfig
export CMAKE_PREFIX_PATH=$R/usr
E2
rm -rf "$T"
echo "sysroot ready: source $R/env.sh"
