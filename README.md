# obs-premiere-patch

OBS plugin (Windows, 64-bit) that fixes recordings for use in Premiere Pro.

## What it does

- **Chapter markers** — automatically translates OBS chapter markers into Premiere Pro's XMP format
- **A/V sync fix** — corrects a timing issue that causes audio to be shorter than the video length

## Requirements

- OBS Studio 31.x (64-bit, Windows)

Chapter markers can be added during recording via **Settings → Hotkeys → Add Chapter Marker**, or with the [streamup-record-chapter-manager](https://github.com/StreamUPTips/obs-record-chapter-manager) plugin. Use **hybrid_mp4** recording format for crash recovery support.

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
