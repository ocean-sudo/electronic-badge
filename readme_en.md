# USB Picture Badge

Firmware and a local browser console for the Waveshare ESP32-S3-Touch-AMOLED-1.75C. A Python service on the USB-connected computer controls the device. GitHub Pages is a static project introduction, not an online USB console: image processing, serial bridging, and device control run locally on the user's computer.

The device's screen-off setting only turns off the display. It does not mean ESP32 deep sleep, power-off, or guaranteed reduction of total device power use.

> The console screenshots show the real web UI with original synthetic artwork, offline and not connected to a device.

![Actual browser console crop preview with synthetic image; offline and disconnected](docs/assets/console-desktop.webp)

[Mobile-size console screenshot](docs/assets/console-mobile.webp) · [Chinese primary README](README.md) · [Chinese project site entry](https://ocean-sudo.github.io/electronic-badge/)

## Features

- Select, drop, or paste JPG, PNG, or WebP, adjust a circular crop, and preview before explicitly uploading. The browser outputs 466×466 JPEG using Canvas `toBlob` quality parameter 0.85; file size varies by image. WebP is decoded in the browser and converted to JPEG. Firmware uses the ESP32-S3 ROM software TJpgDec JPEG decoder and does not decode WebP.
- Browse, replace, delete, display, and read back saved JPEGs; adjust brightness, animation, device menus, slideshow, and screen-off settings. Stable image IDs are 0–2147483646; capacity depends on LittleFS space.
- Firmware-managed slideshow continues without the computer service. Shuffle is opt-in. Playback history retains at most six entries in RAM and resets on reboot.
- Separate USB/battery idle screen-off settings and PMU-reported charging status. USB connection alone does not imply charging.
- Image storage and transfer are CRC checked; failed uploads do not replace the prior image.

## Run the local console

Requires Python 3.10+, a data-capable USB cable, and serial-port permission. From the repository root:

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
cp .env.example .env
python tools/serve.py
```

Open <http://127.0.0.1:8765/>. The service defaults to loopback and detects supported Espressif USB VID/PID; if multiple ports are present, pass --serial <your-port>. The service has no authentication: keep it on loopback by default; do not expose it through public port forwarding or Tailscale Funnel. Private settings belong only in the local, untracked `.env`.

## Build and update safely

Requires Git, Python, PlatformIO Core, and network access. The build pins pioarduino `55.03.312-1` and Waveshare vendor source commit `6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c`. Full build and safe-initialization commands are in the Chinese primary README.

Only a confirmed blank, new device may use the initial PlatformIO `upload`, which installs the bootloader, partition table, and application, followed by an empty filesystem `uploadfs`. The root `data/` directory is a host-computer input to the filesystem image builder, not a device directory created by firmware. If absent, create it with `mkdir data`; if already present, inspect and confirm it is empty before skipping that command. If nonempty, stop without deleting or overwriting anything. Never use these initialization steps on an existing device whose data must be preserved.

**v1 stores images as RGB565, which v2 does not read or migrate.** Before upgrading, export images using v1 while that firmware and its tools are still available, verify an offline backup, and convert the images to 466×466 JPEG. Keep the backup until the migrated JPEGs have been imported and verified in v2.

For an existing device, write only the new `firmware.bin` to the APP partition at `0x10000`. Do not use PlatformIO full upload, uploadfs, whole-chip erase, or restore an old full-flash image when device data must be preserved. LittleFS is mounted without automatic formatting; screen-off is not deep sleep or power-off.

## Measured memory and performance

Two RGB565 source frames, two output frames, and one USB/menu working frame use about 2.07 MiB total, plus a separate 4 KiB JPEG workspace (excluding other runtime memory). On 42 saved natural photos, file read, CRC, decode, and RGB conversion took about 228–284 ms per image; direct display-and-ack took 301–356 ms including display. These are observations for that sample, not guarantees or WebP device-decoding results.

## Privacy and AI-assisted deployment

Never share `.env`, private photos or backups, tokens, private domains, or device serial numbers with public chats, issues, or remote AI. Share only `.env.example`, system details, and redacted errors; enter private values locally. A local agent must not echo or commit secrets. The Chinese primary README includes a copyable prompt that asks an AI to generate reviewed, non-destructive steps; there is no one-click deployment.