# Repackages a built Linux zip: rpmbuild -bb --target <arch> --define "ver <x.y.z>" --define "zip <unpacked zip>" livewdf.spec
%global debug_package %{nil}
%global __os_install_post %{nil}

Name:      livewdf
Packager:  Yahia Kemari <64375557+Yahiake@users.noreply.github.com>
Version:   %{ver}
Release:   1
Summary:   Real-time wave digital filter circuit simulator
License:   AGPL-3.0-only
URL:       https://github.com/Yahiake/LiveWDF
Requires:  alsa-lib freetype fontconfig libX11 libXext libXrandr libXinerama libXcursor libXrender libXi
Suggests:  pipewire-jack-audio-connection-kit

%description
The circuit editor, the standalone player and the VST3 plugin.

%install
LIVEWDF_PREFIX=%{_prefix} LIVEWDF_VST3DIR=%{_prefix}/lib/vst3 LIVEWDF_DESTDIR=%{buildroot} sh %{zip}/install.sh
install -Dm644 %{zip}/LICENSE %{buildroot}%{_datadir}/licenses/%{name}/LICENSE

%files
%{_bindir}/livewdf
%{_bindir}/livewdf-editor
%{_prefix}/lib/vst3/LiveWDF.vst3
%{_datadir}/applications/livewdf.desktop
%{_datadir}/applications/livewdf-editor.desktop
%license %{_datadir}/licenses/%{name}/LICENSE
