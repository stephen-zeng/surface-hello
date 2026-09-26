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
