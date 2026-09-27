# Sky's S2 Fishing Macro

An auto-fishing macro for **Project Slayers 2** (Roblox) on Windows 10/11. It casts, plays the
reel minigame, collects the catch and repeats. It works by reading pixels in regions you calibrate
on your own screen, so it runs at any resolution.

![Sky's S2 Fishing Macro](docs/screenshot.png)

> Some games forbid macros. Use this at your own risk: your account could be penalised.

## Features

- **Full cycle:** cast → wait for a bite → play the minigame → collect (hold **T**) → repeat.
- **Minigame control:** tracks the green zone (including its yellow "warning" state) and the
  white block, using short holds and releases to keep the block in the zone.
- **Auto reposition (set respawn):** resets your character (Esc → R → Enter) every *N* catches
  (default 10), and after 2 missed casts in a row, so you stay on the same spot.
- **Safe focus handling:** clicks and keys are only sent when the Roblox window is really in
  front, so nothing gets typed into other windows.
- **Settings are saved** to `SkysS2FishingMacro.ini` next to the exe (hotkeys, wait time,
  auto-reposition and calibration).

## Setup

1. Build it (see below) or use a `SkysS2FishingMacro.exe` you built yourself.
2. Open Roblox and stand where you want to fish.
3. In the **Setup** tab, calibrate:
   - **Cast point:** click where you want to cast.
   - **Fishing bar:** drag a tight box around the vertical minigame bar.
   - **Exit button:** drag a box around the red *Exit* button that shows during the minigame.
4. Press **Start** (default hotkey `-`). The default exit hotkey is `=`. You can change both in
   the **Hotkeys** tab.

**Tip:** set *Wait for fish* high enough (for example 30–60 s). If no bite comes within that
time, the macro reels in and casts again.

## Building

You need MinGW-w64 (`winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT`) or Visual Studio.
Then run:

```
build.bat
```

This produces `SkysS2FishingMacro.exe`. It is a single file with no dependencies.

## Credits

Based on the *Fishing Hyper Tracker* macro by midnytejay. This version was rebranded and
redesigned, with detection fixes, auto reposition and safer window focus handling.
