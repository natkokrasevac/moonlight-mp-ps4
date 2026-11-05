# Moonlight MP for PlayStation 4

**Moonlight MP** is an unofficial Moonlight Game Streaming client for jailbroken PlayStation 4 / PS4 Pro consoles with **multiplayer/multi-controller support**, **dual-motor rumble**, and **dynamic light bar control**.

> Based on the original **Moonlight DX for PlayStation 4** developed by [Four Letter Workshop](https://fourletterworkshop.com/projects/moonlight_dx_for_playstation_4).

---

## Features

- **Multi-Controller Support**: Connect up to 4 DualShock 4 controllers simultaneously for local multiplayer / co-op streaming. Secondary controllers are detected automatically as users or guests log in.
- **Dual-Motor Force Feedback (Rumble)**: Independent vibration feedback (`largeMotor` and `smallMotor`) routed to each controller.
- **Dynamic In-Game Light Bar Colors**: Real-time game-driven RGB light bar illumination, reverting cleanly to player slot indicators (P1: **Blue**, P2: **Red**, P3: **Green**, P4: **Magenta**).
- **PS4 Pro 4K & HEVC Support**: Hardware-accelerated H.265/HEVC decoding and true 4K streaming pipeline on PS4 Pro.
- **GPU Accelerated**: Fully GPU-accelerated video rendering and UI compositing for cool and quiet operation.
- **Touchpad Mouse Emulation**: Touchpad on Player 1 emulates mouse pointer navigation.
- **Hardware Mouse & Keyboard**: Direct pass-through support for USB/Bluetooth keyboards and mice.
- **Universal Stream Exit**: Press `Options + L3` on any connected controller to exit back to the menu.

---

## Installation

1. Download the latest `moonlight-mp-ps4_*.pkg` from [Releases](https://github.com/natkokrasevac/moonlight-mp-ps4/releases).
2. Copy the `.pkg` file to the root of a USB flash drive formatted as **FAT32** or **exFAT**.
3. Plug the drive into your PS4 and install via the **GoldHEN Package Installer**.
   *(Alternatively, upload via FTP to `/data/pkg/` on your console).*

---

## Building from Source

### Using Docker (Recommended for Windows, macOS, Linux)

A preconfigured Docker builder environment is included so you don't need to manually configure the PS4 cross-compilation toolchain.

**Windows (PowerShell):**
```powershell
.\build-docker.ps1
```

**Manual Docker command:**
```bash
# Build the builder image (one time)
docker build -t moonlight-ps4-builder -f Dockerfile.builder .

# Compile and package the PKG
docker run --rm -v "${PWD}:/workspace" -w /workspace moonlight-ps4-builder make all
```

### Native Toolchain (macOS / Linux)

Prerequisites:
- [OpenOrbis Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) v0.5.4 (`$OO_PS4_TOOLCHAIN`)
- [OpenGNM](https://github.com/OpenOrbis/opengnm) installed into the toolchain sysroot
- Clang 18+, LLD, CMake >= 3.18

```bash
git clone --recursive https://github.com/natkokrasevac/moonlight-mp-ps4.git
cd moonlight-mp-ps4
make
```

---

## Controls

| Action | Control |
|---|---|
| **Navigate UI** | D-Pad / Left Stick |
| **Select / Enter** | ✕ Cross (or ○ Circle based on console setting) |
| **Back / Cancel** | ○ Circle (or ✕ Cross based on console setting) |
| **Exit Stream** | `Options + L3` (any controller) |
| **Toggle Mouse Mode** | Touchpad Click (Player 1) |

---

## Credits & License

- Original PS4 client created by **[Four Letter Workshop](https://fourletterworkshop.com/projects/moonlight_dx_for_playstation_4)**.
- Powered by [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c).
- Open source under the [GNU General Public License v3.0](LICENSE).
