#!/bin/sh
# Capture an ambient/active IR pair from the GREY bridge.
# The default mode only captures two consecutive frames: this machine has no
# verified emitter control. Hardware-specific commands must be supplied.
set -eu

device=/dev/video42
output=.
on_command=
off_command=

usage() {
    cat >&2 <<'USAGE'
Usage: capture_ir_pair.sh [options]

Options:
  -d, --device PATH          GREY V4L2 node (default: /dev/video42)
  -o, --output DIR           output directory (default: .)
      --on-command COMMAND   command to enable a verified IR emitter
      --off-command COMMAND  command to disable a verified IR emitter
      --help

Without both commands, captures two consecutive frames and does not claim an
active/ambient pair.
USAGE
}

run_capture() {
    raw=$1
    v4l2-ctl -d "$device" --stream-mmap --stream-count=1 --stream-to="$raw" >/dev/null
    [ "$(wc -c <"$raw")" -eq 307200 ] || {
        echo "capture_ir_pair: unexpected frame size in $raw" >&2
        exit 1
    }
}

make_pgm() {
    raw=$1
    pgm=$2
    python3 - "$raw" "$pgm" <<'PY'
import pathlib
import sys
raw = pathlib.Path(sys.argv[1]).read_bytes()
if len(raw) != 640 * 480:
    raise SystemExit(f"unexpected GREY frame length: {len(raw)}")
pathlib.Path(sys.argv[2]).write_bytes(b"P5\n640 480\n255\n" + raw)
PY
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -d|--device) device=$2; shift 2 ;;
        -o|--output) output=$2; shift 2 ;;
        --on-command) on_command=$2; shift 2 ;;
        --off-command) off_command=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "capture_ir_pair: unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

[ -e "$device" ] || { echo "capture_ir_pair: missing device: $device" >&2; exit 1; }
mkdir -p "$output"
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
prefix=$output/ir-$timestamp
on_raw=$prefix-on.raw
off_raw=$prefix-off.raw
trap 'rm -f "$on_raw" "$off_raw"' EXIT HUP INT TERM

if [ -n "$on_command" ] || [ -n "$off_command" ]; then
    [ -n "$on_command" ] && [ -n "$off_command" ] || {
        echo "capture_ir_pair: both emitter commands are required" >&2
        exit 2
    }
    sh -c "$on_command"
    run_capture "$on_raw"
    sh -c "$off_command"
    run_capture "$off_raw"
    mode=active-ambient
else
    run_capture "$on_raw"
    run_capture "$off_raw"
    mode=consecutive
fi

make_pgm "$on_raw" "$prefix-on.pgm"
make_pgm "$off_raw" "$prefix-off.pgm"

python3 - "$on_raw" "$off_raw" "$prefix-diff.pgm" <<'PY'
import pathlib
import sys
on = pathlib.Path(sys.argv[1]).read_bytes()
off = pathlib.Path(sys.argv[2]).read_bytes()
if len(on) != len(off) or len(on) != 640 * 480:
    raise SystemExit("input frames have different or invalid sizes")
diff = bytes(abs(a - b) for a, b in zip(on, off))
pathlib.Path(sys.argv[3]).write_bytes(b"P5\n640 480\n255\n" + diff)
PY

rm -f "$on_raw" "$off_raw"
trap - EXIT HUP INT TERM
printf 'capture_ir_pair: mode=%s on=%s-on.pgm off=%s-off.pgm diff=%s-diff.pgm\n' \
    "$mode" "$prefix" "$prefix" "$prefix"
