# process-game.bat — Wii dev-disc → WBFS in one step

Drag-and-drop (or interactive) Windows batch script that takes a Wii
prototype/NDEV `.iso` / `.gcm` / `.rvm`, runs the
`rvthtool retail-recryption` + `wit COPY` + disc-ID rename pipeline,
and stages the resulting WBFS folder ready to drop into your USB HDD's
`wbfs\` directory.

This script prepares a Star Wars Battlefront 3 prototype disc to
boot via the parent repo's USB Loader GX build. See the parent
[README](../../README.md) for the full setup.

## How to use

1. **First time**: double-click `process-game.bat`. It checks if you
   already have `rvthtool` and `wit` on `PATH`. If not, it shows a short
   consent prompt and downloads them into a local `tools\` subfolder.
   Type **YES** to accept.

2. **Every time after**: drag your source `.iso` / `.gcm` / `.rvm` onto
   `process-game.bat`. (Or run it without arguments and paste the
   path when prompted.)

3. The script asks for the **disc ID** you want to assign to this build.

   - Type **H** to open the bundled registry (`disc-id-registry.txt`) in
     Notepad — copy the right ID for the build you have.
   - Type **O** to open the online registry on GitHub.
   - Or just type the 6-char ID (e.g. `RSBE3C`) directly.

   If the ID is in the registry, the script auto-fills the friendly
   folder name and short title. You can override either by typing your
   own, or press Enter to accept the default.

4. Confirm the summary, then watch the three stages run:
   - `[1/3]` rvthtool re-encrypts the partition with retail keys + fakesign
   - `[2/3]` wit packs an FST-clean WBFS with the chosen disc ID inside it
     (auto-falls back to the directory-rebuild route if the iso-path
     produces a 0-dir/0-file WBFS — happens with some debug builds)
   - `[3/3]` moves the WBFS into `out\<friendly name> [GAMEID]\`

5. Copy the resulting folder into your USB HDD's `wbfs\` directory.

## Inventing a new disc ID

Wii disc IDs are 6 chars: `<console><game3><suffix2>`.

- 1st char: **R** for single-layer (≤4.7 GB) or **S** for dual-layer (≤7.9 GB).
- chars 2-4: any 3-letter abbreviation for the game.
- chars 5-6: arbitrary suffix; use this to distinguish multiple protos of
  the same game (e.g. `D1`/`D2` for "dev 1" / "dev 2").

Just don't collide with a known retail disc ID or with another entry in
your library. If unsure, look at the existing registry and pick a free
combination in the same shape.

## Why each step is needed

- **rvthtool retail re-encryption** — most NDEV/dev-build discs are
  debug-encrypted (or "encryption: none" with debug ticket+TMD signatures).
  Retail Wii cIOS d2x with trucha-bug fakesign acceptance can only verify
  fakesigned retail-encrypted partitions. rvthtool's `-k retail extract`
  re-encrypts the partition data with retail keys, then fakesigns the
  ticket + TMD.

- **Custom disc ID** — multiple SWBF3 dev builds (and many other
  prototypes) ship with a placeholder ID like `RABAZZ`. USB Loader GX
  identifies games by ID, so without renaming, all three SWBF3 builds
  would collapse to a single menu entry. The rename also lets the
  game-ID-prefix-gated runtime patches in this fork's USB Loader GX
  recognise the build (the SWBF3 patches only activate when the disc ID
  starts with `RSBE3`).

- **WBFS repack** — USB Loader GX prefers the WBFS container format.
  `wit COPY` builds a scrubbed FAT32-split WBFS with the new disc ID + title
  stamped inside.

## Files in this folder

| File | Purpose |
|------|---------|
| `process-game.bat` | the script you actually run |
| `disc-id-registry.txt` | machine-readable registry of disc IDs (also human-readable) |
| `LICENSE-AGREEMENT.txt` | shown to the user before the tool download |
| `tools\` | created on first run; holds the local rvthtool + wit downloads |
| `work\` | intermediate files (auto-cleaned after success) |
| `out\` | final staged WBFS folders ready to copy to your HDD |
| `README.md` | this file |

## What gets downloaded (and from where)

If either tool is already on `PATH` (or cached in `tools\`), nothing is
fetched for it.

- **rvthtool v2.0.1** (GPL-2-or-later), David Korth / GerbilSoft —
  `https://stuff.gerbilsoft.com/.rvt/rvthtool_2.0.1-win64.zip`
- **wit v3.05a r8638 cygwin64** (GPL-2-or-later), Dirk Clemens / Wiimm —
  `https://wit.wiimm.de/download/wit-v3.05a-r8638-cygwin64.zip`

Both are unpacked into `tools\` and invoked as separate processes; they
are not modified or repackaged here. License text travels inside each
download.

## License

This script: GPL-3.0-or-later.

The downloaded tools keep their own GPL-2.0-or-later licenses (the
license text ships inside each download).
