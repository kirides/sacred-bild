# Sacred ReBorn

SacredReborn.exe is the German sacred.exe with its code patched (same section layout, same addresses, extra code
appended at 0x8e4xxx). SacredBild treats it as another build whose code is *looked at*, not trusted: nothing is
chosen by exe hash, and a patch whose code is not what it expects is skipped and logged.

## What protects each kind of patch

| Patch | Check |
|---|---|
| Function hooks, call sites, constants, globals | Found by byte signature (`sacred_sigs.inc`, `resolution_sites.inc`): a site that is not found exactly once is not patched (`Signature ...: 0 matches`, `Resolution: patched n/m sites`). Globals are taken from the operand of the signature's instruction (`Sig::Take::Abs32`), so ReBorn's moved globals are found where they are. |
| Byte patches (`Patch::verify` / `imm32`) | Written only if the bytes there are the expected ones. |
| Gear's site hooks | Each checks the expected instruction (`Site hook ...: the code is not the expected ...: not installed`). |
| Fixed offsets into the game's objects | `Sacred::Layout::check()` (`src/game/layout_check.cpp`): every `cWorldView` field SacredBild reads or writes at a fixed offset must appear as a displacement in a game function that uses it; else the features that need it stay off (ground rows: row walk, water list, ground mesh; pick list: aim assist; zoom). The water tile array is read from `drawWaterTiles` itself. |

## What ReBorn changes (tools/reborn_diff.py, against the German exe)

- Globals moved: the float constants the resolution code loads (0x88e6f4 ... -> 0xa1efe0 ...), a table at 0xad2468
  -> 0x1ac7758. Sites that load them differ from the signatures, so Resolution leaves them alone (28 of 142).
- `cWorldView`: the water tile array moved from +0x3FF2C to +0x9FF2C (object size 0x970F4 -> 0x1970F4). Every other
  field SacredBild uses is at the same offset in all three builds.
- Jumps into the appended code from inside functions (setZoom, renderTileRow after the ground, drawLoadingScreen, the
  object culling code at 0x6156b8), constants replaced by loads of ReBorn's own variables, a condition flipped in
  dxDriver7::flip. Hooks that call the original are not affected; the ones that replace a function check
  `Sacred::reBorn()`.
