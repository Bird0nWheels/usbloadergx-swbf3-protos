# Preparing a Wii dev-disc for USB Loader GX

End-to-end recipe for turning a Wii prototype/NDEV disc image
(`.iso` / `.gcm` / `.rvm`) into a WBFS that this fork's `boot.dol` will
load on retail Wii hardware. Pick the route that matches the tools you have
installed.

> **Tip — drag-and-drop wrapper.** A bundled Windows batch script
> [`tools/proto-pipeline/process-game.bat`](../tools/proto-pipeline/) runs
> every step below for you. Drop your source image on it and the script
> handles the rest, including downloading rvthtool + wit on first run
> (with explicit YES-to-confirm consent). The manual steps below are the
> ground truth and the script does exactly the same things.

---

## What you need

| Tool | Purpose | Where to get it |
|------|---------|------------------|
| `rvthtool` | Re-encrypt a debug-signed (NDEV) image to retail-fakesign | https://github.com/GerbilSoft/rvthtool/releases |
| `wit` (Wiimms ISO Tool) | Convert ISO → WBFS, rename disc-ID, split for FAT32 | https://wit.wiimm.de/download.html |
| FAT32-formatted USB HDD or SD card | Where the WBFS lives | any |
| USB Loader GX (this fork) | Loads the WBFS | this repo's Releases page |
| cIOS d2x v11-beta3 (base IOS 37 or 38) | Trucha-bug fakesign acceptance | https://github.com/wiidev/d2x-cios/releases |

Disc-image extensions and what to expect:

- `.iso` — full Wii single- or dual-layer image. Use directly.
- `.gcm` — same as `.iso`; just an older naming convention.
- `.rvm` — RVT-H bank dump. Pass directly to `rvthtool`; it knows the
  multi-bank layout.

---

## Workflow (`rvthtool` + `wit`)

This is the workflow used to prepare every disc on the test rig.

### 1. Re-encrypt the disc with retail keys (fakesign)

`rvthtool` reads the debug-encrypted partition data, re-encrypts it with
retail keys, and emits a fakesigned ticket + TMD so cIOS d2x's trucha bug
will accept it.

```bash
rvthtool -k retail extract /path/to/source.iso 1 /path/to/retail.iso
```

`1` is the bank index inside the source. For a plain `.iso` / `.gcm` this is
always `1`. For a multi-bank `.rvm` use `rvthtool list <file>` first to see
the bank table.

The output is an 8 GB iso (single-layer) or up to ~8.5 GB (dual-layer). It
behaves like a normal retail Wii image from this point on.

### 2. Convert to WBFS with a custom disc ID

```bash
wit COPY /path/to/retail.iso \
    "/some/temp/dir/CUSTOMID.wbfs" \
    --psel data --enc SIGN --split-size 3.5G \
    --modify ALL --id CUSTOMID --name "Short Title" \
    --progress --overwrite
```

- `--id CUSTOMID` — the new 6-char disc ID (e.g. `RSBE3C`). See the registry
  at [CUSTOM_DISC_IDS.md](CUSTOM_DISC_IDS.md) for the IDs reserved for known
  builds. **If you skip this step, every SWBF3 dev build collapses to the
  single ID `RABAZZ` in USB Loader GX.**
- `--split-size 3.5G` — FAT32 file-size limit. Each WBFS becomes
  `CUSTOMID.wbfs` + `CUSTOMID.wbf1` if the disc data exceeds 3.5 GB.
- `--modify ALL` — also rewrite the inner boot.bin disc-ID and title so the
  rename sticks at the game level, not just the WBFS slot header.
- **Write to a bracket-free path first.** `wit` cannot create temp files
  inside paths containing brackets. After the copy finishes, `mv` the
  output into the bracket-named final folder.

#### GUI alternative

If you'd rather not run `wit` from the command line, you can use a Wii
backup manager GUI like
[TinyWiiBackupManager](https://github.com/mq1/TinyWiiBackupManager) to
handle the ISO → WBFS conversion + folder layout. Two caveats:

1. **The disc-ID rename is critical** for this fork's setup. Multiple SWBF3
   dev builds ship with the same placeholder `RABAZZ`, and USB Loader GX
   keys games by ID — without unique IDs they collapse to a single entry.
   Verify the GUI you pick supports renaming the disc ID; if it doesn't,
   run `wit EDIT --id CUSTOMID --name "Short Title" --modify ALL <wbfs>`
   afterwards on the converted file.
2. GUI tools usually skip the `rvthtool -k retail` re-encryption step
   (they assume already-retail input). Run step 1 first; pass the
   resulting `retail.iso` into the GUI.

### 3. Move into the WBFS layout USB Loader GX expects

```bash
mkdir -p "/path/to/usbhdd/wbfs/Friendly Name [CUSTOMID]"
mv /some/temp/dir/CUSTOMID.wbf* "/path/to/usbhdd/wbfs/Friendly Name [CUSTOMID]/"
```

The folder name's `[CUSTOMID]` suffix is how USB Loader GX matches the
folder to the disc inside.

### 4. Verify

```bash
wit DUMP "/path/to/usbhdd/wbfs/Friendly Name [CUSTOMID]/CUSTOMID.wbfs"
```

Should report `disc=CUSTOMID`, the custom title, region, a partition count
of 1, and a non-zero scrubbed size with a sensible dir/file count.

**If `wit DUMP` reports `Directories: 0 / Files: 0`** but the WBFS file is
multi-GB, the iso→wbfs conversion mangled the FST. Use the fallback below.

### 4a. Fallback for awkward FSTs (sparse / unusual debug builds)

Some debug builds have FSTs that `wit COPY iso→wbfs` mishandles silently —
the WBFS comes out the right size but contains zero files. Workaround:

```bash
# Extract the retail iso to a directory tree
wit EXTRACT /path/to/retail.iso /some/tmp/extracted --psel data --overwrite

# Rebuild WBFS from the directory (skips wit's iso-FST reader)
wit COPY /some/tmp/extracted /some/tmp/CUSTOMID.wbfs \
    --enc SIGN --split-size 3.5G \
    --modify ALL --id CUSTOMID --name "Short Title" \
    --progress --overwrite
```

Notice `--psel data` is dropped — the source *is* the data partition; there's
nothing else to select. Use this route when the direct `wit COPY` yields
a `0 dirs / 0 files` WBFS; the directory-rebuild route then produces a
correct one.

### 5. Eject, plug into the Wii, launch USB Loader GX, pick the disc

The first launch with a new dev build is a good time to also pull
`sd:/debug.txt` after the boot, in case anything misbehaves.

---

## After install

- `boot.dol` lives at `sd:/apps/usbloader_gx/boot.dol`.
- Every gprintf appends to `sd:/debug.txt`. If anything goes wrong on a
  first launch, that's the first thing to read.
- `sd:/debug.txt` grows unbounded across boots. Periodically delete or
  rotate it.
- The 6 SWBF3 runtime patches only run when disc ID prefix is `RSBE3` —
  retail discs and Lego dev builds are not touched.

---

## Known issues

- **`wit DUMP` for paths containing brackets** sometimes errors with
  `ERROR #76 [CAN'T OPEN FILE]`. Workaround: use a Windows-style path with
  forward slashes (`wit DUMP 'D:/wbfs/Folder [ID]/ID.wbfs'`).
