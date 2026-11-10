# rk-pw-camera

Makes MIPI cameras on Rockchip boards running the vendor kernel show up in
desktop camera apps (GNOME Snapshot, browsers, anything that uses the
PipeWire camera portal).

## Why it is needed

The vendor kernel's ISP driver (`rkisp`) only offers the V4L2 multi-planar
capture API. PipeWire's V4L2 monitor only handles single-planar capture
([pipewire#35](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/35)),
so it lists the camera but never creates a source for it. Desktop apps see
nothing, or a long list of `rkcif`/`rkisp` internal nodes that give no
picture.

## What it does

- `rk-camera-select` (system service, at boot) finds the virtual ISP whose
  CIF input has a probed sensor and writes its main path video node to
  `/run/rk-camera/device`.
- `rk-pw-camera` (user service, next to PipeWire) publishes that node as one
  PipeWire camera. It opens the ISP only while an application is linked,
  so the sensor is off when no app uses the camera.
- A WirePlumber rule hides the internal `rkcif`, `rkisp` and `rkvpss` V4L2
  nodes.

It offers NV12 (one copy per frame) and BGRx (converted with libyuv), at
1280x720 and 1920x1080, 30 fps.

Picture quality (exposure, white balance, colour) comes from the Rockchip
3A server, `rkaiq_3A_server`, which has to run alongside this. On RK3576
that is the `camera-engine-rkaiq-rk3576` package.

## Tested on

- LCKFB TaishanPi 3M (RK3576) with an IMX415 module, Armbian vendor kernel
  6.1, Debian trixie, GNOME 48 (Snapshot).

## Build

```sh
sudo apt install build-essential debhelper pkgconf libpipewire-0.3-dev libyuv-dev
dpkg-buildpackage -us -uc -b
```

## License

MIT
