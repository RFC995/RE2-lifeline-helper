# LifeLine Helper

**A small, open-source cheat tool for Resident Evil 2 (2019) on PC — single-player only.**

[![Latest release](https://img.shields.io/github/v/release/RFC995/RE2-lifeline-helper?include_prereleases&label=release)](https://github.com/RFC995/RE2-lifeline-helper/releases)
[![Downloads](https://img.shields.io/github/downloads/RFC995/RE2-lifeline-helper/total)](https://github.com/RFC995/RE2-lifeline-helper/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

LifeLine Helper is a standalone Windows program. It needs no mod loader (no REFramework, no Fluffy Mod Manager) and **never modifies any game file**: changes exist only in memory while the tool is open, and are reverted when you close it.

> [!NOTE]
> **Status: beta.** The injected code was verified against the real game code in an x86-64 emulator during development, but in-game testing is still limited. Back up your saves before first use.

## Features

| Hotkey | Cheat | What it does |
|:---:|---|---|
| <kbd>F5</kbd> | **God mode** | The player takes no damage and cannot be killed by grabs. |
| <kbd>F6</kbd> | **Infinite ammo** | Magazines never empty, so no reloading. Includes knife durability and grenades. |
| <kbd>F7</kbd> | **Infinite items** | Herbs, sprays, ink ribbons and reserve ammo are never used up. |
| <kbd>F8</kbd> | **One-hit kills** | Any hit takes an enemy's remaining health. |

- Toggle any cheat at any time, from the window or with the hotkeys while the game is focused (a beep confirms).
- The game version is checked byte-for-byte before anything is touched; unknown builds are left alone.
- No installation, no administrator rights, no network access. The interface is in Portuguese.

## Requirements

- Windows 10 or 11 (64-bit).
- **Resident Evil 2 (2019), DirectX 11 / pre-ray-tracing build** (`re2.exe` with PE timestamp `2021-10-13`). On Steam, Capcom offers the DirectX 11 version through *Properties → Betas*. Other builds are detected and left untouched.

## Usage

1. Download `LifeLineHelper.exe` from the [latest release](https://github.com/RFC995/RE2-lifeline-helper/releases/latest).
2. Start the game.
3. Run `LifeLineHelper.exe` (before or after the game, either works).
4. Wait for the green **"Ligado ao RE2"** ("Connected to RE2") status line.
5. Tick the cheats you want, or press <kbd>F5</kbd>–<kbd>F8</kbd> in-game.
6. Close the window when you are done: the game goes back to normal.

## FAQ

<details>
<summary><b>My antivirus flags the file. Is it a virus?</b></summary>

No. Trainers write to another program's memory and listen for hotkeys, which is also what malware does, so heuristic scanners often report them as *generic* or *suspicious*, especially when unsigned. You can read the source or [build it yourself](#building-from-source). The program never accesses the network or your files.
</details>

<details>
<summary><b>Can I get banned?</b></summary>

Resident Evil 2 (2019) is single-player with no anti-cheat, and LifeLine Helper only affects the local game process. Do not use it with online games.
</details>

<details>
<summary><b>The status line says the version is not the expected one.</b></summary>

Your `re2.exe` is a different build (for example, the ray-tracing update), so the tool deliberately does nothing. Open an issue with your build details.
</details>

<details>
<summary><b>One-hit kills don't kill Mr. X.</b></summary>

Expected: Mr. X cannot die outside his scripted boss fights. He goes down after one hit and gets back up later, as in the normal game.
</details>

<details>
<summary><b>The game crashed.</b></summary>

Please [open an issue](https://github.com/RFC995/RE2-lifeline-helper/issues) describing what you were doing and which cheats were on. Closing LifeLine Helper (or restarting the game) always restores the original game code.
</details>

## How it works

The tool finds `re2.exe`, checks the original bytes of three game functions, allocates a small block of memory next to the game module and redirects those functions there:

| Game function | Change while the cheat is on |
|---|---|
| `app.ropeway.HitPointController.addDamage` | Player: damage ignored. Enemies: damage = remaining health. |
| `app.ropeway.HitPointController.checkDead` | Player: lethal attacks report "not dead". |
| `app.ropeway.inventory.Slot.reduce` | Weapon or item slot: reports the amount as used without consuming anything. |

With a cheat off, the game runs its original code unchanged. The injected code is in [`src/patches.c`](src/patches.c), commented instruction by instruction.

## Building from source

Requires [Zig](https://ziglang.org/download/), used as a C compiler that targets Windows from any OS. From the `src` folder:

```sh
zig cc -target x86_64-windows-gnu -municode -O2 -o ../LifeLineHelper.exe lifeline.c patches.c lifeline.rc -luser32 -lkernel32 -lgdi32 -lcomctl32 -Wl,--subsystem,windows
```

## Acknowledgements

[REFramework](https://github.com/praydog/REFramework) by praydog and contributors, whose documentation of RE Engine's type database made it possible to locate the game functions. No REFramework code is included.

## Disclaimer

Not affiliated with, endorsed by or sponsored by Capcom. *Resident Evil* is a trademark of Capcom Co., Ltd. Single-player use only. Provided as is, without warranty; use at your own risk.

## License

[MIT](LICENSE)
