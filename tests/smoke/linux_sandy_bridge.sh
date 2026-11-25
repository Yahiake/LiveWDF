#!/usr/bin/env bash
set -euo pipefail

artifacts="${1:?Pass the extracted Linux release directory}"
ulimit -c 0

xvfb-run -a -e /dev/stderr bash -euc '
    qemu-x86_64 -cpu SandyBridge "$1" &
    app_pid=$!
    trap '\''kill "$app_pid" 2>/dev/null || true; wait "$app_pid" 2>/dev/null || true'\'' EXIT
    for ((attempt = 0; attempt < 180; ++attempt)); do
        if ! kill -0 "$app_pid" 2>/dev/null; then
            wait "$app_pid" || exit "$?"
            echo "LiveWDF Standalone exited before opening a window" >&2
            exit 1
        fi
        if xdotool search --onlyvisible --name "^LiveWDF$"; then
            exit 0
        fi
        sleep 0.25
    done
    echo "LiveWDF Standalone did not open a window on Sandy Bridge" >&2
    exit 1
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
