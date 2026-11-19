#!/bin/sh
# Installs LiveWDF for the current user from the unpacked zip; running it again upgrades in place.
# `--uninstall` removes it. Packages set PREFIX, VST3DIR and DESTDIR to stage a system install.
set -e
prefix=${PREFIX:-$HOME/.local}
bin=$DESTDIR$prefix/bin
apps=$DESTDIR$prefix/share/applications
vst3=$DESTDIR${VST3DIR:-$HOME/.vst3}

rm -rf "$bin/livewdf" "$bin/livewdf-editor" "$vst3/LiveWDF.vst3" "$apps/livewdf.desktop" "$apps/livewdf-editor.desktop"
[ "$1" = --uninstall ] && exit 0

cd "$(dirname "$0")"
mkdir -p "$bin" "$apps" "$vst3"
cp Standalone/LiveWDF "$bin/livewdf"
cp "Editor/LiveWDF Editor" "$bin/livewdf-editor"
cp -R VST3/LiveWDF.vst3 "$vst3/"
printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nCategories=AudioVideo;Audio;\n' \
    LiveWDF "$prefix/bin/livewdf" > "$apps/livewdf.desktop"
printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s %%f\nCategories=AudioVideo;Audio;\n' \
    "LiveWDF Editor" "$prefix/bin/livewdf-editor" > "$apps/livewdf-editor.desktop"
