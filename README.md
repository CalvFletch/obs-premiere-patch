# obs-premiere-patch

OBS plugin (Windows, 64-bit) that fixes recordings for use in Premiere Pro.

## What it does

- **Chapter markers** — automatically imports your OBS chapter markers into Premiere Pro's timeline
- **A/V sync fix** — corrects a timing issue that causes audio and video to appear out of sync in Premiere Pro

## Requirements

- OBS Studio 31.x (64-bit, Windows)
- Recording format: **hybrid_mp4** (Advanced Output Settings → Recording Format)
- [streamup-record-chapter-manager](https://github.com/StreamUPTips/obs-record-chapter-manager) — only needed for chapter markers

## Installation

Download `obs-premiere-patch.dll` from [Releases](https://github.com/CalvFletch/obs-premiere-patch/releases) and copy it to:

```
C:\Program Files\obs-studio\obs-plugins\64bit\
```

Restart OBS.

## Usage

The plugin runs automatically after every recording. Settings are in **Tools → Premiere Patch**:

| Toggle | Default | Effect |
|--------|---------|--------|
| Auto-inject markers | On | Import chapter markers into Premiere Pro |
| Auto A/V trim | On | Fix A/V sync on each recording |

Manual fix actions are also available in the same submenu for processing existing files or folders.

## Building

Requires Visual Studio 2022, CMake 3.24+, and the OBS plugin build system.

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```
