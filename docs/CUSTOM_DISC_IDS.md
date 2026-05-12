# Custom disc-ID registry (SWBF3 dev builds)

This fork's runtime patches are gated to disc IDs starting with `RSBE3`.
The three SWBF3 dev builds covered by this fork are:

| Friendly name | Custom ID |
|---------------|-----------|
| Star Wars Battlefront 3 r1.90431a | `RSBE3A` |
| Star Wars Battlefront 3 r90776a | `RSBE3B` |
| Star Wars Battlefront 3 r2.91120a (Nov 21 2008) | `RSBE3C` |

## ID format

Wii disc IDs are 6 chars. The pattern used here is:

- 1st char: **R** for single-layer disc (≤4.7 GB) or **S** for dual-layer.
- chars 2-4: 3-letter game code.
- chars 5-6: arbitrary suffix to distinguish multiple variants of the same
  game (e.g. `D1`/`D2` for "dev 1" / "dev 2", or `3A`/`3B` for sibling
  builds of the same revision family).

## Other Wii prototypes

Most other Wii prototype/dev builds (dev builds of games that DID ship,
or unreleased builds that don't carry SWBF3's particular runtime size
check) boot on **stock USB Loader GX** once they're properly rvthtool
retail-recrypted and wit-repacked with a unique disc ID. They do not
need this fork's runtime patches.

This fork is specifically what you want for SWBF3 -- the three builds
above need both apploader-time fixups (production-boundary rewrite and
MEM-size clamp for the NDEV-vs-retail mismatch) and runtime patches that
only activate when the disc ID starts with `RSBE3`.
