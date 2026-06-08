# obs-premiere-patch

OBS plugin (Windows, 64-bit) that injects Premiere Pro-compatible XMP chapter markers into recorded MP4 files.

## What it does

- On recording stop: reads chapters from [streamup-record-chapter-manager](https://github.com/StreamUPTips/obs-record-chapter-manager) and writes `xmpDM:markers` metadata into the MP4's `moov.udta.XMP_` box
- Fixes the AAC tail-drop A/V gap (~21 ms) via lossless stream copy trim
- On OBS startup: remuxes any orphaned `.mkv` files and trims A/V on existing MP4s in the recording folder

## Requirements

- OBS Studio 31.x (64-bit, Windows)
- [streamup-record-chapter-manager](https://github.com/StreamUPTips/obs-record-chapter-manager)
- Recording format: **hybrid_mp4** (Advanced Output Settings → Recording Format)

## Installation

Download `obs-premiere-patch.dll` from [Releases](https://github.com/CalvFletch/obs-premiere-patch/releases) and copy it to:

```
C:\Program Files\obs-studio\obs-plugins\64bit\
```

Restart OBS.

## Usage

The plugin runs automatically. Settings are in **Tools → Marker Patch**:

| Toggle | Default | Effect |
|--------|---------|--------|
| Auto-inject markers | On | Inject XMP on each recording stop |
| Auto A/V trim | On | Trim video tail to audio duration |

Manual fix actions are also available in the same submenu for processing existing files or folders.

## Building

Requires Visual Studio 2022, CMake 3.24+, and the OBS plugin build system.

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```
