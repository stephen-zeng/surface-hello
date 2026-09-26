#!/bin/sh
# Discover and configure the OV7251 media graph. The media node number can
# change across boots, so this intentionally scans all /dev/media* devices.
set -eu

runtime_file=/run/surface_ir_bridge_dev
found_media=

for media in /dev/media*; do
    [ -e "$media" ] || continue
    topology=$(media-ctl -d "$media" -p 2>/dev/null || true)
    printf '%s\n' "$topology" | grep -q 'ov7251' || continue

    sensor=$(printf '%s\n' "$topology" | sed -n 's/^-[^:]*: \(ov7251 [^ ]*\).*$/\1/p' | head -n1)
    csi=$(printf '%s\n' "$topology" | awk -v sensor="$sensor" '
        $0 ~ "entity .*: " sensor " " { in_sensor=1; next }
        in_sensor && /-> "ipu3-csi2 / {
            sub(/^.*-> "/, ""); sub(/":0.*$/, ""); print; exit
        }
        in_sensor && /^- entity / { exit }
    ')
    cio=$(printf '%s\n' "$topology" | awk -v csi="$csi" '
        $0 ~ "entity .*: " csi " " { in_csi=1; next }
        in_csi && /-> "ipu3-cio2 / {
            sub(/^.*-> "/, ""); sub(/":0.*$/, ""); print; exit
        }
        in_csi && /^- entity / { exit }
    ')
    [ -n "$sensor" ] && [ -n "$csi" ] && [ -n "$cio" ] || {
        echo "setup_ipu3: could not identify OV7251 CSI/CIO2 entities" >&2
        exit 1
    }

    media-ctl -d "$media" -l "\"$sensor\":0->\"$csi\":0[1]"
    media-ctl -d "$media" -V "\"$csi\":0 [fmt:Y10_1X10/640x480]"
    media-ctl -d "$media" -V "\"$csi\":1 [fmt:Y10_1X10/640x480]" 2>/dev/null || true
    video=$(media-ctl -d "$media" -e "$cio")
    case "$video" in
        /dev/video[0-9]*) ;;
        *) echo "setup_ipu3: invalid capture node: $video" >&2; exit 1 ;;
    esac
    printf '%s\n' "$video" > "$runtime_file"
    chmod 0644 "$runtime_file"
    printf 'setup_ipu3: media=%s sensor=%s capture=%s\n' "$media" "$sensor" "$video"
    found_media=1
    break
done

[ "$found_media" = 1 ] || {
    echo "setup_ipu3: OV7251 media entity not found" >&2
    exit 1
}
