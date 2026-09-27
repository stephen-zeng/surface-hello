# Surface IR implementation

This repository contains the reproducible part of the Surface Pro 4 IR
camera experiment:

```text
OV7251 -> IPU3 CSI-2 -> CIO2 (/dev/videoN) -> ip3y bridge -> /dev/video42 -> Gaze
```

The kernel currently exposes the sensor as `ov7251 3-0060` and the CIO2 node
as `/dev/video2` on this machine. Device numbering is not stable, so
`scripts/setup_ipu3.sh` discovers the media device and writes the actual
capture node to `/run/surface_ir_bridge_dev` on every service start.

## Build and install

```sh
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now surface-ir-camera.service
```

If an older `v4l2loopback` instance is already loaded, reload it once so the
new `exclusive_caps=0` setting takes effect (this briefly removes `/dev/video42`):

```sh
sudo systemctl stop surface-ir-camera.service
sudo modprobe -r v4l2loopback
sudo modprobe v4l2loopback
```

Check the bridge and loopback formats before pointing Gaze at the node:

```sh
cat /run/surface_ir_bridge_dev
v4l2-ctl -d /dev/video42 --all
journalctl -u surface-ir-camera.service -b
gst-launch-1.0 -q v4l2src device=/dev/video42 num-buffers=1 \
  ! video/x-raw,format=GRAY8,width=480,height=640 ! fakesink
```

The bridge converts 640x480 IPU3 packed monochrome to 480x640 `GREY`, rotating
the image 90 degrees counterclockwise. It does not control the IR emitter;
the separately verified Gaze I2C profile handles that operation.
The setup script applies the OV7251 driver's advertised maximum exposure and
a conservative analogue gain (`1704` and `512`); adjust them with `v4l2-ctl`
if a different lighting environment needs it.

To save two frames for inspection, install `capture_ir_pair.sh` and run:

```sh
sudo capture_ir_pair.sh --output ~/sp4-ir-debug
```

This produces two PGM frames and an absolute-difference image. They are
labelled `consecutive` because no emitter control is assumed. Once a verified
hardware control command exists, pass both `--on-command` and `--off-command`
to request on/off states around two captures. The script attempts the off
command on any exit after the on command starts, including capture failure or
a handled signal. Its `commanded-on-off` label records requested states, not
measured illumination or frame synchronization; it reports only image
difference and does not interpret it as depth or liveness.

## Safe strobe investigation status (2026-09-26)

The upstream Linux `ov7251.c` mode tables initialise registers in the
`0x3b80`--`0x3b96` range, which is useful evidence that the sensor has a
strobe-related register block. The same driver exposes only exposure, gain,
blanking, flip, test-pattern, link-frequency and pixel-rate controls. It does
not expose a V4L2 flash/strobe control or an emitter backend. The live OV7251
subdevice likewise reports only `MEDIA_BUS_FMT_Y10_1X10` and those image
controls.

`i2c-tools` is installed for read-only investigation. A root probe of
`i2c-3`, address `0x60`, register `0x3b80` was refused with `Device or
resource busy` because the kernel sensor driver owns the device. The probe did
not use `-f`, did not stop the camera service, and performed no write. This
means the register values remain unverified on this machine; the project must
not turn the upstream mode-table values or another Surface model's dump into a
write sequence.

The pre-rotation-fix passive capture produced two `640x480` GREY frames with payload
statistics of min `0`, max `255`, mean `48.759` for both frames. Their absolute
difference was zero. This confirms a valid, stable passive stream under the
then-current setup, but it is not an active/ambient result and says nothing about
emitter state, depth, or liveness.

A follow-up continuous-stream check captured 120 frames from `/dev/video42`.
The output was exactly `36,864,000` bytes (`120 * 307,200`), with non-zero
samples and no short-frame result. A separate GStreamer run consuming 120
`GRAY8` frames also exited successfully. The loopback node accepts one active
consumer at a time, so concurrent consumers can correctly report `Device busy`;
the successful serial runs are the relevant result.

Run `python3 scripts/check_ir_stream.py --frames 120` to check that the
loopback payload changes as well as having the expected byte count. It captures
to a temporary file that is deleted on exit and prints JSON with frame hashes,
brightness range, and changed-pixel counts. Two runs on this machine each
produced 120 distinct frame hashes out of 120. In the second run, consecutive
frames differed in 274,709–275,556 pixels out of 307,200; mean brightness
ranged from 10.288 to 10.418 under the then-current lighting. This guards
against an exactly repeated loopback payload in those windows, not replay,
face motion, active illumination, or liveness. A source-side `SIGUSR1` check
during the first run showed `input/output` increasing from 17,448 to 17,567,
with no new sequence anomaly, estimated missing frame, or flagged error.

The bridge can report cumulative CIO2 source statistics without changing the
stream. Send `SIGUSR1` to its systemd `MainPID` before and after a capture:

```sh
pid=$(systemctl show -P MainPID surface-ir-camera.service)
sudo kill -USR1 "$pid"
gst-launch-1.0 -q v4l2src device=/dev/video42 num-buffers=120 \
  ! video/x-raw,format=GRAY8,width=480,height=640 ! fakesink
sudo kill -USR1 "$pid"
sudo journalctl -u surface-ir-camera.service -b --no-pager | grep 'stats input='
```

Each stats line reports source frames dequeued, GREY frames written, brightness
filtered frames, sequence anomalies, estimated missing sequence numbers,
kernel-flagged error buffers, and the last source sequence/timestamp. The
missing count is an estimate based on forward sequence gaps; it does not claim
anything about emitter timing. `--debug` additionally prints each source
sequence, timestamp, flags and sampled brightness.

In one 120-frame GStreamer validation window, the source counters advanced
from `input=468 output=468` to `input=594 output=594`: 126 input and output
frames, zero new sequence anomalies, zero estimated missing frames and zero
kernel-flagged error buffers. This completes the runbook's 120-frame passive
CIO2/bridge check for that window, not an active IR or long-duration test.

`surface-ir-capture` records packed CIO2 frames and the corresponding V4L2
dequeue sequence, monotonic EOF timestamp, frame size, and SHA-256 in a local
archive. It also snapshots OV7251 exposure/gain before capture; these are not
per-frame control measurements. The capture pauses and restores the bridge
only when `--stop-bridge` is given:

```sh
surface-ir-capture --mode passive --frames 120 --stop-bridge \
  --out "$HOME/sp4-ir-debug/passive-$(date -u +%Y%m%dT%H%M%SZ)"
```

The output directory must be new and outside every Git worktree. It is mode
`0700`, with raw files mode `0600`; partial output is removed on failure. The
tool refuses `--mode active-pair` until a real emitter backend and illumination
metadata exist. Its `passive-uncommanded` label never asserts that the emitter
was physically off.

On this machine a direct 120-frame run produced 47,923,200 bytes of `ip3y`
(`120 * 399,360`), 120 distinct raw-frame hashes, contiguous source sequences
1–120, and 120 monotonic timestamps. Intervals ranged from 32,826 to 33,904
microseconds (30.002 fps from first to last), with a static control snapshot
of exposure 1704 and analogue gain 512. Illumination remains `not_measured`.
An invalid-device failure and SIGTERM during streaming both restored the
bridge and left no archive. A controlled bridge process kill incremented
systemd's restart count from 0 to 1; a new process then delivered 30 distinct
GREY frames. This validates process recovery, not every possible input fault.

The capture tool now checks the six reserved bits at the end of every 25-pixel
`ip3y` group before hashing frames. A 60-frame live CIO2 capture checked
`60 * 480 * 26 = 748,800` reserved bytes and found zero non-zero masks; all
60 packed-frame hashes were distinct, with source timestamps corresponding to
30.005 fps. The check is a packing-integrity guard only. It does not measure
illumination, identify an emitter, or prove frame-level synchronization.

The extracted 2016 Windows package gives a more specific software-side clue:
`IntelCameraPlugin64.dll` identifies this module as `OV7251`, `MSHW0072`,
`MONO IR` and exports `IAdvCIFlashControl`; `iacamera64.sys` contains
`IsFlashTriggered`, `GetFrameFlashStage` and `IsFrameIlluminated` input-frame
metadata paths. This supports the model of a separate flash controller plus
illumination metadata carried through the camera pipeline. It still does not
identify the Pro 4 emitter's GPIO, I2C device, or sensor-strobe wiring:
`SkcController.inf` only binds the generic `ACPI\INT3472` controller, and no
Surface-specific flash resource is present in the package. These strings are
therefore implementation clues, not a Linux control recipe.

The referenced linux-surface issue is explicitly labelled for Surface Book 2.
Its dump reports changing `0x3b8e`/`0x3b8f`, while the discussion questions
whether those changes drive that platform's LED; a separate comment reports a
very dim sensor-strobe response on Surface Go 2. The issue also notes that the
sensor must be streaming for the strobe-related access to work. None of these
observations identifies the Pro 4 wiring, so they remain comparative evidence
only.

## Read-only Windows register-table analysis (2026-09-26)

The extracted `ov7251.sys` was inspected without loading it or sending any
device commands. Four repeated 137-record tables use a 16-byte record layout
with a register address and an 8-bit value. Each table contains the contiguous
`0x3b80`--`0x3b8f` and `0x3b94`--`0x3b96` ranges. The first two tables are
identical; the latter two change several mode values, including `0x3b81`,
`0x3b8b`, `0x3b8e`, `0x3b8f`, and `0x3b96`.

This is stronger evidence that the Windows package carries multiple OV7251
strobe-related sensor mode tables. It is still only static evidence: the file
does not identify which table the Pro 4 selects at runtime, whether the sensor
strobe is wired to an LED on this platform, or what frame metadata accompanies
it. No value from these tables is used by the Linux bridge, and no register
write is permitted based on this analysis.

The bridge unpacker was also checked with an independent synthetic packer for
all 640 ten-bit pixel positions, including the 25-pixel group boundary. Every
decoded byte matched `value >> 2` (maximum error zero). This validates the
bit-layout test path; it does not establish that every reserved/padding bit in
a live CIO2 frame has a particular value.

## IR emitter investigation

On this Surface Pro 4, the DSDT identifies the OV7251 as `INT347E`/`CAM3` at
I2C address `0x60`. Its `INT3472` dependency (`SKC2`) lists two GPIO resources:
pin `0x4f` has function `0x0c` (clock enable), and pin `0x50` has function
`0x00` (reset). These function codes match the Linux INT3472 driver's GPIO
type mapping. None of the twelve static and dynamic SSDTs adds a camera or
emitter definition.
The kernel exposes privacy LEDs, but no `ir_flood` LED or V4L2 flash control.
Thus neither of these two GPIOs is evidence of an emitter control line.

## ACPI control-logic resolution (2026-09-26)

The live ACPI dump resolves the dependency chain for this exact machine:

```text
CAM3 (INT347E, MSHW0072, OV7251 at 0x60)
  _DEP -> SKC2 (INT3472, UID 2)
  SKC2 CLDB -> version 0, control_logic_type 1 (DISCRETE), id 2, SKU 0x20
  SKC2 GPIOs -> pin 0x4f / type 0x0c (clock enable)
                 pin 0x50 / type 0x00 (reset)
```

The kernel header defines control-logic type `1` as `DISCRETE(CRD-D)` and
type `2` as `PMIC TPS68470`. On this machine `INT3472:02` is bound to
`int3472-discrete`, not the TPS68470 backend. The live GPIO debug output shows
the camera's clock/reset/power GPIO consumers, while `/sys/class/leds`
contains only privacy LEDs and no flash, torch, or `ir_flood` endpoint.

Upstream `intel_skl_int3472_discrete` maps ACPI GPIO type `0x02` (strobe) to
an `ir_flood` LED. The Surface Pro 4 `SKC2` `_DSM` returns only `0x01004F0C`
(clock enable) and `0x01005000` (reset); it has no type `0x02` strobe entry.
This is direct ACPI evidence that the known INT3472 GPIO path is not the IR
emitter path on this machine.

The other two live `INT3472` devices were also checked in the same DSDT.
The front RGB `CAMF` (OV5693, `MSHW0070`) depends on `SKC1` (UID 1), whose
`_DSM` GPIO entries are `0x0100540C`, `0x01004D00`, and `0x0100160D`.
The rear RGB `CAMR` (OV8865, `MSHW0071`) depends on `SKC0` (UID 0), whose
entries are `0x0100530C`, `0x01004E00`, and `0x01002B0D`. The live ACPI bus
lists exactly `INT3472:00`, `:01`, and `:02`; none of their declared GPIO
functions is type `0x02` (strobe). This narrows the ACPI evidence to all
three controllers, but does not identify the emitter or exclude a separate
control path.

This rules out treating the Windows package's generic TPS68470 flash symbols
as a verified control path for this Surface Pro 4. The two ACPI GPIOs are
camera power sequencing resources; they are not an identified IR emitter
interface. Active illumination still requires a separate hardware trace,
Windows runtime trace, or direct optical/electrical observation.

The DSDT also contains an `MSHW0085` device (`CWHD`) without a resource or
control method in that device declaration. Microsoft's `Surface Camera Windows
Hello` package for `ACPI\MSHW0085` (Update Catalog ID
`c22078bc-ab45-43b6-bd98-670a700ac358`, driver version `1.0.45.0`) installs
`FaceMF.Provider.dll` and registers
`FaceMF.SourceProvider` as a face authentication source. The DLL has diagnostic
strings for infrared frames and a missing *illumination attribute* in a frame
sample. The package's INF specifies no emitter GPIO or I2C device, so it does
not identify the hardware control path. This supports investigating the lower
Windows camera driver and its frame metadata next. A Windows trace or direct
emitter observation is still needed to establish control and timing before
active/ambient frame pairs or depth cues can be produced by this bridge.

The 2016 Intel camera package listed as `Microsoft IR Camera Front` in the
Microsoft Update Catalog (update `f2ed5505-374c-4a30-8c96-138f6bc73fa3`)
is more specific to this device. Its `ov7251.inf` matches `INT347E` with
subsystem `MSHW0072` and installs `OV7251_MSHW0072_SKY.cpf` plus a matching
pipeline configuration. It also installs `SkcController.sys` for `INT3472`
and `iacamera64.sys` for the Intel AVStream camera. The latter binary contains
`IRFlashLedIntensity`, `TriggerRollingShutterIRByCIO2`, and
`IRRollingShutterFlashController` identifiers. These are evidence of an Intel
IR flash control path, but do not establish which signal drives this Surface's
emitter or the register sequence. The current Linux IPU3 CIO2 driver exposes
no corresponding flash control. The next investigation is to identify the
Windows driver's actual trigger and frame metadata behavior before adding a
Linux emitter backend.

Microsoft's Windows Hello camera driver guide defines two mutually exclusive
FaceAuth modes: alternating illuminated/ambient frames, or a driver-produced
ambient-subtracted frame. `MF_CAPTURE_METADATA_FRAME_ILLUMINATION` marks each
frame in the alternating mode. The actual mode used by this Surface Pro 4 has
not been observed. A Windows runtime trace must identify its
`KSPROPERTY_CAMERACONTROL_EXTENDED_FACEAUTH_MODE` setting and, if alternating,
compare illumination metadata with an optical measurement before a Linux
capture path claims matched active/ambient pairs. See the Microsoft
[bring-up guide](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/windows-hello-camera-driver-bring-up-guide)
and [FaceAuth mode property](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/ksproperty-cameracontrol-extended-faceauth-mode).
The guide's minimum is 15 illuminated plus 15 ambient frames per second for
alternating mode, or 15 ambient-subtracted frames per second, at 320x320 or
better. The present approximately 30 fps passive GREY stream is not evidence
that either active mode meets this target.

The reference machine's current internal disk has only Debian EFI, ext4, and
swap partitions. Its firmware still lists a Windows Boot Manager entry, but
there is no Windows system partition available for a Hello runtime trace in
this session. Static INF and binary inspection cannot establish the emitted
light waveform or its frame association.

Static strings in the same package provide a narrower clue: `ov7251.sys`
contains the sensor-side modes `Strobe`, `Torch`, and `Flash`, while
`SkcController.sys` contains `TPS68470` flash methods for both strobe and I2C
command operation, plus GPIO operations. This confirms that the Windows
camera stack has generic flash-controller support. It still does not prove
that this Pro 4 routes its emitter through TPS68470, nor does it provide a
safe Linux GPIO, I2C address, or register sequence. No such values are used by
this project.

The Intel AVStream binary adds the same architectural clue: it has a
`FlashControllerDriverProxy`, `PMIC FLASH Driver`, `Trigger IR Flash`,
`IRRollingShutterFlashController::SetStrobePattern`, and a
`flash_gpio_pin` configuration field. This indicates that Windows expected a
separate flash-controller device and carried the trigger metadata through the
camera pipeline. The package still contains no Surface-Pro-4-specific binding
from that proxy to an emitter, so it is not enough to implement a Linux
backend.

The package's `OV7251_MSHW0072_SKY_pipeCfg.bin` and generic
`OV7251_5SF010T2_SKY_pipeCfg.bin` are byte-identical (7,264 bytes, SHA-256
`f70c8d3d4a85fd76b7a90b6ec9e9faa36dd56a068ccdf71b11d2daffa8d04a28`).
The two `.cpf` files differ, but their undocumented binary contents have not
been mapped to an emitter. The matching pipeline files supply no
Surface-specific control sequence.

Additional upstream evidence is available in linux-surface issue #739
(`cameras/ov7251: Register dump for strobe`). The dump was captured from
Windows on a Surface Book 2, not this Surface Pro 4, and the discussion says
the sensor strobe can trigger an IR LED on that platform only while streaming.
It also notes a separate TPS68470 flash/torch timeout and possible `S_STROBE`
trigger. The issue is useful for identifying the OV7251 strobe block, but it is
not a safe register recipe for this machine: the Pro 4's ACPI resources and
illumination controller have not been matched to it. No register writes are
made by this project.

## Gaze configuration order

Enrollment is deferred by the user. Validate the camera and daemon with
`gaze doctor`, then make a timestamped backup of `/etc/gaze/config.toml` and
set:

```toml
[cameras]
rgb = "primary"
ir = "/dev/video42"
emitter_enabled = false
parallel_capture = "never"
```

Restart `gazed`, run `gaze doctor`, and inspect `journalctl -u gazed`. Perform
enrollment and `gaze auth` only when the user opts in. Existing PAM/GDM entries
are installation state, not an authentication test; do not expand them until
interactive authentication and TTY/password fallback have both been tested.

On the reference machine, `gaze doctor --benchmark` runs the detector, RGB and
IR recognizers, and MiniFASNet liveness model successfully on CPU. This proves
the inference path is usable; it does not replace a real enrollment and
`gaze auth` test in front of the camera.

The released Gaze package supplies RGB MiniFASNet liveness and an IR camera
input, but it has no depth stream API. The OV7251 is a single monochrome
camera and this Surface exposes no verified depth or IR torch device. The
current configuration is therefore passive IR plus software liveness; it does
not claim Windows Hello structured-light depth or active-IR anti-spoofing.
That requires a verified emitter backend and calibration, followed by Gaze
support for an active response/depth signal.

Gaze's emitter blaster is also specifically a UVC path: it resolves a USB
camera, probes the Microsoft Face Authentication extension-unit control, and
uses a built-in VID:PID/profile table. The Surface OV7251 is an I2C/IPU3
sensor, and the `/dev/video42` loopback node has no extension-unit controls;
`v4l2-ctl --list-ctrls` shows only loopback queue controls. Setting
`emitter_enabled = true` therefore cannot control this camera without adding a
separate, hardware-specific backend.

## TPM protected storage

The Surface TPM is TPM 2.0 (`/dev/tpmrm0`, Infineon SLB9665). Debian's
`tpm2-tools` can query it as root. Gaze can seal face templates to the TPM:

```toml
[storage]
encrypt_templates = true
unlock_gnome_keyring = false
```

After enabling it, restart `gazed`, run `gaze doctor`, and re-enroll templates
if Gaze requests it. Encrypted templates are bound to this TPM and may not be
recoverable after motherboard/TPM replacement. Keep a tested password login
and a root recovery path before enabling this setting.

`unlock_gnome_keyring = true` is a separate opt-in. It requires encrypted
templates, liveness, and `sudo gaze keyring`; enable it only after face auth
works reliably and the keyring password has been enrolled interactively.

## Verification on the reference Surface Pro 4

On 2026-09-26 the installed service was active and reported `/dev/video2` as
the current CIO2 input. Before the rotation was restored, that bridge exposed
640x480 `GREY` frames (307,200 bytes each) on `/dev/video42`.
`capture_ir_pair.sh` successfully saved two
consecutive frames and a PGM absolute-difference image; the observed
difference was zero while no emitter command was supplied.

`gaze doctor --benchmark` completed with 26 passed, 1 optional feature off, 2
warnings, and 0 errors. The warnings were expected while enrollment is deferred: the
GNOME extension is installed but not enabled, and no faces are enrolled. The
TPM, daemon, PAM installation, camera nodes, and encrypted-template setting
were all accepted by the doctor. Interactive enrollment and authentication
remain intentionally unperformed.

Running `gaze auth --verbose --user stephenzeng` without enrollment exits
cleanly with `No faces enrolled`; it does not alter the camera service or
configuration. PAM/GDM fallback remains untested until a face is enrolled.

The benchmark measured the local inference path on CPU: face detector 6.7 ms
average, RGB recognizer 12.0 ms, IR recognizer 11.4 ms, and MiniFASNet
liveness 2.7 ms. These timings verify that the model components are usable;
Gaze's current IR authentication path uses eye-motion liveness rather than
the RGB MiniFASNet model. They do not verify enrollment, active IR
illumination, depth, or Windows Hello equivalence.

## Current Gaze and login integration status (2026-09-26)

The installed Gaze 0.3.3 configuration has `ir = "/dev/video42"`,
`emitter_enabled = false`, `encrypt_templates = true`, liveness enabled, and
GNOME Keyring unlock disabled. After restarting `gazed`, its journal reported
"Template encryption enabled (AES-256-GCM under a TPM-sealed key)". This
confirms the daemon initialised TPM-backed encryption; there is no enrolled
face template yet, so encrypted-template storage and recovery have not been
tested with user data.

The Gaze source loads and unseals the existing DEK when both sealed blobs are
present. On this machine `/var/lib/gaze/tpm` is root-owned mode `0700`, and
`dek.pub`/`dek.priv` are root-owned mode `0600`. Both blobs retained their
10:26 creation/modification time across the 22:20 `gazed` restart, whose log
again confirmed template encryption. This verifies reuse of the persistent
TPM-sealed key across a daemon restart. It does not verify an enrolled template
or a recovery path after TPM reset/replacement.

The current installation references `pam_gaze.so` in `common-auth`, a direct
`polkit-1` entry, and `gdm-face`; GDM's dconf override requests face
authentication. `common-auth` still includes `pam_unix.so` after Gaze, but
actual password, TTY, and GDM fallback have not been exercised interactively.
The GNOME extension files are installed, while the current Shell session does
not list or enable the extension. These existing settings are installation
state, not proof of a working face login. No further PAM/GDM changes or face
enrollment are part of this validation.

One `gaze doctor --benchmark` invocation reported a 30-second benchmark
timeout while the daemon remained active. A plain `gaze doctor` still reported
22 passed, 1 optional feature off, 2 warnings, and 0 errors. After a `gazed`
restart, an isolated benchmark completed with 26 passed, 1 off, 2 warnings,
and 0 errors; CPU means were 6.6 ms for detection, 9.1 ms for RGB recognition,
9.9 ms for IR recognition, and 4.0 ms for MiniFASNet. The timeout's root cause
is unconfirmed. Gaze's libcamera enumeration also logs a media-link `Device
or resource busy` while this bridge owns the OV7251 link, although its doctor
checks and the independent 120-frame GREY capture succeed.

## Current status recheck (2026-09-26 23:46 +08)

The bridge and `gazed` services are both active with zero current systemd
restarts. `gaze doctor` reports 22 passed, 1 optional feature off, 2 warnings,
and 0 errors; the warnings remain the disabled GNOME extension and deferred
enrollment. A direct `gaze auth --verbose --user stephenzeng` exits with code 1
and `No faces enrolled`, without changing the service or configuration.

The live configuration still points IR at `/dev/video42`, leaves
`emitter_enabled = false`, enables encrypted templates, and leaves GNOME
Keyring unlock off. `/var/lib/gaze/tpm` is root-owned mode `0700`; `dek.pub`
and `dek.priv` are root-owned mode `0600`. The TPM is Infineon SLB9665 and
reports TPM 2.0. `gazed` logs `Template encryption enabled (AES-256-GCM under a
TPM-sealed key)` after its current start. This confirms daemon initialization
and key-material permissions, but not encrypted user-template storage,
recovery after TPM replacement, or face authentication.

## Active IR emitter integration (2026-09-27)

The earlier snapshots above intentionally record the state before an emitter
backend existed. The current Gaze test installation includes the patch in
`patches/gaze-surface-pro4-ir-emitter.patch`, based on upstream Gaze commit
`da99c32` (`gaze 0.3.3`). The patch adds a hardware-specific backend to
`gaze-core/src/ir/led.rs`; it is selected only when all of these are true:

* the configured IR node is a real `/dev/videoN` node;
* `/run/surface_ir_bridge_dev` identifies an existing source node using the
  `ipu3-cio2` driver;
* `INT347E:00` reports the `ov7251` driver; and
* `/dev/i2c-3` exists.

For this Surface Pro 4 the backend selects I2C address `0x60` and writes the
OV7251 register `0x3005`: `0x08` enables the emitter and `0x00` disables it.
Gaze's existing `EmitterGuard` now owns the lifetime: it enables the emitter
when an IR enrollment or authentication thread starts, and disables it on
success, failure, cancellation, or thread cleanup.

To reproduce the source build without storing the full Gaze checkout here:

```sh
git clone https://github.com/GunduLabs/gaze.git /tmp/gaze-source
cd /tmp/gaze-source
git checkout da99c32
git apply /path/to/surface-hello/patches/gaze-surface-pro4-ir-emitter.patch
cargo build -p gaze --release --bin gazed
```

The test installation backed up `/usr/bin/gazed`, installed the resulting
release binary, kept `encrypt_templates = true`, and set
`emitter_enabled = true`. `gazed` and `surface-ir-camera.service` are active.
The daemon journal recorded:

```text
IR emitter enabled via Surface Pro 4 OV7251 IR emitter (I2C) on /dev/video42
```

Direct hardware checks measured register readback `0x00` with the emitter off
and `0x08` after the on command. A 30-frame bridge sample changed from roughly
9 mean luma (off) to roughly 35 mean luma (on), then returned to `0x00` after
cleanup. Gaze enrollment of the `active-ir` template ran with
`run_rgb: false, run_ir: true`, saved five IR captures, and returned the
register to `0x00`. Direct `gaze auth -u stephenzeng -v` succeeded, and the
GNOME lock-screen face authentication path also unlocked successfully.

The GNOME extension package `gaze-gnome-extension 0.3.3-1~debian13` is
installed. The extension ID `gaze@gundulabs.com` is enabled for the user and
`enable-face-authentication` is true. A logout/login was required for the
Wayland GNOME Shell to rescan the system extension directory.

The current `active-ir` template was enrolled while `rgb = ""` so that the
broken `/dev/video6` RGB path could not abort enrollment before the IR thread
started. Restore `rgb = "primary"` for normal configuration after the IR-only
test; the existing IR template remains usable. The RGB path still needs a
separate format/media-graph fix before creating a combined RGB+IR template.
