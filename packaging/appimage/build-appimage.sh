#!/usr/bin/env bash
# Builds SpaceWindow-<version>-x86_64.AppImage: one file that runs on most
# Linux distributions, Qt and its plugins inside (the Linux counterpart of
# the Windows build's portable folder).
#
#   bash packaging/appimage/build-appimage.sh [output directory]
#
# Needs CMake 3.25, a C++20 compiler and Qt 6.4 or later (qmake6 on PATH, or
# QMAKE naming it).  Build on the oldest distribution the AppImage should run
# on (glibc is not bundled): CI uses Ubuntu 22.04.  Downloads linuxdeploy and
# its Qt plugin, pinned and checked against their SHA-256, into the build folder.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$root/out/appimage"}
build="$root/out/build/appimage"
appdir="$build/AppDir"
version=$(head -n 1 "$root/VERSION")
arch=$(uname -m)

linuxdeploy_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-$arch.AppImage"
qt_plugin_url="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-$arch.AppImage"
case "$arch" in
    x86_64)
        linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
        qt_plugin_sha256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
        ;;
    aarch64)
        linuxdeploy_sha256="620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff"
        qt_plugin_sha256="bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1"
        ;;
    *)
        echo "No pinned linuxdeploy checksums for $arch." >&2
        exit 1
        ;;
esac

fetch() {  # url, sha256, file
    if [[ ! -f "$3" ]] || ! echo "$2  $3" | sha256sum --check --status; then
        curl --fail --location --silent --show-error --output "$3" "$1"
        echo "$2  $3" | sha256sum --check --quiet
    fi
    chmod +x "$3"
}

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build "$build" --parallel "$(nproc)"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build"

tools="$build/tools"
mkdir -p "$tools" "$out"
fetch "$linuxdeploy_url" "$linuxdeploy_sha256" "$tools/linuxdeploy-$arch.AppImage"
fetch "$qt_plugin_url" "$qt_plugin_sha256" "$tools/linuxdeploy-plugin-qt-$arch.AppImage"

# Qt's plugins beyond the X11 platform and the portal platform theme (the
# desktop's file dialogs, light or dark) that the Qt plugin takes anyway:
# Wayland, with its shell, decoration and graphics plugins (which the plugin
# deploys for "waylandcompositor", the module they belong to).  Not every
# platform theme: Qt's GTK one would bring GTK itself into the AppImage.
# APPIMAGE_EXTRACT_AND_RUN: no FUSE needed (containers, CI).
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
export EXTRA_QT_MODULES="waylandcompositor"
export APPIMAGE_EXTRACT_AND_RUN=1
export LINUXDEPLOY_OUTPUT_VERSION="$version"
app=io.github.techieandroid.SpaceWindow
(
    cd "$out"
    "$tools/linuxdeploy-$arch.AppImage" --appdir "$appdir" --plugin qt --output appimage \
        --desktop-file "$appdir/usr/share/applications/$app.desktop" \
        --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/$app.png"
)
ls -l "$out"/SpaceWindow-*.AppImage
