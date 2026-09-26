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
  ! video/x-raw,format=GRAY8,width=640,height=480 ! fakesink
```

The bridge converts IPU3 packed 10-bit monochrome to 8-bit `GREY`. It does
not write OV7251 registers or GPIOs: this machine has no verified Linux IR
emitter control endpoint. `emitter_enabled` must therefore remain `false`.
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
to capture an active/ambient pair; the script still reports only image
difference and does not interpret it as depth or liveness.

## IR emitter investigation

On this Surface Pro 4, the DSDT identifies the OV7251 as `INT347E`/`CAM3` at
I2C address `0x60`. Its `INT3472` dependency (`SKC2`) lists two GPIO resources:
pin `0x4f` has function `0x0c` (clock enable), and pin `0x50` has function
`0x00` (reset). These function codes match the Linux INT3472 driver's GPIO
type mapping. None of the twelve static and dynamic SSDTs adds a camera or
emitter definition.
The kernel exposes privacy LEDs, but no `ir_flood` LED or V4L2 flash control.
Thus neither of these two GPIOs is evidence of an emitter control line.

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

Static strings in the same package provide a narrower clue: `ov7251.sys`
contains the sensor-side modes `Strobe`, `Torch`, and `Flash`, while
`SkcController.sys` contains `TPS68470` flash methods for both strobe and I2C
command operation, plus GPIO operations. This confirms that the Windows
camera stack has generic flash-controller support. It still does not prove
that this Pro 4 routes its emitter through TPS68470, nor does it provide a
safe Linux GPIO, I2C address, or register sequence. No such values are used by
this project.

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

First validate RGB enrollment and `gaze auth`. Then make a timestamped backup
of `/etc/gaze/config.toml` and set:

```toml
[cameras]
rgb = "primary"
ir = "/dev/video42"
emitter_enabled = false
parallel_capture = "never"
```

Restart `gazed`, run `gaze doctor`, and inspect `journalctl -u gazed`. Do not
enable PAM, GDM, or the GNOME extension until interactive authentication and
TTY/password fallback have both been tested.

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
the current CIO2 input. The bridge exposed 640x480 `GREY` with a 307200-byte
frame size on `/dev/video42`. `capture_ir_pair.sh` successfully saved two
consecutive frames and a PGM absolute-difference image; the observed
difference was zero while no emitter command was supplied.

`gaze doctor` completed with 22 passed, 1 optional feature off, 2 warnings,
and 0 errors. The warnings were expected while enrollment is deferred: the
GNOME extension is installed but not enabled, and no faces are enrolled. The
TPM, daemon, PAM installation, camera nodes, and encrypted-template setting
were all accepted by the doctor. Interactive enrollment and authentication
remain intentionally unperformed.
