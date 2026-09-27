#!/usr/bin/env python3
"""Check a GREY V4L2 stream for short frames and repeated image payloads.

The capture is kept in a temporary file and removed on exit. This is a
passive-stream diagnostic, not an illumination, depth, or liveness test.
"""

import argparse
import hashlib
import json
import pathlib
import statistics
import subprocess
import sys
import tempfile


def positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/video42")
    parser.add_argument("--frames", type=positive_int, default=120)
    parser.add_argument("--width", type=positive_int, default=480)
    parser.add_argument("--height", type=positive_int, default=640)
    parser.add_argument("--timeout", type=positive_int, default=30)
    args = parser.parse_args()

    frame_size = args.width * args.height
    with tempfile.NamedTemporaryFile(prefix="surface-ir-check-", suffix=".raw") as capture:
        command = [
            "v4l2-ctl", "-d", args.device, "--stream-mmap",
            f"--stream-count={args.frames}", f"--stream-to={capture.name}",
        ]
        try:
            subprocess.run(
                command, check=True, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, timeout=args.timeout,
            )
        except FileNotFoundError:
            print("check_ir_stream: v4l2-ctl is not installed", file=sys.stderr)
            return 1
        except subprocess.TimeoutExpired:
            print("check_ir_stream: capture timed out", file=sys.stderr)
            return 1
        except subprocess.CalledProcessError as error:
            detail = error.stderr.decode(errors="replace").strip()
            print(f"check_ir_stream: capture failed: {detail}", file=sys.stderr)
            return 1

        data = pathlib.Path(capture.name).read_bytes()

    expected = args.frames * frame_size
    if len(data) != expected:
        print(
            f"check_ir_stream: expected {expected} bytes, got {len(data)}",
            file=sys.stderr,
        )
        return 1

    view = memoryview(data)
    frames = [view[offset:offset + frame_size]
              for offset in range(0, len(data), frame_size)]
    hashes = [hashlib.sha256(frame).hexdigest() for frame in frames]
    means = [sum(frame) / frame_size for frame in frames]
    changed = [
        sum(left != right for left, right in zip(previous, current))
        for previous, current in zip(frames, frames[1:])
    ]
    report = {
        "device": args.device,
        "format": "GREY8",
        "width": args.width,
        "height": args.height,
        "frames": len(frames),
        "bytes": len(data),
        "unique_frame_hashes": len(set(hashes)),
        "first_frame_sha256": hashes[0],
        "last_frame_sha256": hashes[-1],
        "mean_brightness_min": min(means),
        "mean_brightness_max": max(means),
        "changed_pixels_min": min(changed) if changed else 0,
        "changed_pixels_median": statistics.median(changed) if changed else 0,
        "changed_pixels_max": max(changed) if changed else 0,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
