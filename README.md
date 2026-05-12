# USB Loader GX — SWBF3 prototype support

A fork of [wiidev/usbloadergx](https://github.com/wiidev/usbloadergx)
that launches the three cancelled **Star Wars Battlefront 3** (Free
Radical / Pandemic) Wii prototype builds on retail Wii / vWii hardware
via USB Loader GX + cIOS d2x.

---

## How to use it (start here)

You need:

- A retail Wii **or** Wii U (vWii) with the Homebrew Channel installed.
- **cIOS d2x v11-beta3** (base IOS 37 or 38) installed via
  `d2x-cios-installer`. Trucha-bug fakesign acceptance is required —
  stock IOS won't accept the recrypted disc or this fork's embedded
  save-stub WAD.
- A FAT32 USB HDD or SD card with a `wbfs\` folder.
- A SWBF3 prototype disc image (`.iso` / `.gcm` / `.rvm`).

Steps:

1. **Install this fork's `boot.dol`.** Download from the latest
   [Release](../../releases) or build it yourself
   (see [Build from source](#build-from-source) below). Copy to
   `sd:/apps/usbloader_gx/boot.dol`. Keep any existing GX config files
   in that folder — config format is unchanged from upstream.

2. **Prepare the disc.** Drag-drop your SWBF3 disc image onto
   [`tools/proto-pipeline/process-game.bat`](tools/proto-pipeline/).
   First run: the script offers to download `rvthtool` and `wit`
   locally (explicit YES-to-confirm). Subsequent runs: no prompts.

   When asked for a disc ID, use one starting with `RSBE3`:
   - `RSBE3A` — r1.90431a
   - `RSBE3B` — r90776a
   - `RSBE3C` — r2.91120a (Nov 21 2008)

   The `RSBE3` prefix is what arms this fork's runtime patches.

3. **Copy the staged folder** from `tools/proto-pipeline/out/` into your
   USB HDD's `wbfs\` directory (or `sd:/wbfs/` if you load games from SD).

4. **Boot.** HBC → USB Loader GX → pick the game. The first time you
   launch SWBF3 on a given console, this fork auto-installs a small
   stub title (`00010000-30303030`) into NAND so the game's hardcoded
   profile-save path resolves; subsequent launches skip the install.

If profile-save doesn't work after that, see
[`debug.txt`](#logging) on the SD root — every step the loader takes
is logged there.

> **Optional opt-out toggle.** Dropping an empty file
> `sd:/SWBF3_INSTANT_ACTION_ONLY` on the SD root disables the four
> campaign-mode runtime patches (the two always-on patches still
> apply). Useful for isolating an instant-action-only failure from a
> campaign-only failure; not normally needed.

---

## Scope and what's actually been tested

This fork is built for and tested against **SWBF3 prototype builds only.**
Don't assume the runtime patches will help any other game — they are
gated to disc IDs starting with `RSBE3` and won't fire otherwise.

**Confirmed on retail Wii hardware:**

| Disc ID | Build | Confirmed |
|---------|-------|-----------|
| `RSBE3B` | SWBF3 r90776a | menus, gameplay |
| `RSBE3C` | SWBF3 r2.91120a (Nov 21 2008) | menus, **campaign playable, profile save persists** |

`RSBE3A` (r1.90431a) is packaged through the pipeline's directory-rebuild
fallback but not yet runtime-confirmed.

Also confirmed on **Wii U / vWii** (same `boot.dol`, same disc image): GX
boots, RSBE3C plays, profile save persists. The embedded save-stub WAD
auto-installs on first launch.

The fork's `boot.dol` is the same PowerPC binary on both targets — no
build-time switch.

### About the apploader-time patches

Three of the changes (production-boundary rewrite `lis 0x8090→0x8180`,
the MEM_size clamp, and the opt-in `Apploader_DirectLoad` path) are not
gated to any disc-ID. They are structurally generic: any disc whose
apploader has the same instruction shape will hit the patch on load.
In practice that means they are *probably* harmless on other Oct-2007–era
SDK apploaders and *might* even help similar dev/proto builds boot, but
**no other game has been validated end-to-end against this fork yet.**
If you try one and it works (or doesn't), open an issue with the log
and the disc-ID.

---

## The patches

Two source files contain everything fork-specific:

- [`source/usbloader/apploader.c`](source/usbloader/apploader.c) —
  apploader-time patches (run inside `Apploader_Run()` while the dev-disc
  apploader still controls memory layout).
- [`source/patches/gamepatches.c`](source/patches/gamepatches.c) — runtime
  gamepatches (run after the apploader returns, before the entrypoint).

Plus the embedded stub WAD + ES_Identify wiring in
[`source/usbloader/GameBooter.cpp`](source/usbloader/GameBooter.cpp)
and the two `swbf3_savestub_*` files.

### Apploader-time (un-gated, fire on every disc)

#### 1. Production-boundary rewrite `lis 0x8090 → 0x8180`

The Oct 2007 dev-disc apploader asserts that section destination addresses
don't cross `0x80900000`. Retail Wii MEM1 actually extends to `0x81800000`.
Patch rewrites every `lis rD, 0x8090` instruction inside the loaded
apploader to `lis rD, 0x8180`, letting DOL sections with `0x817FFDxx`
overlay addresses load instead of aborting.

Site: `source/usbloader/apploader.c`, search `production boundary`.

#### 2. `Mem_Size` / `Mem2_Size` clamp

After the apploader runs, the SDK globals at `0x80000028` / `0x800000F0`
(MEM1) and `0x80003118` (MEM2) are clamped to retail values
(`0x01800000` / `0x04000000` — 24 MB / 64 MB) if they're larger. NDEV
apploaders typically set these to 96 MB / 128 MB; the game's `__OSInit`
then builds heap arenas past the real RAM boundary and the first
allocation past it faults. Pure value-clamp — retail discs never trip it.

#### 3. `Apploader_DirectLoad` opt-in

An alternate boot path for discs whose apploader can't be coaxed through
`Apploader_Run()`. Enable per-game by placing an empty file
`sd:/<gameid>_USE_DIRECT` on the SD card. Not required for any
confirmed-working build; included as a future escape hatch.

### Runtime (gated to `RSBE3*` disc-IDs)

All six runtime patches live inside

```c
if (memcmp((const void *)0x80000000, "RSBE3", 5) == 0) { ... }
```

in `source/patches/gamepatches.c:149`. The gate exists because one of
the patches' 16-byte signatures was observed false-matching inside an
unrelated retail game's compiled DOL during development, NOPing a real
`bne` and producing a silent black-screen boot. The gate stops that.
The four campaign-mode
patches are additionally bypassed by a `sd:/SWBF3_INSTANT_ACTION_ONLY`
opt-out file.

| Function | What it does |
|----------|--------------|
| `swbf3_mem2_check_fix` | NOPs the conditional in SWBF3's runtime "is MEM2 == 128 MB" check so the game keeps booting on retail's 64 MB. |
| `swbf3_instant_action_null_fix` | NOPs an `lwz r3, 0(r3)` that dereferences a `lookup_obj(-1)` NULL return at the Instant-Action entry. |
| `swbf3_campaign_lookup_fix` | Rewrites the prologue of two sibling leaf getters at `0x8039A04C` / `0x8039A078` from a `-1`-only sentinel check to a magnitude check that also rejects `idx > 0x40000`. |
| `swbf3_campaign_loop_fix` | Same magnitude transform applied to the parent loop function prologue at `0x80399A80`. |
| `swbf3_campaign_vec3_guard_fix` | Flips two `cmpwi r,0; beq +16` guards in front of `vec3_copy` calls to `bge +16` so NULL / small-int garbage destinations skip the copy while real Wii pointers (`0x8xxxxxxx`, negative under signed compare) still fall through. |
| `swbf3_campaign_transform_null_fix` | At per-frame draw site `0x80254450`, hijacks two of the four setup instructions into a `cmpwi r5,0; beq +0x120` so a null transform pointer jumps to the function's existing early-exit instead of crashing on `lfs f2, 12(r5)`. |

Signature patterns and replacement bytes are all visible in the source;
nothing is computed at runtime beyond `memcmp` for the search.

> Note: the disc-ID gate fires for `RSBE3B` (r90776a) too, but whether
> each individual signature also matches in r90776a's compiled DOL has
> not been logged per-patch. The game reaches gameplay, which doesn't
> prove every patch found a match.

### Save-profile path (the NAND title-ownership fix)

SWBF3 dev binaries hardcode their save path as
`/title/00010000/30303030/data/p33_<NAME>.bf`. The placeholder TID
`30303030` is baked into the compiled DOL. On retail Wii, ES requires
the *running title* to own that path before it will grant ISFS write
access — and a stock GX disc-launch never tells ES that the running
title changed.

The fix lives at the start of `GameBooter::BootPartition`:

1. `ISFS_Initialize()` re-attaches NAND after the cIOS reload that
   just happened.
2. `swbf3_savestub_install_if_missing()` checks if title
   `00010000-30303030` is already in NAND. If not, it installs an
   embedded ~4 KB fakesigned WAD via
   `ES_AddTicket` / `ES_AddTitleStart` / `ES_AddContent*`
   / `ES_AddTitleFinish`. Idempotent — subsequent boots see the title
   and skip.
3. `ES_Identify(0x0001000030303030)` is called with the now-NAND-resident
   TMD. ES sets the running TID to `00010000-30303030`, and the game's
   subsequent NAND writes succeed.

The embedded WAD source bytes live in
`source/usbloader/swbf3_savestub_wad.c` (autogenerated by
`build-savestub-wad.js` in the workspace, kept out of this repo).

### Logging

`gecko.c` has `DEBUG_TO_FILE` defined, so every `gprintf()` appends to
`sd:/debug.txt`. `InitGecko()` writes a banner so multi-boot log files
are self-sectioned. `GameBooter::BootGame()` defers
`ShutDownDevices()` until after `gamepatches()` runs (vanilla GX shuts
down before, which unmounts SD and silences the log).

---

## Build from source

The bundled Docker recipe is the build path used to produce the
release artifact:

```
docker build -o . .
```

Produces a loose `boot.dol` plus `usbloader_gx.zip` containing
`boot.dol`, `boot.elf`, `meta.xml`, `icon.png`. The container image is
`devkitpro/devkitppc:20250527`.

A native build outside Docker requires devkitPPC r42+ and an older
libogc (2.3.1) — libogc 3.0.4 introduces a `socklen_t` redefinition
that conflicts with `portlibs/include/sys/socket.h`.

---

## Preparing a dev-disc as a WBFS

The end-to-end workflow uses two existing tools — **`rvthtool`** for
retail re-encryption + fakesign, and **`wit` (Wiimms ISO Tool)** for
the WBFS repack and disc-ID rename.

- Manual commands: [`docs/INSTRUCTIONS.md`](docs/INSTRUCTIONS.md).
- Windows drag-and-drop wrapper:
  [`tools/proto-pipeline/process-game.bat`](tools/proto-pipeline/) —
  auto-downloads both tools on first use with explicit consent.

### Why the custom disc IDs

All three SWBF3 dev builds ship with the same placeholder disc ID
`RABAZZ` and title "Sample Game Name". USB Loader GX identifies games
by ID, so without renaming, all three builds collapse to a single menu
entry. The custom IDs `RSBE3A` / `RSBE3B` / `RSBE3C` are arbitrary
unique-per-build 6-char identifiers in the standard Wii format; they
don't collide with any retail disc ID and they share the `RSBE3` prefix
that this fork's runtime gamepatches key off via
`memcmp((u8*)0x80000000, "RSBE3", 5)`.

See [`docs/CUSTOM_DISC_IDS.md`](docs/CUSTOM_DISC_IDS.md) for the format
and the per-build assignments.

---

## License

Source is **GPL-3.0** (inherited from upstream `wiidev/usbloadergx`).

External tools referenced by the workflow but not redistributed here:
- `rvthtool` (David Korth / GerbilSoft) — GPL-2.0-or-later.
- `wit` (Dirk Clemens / Wiimm) — GPL-2.0-or-later.

This project has no affiliation with and is not endorsed by Nintendo,
Free Radical, Pandemic, LucasArts, Activision, or any of the original
prototype developers.
