# 3D audio with DSOAL (optional)

Sacred plays its sound through Miles Sound System, which only offers **3D, alle Effekte** when Windows reports a sound card with hardware DirectSound3D and EAX. No current PC has
one, so that row in the sound options is greyed out and the game plays flat stereo. [DSOAL](https://github.com/kcat/dsoal) makes the game see such a card again and mixes the sound
with [OpenAL Soft](https://openal-soft.org/) on any sound card: positional 3D sound and EAX-style reverb.

It is independent of Sacred-Bild and Sacred Gear and works with or without them.

## Install with the Gear Mod Manager

The Sacred-Bild source of the manager (`mod-index.json`) lists three plugins in the *Plugins* tab. They come from the **official** DSOAL release; this repository only
relays them (`sacred-dsoal.json`) and adds the tiny speaker setting:

| Plugin | Puts next to `Sacred.exe` | Pick it when |
|---|---|---|
| **sacred-dsoal (base)** | `dsound.dll`, `dsoal-aldrv.dll` | always (the two others install it for you); alone it uses OpenAL Soft's own defaults |
| **DSOAL Headphones** | `alsoft.ini` (official DSOAL+HRTF: HRTF spatialisation) | you play with headphones or a headset |
| **DSOAL Speakers** | `alsoft.ini` (stereo as speakers, plain panning; `stereo-mode=speakers`, `stereo-encoding=basic`) | you play on speakers (stereo, 5.1, 7.1: the layout follows the Windows sound settings) |

Speakers and Headphones both write `alsoft.ini`, so only one of them can be installed. Installing the other asks whether to remove the first (and shows its files); *cancel* changes
nothing. The question before each install shows where every file comes from. The Speakers ini is a *loose file*: the manager downloads it from this repository's raw address and
installs it only when its SHA-256 matches the one in `sacred-dsoal.json` (see the manager's `docs/ModIndex.md`, "Loose files"). The two DLLs and the HRTF ini come out of the official
zip `DSOAL_r697.zip` (SHA-256 pinned in the index).

## Then, once

Needs the **Microsoft Visual C++ 2015-2022 Redistributable (x86)** (most PCs have it; if the game stops with a missing DLL, install it from Microsoft).

Open the game's sound options, choose **3D, alle Effekte**, confirm and **restart the game**: the choice is read at start-up.

## Uninstall

Remove the plugins in the manager (the Speakers or Headphones one, and the base): the files disappear and a `dsound.dll` or `alsoft.ini` you had before comes back. By hand: delete
`dsound.dll`, `dsoal-aldrv.dll` and `alsoft.ini` from the game folder. Nothing else is touched (no registry entries, no files outside the game folder).

To see what DSOAL is doing, set the environment variables `DSOAL_LOGLEVEL` (0 to 3) and `DSOAL_LOGFILE` (a file path) before starting the game.

## Files and licences

The DLLs are **linked, not redistributed by this repository**: the manager downloads them from the official release of the upstream author.

* DSOAL: LGPL-2.1, `https://github.com/kcat/dsoal`
* OpenAL Soft: LGPL-2.0 or later (plus BSD/Apache/MIT parts), `https://github.com/kcat/openal-soft`
* The official release used: `https://github.com/kcat/dsoal/releases/download/archive/DSOAL_r697.zip` (revision r697; the zip holds `DSOAL_r697.zip` with the `Win32` and `Win64` folders,
  only `Win32` is used because Sacred is a 32-bit game).
* `docs/dsoal/alsoft-speakers.ini` is a two-setting configuration file (no code) written for this index; every key is documented in the commented default `alsoft.ini` of the official zip.

To move to a newer revision pick `DSOAL_rNNN.zip` from the upstream `archive` release, update the address, the revision and the SHA-256 in `tools/make_dsoal_index.py`, run it and check the
files in the Win32 folders before pushing. `python tools/make_dsoal_index.py --check` (also run in CI) fails when `sacred-dsoal.json` and the speaker ini disagree: change the ini and
the index together, in one push (the index pins the ini's hash).
