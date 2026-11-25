#!/usr/bin/env bash
set -euo pipefail

artifacts="${1:?Pass the CMake build directory}/plugin/LiveWDF_Plugin_artefacts/Release"
ulimit -c 0

xvfb-run -a bash -euc '
    qemu-x86_64 -cpu SandyBridge "$1" &
    app_pid=$!
    trap '\''kill "$app_pid" 2>/dev/null || true; wait "$app_pid" 2>/dev/null || true'\'' EXIT
    timeout 45s xdotool search --sync --onlyvisible --name "^LiveWDF$"
    kill -0 "$app_pid"
' bash "$artifacts/Standalone/LiveWDF"

# The manifest helper does not load the VST3 module. Load the shipped binary
# and query its factory entry point through the same ABI a host uses.
qemu-x86_64 -cpu SandyBridge /usr/bin/python3 - "$artifacts/VST3/LiveWDF.vst3/Contents/x86_64-linux/LiveWDF.so" <<'PY'
import ctypes
import os
import sys

module = ctypes.CDLL(os.path.abspath(sys.argv[1]))
module.ModuleEntry.argtypes = [ctypes.c_void_p]
module.ModuleEntry.restype = ctypes.c_bool
module.GetPluginFactory.argtypes = []
module.GetPluginFactory.restype = ctypes.c_void_p
module.ModuleExit.argtypes = []
module.ModuleExit.restype = ctypes.c_bool
assert module.ModuleEntry(module._handle), "VST3 module entry failed"
assert module.GetPluginFactory(), "VST3 factory unavailable"
assert module.ModuleExit(), "VST3 module exit failed"
print("LiveWDF VST3 factory loaded on Sandy Bridge")
PY
