# Surface Pro 4 IR camera support for Gaze

This repository contains a Linux bridge for the Surface Pro 4's OV7251
infrared camera and setup notes for using it with Gaze. The tested path is:

```text
OV7251 -> IPU3/CIO2 -> Surface IR bridge -> /dev/video42 -> Gaze
```

The bridge rotates the Surface Pro 4 sensor image 90 degrees counterclockwise
and exposes a 480x640 GREY stream. The companion
[Surface Gaze fork](https://github.com/stephen-zeng/surface-gaze) adds the
verified Surface Pro 4 I2C emitter control used by `emitter_enabled`.
This setup has been tested on this Surface Pro 4; other Surface models and
OV7251 platforms need their own hardware validation.

## Requirements

- A Surface Pro 4 with the OV7251 IR sensor enabled by the running Linux kernel.
- The IPU3/CIO2 media pipeline and `/dev/i2c-3` exposed by the kernel.
- Debian/Ubuntu packages for the bridge and Gaze build dependencies:

```sh
sudo apt install build-essential git curl pkg-config clang libclang-dev \
  libopencv-dev libv4l-dev libpam0g-dev libtss2-dev libssl-dev \
  libgtk-4-dev libadwaita-1-dev libcairo2-dev libglib2.0-dev \
  libgdk-pixbuf-2.0-dev libpango1.0-dev libgraphene-1.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-pipewire v4l-utils v4l2loopback-dkms i2c-tools
```

Install a Rust toolchain using [rustup](https://rustup.rs). Gaze's source
build and package prerequisites are documented in its development guide.

## Install the IR bridge

Build and install the bridge from this repository:

```sh
make
make test
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now surface-ir-camera.service
```

Confirm that the bridge found the CIO2 input and started the GREY output:

```sh
cat /run/surface_ir_bridge_dev
v4l2-ctl -d /dev/video42 --all
journalctl -u surface-ir-camera.service -b --no-pager
gst-launch-1.0 -q v4l2src device=/dev/video42 num-buffers=1 \
  ! video/x-raw,format=GRAY8,width=480,height=640 ! fakesink
```

The source device number can change between boots; the service discovers it
and records it in `/run/surface_ir_bridge_dev`. The Gaze backend recognizes the
bridge output by `/dev/video42` and its `Surface IR Camera` device name.
If you enrolled a face with the older unrotated 640x480 bridge, re-test
authentication after updating; the changed frame orientation may require a
new enrollment.

## Build the Surface Gaze daemon

Install Gaze's normal system package first so its CLI, systemd unit, PAM
integration, and default configuration are present. Then build the daemon from
the Surface Gaze fork, which contains the Surface Pro 4 emitter backend:

```sh
git clone https://github.com/stephen-zeng/surface-gaze.git
cd surface-gaze
cargo build -p gaze --release --bin gazed
sudo install -m 0755 target/release/gazed /usr/bin/gazed
```

The daemon needs access to `/dev/i2c-3`; the packaged `gazed` system service
runs with the required privileges. Keep the bridge and daemon enabled:

```sh
sudo systemctl enable --now surface-ir-camera.service gazed
```

## Configure Gaze

Back up `/etc/gaze/config.toml`, then set the camera and storage options:

```toml
[cameras]
rgb = ""
ir = "/dev/video42"
emitter_enabled = true
parallel_capture = "never"

[storage]
encrypt_templates = true
```

On systems without a usable TPM 2.0, Gaze will refuse to start when template
encryption is enabled. Do not disable encryption without deciding how face
templates should be protected. The encryption key is sealed to the local TPM
and cannot be recovered on a replacement motherboard or TPM.

Restart and inspect the daemon:

```sh
sudo systemctl restart gazed
gaze doctor
journalctl -u gazed -b --no-pager
```

The log should identify the `Surface Pro 4 OV7251 IR emitter (I2C)` backend
when an IR enrollment or authentication operation starts. Gaze turns the
emitter off when that operation completes. For an interactive enrollment and
authentication check:

```sh
gaze add-face active-ir -u "$USER"
gaze auth --verbose -u "$USER"
```

The validated Surface Pro 4 RGB node was not usable, so the tested setup leaves
`rgb` empty and uses IR only. Set `rgb = "primary"` only after confirming that
your RGB camera streams correctly; then enroll a new combined template.

For GNOME lock-screen integration, install and enable the Gaze GNOME extension
following Gaze's GNOME guide, then test from the lock screen. Keep password
authentication available as a fallback.

## Scope and limitations

- Active emitter control was verified on one Surface Pro 4: I2C address
  `0x60`, OV7251 register `0x3005`, `0x08` on and `0x00` off.
- The bridge provides a single monochrome IR stream. It does not provide a
  depth map, structured-light reconstruction, synchronized active/ambient
  frame pairs, or Windows Hello-equivalent presentation-attack detection.
- The Surface Pro 4 RGB camera path was not working in the validated setup.
  The known-good enrollment used IR only; a combined RGB/IR configuration
  requires separately validating the RGB media path.
- Do not apply the I2C register sequence to another Surface or OV7251 device
  without independently verifying its hardware and emitter behavior.

See [docs/implementation.md](docs/implementation.md) for hardware investigation,
stream checks, implementation details, and validation history.
