#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

# Runs the packaged executable's actual NSApplication delegate path. Requires
# an interactive macOS GUI session because the test asserts a visible window.
./build.sh
log="${TMPDIR:-/tmp}/aes67-manager-window-self-test.log"
python3 - "$PWD/AES67Manager.app/Contents/MacOS/AES67Manager" "$log" <<'PY'
import pathlib, subprocess, sys

binary, log_path = sys.argv[1:]
with open(log_path, "w") as log:
    process = subprocess.Popen([binary, "--self-test-window"], stdout=log, stderr=subprocess.STDOUT)
    try:
        status = process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        with open(log_path + ".stack", "w") as stack:
            try:
                subprocess.run(["/usr/bin/sample", str(process.pid), "5", "1", "-file", log_path + ".stack"],
                               stdout=stack, stderr=subprocess.STDOUT, timeout=8, check=False)
            except subprocess.TimeoutExpired:
                stack.write("sample timed out\n")
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        print(f"Window startup self-test timed out; diagnostics: {log_path} and {log_path}.stack", file=sys.stderr)
        sys.exit(124)

text = pathlib.Path(log_path).read_text(errors="replace")
print(text, end="")
if status != 0:
    print(f"Window startup self-test failed (exit {status}); diagnostics: {log_path}", file=sys.stderr)
    sys.exit(status or 1)
if not any(line.startswith("WINDOW_SELF_TEST status=pass visible=true frame=") for line in text.splitlines()):
    print(f"Window startup self-test produced no passing frame diagnostic; diagnostics: {log_path}", file=sys.stderr)
    sys.exit(12)
print(f"Window startup self-test passed; diagnostics: {log_path}")
PY
echo "Window startup self-test passed; diagnostics: $log"
