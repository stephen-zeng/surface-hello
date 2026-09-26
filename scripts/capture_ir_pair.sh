#!/bin/sh
# Capture two GREY frames for passive or commanded-emitter comparison.
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
      --on-command COMMAND   command to request a verified IR emitter on
      --off-command COMMAND  command to request a verified IR emitter off
      --help

Without both commands, captures two consecutive frames and does not claim an
active/ambient pair. Commands alone do not prove illumination or frame sync.
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
emitter_maybe_on=0

cleanup() {
    status=$?
    trap - EXIT
    trap '' HUP INT TERM
    if [ "$emitter_maybe_on" -eq 1 ]; then
        if ! sh -c "$off_command"; then
            echo "capture_ir_pair: emitter off command failed during cleanup" >&2
            [ "$status" -ne 0 ] || status=1
        fi
    fi
    rm -f "$on_raw" "$off_raw" || true
    exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -n "$on_command" ] || [ -n "$off_command" ]; then
    [ -n "$on_command" ] && [ -n "$off_command" ] || {
        echo "capture_ir_pair: both emitter commands are required" >&2
        exit 2
    }
    # The on command may have changed hardware state even if it returns an error.
    emitter_maybe_on=1
    sh -c "$on_command"
    run_capture "$on_raw"
    sh -c "$off_command"
    emitter_maybe_on=0
    run_capture "$off_raw"
    mode=commanded-on-off
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

printf 'capture_ir_pair: mode=%s on=%s-on.pgm off=%s-off.pgm diff=%s-diff.pgm\n' \
    "$mode" "$prefix" "$prefix" "$prefix"
