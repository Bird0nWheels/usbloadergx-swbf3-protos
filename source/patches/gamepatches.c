#include <ogc/machine/processor.h>
#include <ogc/libversion.h>
#include <gccore.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "usbloader/disc.h"
#include "wip.h"
#include "gecko.h"
#include "patchcode.h"
#include "gamepatches.h"
#include "memory/memory.h"
#include "memory/mem2.h"
#include "settings/SettingsEnums.h"
#include "version.h"
#include "kirbypatch.h"
/* Riivolution bridge not present in this tree — feature omitted from this
 * SWBF3-focused build. Original: #include "Riivolution/RiivolutionCBridge.h" */
#include "gecko_vprintf_stub.h"  /* payload for gecko_tty_universal_patch() */

/* GCC 11 false positives */
#if __GNUC__ > 10
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif

#define OGC_VERSION (_V_MAJOR_ * 10000 + _V_MINOR_ * 100 + _V_PATCH_)

typedef struct
{
    u8 *dst;
    int len;
} appDOL;

typedef struct
{
    u32 viTVMode;
    u16 fbWidth;
    u16 efbHeight;
    u16 xfbHeight;
    u16 viXOrigin;
    u16 viYOrigin;
    u16 viWidth;
    u16 viHeight;
    u32 xfbMode;
    u8 field_rendering;
    u8 aa;
    u8 sample_pattern[12][2];
    u8 vfilter[7];
} GXRModeObjRVL;

static appDOL *dolList = NULL;
static int dolCount = 0;
extern GXRModeObj *rmode;

bool exclude_game(u8 *gameid, bool checkEmuNAND)
{
    // Used for games that won't work with EmuNAND saves
    if (checkEmuNAND)
    {
        // Excite Truck
        if (memcmp(gameid, "REX", 3) == 0)
            return true;

        return false;
    }
    // Prince of Persia, Driver, Tintin & We Dare
    if (memcmp(gameid, "RPW", 3) == 0 || memcmp(gameid, "SPX", 3) == 0 ||
        memcmp(gameid, "SDV", 3) == 0 || memcmp(gameid, "STN", 3) == 0 ||
        memcmp(gameid, "SLVP41", 6) == 0)
    {
        return true;
    }
    return false;
}

void RegisterDOL(u8 *dst, int len)
{
    if (!dolList)
        dolList = (appDOL *)MEM2_alloc(sizeof(appDOL));

    appDOL *tmp = (appDOL *)MEM2_realloc(dolList, (dolCount + 1) * sizeof(appDOL));
    if (!tmp)
    {
        MEM2_free(dolList);
        dolCount = 0;
        return;
    }

    dolList = tmp;
    dolList[dolCount].dst = dst;
    dolList[dolCount].len = len;
    dolCount++;
}

void ClearDOLList()
{
    if (dolList)
        MEM2_free(dolList);
    dolList = NULL;
    dolCount = 0;
}


/* -- OSReport / printf -> USB Gecko redirect ------------------------------
 *
 * Dev/NDEV SDK builds send their OSReport/printf TTY to the development
 * hardware's serial port = EXI channel 2, which does not exist on a retail
 * Wii, so the output is silently dropped. The USB Gecko is on EXI channel 1.
 *
 * gecko_tty_universal_patch() (below) locates the game's own vsprintf and
 * vprintf in the loaded DOL by SDK/MSL instruction signatures, then replaces
 * vprintf with a stub that reformats via the game's own vsprintf and streams
 * the bytes to the USB Gecko. All SDK serial paths funnel through vprintf
 * (OSReport -> vprintf, printf, asserts/OSPanic, game loggers -> vprintf),
 * so one hook captures everything. No per-game hardcoded addresses -- works
 * on any CodeWarrior/MSL game and survives a game rebuild.
 *
 * Gated per-game by the "USB Gecko TTY" toggle in Game Load Settings
 * (GameCFG.USBGeckoTTY). GameBooter sets GeckoTTYEnabled = 1 before running
 * gamepatches() so serial_gecko_toggle_on() lets the hook install. */

/* Set by GameBooter (BootGame) from the per-game "USB Gecko TTY" setting
 * before gamepatches() runs. Default 0 = off, hook never installed. */
int GeckoTTYEnabled = 0;

static bool serial_gecko_toggle_on(void)
{
    return GeckoTTYEnabled != 0;
}

/* ---- GeckoTTY: vprintf-entry hook for any RVL-SDK / MSL game -------------
 * Locates the game's own vprintf + vsprintf in the loaded image by SDK/MSL
 * instruction signatures (validated on Rayman4, SWBF3, Mighty3 -- 3/3) and
 * redirects vprintf to the EXI1 USB Gecko.  No per-game addresses, so it works
 * for any CodeWarrior/MSL game AND survives a game rebuild (the per-ID hooks
 * above need their addresses re-derived every build; this one does not).
 *
 * Every SDK serial path funnels through vprintf: OSReport->vprintf, printf,
 * game loggers->vprintf, asserts/OSPanic.  vprintf's sink is the dev EXI2
 * channel, dead on retail; we replace vprintf with a stub that reformats via
 * the game's own vsprintf and streams the bytes to the gecko on EXI1.
 *
 * Discovery (see USBGecko+Wii-Serial-Research.md sec 10):
 *   1. vsprintf    = `mr r6,r5; mr r5,r4` (7CA62B78 7C852378), stwu 8B before.
 *   2. __pformatter= first bl inside vsprintf; strcb = the string-writer
 *      callback constant vsprintf builds (lis+addi, >=0x80000000, != pform).
 *   3. output-family = funcs that bl __pformatter but DON'T load strcb (that
 *      excludes sprintf/vsnprintf); vprintf is the one that RECEIVES a va_list
 *      (no `lis r0,0x100` / no `stfd f1` spill -> excludes printf/fprintf).
 *      Lowest address wins (vprintf precedes vfprintf in MSL link order).
 *   4. Patch vprintf[0] = `b cave`; cave = gecko_vprintf_stub, vsprintf poked in.
 */
#define GTTY_SCAN_LO 0x80004000u
#define GTTY_SCAN_HI 0x80900000u

static u32 gtty_blt(u32 w, u32 va)   /* PPC bl target, or 0 if not a bl */
{
    s32 d;
    if ((w & 0xFC000003u) != 0x48000001u) return 0;
    d = (s32)(w & 0x03FFFFFCu);
    if (d & 0x02000000) d -= 0x04000000;
    return va + (u32)d;
}

static u32 gtty_func_start(u32 a)    /* nearest `stwu r1,-X(r1)` above a */
{
    u32 b;
    for (b = a; b > a - 0x800u && b >= GTTY_SCAN_LO; b -= 4)
        if ((*(volatile u32 *)b >> 16) == 0x9421u) return b;
    return 0;
}

/* does func f build constant c via lis(+addi) within its first 0x80 bytes? */
static int gtty_loads_const(u32 f, u32 c)
{
    u32 his[32]; u8 hv[32]; u32 a; int i;
    for (i = 0; i < 32; i++) { his[i] = 0; hv[i] = 0; }
    for (a = f; a < f + 0x80u; a += 4)
    {
        u32 w = *(volatile u32 *)a, op = w >> 26;
        if (op == 15) { u32 rt = (w >> 21) & 31; hv[rt] = 1; his[rt] = (w & 0xffff) << 16; }
        else if (op == 14) { u32 ra = (w >> 16) & 31; s32 lo = (s16)(w & 0xffff);
            if (hv[ra] && (his[ra] + (u32)lo) == c) return 1; }
        else if (op == 24) { u32 rs = (w >> 21) & 31;   /* ori rA,rS,UIMM (lis+ori addr build) */
            if (hv[rs] && (his[rs] | (w & 0xffff)) == c) return 1; }
    }
    return 0;
}

static int gtty_builds_valist(u32 f)
{
    u32 a;
    for (a = f; a < f + 0x80u; a += 4)
    {
        u32 w = *(volatile u32 *)a;
        if (w == 0x3C000100u) return 1;   /* lis r0,0x100 (va_list count init) */
        if (w == 0xD8210028u) return 1;   /* stfd f1,40(r1) (vararg float spill) */
    }
    return 0;
}

static u32 gtty_find_cave(u32 need)  /* zero-pad run after code, >= need bytes */
{
    u32 a, run = 0, start = 0;
    need = (need + 3u) & ~3u;
    for (a = GTTY_SCAN_LO; a < GTTY_SCAN_HI; a += 4)
    {
        if (*(volatile u32 *)a == 0u)
        {
            if (!run) { if (a > GTTY_SCAN_LO && *(volatile u32 *)(a - 4) != 0u) { start = a; run = 4; } }
            else run += 4;
            if (run && run >= need + 8u) return start;
        }
        else run = 0;
    }
    return 0;
}

int gecko_tty_universal_patch(void)
{
    static int done = 0;
    u32 a, b, vsp = 0, pform = 0, strcb = 0, vpr = 0, cave;
    u32 vsp_seen = 0;
    u32 his[32]; u8 hv[32]; int i;

    if (done) return 1;
    if (!serial_gecko_toggle_on()) return 0;

    /* 1+2. vsprintf + its __pformatter call, in one candidate-scanning pass.
     *
     * vsprintf is identified by the arg-shuffle `mr r6,r5; mr r5,r4`
     * (7CA62B78 7C852378) with a `stwu r1,-X(r1)` prologue 8 bytes before, and
     * __pformatter is the first bl inside it.  That two-instruction shuffle can
     * false-match an unrelated function (same register copy, a stwu 8B before,
     * but no __pformatter call within range).  The old code `break`ed on the
     * FIRST signature hit and, if it was such a false positive, printed
     * "__pformatter not found" and aborted the ENTIRE hook -- so e.g. RLGEPR
     * (LEGO Racers proto) got no TTY even with the toggle on.  Instead, keep
     * scanning: a candidate is only the real vsprintf if it actually contains a
     * bl (= __pformatter).  Accept the first candidate that does. */
    for (a = GTTY_SCAN_LO; a < GTTY_SCAN_HI && !vsp; a += 4)
    {
        u32 cand, p = 0, sc = 0;
        if (*(volatile u32 *)a != 0x7CA62B78u || *(volatile u32 *)(a + 4) != 0x7C852378u)
            continue;
        cand = a - 8;
        if ((*(volatile u32 *)cand >> 16) != 0x9421u) continue;   /* needs stwu prologue */
        vsp_seen++;

        for (i = 0; i < 32; i++) { his[i] = 0; hv[i] = 0; }
        for (b = cand; b < cand + 0x60u; b += 4)
        {
            u32 w = *(volatile u32 *)b, op = w >> 26, t;
            if (!p && (t = gtty_blt(w, b)) != 0) p = t;
            if (op == 15) { u32 rt = (w >> 21) & 31; hv[rt] = 1; his[rt] = (w & 0xffff) << 16; }
            else if (op == 14) { u32 ra = (w >> 16) & 31; s32 lo = (s16)(w & 0xffff);
                if (hv[ra]) { u32 v = his[ra] + (u32)lo; if (v >= 0x80000000u && v != p) sc = v; } }
            else if (op == 24) { u32 rs = (w >> 21) & 31;   /* lis+ori address build */
                if (hv[rs]) { u32 v = his[rs] | (w & 0xffff); if (v >= 0x80000000u && v != p) sc = v; } }
        }
        /* A real MSL vsprintf builds BOTH a __pformatter call AND a string-writer
         * callback constant (lis+addi/ori of a code address it passes as the
         * output proc). The `mr r6,r5; mr r5,r4` shuffle alone also occurs in
         * float/matrix helpers (e.g. LEGO Racers RLGEPR 0x801A7C90, which has a
         * bl but loads only SDA floats -> no callback). Requiring BOTH skips
         * those and finds the real vsprintf further along (RLGEPR 0x8025EC14,
         * strcb 0x8025E9DC). Verified live over USB Gecko. */
        if (!p || !sc) continue;
        vsp = cand; pform = p; strcb = sc;
    }
    if (!vsp)
    {
        if (vsp_seen)
            gprintf("GeckoTTY/uni: %u vsprintf candidate(s) but none had pformatter+callback\n",
                    (unsigned)vsp_seen);
        else
            gprintf("GeckoTTY/uni: vsprintf not found\n");
        return 0;
    }

    /* Without strcb we cannot tell sprintf/vsnprintf apart from vprintf, so the
     * vprintf search below would fall through to vsprintf itself (it calls
     * __pformatter, doesn't build a va_list, and isn't excluded) and we'd hook
     * vsprintf -- corrupting it and crashing the game on its first formatted
     * print (observed on RGEED1/SJBE52: vprintf==vsprintf). Bail instead of
     * installing a hook we can't trust. */
    if (!strcb)
    {
        gprintf("GeckoTTY/uni: string-callback not found (vsprintf=%08X pform=%08X); "
                "low-confidence, not hooking\n", vsp, pform);
        return 0;
    }

    /* 3. vprintf = output-family member that receives a va_list */
    for (a = GTTY_SCAN_LO; a < GTTY_SCAN_HI; a += 4)
    {
        u32 w = *(volatile u32 *)a, f;
        if (gtty_blt(w, a) != pform) continue;
        f = gtty_func_start(a);
        if (!f) continue;
        if (f == vsp) continue;                             /* never hook vsprintf itself */
        if (gtty_loads_const(f, strcb)) continue;           /* sprintf/vsnprintf */
        if (gtty_builds_valist(f)) continue;                /* printf/fprintf */
        if (!vpr || f < vpr) vpr = f;
    }
    if (!vpr) { gprintf("GeckoTTY/uni: vprintf not found\n"); return 0; }

    gprintf("GeckoTTY/uni: vsprintf=%08X pformatter=%08X strcb=%08X vprintf=%08X\n",
            vsp, pform, strcb, vpr);

    if ((*(volatile u32 *)vpr >> 16) != 0x9421u)   /* already a branch -> hooked */
    {
        gprintf("GeckoTTY/uni: vprintf already hooked\n");
        done = 1;
        return 1;
    }

    /* 4. cave + install */
    cave = gtty_find_cave(sizeof(gecko_vprintf_stub));
    if (!cave) { gprintf("GeckoTTY/uni: no cave for %u B stub\n",
                         (unsigned)sizeof(gecko_vprintf_stub)); return 0; }

    memcpy((void *)cave, gecko_vprintf_stub, sizeof(gecko_vprintf_stub));
    *(volatile u16 *)(cave + GECKO_VPRINTF_STUB_VSP_HI) = (u16)(vsp >> 16);
    *(volatile u16 *)(cave + GECKO_VPRINTF_STUB_VSP_LO) = (u16)(vsp & 0xffff);
    DCFlushRange((void *)cave, sizeof(gecko_vprintf_stub));
    ICInvalidateRange((void *)cave, sizeof(gecko_vprintf_stub));

    {
        s32 disp = (s32)(cave - vpr);
        *(volatile u32 *)vpr = 0x48000000u | ((u32)disp & 0x03FFFFFCu);  /* b cave */
        DCFlushRange((void *)vpr, 4);
        ICInvalidateRange((void *)vpr, 4);
    }
    gprintf("GeckoTTY/uni: hooked vprintf %08X -> b cave %08X (stub %u B, vsprintf %08X)\n",
            vpr, cave, (unsigned)sizeof(gecko_vprintf_stub), vsp);
    done = 1;
    return 1;
}

void gamepatches(u8 videoSelected, u8 videoPatchDol, u8 aspectForce, u8 languageChoice, u8 patchcountrystring,
                 u8 vipatch, u8 deflicker, u8 disableMotor, u8 disableSpeaker,
                 u8 sneekVideoPatch, u8 hooktype, u8 videoWidth, u64 returnTo, u8 privateServer, const char *serverAddr)
{
    int i;
    u8 vfilter_off[7] = {0, 0, 21, 22, 21, 0, 0};
    u8 vfilter_low[7] = {4, 4, 16, 16, 16, 4, 4};
    u8 vfilter_medium[7] = {4, 8, 12, 16, 12, 8, 4};
    u8 vfilter_high[7] = {8, 8, 10, 12, 10, 8, 8};

    /* Dev-disc bring-up (RM4E01 = Rayman 4 / King Kong Jade prototype, NDEV).
     *
     * Symptom on retail (USB Loader GX): apploader runs fully and jumps to the
     * DOL entry 0x80004060, then black screen with NO OSReport / banner / HDD
     * activity -- i.e. it dies in the __start/OSInit era, BEFORE the game's
     * memory-pool setup (which would print "Mem1/Mem2 size ..." first). On the
     * NDEV the exact same DOL boots all the way in-game, so this is the classic
     * NDEV->retail low-memory gap (see RKPED7 below + the dev_builds_mem2 note).
     *
     * Same root cause as RKPED7: the dev DOL's decrementer / external-interrupt
     * handler at lowmem 0x900 dereferences MEM[0x800000C0] as its OSContext
     * save-area pointer. NDEV IPL populates it; retail leaves it 0, so the first
     * interrupt does `stw rX, 0xC(0)` -> DSI fault on physical 0x0C -> uncatchable
     * cascade -> black screen before OSReport. Fix = stamp MEM[0xC0] with a valid
     * lowmem buffer (0x80003C00, the zero-padded tail of the BI2 area, same slot
     * RKPED7 uses). Also correct the MEM2 *physical size* mirror 0x80003118 to the
     * retail truth (64MB) -- the loader's apploader.c clamps it low, and Jade's
     * MEMdynOpt reads OSGetPhysicalMem2Size() to size the MEM2 heaps (NDEV showed
     * 128MB there). MEM1-size globals (0x28/0xF0) are left as Disc_SetLowMem set
     * them (= 24MB, correct for retail; do NOT clobber -- see the WIFUFR note).
     *
     * Falls through (no return) so the normal video patches AND the RM4E01
     * GeckoTTY OSReport hook still run once it survives early init. */
    if (memcmp((const void *)0x80000000, "RM4E01", 6) == 0)
    {
        *(volatile u32 *)0x800000C0 = 0x80003C00;  /* OSContext save ptr (dec handler) */
        *(volatile u32 *)0x80003118 = 0x04000000;  /* MEM2 physical size = retail 64MB */
        DCFlushRange((void *)0x800000C0, 4);
        DCFlushRange((void *)0x80003118, 4);
        ICInvalidateRange((void *)0x800000C0, 4);

        /* __OSInitAudioSystem debug-assert bypass (OSAudioSystem.c 253/257/261/333):
         * REMOVED from the loader 2026-06-18.  These asserts are now bypassed in the
         * DOL itself by patch_dol.py, which SELF-DERIVES __OSInitAudioSystem (nm) and
         * patches the 4 gates every rebuild -- always correct.  The loader's hardcoded
         * gate addresses went stale on a wide-recompile build (__OSInitAudioSystem
         * 0x8019f280 -> 0x8019f880, +0x600): the "self-protect" bc-check passed on
         * UNRELATED bc instructions at the stale addresses and corrupted 4 code sites
         * in OS init -> Unhandled Exception 3 (ISI), jump to 0x0, retail boot dead.
         * Letting the DOL own this (self-deriving) removes the per-build staleness. */

        gprintf("RM4E01: NDEV->retail fixup (MEM[C0], MEM2=64MB; audio asserts handled in-DOL by patch_dol).\n");
        /* intentional fall-through */
    }

    /* Dev-disc bring-up (RKPED7 = Kung Fu Panda Dec 7 2007 prototype).
     *
     * Key finding (validated via Dolphin source patches + dolphin-mcp live
     * memory inspection): the dev DOL's decrementer exception handler at
     * lowmem 0x80000900 dereferences `MEM[0x800000C0]` as the context save
     * pointer, then writes saved GPRs to `*MEM[0xC0] + 0x0C/0x10/...`. On
     * NDEV hardware IPL populates MEM[0xC0] with a valid OSContext-sized
     * buffer address; on retail Wii it stays 0, so when MSR[EE]=1 fires
     * the first decrementer interrupt, the handler does `stw r3, 0xC(0)`
     * which faults DSI on physical 0x0C → uncatchable cascade → black
     * screen.
     *
     * Fix: stamp MEM[0xC0] with a valid lowmem buffer address (0x80003C00,
     * inside the BI2 area which is zero-padded after the first 0x40 bytes
     * — comfortably 0x200+ bytes of unused space for the context save).
     * The dispatch table at MEM[0x80003020+] gets populated by OSInit's
     * vector-commandeer pass with valid handler chain entries (verified),
     * so once the dec handler can save state, the rest of the OS chain
     * runs normally.
     *
     * Also keep MEM2-size correction. */
    if (memcmp((const void *)0x80000000, "RKPED7", 6) == 0)
    {
        /* MEM2 size = 64MB (retail truth) at all three mirrors the SDK
         * reads. Loader's apploader.c clamps these to 24MB by default. */
        *(volatile u32 *)0x80000028 = 0x04000000;
        *(volatile u32 *)0x800000F0 = 0x04000000;
        *(volatile u32 *)0x80003118 = 0x04000000;

        /* THE fix: valid OSContext save-area pointer for the dev DOL's
         * decrementer / external-interrupt handler at lowmem 0x900.
         * Even with EE disabled, leave this set in case any path fires. */
        *(volatile u32 *)0x800000C0 = 0x80003C00;

        /* === Maximum-compat patch set ===
         * Dolphin investigation showed: with EE enabled the OS handler
         * chain at 0x80535EA8 eventually crashes (PC ends up at uninit
         * 0x8132ffc8). With EE disabled the SDA-flag spins are infinite
         * (no ISR sets the flag). Resolution: disable EE AND NOP every
         * known wait spin so they fall through. State init won't fully
         * complete but boot can progress to visible video. */

        /* Don't enable EE at this OSInit site (avoids unbounded handler
         * chain). The bl to sub_80539DB4 becomes a NOP. */
        *(volatile u32 *)0x8053C3D8 = 0x60000000;

        /* All known SDA-flag/state spin back-branches → NOP. */
        *(volatile u32 *)0x80535A28 = 0x60000000;  /* OSInit do-while */
        *(volatile u32 *)0x8053C3E4 = 0x60000000;  /* SDA flag inner spin */
        *(volatile u32 *)0x8053C3F4 = 0x60000000;  /* SDA flag outer spin */
        *(volatile u32 *)0x8054DFBC = 0x60000000;  /* SDA flag wait #2 */

        /* AR/DSP init nuke — six DSP-register infinite polls on retail. */
        *(volatile u32 *)0x80536C94 = 0x4E800020;  /* blr */

        /* DSP status accessor — return ready. */
        *(volatile u32 *)0x80555B64 = 0x38600001;  /* li r3, 1 */
        *(volatile u32 *)0x80555B68 = 0x4E800020;  /* blr */

        /* IPC sync-wait bypass. */
        *(volatile u32 *)0x80561AA8 = 0x3860FFFD;  /* li r3, -3 */
        *(volatile u32 *)0x80561AAC = 0x4E800020;  /* blr */

        DCFlushRange((void *)0x80000000, 0x3f00);
        DCFlushRange((void *)0x80003100, 0x40);
        DCFlushRange((void *)0x8053C3D8, 4);
        DCFlushRange((void *)0x80535A28, 4);
        DCFlushRange((void *)0x8053C3E4, 4);
        DCFlushRange((void *)0x8053C3F4, 4);
        DCFlushRange((void *)0x8054DFBC, 4);
        DCFlushRange((void *)0x80536C94, 4);
        DCFlushRange((void *)0x80555B64, 8);
        DCFlushRange((void *)0x80561AA8, 8);
        ICInvalidateRange((void *)0x8053C3D8, 4);
        ICInvalidateRange((void *)0x80535A28, 4);
        ICInvalidateRange((void *)0x8053C3E4, 4);
        ICInvalidateRange((void *)0x8053C3F4, 4);
        ICInvalidateRange((void *)0x8054DFBC, 4);
        ICInvalidateRange((void *)0x80536C94, 4);
        ICInvalidateRange((void *)0x80555B64, 8);
        ICInvalidateRange((void *)0x80561AA8, 8);

        gprintf("RKPED7: max-compat (no-EE + spin NOPs + DSP/IPC + MEM[C0]).\n");
        free_wip();
        ClearDOLList();
        return;
    }

    /* Wii Self Defense (debug build).
     *
     * Source disc ID "WIFUFR"; we repack via wit with ID "RWSDDB" — accept
     * both since the same main.dol bytes are loaded either way.
     *
     * Crash signature (log00001.txt on retail Wii):
     *   Error 02 (Machine Check), SRR0=0x8009f5e0, DAR=0xffffffff
     *   Stack: sub_8009F568 <- sub_800A019C <- ...
     *
     * Root cause: this game is sized for NDEV's 128 MB MEM2; on retail's
     * 64 MB MEM2 a runtime allocation for a text/glyph manager returns -1.
     * sub_800A019C loads *(0x803E0140), reads field +0x18 (= 0xFFFFFFFF
     * on retail) and passes it as r3 to sub_8009F568, which immediately
     * dereferences with `lwz r0, 0(r31)`. On Dolphin with MEM2 bumped to
     * 128 MB the allocation succeeds and the field is a real pointer, so
     * Dolphin doesn't see this — the bug is retail-only.
     *
     * Fix: route the call through a tiny code cave that returns early
     * when r3 == -1, otherwise tail-jumps to the original function. The
     * cave lives in the dead 32-byte gap between data[5] end (0x8023B4A0)
     * and BSS start (0x8023B4C0). No xrefs in IDA; verified zero in
     * Dolphin's live MEM1. */
    if (memcmp((const void *)0x80000000, "RWSDDB", 6) == 0 ||
        memcmp((const void *)0x80000000, "WIFUFR", 6) == 0)
    {
        /* MEM2 arena setup — verified live in Dolphin where WSD works.
         * USB Loader GX's Disc_SetLowMem already writes 0x80000028 and
         * 0x800000F0 = 0x01800000 (MEM1 size). DO NOT overwrite those —
         * earlier versions of this patch set them to 0x04000000 thinking
         * they were MEM2 size, but that's wrong; the CLAUDE.md note was
         * misleading. WSD reads them strictly as MEM1 size.
         *
         * The MEM2 layout the SDK actually needs (per Dolphin live state):
         *   0x80003118 = 0x04000000  ; MEM2 size = 64MB
         *   0x80003120 = 0x93600000  ; MEM2 top
         *   0x80003124 = 0x90000800  ; MEM2 arena lo
         *   0x80003128 = 0x935E0000  ; MEM2 arena hi
         *   0x80003130 = 0x935E0000  ; IPC buffer lo
         *   0x80003134 = 0x93600000  ; IPC buffer hi
         *
         * Without these, sub_80108EBC(arena_lo) is never called, the
         * slab manager pool stays empty, and sub_80049BB8 dereferences
         * 0 + (-16) = 0xFFFFFFF0 → DSI exception (log00005-007). */
        *(volatile u32 *)0x80003118 = 0x04000000;
        *(volatile u32 *)0x80003120 = 0x93600000;
        *(volatile u32 *)0x80003124 = 0x90000800;
        *(volatile u32 *)0x80003128 = 0x935E0000;
        *(volatile u32 *)0x80003130 = 0x935E0000;
        *(volatile u32 *)0x80003134 = 0x93600000;
        DCFlushRange((void *)0x80003100, 0x40);

        /* NEW MINIMAL WSD RECIPE — per NOTES_firmware_version_check_bypass.md
         *
         * Per the proven Mighty 3 fix, the actual ERROR #002 source on
         * SDK 4.x dev/proto builds is the kernel firmware-version check
         * inside the OSInit-equivalent function. The kernel reads:
         *   - *0xC0003140/0xC0003144 = running IOS firmware version
         *   - *0x80003188              = SDK-expected version (in DOL)
         * If running IOS < expected, it calls __OSPanic → ERROR #002.
         *
         * WSD's check lives inside sub_80107960:
         *   80107CF0  lwz r20, 0x3144(r4)   ; running low
         *   80107CF8  lwz r0,  0x3140(r4)   ; running high
         *   80107D08  lwz r4,  0x3188(r3)   ; expected
         *   80107D60  cmplw r12, r3         ; cmp high
         *   80107DA4  bne loc_80107DB4      ; ← MISMATCH → error
         *   80107DA8  bne loc_80107DF4      ; (paired)
         *   80107DAC  cmplw r6, r4         ; cmp low
         *   80107DB0  bge loc_80107DF4      ; running >= expected -> skip
         *   80107DB4  addi r3, r31, 0x318   ; r31=0x80217598 -> 0x802178B0
         *                                       = "OS ERROR: This firmware..."
         *   80107DBC  bl OSReport
         *   80107DF0  bl panic
         *   80107DF4  bl sub_80126EDC      ; success continuation
         *
         * Patch the bne at 0x80107DA4 to an unconditional `b +0x50` so
         * execution jumps straight to the success continuation, skipping
         * both the mismatch path and the secondary low-half comparison. */
        *(volatile u32 *)0x80107DA4 = 0x48000050;
        DCFlushRange((void *)0x80107DA4, 4);
        ICInvalidateRange((void *)0x80107DA4, 4);

        /* ===== TEXT-MGR ROOT-CAUSE HOOK (replaces former per-callsite caves) =====
         *
         * Every text-mgr crash so far (sub_8009F568, sub_8009F6C4, sub_8009FC1C,
         * sub_8009FCD0, ...) traces back to ONE bad pointer: the field at
         * *(*(0x803E0140) + 0x18). That field is written exactly ONCE in the
         * entire game — by the text-mgr init function sub_8009F84C, at
         * 0x8009F96C `stw r3, 0x18(r4)`, with r3 = return value of
         * sub_8009ECB0(...). On Dolphin the call returns a valid 0x9089xxxx
         * MEM2 pointer; on retail it returns HVS sentinel filler
         * (0xB1E6B1E6) or -1 because some internal alloc inside fails.
         *
         * Instead of patching each downstream consumer (whack-a-mole), we
         * intercept the SINGLE store site: replace the stw with a bl into a
         * cave that substitutes a safe pointer to a guaranteed-zero region.
         * All downstream reads then see zeros and exit their loops
         * harmlessly, no crash.
         *
         * SAFE PTR = 0x80004170. This sits inside a 208-byte zero
         * alignment-padding block (0x80004163..0x80004233) immediately
         * after the "Target Resident Kernel for PowerPC\0" string in the
         * DOL's text section. The bytes are LOADED by apploader, NOT
         * zeroed by _start (text segment, not BSS), and NOT written by
         * game code (it's text/rodata — consumers only READ pool fields,
         * never write). Earlier version of this hook used 0x804FE000 in
         * BSS, but that turned out to be inside the MEM1 heap arena (the
         * DEADBABE canary sits at 0x804FC470), so game-time allocations
         * could clobber it — causing the Try-press hang user reported.
         *
         * 16-byte cave @ 0x8023B4A0:
         *   lis  r3, 0x8000        ; 3C 60 80 00
         *   ori  r3, r3, 0x4170    ; 60 63 41 70  -> r3 = 0x80004170 safe ptr
         *   stw  r3, 0x18(r4)      ; 90 64 00 18  (original store, fixed value)
         *   blr                    ; 4E 80 00 20  -> 0x8009F970
         */
        *(volatile u32 *)0x8023B4A0 = 0x3C608000;
        *(volatile u32 *)0x8023B4A4 = 0x60634170;
        *(volatile u32 *)0x8023B4A8 = 0x90640018;
        *(volatile u32 *)0x8023B4AC = 0x4E800020;
        DCFlushRange((void *)0x8023B4A0, 16);
        ICInvalidateRange((void *)0x8023B4A0, 16);

        /* Hook site: 0x8009F96C was `stw r3, 0x18(r4)` (0x90640018).
         * Patch to `bl 0x8023B4A0`:
         *   disp = 0x8023B4A0 - 0x8009F96C = 0x0019BB34
         *   bl   = 0x48000001 | (disp & 0x03FFFFFC) = 0x4819BB35 */
        *(volatile u32 *)0x8009F96C = 0x4819BB35;
        DCFlushRange((void *)0x8009F96C, 4);
        ICInvalidateRange((void *)0x8009F96C, 4);

        /* SECOND ROOT-CAUSE HOOK (added after log00015):
         * Short-circuit sub_8009ECB0 entry. The cave-via-stw hook above
         * relies on the bl-into-cave taking effect at 0x8009F96C; log00015
         * shows GPR03 = -1 still reaching sub_8009F568, meaning either
         * the cave didn't run, or struct+0x18 was overwritten back to -1
         * elsewhere. Belt-and-suspenders: also patch sub_8009ECB0 to
         * IMMEDIATELY return 0x80004170 in r3, so the ORIGINAL stw at
         * 0x8009F96C (if our cave hook somehow misses) still writes a
         * valid pool ptr. sub_8009ECB0 has exactly ONE caller (sub_8009F84C
         * at 0x8009F960), so short-circuiting is safe — that function's
         * sub-allocations are all garbage on retail anyway.
         *
         * Overwrite first 3 instructions of sub_8009ECB0:
         *   stwu r1, ...     ->  lis  r3, 0x8000     (0x3C608000)
         *   mflr r0          ->  ori  r3, r3, 0x4170  (0x60634170)
         *   stw  r0, ...     ->  blr                  (0x4E800020)
         * No stack frame is built (we never allocate, never mflr), so the
         * lack of epilogue cleanup is fine — bl set LR; blr returns to it. */
        *(volatile u32 *)0x8009ECB0 = 0x3C608000;
        *(volatile u32 *)0x8009ECB4 = 0x60634170;
        *(volatile u32 *)0x8009ECB8 = 0x4E800020;
        DCFlushRange((void *)0x8009ECB0, 12);
        ICInvalidateRange((void *)0x8009ECB0, 12);

        /* RESTORED defense-in-depth (removed in the prior build, which
         * regressed to a startup crash — log00015 — when neither hook
         * variant produced the expected safe pointer; without these the
         * dangerous lwz fires immediately at sub_8009F568). Each turns a
         * dangerous deref into a literal `li r0/r3, 0`. Harmless if the
         * hook works (the deref reads zero anyway from 0x80004170); a
         * crash-saver if it doesn't. */
        *(volatile u32 *)0x8009F5E0 = 0x38000000;  /* li r0, 0  (was lwz r0,0(r31)) */
        DCFlushRange((void *)0x8009F5E0, 4);
        ICInvalidateRange((void *)0x8009F5E0, 4);
        *(volatile u32 *)0x8009F6A0 = 0x38600000;  /* li r3, 0  (was lwz r3,0(r31)) */
        DCFlushRange((void *)0x8009F6A0, 4);
        ICInvalidateRange((void *)0x8009F6A0, 4);

        /* Allocator slab-pool fix (two parts).
         *
         * Part A: empty-pool (v34 == 0). Patch 0x80049E80 (`bl assertion`)
         * to `b loc_80049F4C` so when the v34==0 branch falls through to
         * the assertion path, we exit to fallback allocator instead of
         * crashing on lbz 0(0xFFFFFFF0). */
        *(volatile u32 *)0x80049E80 = 0x480000CC; /* b +0xCC -> 0x80049F4C */
        DCFlushRange((void *)0x80049E80, 4);
        ICInvalidateRange((void *)0x80049E80, 4);

        /* RESTORED defense-in-depth skip patches (removed in the prior
         * build that regressed to log00015). If both hooks above place a
         * valid pool ptr at struct+0x18, these are no-ops (the original
         * code reads zeros and exits normally). If a hook misses, these
         * keep the dangerous derefs from faulting. */

        /* sub_8009FCD0 lwz crash (log00014 origin). */
        *(volatile u32 *)0x8009FCF4 = 0x38600000; /* li r3, 0 */
        *(volatile u32 *)0x8009FCF8 = 0x48000048; /* b +0x48 -> 0x8009FD40 */
        DCFlushRange((void *)0x8009FCF4, 8);
        ICInvalidateRange((void *)0x8009FCF4, 8);

        /* sub_8009FC1C lwz crash (log00013 origin). */
        *(volatile u32 *)0x8009FC5C = 0x48000044; /* b +0x44 -> 0x8009FCA0 */
        DCFlushRange((void *)0x8009FC5C, 4);
        ICInvalidateRange((void *)0x8009FC5C, 4);

        /* sub_8009F6C4 lwz crash (log00012 origin). */
        *(volatile u32 *)0x8009F6F4 = 0x48000048; /* b +0x48 -> 0x8009F73C */
        DCFlushRange((void *)0x8009F6F4, 4);
        ICInvalidateRange((void *)0x8009F6F4, 4);

        /* sub_80093F90 MSR-fiddle Program Interrupt (Try crash, log00012).
         * After sub_8009F9AC returns, the caller at 0x800940C8 does:
         *   mfmsr r0; ori r3, r0, 0x400; mtmsr r3; mtmsr r0
         * which enables the SE (Single-Step Trace) bit then immediately
         * disables it. On NDEV the trace exception is handled (probably
         * for SDK logging). On retail there is no trace handler, so the
         * mtmsr r3 fires a Program Interrupt → ERROR 10. Skip the whole
         * 4-instruction MSR-fiddle by replacing `mfmsr r0` at 0x800940C8
         * with `b +0x10` (= 0x800940D8, just past mtmsr r0). */
        *(volatile u32 *)0x800940C8 = 0x48000010; /* b +0x10 -> 0x800940D8 */
        DCFlushRange((void *)0x800940C8, 4);
        ICInvalidateRange((void *)0x800940C8, 4);

        /* sub_8009F9AC pool-slot allocator crash (Interceptor Try crash).
         * Function dereferences *(0x803E0140 + 0x10) which is uninitialized
         * 0xFFFFFFFF on retail, producing lhzx r4, r6, r4 with
         * r6=0xFFFFFFFF → DAR=0x0001FFFD (log00011). Patch the crashing
         * lhzx at 0x8009F9DC with `b 0x8009FA50` (jump to function epilogue).
         * The function returns with r3 unchanged (still = a1 = struct+0x5C
         * from the caller's `addi r3, r3, 0x5C` at 0x8009408C), which is a
         * valid pointer; the caller then stores it and later derefs at +8
         * (= struct+0x64) which is a normal field. No new pool slot is
         * created but no crash either. */
        *(volatile u32 *)0x8009F9DC = 0x48000074; /* b +0x74 -> 0x8009FA50 */
        DCFlushRange((void *)0x8009F9DC, 4);
        ICInvalidateRange((void *)0x8009F9DC, 4);

        /* Part B: bad-terminator (v34 == 0xFFFFFFFF). The loop iterates
         * a circular slab list expecting v34 to wrap back to v28
         * (=0x8028C744). On retail, after some iterations v34 becomes
         * 0xFFFFFFFF (uninitialized "next" pointer in the last slab),
         * the exit check `cmplw r28, r27` finds them unequal, and the
         * loop continues with v34 = -1 → crash at lbz 0(0xFFFFFFEF).
         *
         * Fix: replace `cmplw r28, r27` at 0x80049F40 with `bl cave_b`.
         * Cave sets CR0 EQ if v34 == -1 OR v34 == v28 (else NE), then
         * blr. The subsequent `bne` at 0x80049F44 uses CR0:
         *   EQ → fall through to exit (0x80049F48)
         *   NE → continue loop (back to 0x80049E54)
         *
         * Cave @ 0x8023B4B0..0x8023B4BF (4 instr in dead gap after the
         * text-mgr cave): */
        *(volatile u32 *)0x8023B4B0 = 0x2C1BFFFF; /* cmpwi r27, -1  */
        *(volatile u32 *)0x8023B4B4 = 0x41820008; /* beq +8 -> blr  */
        *(volatile u32 *)0x8023B4B8 = 0x7C1CD840; /* cmplw r28, r27 */
        *(volatile u32 *)0x8023B4BC = 0x4E800020; /* blr            */
        DCFlushRange((void *)0x8023B4B0, 16);
        ICInvalidateRange((void *)0x8023B4B0, 16);
        *(volatile u32 *)0x80049F40 = 0x481F1571; /* bl 0x8023B4B0  */
        DCFlushRange((void *)0x80049F40, 4);
        ICInvalidateRange((void *)0x80049F40, 4);

        /* Software-only on-Wii log capture was attempted (panic-logger
         * hook + ISFS write to /shared2 + MEM2 ring buffer + SD dump on
         * next boot) and abandoned -- see
         * Wii-self-defense/NOTES_log_capture_attempts.md.  Real-time
         * crash logs will come from a hardware EXI serial dongle
         * (ESP32-S3 USB-Gecko replacement, similar to GexUSB), which
         * picks up OSReport/__OSPanic output from the SDK directly --
         * no in-game hook required. */

        /* Diagnostic readback */
        gprintf("WSD-verify: 80107DA4=%08X 8009F96C=%08X 8023B4A0=%08X "
                "8023B4A4=%08X 8023B4A8=%08X 8023B4AC=%08X 80049E80=%08X "
                "80003118=%08X 80003124=%08X 80003128=%08X\n",
                *(volatile u32 *)0x80107DA4,
                *(volatile u32 *)0x8009F96C,
                *(volatile u32 *)0x8023B4A0,
                *(volatile u32 *)0x8023B4A4,
                *(volatile u32 *)0x8023B4A8,
                *(volatile u32 *)0x8023B4AC,
                *(volatile u32 *)0x80049E80,
                *(volatile u32 *)0x80003118,
                *(volatile u32 *)0x80003124,
                *(volatile u32 *)0x80003128);
        gprintf("WSD: firmware-check NOP + text-mgr init hook @0x8009F96C "
                "+ allocator/MSR-fiddle skips applied.\n");
        
        /* Install OSReport/printf -> USB Gecko hook (per-game toggle). */
        gecko_tty_universal_patch();

        free_wip();
        ClearDOLList();
        return;
        /* OLD CODE BELOW — preserved as documentation, no longer reached */
        /* HVS "unauthorized device" gate at sub_8012C730:
         *     bl     sub_8010E3F0       ; r3 <- *0x80003118 (MEM2 size)
         *     addis  r0, r3, -0x800     ; r0 = mem2 - 128MB
         *     cmplwi r0, 0
         *     bne    fail               ; if !=128MB -> Error #001
         *     li     r3, 1              ; success
         * NOP the bne at 0x8012C738. */
        *(volatile u32 *)0x8012C738 = 0x60000000;
        DCFlushRange((void *)0x8012C738, 4);
        ICInvalidateRange((void *)0x8012C738, 4);

        /* Free Radical "ERROR 2" crash — DSI exception at 0x8009F5E0,
         * DAR=0xFFFFFFFF (per log00001.txt).
         *
         *   sub_8009F568:               ; text-mgr / glyph manager
         *     8009F568..8009F574: prologue (stwu, mflr, stw, addi r11)
         *     8009F578: bl __save_gpr_28
         *     8009F57C: mr r31, r3       ; r31 = caller's argument
         *     ...
         *     8009F5E0: lwz r0, 0(r31)   ; CRASH: r31 == 0xFFFFFFFF
         *     ...
         *     8009F6AC..8009F6C0: epilogue (addi r11, bl __restore_gpr_28,
         *                                    lwz r0, mtlr, addi r1, blr)
         *
         * The argument r3 is -1 because sub_800A019C reads a pointer
         * from *(0x803E0140), then field +0x18 of that pointer which is
         * uninitialized to -1 on retail (allocation never happened on
         * 64MB MEM2 — would have on NDEV's 128MB).
         *
         * Fix: intercept the bl at 0x8009F578. The cave first forwards
         * to __save_gpr_28 (preserving original behavior — r28..r31
         * still saved correctly on the caller's frame at r1+0x10..+0x1C).
         * Then if r3 was -1, we jump straight to the function's own
         * epilogue at 0x8009F6AC, which restores via __restore_gpr_28
         * and returns to the original caller cleanly. This bypasses the
         * entire body (both loops) without needing a "safe pointer"
         * substitution — avoids relying on unverified zero memory. */
        /* Cave @ 0x8023B4A0..0x8023B4BF (32 bytes in dead gap between
         * data[5] end (0x8023B4A0) and BSS start (0x8023B4C0)). */
        *(volatile u32 *)0x8023B4A0 = 0x7D8802A6; /* mflr  r12              */
        *(volatile u32 *)0x8023B4A4 = 0x4BEB8BCD; /* bl    sub_800F4070     */
        *(volatile u32 *)0x8023B4A8 = 0x2C03FFFF; /* cmpwi r3, -1           */
        *(volatile u32 *)0x8023B4AC = 0x40820008; /* bne   +0x8 -> normal   */
        *(volatile u32 *)0x8023B4B0 = 0x4BE641FC; /* b     0x8009F6AC (epilogue) */
        *(volatile u32 *)0x8023B4B4 = 0x7D8803A6; /* mtlr  r12   (normal:)  */
        *(volatile u32 *)0x8023B4B8 = 0x4E800020; /* blr                    */
        *(volatile u32 *)0x8023B4BC = 0x00000000; /* (pad, kept zero)       */
        DCFlushRange((void *)0x8023B4A0, 32);
        ICInvalidateRange((void *)0x8023B4A0, 32);

        /* Site patch: 0x8009F578 was `bl __save_gpr_28`; redirect to cave. */
        *(volatile u32 *)0x8009F578 = 0x4819BF29; /* bl 0x8023B4A0 */

        /* Allocator slab-pool-empty fix (per Dolphin/retail comparison).
         * The slab manager at 0x8028C700 lives in BSS (zeroed by _start),
         * so we cannot pre-init it from gamepatches. On retail, by the
         * time sub_80049BB8 is called, *v28 (first slab head) = 0 because
         * SDK's heap manager never populated it — the lowmem arena setup
         * alone isn't sufficient; some IOS-mediated init step is missing.
         *
         * Workaround: detect the v34==0 case at the assertion call site
         * and skip past the loop entirely. When v34 != 0 (Dolphin/normal
         * case), the bne at 0x80049E64 jumps directly to 0x80049E84,
         * bypassing our patch. When v34 == 0 (retail bug), execution
         * reaches 0x80049E80; we replace that `bl sub_800B6A9C` with a
         * `b loc_80049F4C` (skip past loop end → fallback allocator
         * sub_80096CE8 at 0x8004A0F8). */
        *(volatile u32 *)0x80049E80 = 0x480000CC; /* b +0xCC = 0x80049F4C */
        DCFlushRange((void *)0x80049E80, 4);
        ICInvalidateRange((void *)0x80049E80, 4);
        DCFlushRange((void *)0x8009F578, 4);
        ICInvalidateRange((void *)0x8009F578, 4);

        gprintf("RWSDDB/WIFUFR: MEM2=64MB + dev-check NOPed + text-mgr -1 guard "
                "(cave jumps to epilogue at 0x8009F6AC when r3==-1).\n");
        free_wip();
        ClearDOLList();
        return;
    }

    /* Disney Universe (prototype) — Eurocom dev build, disc ID RDUNPR.
     *
     * Boot reaches Apploader_Run ret=0, 480p patch applies, then black
     * screen on jump to entry. Same SDK "is this NDEV" gate WSD has,
     * just at a different VA. IDA shows it at sub_8072B8F0 +0x20:
     *   8072b908  lis   r29, 0x460A
     *   8072b90c  bl    sub_8070E740        ; r3 <- *(0x80003118) MEM2 size
     *   8072b910  addis r0, r3, -0x800      ; r0 = mem2 - 128MB
     *   8072b914  cmplwi r0, 0
     *   8072b918  bne   8072B924            ; if !=128MB -> fall through to fail
     *   8072b91c  li    r3, 1               ; success
     *   8072b920  b     8072BB50            ; return 1
     *   ...
     *   807078b4  bl    sub_8072B8F0        ; caller in sub_807073D0
     *   807078b8  cmpwi r3, 0
     *   807078bc  bne   end                 ; if returned 0, halt:
     *   807078c0  bl    sub_8070FA50        ; -> sub_8070F7C0(0) noreturn
     *
     * Caller is gated by `*(0x80003184) == 128` (the SDK device-class
     * byte). On a real NDEV that's 0x80; USB Loader GX's apploader
     * leaves it as whatever the dev disc set, so the gate fires.
     *
     * Fix: same as RWSDDB — write retail 64MB into lowmem MEM2 size
     * fields (so the SDK heap manager configures honestly), and NOP
     * the bne at 0x8072B918 so the dev gate always returns 1. Disney
     * also reads 0x80003118 directly via sub_8070E740 — write 64MB
     * there too so any other code paths reading it stay consistent
     * with reality. */
    /* Shared SDK dev-detect path used by Disney, NFS Montreal, NFS Focus
     * Test, Oscar MS4, and others built with the same Revolution SDK
     * version. The OSInit-equivalent function reads `lbz 0x3184(r3)`
     * where r3 = 0x80000000. USB Loader GX's Disc_SetLowMem (disc.c:67)
     * sets *(0x80003184) = 0x80000000; the high byte is 0x80 (= 128),
     * which the SDK interprets as "running on NDEV". The SDK then calls
     * a dev-validate function; a 0 return triggers __OSPanic → ERROR #002.
     *
     * Previous attempt cleared 0x80003184 = 0, but that ADDRESS IS the
     * libogc GameID_Address holder — clearing it destroyed the GameID
     * (which is critical for many SDK paths), making things worse. The
     * SWBF3-proven approach is to leave that byte alone and NOP the bne
     * inside the dev-validate function (force success-return). */

    /* SDK 4.x proto builds: OSInit firmware-version-check NOP.
     *
     * Same root cause as the WSD/Mighty 3 fix above — the kernel's
     * OSInit-equivalent reads running IOS firmware version (lwz from
     * 0x3140/0x3144) and the SDK-expected version (lwz from 0x3188),
     * then `bne` to a __OSPanic block when they disagree. On retail
     * IOS the bne fires and the game shows ERROR #002.
     *
     * VAs were found per-game by byte-pattern scan of each title's
     * extracted main.dol. The 16-byte signature
     *   40 82 00 10  40 8x xx xx  7C xx xx 40  40 80 xx xx
     * (bne +0x10 ; bne ; cmplw ; bge) is unique enough to pin the
     * patch site. The bne-displacement variant determines how far we
     * jump to reach the success continuation; verified by checking
     * that the target VA lands on a `bl` instruction.
     *
     * Disney Universe (RDUNPR) is NOT in this block — its failure
     * mode is a silent post-apploader black screen, not ERROR #002.
     * Keep it on BASELINE until we capture a real failure signature. */
    if (memcmp((const void *)0x80000000, "RNFA09", 6) == 0)
    {
        /* NFS Montreal (Apr 9 2009 proto). bne +0x10 at 0x805246D8 ->
         * b +0x50 lands at success continuation `bl ...`. */
        *(volatile u32 *)0x805246D8 = 0x48000050;
        DCFlushRange((void *)0x805246D8, 4);
        ICInvalidateRange((void *)0x805246D8, 4);
        gprintf("RNFA09: firmware-check bne->b +0x50 @0x805246D8 "
                "(readback=%08X)\n",
                *(volatile u32 *)0x805246D8);
        free_wip();
        ClearDOLList();
        return;
    }
    if (memcmp((const void *)0x80000000, "RNFF11", 6) == 0)
    {
        /* NFS Focus Test (Mar 11 2009 proto). Same displacement as RNFA09. */
        *(volatile u32 *)0x804E6C1C = 0x48000050;
        DCFlushRange((void *)0x804E6C1C, 4);
        ICInvalidateRange((void *)0x804E6C1C, 4);
        gprintf("RNFF11: firmware-check bne->b +0x50 @0x804E6C1C "
                "(readback=%08X)\n",
                *(volatile u32 *)0x804E6C1C);
        free_wip();
        ClearDOLList();
        return;
    }
    if (memcmp((const void *)0x80000000, "ROSCM4", 6) == 0)
    {
        /* HVS Oscar MS4 v2 (build 2752) — four panic gates inside the
         * OSInit-equivalent sub_800C57B0.  All confirmed via IDA Pro
         * decompile of the extracted main.dol. */

        /* (1) "Refuse to run on production mode" check, gate A:
         *       lbz r0, 0x315C(r3)   ; byte at 0x8000315C
         *       cmpwi r0, 0x81
         *       beq panic_line_1160  ; <-- 0x800C5AE0 patch site
         *     If LowMem 0x8000315C == 0x81 (= "production class"), the SDK
         *     prints "production mode" and __OSPanics.  On retail Wii the
         *     byte IS 0x81, so this is the first silent kill.
         *     Fix: NOP the beq.  The following `bge`/`b` already cover the
         *     not-equal cases, so the panic is unreachable after NOP. */
        *(volatile u32 *)0x800C5AE0 = 0x60000000;
        DCFlushRange((void *)0x800C5AE0, 4);
        ICInvalidateRange((void *)0x800C5AE0, 4);

        /* (2) Same check, gate B at 0x8000315D == 0x81 (panic line 1178). */
        *(volatile u32 *)0x800C5B18 = 0x60000000;
        DCFlushRange((void *)0x800C5B18, 4);
        ICInvalidateRange((void *)0x800C5B18, 4);

        /* (3) Firmware-version high/low check at 0x800C5BC8.
         *     bne +0x10 -> panic; jump +0x48 to success continuation
         *     (lands on sub_800E48F0 bl). */
        *(volatile u32 *)0x800C5BC8 = 0x48000048;
        DCFlushRange((void *)0x800C5BC8, 4);
        ICInvalidateRange((void *)0x800C5BC8, 4);

        /* (4) Device-class gate at 0x800C5C84.
         *     bne +0x20 -> skip dev-validate.  Force unconditional `b`
         *     so dev-validate (which contains panics) is always skipped. */
        *(volatile u32 *)0x800C5C84 = 0x48000020;
        DCFlushRange((void *)0x800C5C84, 4);
        ICInvalidateRange((void *)0x800C5C84, 4);

        /* (5) HEAP-SIZE FIX (SWBF3-proven) — the real cause of the
         *     "loadOptimizedTextureGC::loadOptimized - Out of memory"
         *     (wiirend.cpp) crash on retail.
         *
         *     USB Loader GX's apploader clamps the lowmem MEM2-size words
         *     0x80000028 / 0x800000F0 to 0x01800000 (24MB), treating them
         *     as MEM1 size. The HVS engine's heap manager reads these to
         *     size its MEM2 arena, so a 24MB clamp starves the texture
         *     heap -> the on-demand texture load fails its allocation.
         *
         *     This game is NOT genuinely a 128MB title: NDEV just reports
         *     more. Setting these to retail's TRUE 64MB MEM2 size gives the
         *     heap a full 64MB arena (same fix that makes SWBF3 r2.91120a
         *     run on retail). Do NOT fake 96/128MB here — that makes the
         *     game allocate past physical RAM (BAT-alias) and corrupt.
         *
         *     Also write the 0x80003118 MEM2-size mirror (some SDK paths
         *     read it) and the MEM2 arena hi/lo to the standard retail
         *     layout, matching the WSD/Mighty3 recipe. */
        *(volatile u32 *)0x80000028 = 0x04000000; /* MEM2 size = 64MB (retail truth) */
        *(volatile u32 *)0x800000F0 = 0x04000000; /* MEM2 size alt (simulated) = 64MB */
        DCFlushRange((void *)0x80000020, 0x100);
        *(volatile u32 *)0x80003118 = 0x04000000; /* MEM2 physical size = 64MB */
        *(volatile u32 *)0x80003120 = 0x93600000; /* MEM2 arena hi (retail) */
        *(volatile u32 *)0x80003124 = 0x90000800; /* MEM2 arena lo */
        *(volatile u32 *)0x80003128 = 0x935E0000; /* MEM2 arena hi (alt) */
        DCFlushRange((void *)0x80003100, 0x40);

        /* (6) TEXTURE MIP-SKIP — fit the ~128MB gameplay texture working set
         *     into retail's 64MB MEM2.  The loader sub_806049F0
         *     (loadOptimizedTextureGC, wiirend.cpp:8479 etc.) allocates the
         *     full mip pyramid (from level 0) for every texture.  For LARGE
         *     textures (width AND height > 128) we halve the stored
         *     width/height and drop one mip -> ~1/4 the bytes -> fits 64MB.
         *
         *     Trampoline lives in a STABLE DOL-text cave at 0x800043B0
         *     (verified zero-padding in sec0; loaded, never cleared -- the
         *     first attempt at lowmem 0x80003200 was zeroed by the game's
         *     init and crashed).  Texture header (loader arg r4): +4 width,
         *     +8 height, +0x10 mip count.  Clobbers r0/r12 (both scratch at
         *     entry; mflr at loader+4 reloads r0).
         *
         *     This iteration reads the reduced byte-count from the start of
         *     the source (large textures may render rough); the goal is to
         *     clear the OOM and reach gameplay.  If the reader proves to be
         *     a shared/sequential stream we'll switch to consume-level-0. */
        {
            static const u32 mipskip_tramp[17] = {
                0x9421FFC0, /* stwu  r1, -0x40(r1)   ; relocated original instr */
                0x80040004, /* lwz   r0, 4(r4)       ; width                    */
                0x2C000080, /* cmpwi r0, 0x80        ; > 128 ?                  */
                0x40810034, /* ble   skip            ; small -> leave           */
                0x81840008, /* lwz   r12, 8(r4)      ; height                   */
                0x2C0C0080, /* cmpwi r12, 0x80                                   */
                0x40810028, /* ble   skip                                        */
                0x7C000E70, /* srawi r0, r0, 1       ; width  >>= 1             */
                0x90040004, /* stw   r0, 4(r4)                                   */
                0x7D8C0E70, /* srawi r12, r12, 1     ; height >>= 1             */
                0x918C0008, /* stw   r12, 8(r4)                                  */
                0x80040010, /* lwz   r0, 0x10(r4)    ; mip count                */
                0x2C000001, /* cmpwi r0, 1                                       */
                0x4081000C, /* ble   skip            ; keep >=1 level           */
                0x3800FFFF, /* addi  r0, r0, -1      ; one fewer mip            */
                0x90040010, /* stw   r0, 0x10(r4)                                */
                0x48600604, /* skip: b 0x806049F4    ; back to loader+4         */
            };
            u32 i;
            for (i = 0; i < 17; i++)
                *(volatile u32 *)(0x800043B0 + i*4) = mipskip_tramp[i];
            DCFlushRange((void *)0x800043B0, 17 * 4);
            ICInvalidateRange((void *)0x800043B0, 17 * 4);
            /* Hook loader entry 0x806049F0 -> b 0x800043B0 (disp -0x600640). */
            *(volatile u32 *)0x806049F0 = 0x4B9FF9C0;
            DCFlushRange((void *)0x806049F0, 4);
            ICInvalidateRange((void *)0x806049F0, 4);
            gprintf("ROSCM4: texture mip-skip hook @0x806049F0=%08X cave@0x800043B0=%08X\n",
                    *(volatile u32 *)0x806049F0, *(volatile u32 *)0x800043B0);
        }

        gprintf("ROSCM4: gates -> 5AE0=%08X 5B18=%08X 5BC8=%08X 5C84=%08X | "
                "mem2-size 28=%08X F0=%08X 3118=%08X\n",
                *(volatile u32 *)0x800C5AE0,
                *(volatile u32 *)0x800C5B18,
                *(volatile u32 *)0x800C5BC8,
                *(volatile u32 *)0x800C5C84,
                *(volatile u32 *)0x80000028,
                *(volatile u32 *)0x800000F0,
                *(volatile u32 *)0x80003118);
        free_wip();
        ClearDOLList();
        return;
    }
    if (memcmp((const void *)0x80000000, "RDUNPR", 6) == 0)
    {
        gprintf("RDUNPR: BASELINE -- silent black screen after apploader, "
                "no patch attempted yet.\n");
        free_wip();
        ClearDOLList();
        return;
    }

    patch_nsmb((u8 *)0x80000000);
    patch_pop((u8 *)0x80000000);
    patch_kirby((u8 *)0x80000000);
    patch_re4((u8 *)0x80000000);

    for (i = 0; i < dolCount; ++i)
    {
        u8 *dst = dolList[i].dst;
        int len = dolList[i].len;

        /* Dev-disc support: force in-DOL rmode patching ON for SWBF3 r2
         * (RSBE3*) so patch_videomode() always runs for this game,
         * regardless of the user's "Video Patch DOL" setting. Required
         * because the swbf3-specific dimensional preservation in
         * patch_videomode (the PAL-crash workaround at the nav-heap
         * MEM2 envelope) is gated on patch_videomode actually being
         * invoked. With the user setting at OFF (the default), the
         * SWBF3 PAL fix would never apply and PAL forcing would crash
         * the game at boot. */
        u8 effectiveVideoPatchDol = videoPatchDol;
        if (memcmp((const void *)0x80000000, "RSBE3", 5) == 0 &&
            effectiveVideoPatchDol == VIDEO_PATCH_DOL_OFF)
        {
            effectiveVideoPatchDol = VIDEO_PATCH_DOL_REGION;
            static int _vp_logged = 0;
            if (!_vp_logged) {
                gprintf("SWBF3: forcing videoPatchDol = REGION so patch_videomode runs (was OFF).\n");
                _vp_logged = 1;
            }
        }
        VideoModePatcher(dst, len, videoSelected, effectiveVideoPatchDol);

        if (hooktype)
            dogamehooks(hooktype, dst, len);

        if (vipatch)
            vidolpatcher(dst, len);

        if (sneekVideoPatch)
            sneek_video_patch(dst, len);

        // LANGUAGE PATCH - FISHEARS
        langpatcher(dst, len, languageChoice);

        // Thanks to WiiPower
        if (patchcountrystring == 1)
            PatchCountryStrings(dst, len);

        do_wip_code(dst, len);

        /* Riivolution memory-patch bridge omitted in this tree (feature not
         * present). Original call: Riivolution_ApplyOnDOL_C(dst, len); */

        anti_002_fix(dst, len);
        /* Gate SWBF3 patches to discs whose ID starts with "RSBE3"
         * (RSBE3A=r1.90431a, RSBE3B=r90776a, RSBE3C=r2.91120a).  The
         * short 16-byte sigs can false-match unrelated `addis;cmplwi;bne;li`
         * sequences in other games' DOLs -- previously broke Lego Racers
         * (RLGED1) by NOPing a real bne at 0x801DAAD8. */
        if (memcmp((const void *)0x80000000, "RSBE3", 5) == 0)
        {
            /* v6Y-rev2: USBLoaderGX's apploader.c clamps 0x80000028 / 0x800000F0
             * to 0x01800000 (24MB) treating them as MEM1 size. But Dolphin shows
             * these as 0x06000000 (96MB) on NDEV — i.e. they're MEM2 size, not
             * MEM1 size. Game's heap manager reads these to size its MEM2 arena.
             * With them clamped to 24MB, heap is configured TINY → constructors
             * can't allocate sub-objects → NULL vtables.
             *
             * Set to 0x04000000 (retail's TRUE 64MB MEM2 size — not faked 96MB,
             * so no BAT-alias data corruption). This is the actual NDEV memory
             * mapping working on retail: tell game the correct retail MEM2 size
             * rather than the bogus 24MB-clamp.
             *
             * v6Y first attempt was 96MB which caused title-screen corruption
             * because game allocated beyond physical memory via BAT alias. */
            {
                static int _v6y_logged = 0;
                *(volatile u32 *)0x80000028 = 0x04000000;  /* MEM2 size = 64MB (retail) */
                *(volatile u32 *)0x800000F0 = 0x04000000;  /* MEM2 size alt = 64MB */
                DCFlushRange((void *)0x80000020, 0x100);
                if (!_v6y_logged) {
                    gprintf("SWBF3 v6Y-rev2: lowmem MEM2-size set to retail 64MB (0x80000028/F0); 0x80003118 left as-is\n");
                    _v6y_logged = 1;
                }
            }

            swbf3_mem2_check_fix(dst, len);
            swbf3_instant_action_null_fix(dst, len);

            /* "Unlock Everything" debug-menu cheat fix. Always on for
             * RSBE3* -- not gated by SWBF3_INSTANT_ACTION_ONLY because
             * it's a menu/UI fix, not a crash fix. */
            swbf3_unlock_cheat_fix(dst, len);

            /* Havok physics OOM crash chain (Dantooine load).  Three
             * cooperating patches; always on for RSBE3* since they're
             * pure crash fixes, not campaign-mode behavior changes.
             * Order matters: alloc-fail fixes the FE1 trap, loop-skip
             * fixes the downstream slab-list-prepend crash, caller-
             * ptr-guard fixes the still-further-downstream "fn returned
             * garbage non-zero pointer" crash. */
            swbf3_havok_alloc_fail_fix(dst, len);
            swbf3_havok_slab_loop_skip_fix(dst, len);
            swbf3_havok_caller_ptr_guard_fix(dst, len);
            swbf3_havok_cutscene_null_guard(dst, len);
            swbf3_havok_psq_stx_null_guard(dst, len);
            swbf3_dantooine_string_scan_guard(dst, len);
            swbf3_dantooine_vfn_r3_guard(dst, len);
            swbf3_dantooine_array_data_guard(dst, len);
            /* v6A/v6B DISABLED (v7B): grow-slab-pool 8KB→16KB was a workaround
             * for the bad-memory-config era when MEM2 was clamped to 24MB. With
             * v6Y-rev2 giving the heap a real 64MB MEM2, the original 8KB slabs
             * fit fine. Worse, v6A/v6B's growth seems to be CLOBBERING r23
             * during sub_80534684 execution (callee-saved violation), causing
             * the r23=4 crash at 0x805D96D0 after bcctrl returns. Revert.
             *   swbf3_havok_grow_slab_pool(dst, len);
             */
            /* v6C: validate freelist head in sub_8053B97C — clears
             * corrupted head pointers and forces fresh slab refill. */
            swbf3_havok_freelist_guard(dst, len);
            /* v6D: validate vfn[5] result for LARGE allocs (> 0x2000)
             * which take the bctr tail-call path bypassing v6C. */
            swbf3_havok_large_alloc_guard(dst, len);
            /* v6E: validate sub_8053B3D8 result for SMALL-fallback
             * (when freelist exhausted/zeroed by v6C). */
            swbf3_havok_small_fallback_guard(dst, len);
            /* v6F: guard lbz at 0x805D81F4 — r3 cascades into NDEV-only
             * 0xA0000000+ region; substitute r0=0 placeholder. */
            swbf3_dantooine_lbz_guard(dst, len);
            /* v6G: ROOT-CAUSE — alias NDEV mirror 0xA0000000-0xA3FFFFFF
             * to physical MEM2 via DBAT5. Makes NDEV pointer math work
             * on retail Wii without per-site guards. */
            swbf3_ndev_bat_setup_fix(dst, len);
            /* v6H: guard lwz r6, 4(r3) at 0x805D8160 — r3 cascades into
             * low-MEM1 range (0x00xxxxxx) where no BAT applies. */
            swbf3_dantooine_lwz_r6_guard(dst, len);
            /* v6I: NULL guard at 0x805A8EA8 lwz r0,0x28(r8). r8 loaded
             * from r25+0x24 (NULL). r0=0 substitution triggers the
             * intended NULL-path branch in the next cmpwi/beq. */
            swbf3_dantooine_lwz_r0_null_guard(dst, len);
            /* v6J: NULL-r3 entry guard at sub_805BF554 (leaf setter
             * called from 0x805A8F88). Early-return if r3 NULL. */
            swbf3_dantooine_setter_null_guard(dst, len);
            /* v6K: NULL guard at 0x80495334 lhz r0,0x18(r3). r3 from
             * SDA-rel global at r13-0x72AC (uninit). r0=0 placeholder. */
            swbf3_dantooine_lhz_sda_null_guard(dst, len);
            /* v6L: guard lwz r5,0(r5) at 0x805D8170. r5 from sp+0x10
             * (= 0x99 junk). Substitute r5=0 → passes NULL to vfn[2]. */
            swbf3_dantooine_lwz_r5_guard(dst, len);
            /* v6M: double-NULL guard at 0x805A7F5C — validates r27 and
             * r28, substitutes zero-buffer 0x80AAEEC0 if NULL. */
            swbf3_dantooine_r27_r28_guard(dst, len);
            /* v6N: NULL guard at 0x805BCDEC lwz r5,0x178(r3). r3=NULL. */
            swbf3_dantooine_lwz_r3_178_guard(dst, len);
            /* v6O: 4 sites of lwz r3,0x178(r26) with r26=NULL — same
             * struct accessed in 4 copy-pasted code blocks (cutscene
             * fade-in code, post-loop accesses). */
            swbf3_dantooine_lwz_r26_178_guard(dst, len);
            /* v6P/v6Q RE-ENABLED (under v6Y-rev2 + v6Z baseline): the v6V
             * blr-bail still leaves caller with corrupted r14-r31 (causing
             * r23=4 crash at 0x805D96D0 after v6V fires). v6P/v6Q intercept
             * the NULL-vtable bctrl calls UPSTREAM, preventing the bad call
             * path that leads to the v6V bail in the first place. Combined
             * with v6Y-rev2's heap-config fix, the residual NULL vtables
             * should be rare; v6P substitutes a stub-blr so bctrl is no-op
             * and v6Q skips the entire vfn block. */
            swbf3_dantooine_vtable_null_guard(dst, len);
            swbf3_dantooine_skip_vfn_block_fix(dst, len);
            /* v6R/v6U: substitute allocator obj address (0x8075EBA0) at
             * sub_804219CC entry when r3 is positive garbage. PROVEN via
             * Dolphin trace at 0x804219CC: r3=0x8075EBA0 valid, r4=0x38
             * size, LR=0x803687A0 (sub_8036877C reading *(0x807526D0)). */
            swbf3_dantooine_lwz_r3_3C_guard(dst, len);
            /* v7D: hook sub_805D7D50 entry to save r14-r31 to scratch
             * BEFORE prologue runs. (v7D may not help if the bcctrl at
             * 0x805D96C8 actually calls a different function — vfn from
             * table; keep as defense in depth.) */
            swbf3_dantooine_entry_save_callee_regs(dst, len);
            /* v6V (v7D-aware): bail with r14-r31 restore. */
            swbf3_dantooine_epilogue_r10_guard(dst, len);
            /* v7E: wrap the bcctrl at 0x805D96C8 to save r23 BEFORE the call
             * and restore it AFTER. This works regardless of what vfn the
             * bcctrl invokes — caller's r23 is always preserved across the
             * specific bcctrl that causes the r23=4 crash at 0x805D96D0. */
            swbf3_dantooine_callsite_r23_preserve(dst, len);
            /* v4-v7 DISABLED: these patches were unconditional (changed
             * behavior for every invocation, not just OOM/NULL cases),
             * which broke loading on ALL maps -- not just Dantooine.
             * The previous-iteration patches break normal-map flows.
             * Restored to v3 baseline; Dantooine will crash at the v3
             * site (0x805d8f3c stw to garbage r18) but other stages work.
             *   swbf3_havok_node_init_null_skip(dst, len);
             *   swbf3_havok_r31_block_bypass_fix(dst, len);
             *   swbf3_havok_field4_force_alt_fix(dst, len);
             *   swbf3_havok_field4_vfn_skip_fix(dst, len);
             */

            /* Campaign-mode patches.  Opt out by creating empty file
             *   sd:/SWBF3_INSTANT_ACTION_ONLY
             * to isolate save/profile-side behavior from campaign rewrites. */
            {
                FILE *toggle = fopen("sd:/SWBF3_INSTANT_ACTION_ONLY", "rb");
                if (toggle)
                {
                    fclose(toggle);
                    static int _swbf3_ia_logged = 0;
                    if (!_swbf3_ia_logged) {
                        gprintf("SWBF3 campaign-mode patches DISABLED via sd:/SWBF3_INSTANT_ACTION_ONLY\n");
                        _swbf3_ia_logged = 1;
                    }
                }
                else
                {
                    static int _swbf3_cm_logged = 0;
                    if (!_swbf3_cm_logged) {
                        gprintf("SWBF3 campaign-mode patches ENABLED\n");
                        _swbf3_cm_logged = 1;
                    }
                    swbf3_campaign_lookup_fix(dst, len);
                    swbf3_campaign_loop_fix(dst, len);
                    swbf3_campaign_vec3_guard_fix(dst, len);
                    swbf3_campaign_transform_null_fix(dst, len);
                }
            }
        }

        if (!exclude_game((u8 *)0x80000000, false))
        {
            if (disableMotor)
                motor_patch(dst, len);
            
            if (disableSpeaker)
                speaker_patch(dst, len);

            if (videoWidth == WIDTH_FRAMEBUFFER)
                patch_width(dst, len);

            if (deflicker == DEFLICKER_ON_LOW)
            {
                patch_vfilters(dst, len, vfilter_low);
                patch_vfilters_rogue(dst, len, vfilter_low);
            }
            else if (deflicker == DEFLICKER_ON_MEDIUM)
            {
                patch_vfilters(dst, len, vfilter_medium);
                patch_vfilters_rogue(dst, len, vfilter_medium);
            }
            else if (deflicker == DEFLICKER_ON_HIGH)
            {
                patch_vfilters(dst, len, vfilter_high);
                patch_vfilters_rogue(dst, len, vfilter_high);
            }
            else if (deflicker != DEFLICKER_AUTO)
            {
                patch_vfilters(dst, len, vfilter_off);
                patch_vfilters_rogue(dst, len, vfilter_off);
                // This might break fade and brightness effects
                if (deflicker == DEFLICKER_OFF_EXTENDED)
                    deflicker_patch(dst, len);
            }
        }

        if (returnTo)
            PatchReturnTo(dst, len, (u32)returnTo);

        if (aspectForce < 2)
            PatchAspectRatio(dst, len, aspectForce);

        if (privateServer)
            PrivateServerPatcher(dst, len, privateServer, serverAddr);

        if (privateServer == PRIVSERV_WIIMMFI)
        {
            // If we end up here, that means it's a NON-MKWii Wiimmfi patch
            // add the new patches.
            do_new_wiimmfi_nonMKWii(dst, len);
        }

        DCFlushRange(dst, len);
        ICInvalidateRange(dst, len);
    }

    // Set by the system menu on Japanese consoles
    if (((char *)0x80000003)[0] == 'J')
    {
        u32 region = *HW_VI1CFG;
        *HW_VI1CFG = region | (1 << 17);
        DCFlushRange((void *)HW_VI1CFG, 4);
    }

    // ERROR 002 fix (thanks to WiiPower for sharing this)
    *(u32 *)0x80003140 = *(u32 *)0x80003188;

    DCFlushRange((void *)0x80000000, 0x3f00);

    /* SWBF3 PAL-crash workaround (stage 2 -- the actual fix).
     *
     * Stage 1 (patch_videomode RSBE3* early-return) preserves the DOL's
     * NTSC rmode dimensions while still swapping in PAL viTVMode, so VI
     * scans out at PAL timing.  That alone is NOT enough: the game also
     * reads lowmem 0x800000CC (Video_Mode) directly in ~40 sites and
     * sizes its own framebuffer/Z-buffer allocations from it.  With
     * Video_Mode == VI_PAL/VI_EURGB60 the game tries to allocate
     * 528-line buffers, which overflows the 24-byte gap between the
     * all_root heap top (0x97e2ffe8) and the nav heap base (0x97e30000),
     * clobbering the nav-heap descriptor.  The nav allocator then dies
     * at SRR0=0x803a3800 (lbzx r0, r5, r0 with EA=0xfffffff8).
     *
     * Force Video_Mode back to VI_NTSC (0) for RSBE3* AFTER the standard
     * Disc_SetVMode write.  VI scan-out is unaffected -- VIDEO_Configure
     * was already called with the user's PAL rmode, and the patched
     * Franken-rmode (PAL viTVMode + NTSC dims) reaches any subsequent
     * VIDEO_Configure call.  Net result: PAL Wii hardware still drives
     * a PAL signal; game allocates NTSC-sized buffers that fit. */
    if (memcmp((const void *)0x80000000, "RSBE3", 5) == 0)
    {
        u32 prev_cc = *((vu32 *)0x800000CC);
        if (prev_cc != 0)
        {
            *((vu32 *)0x800000CC) = 0; /* VI_NTSC */
            DCFlushRange((void *)0x800000CC, 4);
            gprintf("SWBF3: forced lowmem 0x800000CC = 0 (NTSC) to keep framebuffer at 480 lines (was 0x%x).\n", prev_cc);
        }
        else
        {
            gprintf("SWBF3: lowmem 0x800000CC already 0 (NTSC) -- no PAL override needed.\n");
        }
    }

    /* Redirect SDK OSReport / printf (EXI2 dev-serial — dead on retail) to
     * the USB Gecko (EXI1). Runs after all DOL sections are loaded/patched,
     * before the game launches. Gated per-game by GameCFG.USBGeckoTTY (see
     * GameBooter -> GeckoTTYEnabled). Signature-based; works on any RVL-SDK
     * / MSL game and survives game rebuilds. */
    gecko_tty_universal_patch();

    free_wip();
    ClearDOLList();
}

/** Anti 002 fix for IOS 249 rev > 12 thanks to WiiPower **/
void anti_002_fix(u8 *addr, u32 len)
{
    u32 SearchPattern[3] = {0x2C000000, 0x48000214, 0x3C608000};
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SearchPattern);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            *((u32 *)addr_start + 1) = 0x40820214;
            return;
        }
        addr_start += 4;
    }
}

/* SWBF3 (RABAZZ prototype) MEM2-size AP-check bypass.
 *
 * The dev-build Star Wars Battlefront 3 r2.91120a calls a tiny SDK
 * getter that returns Mem2_Size (`*0x80003118`), then checks
 *
 *     if (mem2 != 0x08000000)  // game expects 128 MB MEM2 (NDEV)
 *         go_fail();          // "Error #001 unauthorized device"
 *
 * The check sequence in main.dol at mem 0x804A9920 (file 0x4A5960) is
 * a unique 16-byte signature:
 *
 *     3C 03 F8 00   addis  r0, r3, -2048   ; r0 = mem2 - 0x08000000
 *     28 00 00 00   cmplwi r0, 0
 *     40 82 00 0C   bne+   +0xC            ; if not 128 MB -> fail path
 *     38 60 00 01   li     r3, 1           ; success
 *
 * Patching the bne to nop makes the success path always taken so the
 * game keeps booting on retail Wii (24+64 MB).  Allocations later in
 * the game may exceed retail caps and fail -- those become the next
 * patch targets but at least we get past the initial gate.
 *
 * Pattern is exact 16 bytes; false-positive risk is essentially zero
 * (the addis-immediate -2048 followed by cmplwi r0,0 + this exact bne
 * + li r3,1 is highly distinctive).  Run unconditionally on every
 * loaded section -- harmless for any other game.
 */
void swbf3_mem2_check_fix(u8 *addr, u32 len)
{
    static const u32 SearchPattern[4] = {
        0x3C03F800,   /* addis r0, r3, -2048 */
        0x28000000,   /* cmplwi r0, 0        */
        0x4082000C,   /* bne+ +0xC           */
        0x38600001,   /* li r3, 1            */
    };
    if (len < sizeof(SearchPattern)) return;
    u8 *p = addr;
    u8 *end = addr + len - sizeof(SearchPattern);
    while (p <= end)
    {
        if (memcmp(p, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            /* Replace the bne (offset +8 within the pattern) with nop. */
            *((u32 *)(p + 8)) = 0x60000000;
            gprintf("SWBF3 MEM2 AP-check NOPed at %p (next instr was bne+12 -> nop)\n",
                    (void *)p);
            return;
        }
        p += 4;
    }
}

/* SWBF3 Instant-Action null-deref crash bypass.
 *
 * After the MEM2 AP-check is bypassed, picking Instant Action triggers
 * a chain of lookups that fail under retail's smaller MEM2 budget.  The
 * specific crash at PC=0x80254610 is:
 *
 *     3B 80 FF FF   li     r28, -1                ; sentinel: "no ID"
 *     7F 83 E3 78   mr     r3, r28
 *     4B DD 6E F9   bl     0x8002B504             ; lookup_by_id(-1) -> NULL
 *     80 63 00 00   lwz    r3, 0(r3)              ; CRASH: deref NULL
 *
 * The lookup function at 0x8002B504 explicitly returns NULL for id=-1
 * and the calling code never null-checks before dereferencing.  NOPing
 * the lwz lets r3 stay whatever it was (NULL); the next call may handle
 * NULL, may crash again -- if so, that's the next patch target.
 *
 * 16-byte signature is unique to SWBF3 (the specific bl displacement
 * 0x4BDD6EF9 only matches when the calling code is at exactly the right
 * VA, which is true only for SWBF3 r2.91120a's main.dol).
 */
void swbf3_instant_action_null_fix(u8 *addr, u32 len)
{
    static const u32 SearchPattern[4] = {
        0x3B80FFFF,   /* li r28, -1                 */
        0x7F83E378,   /* mr r3, r28                 */
        0x4BDD6EF9,   /* bl 0x8002B504 (relative)   */
        0x80630000,   /* lwz r3, 0(r3)              */
    };
    if (len < sizeof(SearchPattern)) return;
    u8 *p = addr;
    u8 *end = addr + len - sizeof(SearchPattern);
    while (p <= end)
    {
        if (memcmp(p, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            /* Replace the lwz (offset +12) with nop. */
            *((u32 *)(p + 12)) = 0x60000000;
            gprintf("SWBF3 instant-action null-deref NOPed at %p\n", (void *)(p + 12));
            return;
        }
        p += 4;
    }
}

/* Dev-disc support: SWBF3 r2.91120a Campaign-mode lookup-table crash.
 * Replaces the addis/cmplwi/beq sentinel-only check at the prologue of
 * two sibling leaf getters (0x8039A04C / 0x8039A078) with a magnitude
 * check that also rejects garbage idx > 0x40000. */
void swbf3_campaign_lookup_fix(u8 *addr, u32 len)
{
    static const u32 SearchPattern[6] = {
        0x3C030001, 0x2800FFFF, 0x4182001C,
        0x1C030078, 0x3C608082, 0x3863B2B8,
    };
    static const u32 Replacement[3] = {
        0x3C000004, 0x7C001840, 0x4180001C,
    };
    if (len < sizeof(SearchPattern)) return;
    u8 *p = addr;
    u8 *end = addr + len - sizeof(SearchPattern);
    while (p <= end)
    {
        if (memcmp(p, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            memcpy(p, Replacement, sizeof(Replacement));
            gprintf("SWBF3 campaign lookup hardened at %p\n", (void *)p);
            p += sizeof(SearchPattern);
            continue;
        }
        p += 4;
    }
}

/* Dev-disc support: SWBF3 r2.91120a Campaign-mode loop-prologue crash.
 * Magnitude-check transform applied to the loop function prologue at
 * 0x80399A80 (where garbage entity-ids land in r3 and produce NULL+12
 * destinations downstream). */
void swbf3_campaign_loop_fix(u8 *addr, u32 len)
{
    static const u32 SearchPattern[7] = {
        0x480D4CF5, 0x3C030001, 0x7C9D2378, 0x2800FFFF,
        0x7CBE2B78, 0x7CDF3378, 0x418200C0,
    };
    if (len < sizeof(SearchPattern)) return;
    u8 *p = addr;
    u8 *end = addr + len - sizeof(SearchPattern);
    while (p <= end)
    {
        if (memcmp(p, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            *((u32 *)(p + 4))  = 0x3C000004;
            *((u32 *)(p + 12)) = 0x7C001840;
            *((u32 *)(p + 24)) = 0x418000C0;
            gprintf("SWBF3 campaign loop hardened at %p\n", (void *)p);
            return;
        }
        p += 4;
    }
}

/* Dev-disc support: SWBF3 r2.91120a Campaign per-frame transform null deref.
 * Hijacks `lis r4,0x8062` + `lfs f3,-23136(r4)` at 0x80254454/0x8025445C
 * into a `cmpwi r5,0; beq +0x120` so a null transform pointer branches
 * to the function's existing early-exit instead of crashing on lfs f2,12(r5). */
void swbf3_campaign_transform_null_fix(u8 *addr, u32 len)
{
    static const u32 SearchPattern[4] = {
        0x80BF0094, 0x3C808062, 0x7C7D1B78, 0xC064A5A0,
    };
    if (len < sizeof(SearchPattern)) return;
    u8 *p = addr;
    u8 *end = addr + len - sizeof(SearchPattern);
    while (p <= end)
    {
        if (memcmp(p, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            *((u32 *)(p +  4)) = 0x2C050000;
            *((u32 *)(p + 12)) = 0x41820120;
            gprintf("SWBF3 transform-null guard added at %p\n", (void *)p);
            return;
        }
        p += 4;
    }
}

/* Dev-disc support: SWBF3 r2.91120a "Unlock Everything" debug-menu cheat fix.
 *
 * SWBF3's debug menu (hold "1" at title) exposes an "Unlock Everything"
 * toggle, registered with the cheat string "unlockall" at 0x8069BE90.
 * The cheat backing byte lives at 0x80AAEEBE (r13/SDA offset -0x77C2):
 * 1 = on, 0 = off.
 *
 * Under Dolphin the toggle works -- flipping it on unlocks every campaign
 * chapter. Under USBLGX on retail Wii the same toggle does nothing:
 * chapters stay greyed and entering them prints "The selected chapter is
 * currently locked." The break is in the propagation between the cheat
 * byte (0x80AAEEBE) and the campaign-manager's "lock-check-enabled" byte
 * at +0x10C8 -- which the gating code reads. We short-circuit it.
 *
 * Four sites read `lbz r0, 0x10C8(rA)` (rA = r22 at the writer-loop site,
 * r25 at the three gate-check sites). Rewrite each lbz to read the cheat
 * byte directly via SDA, and flip the polarity of the branch that follows
 * the cmpwi -- the cheat byte and the manager byte have inverse meaning:
 *
 *     manager.byte_at_0x10C8 == 0  <=>  cheat ON  ("skip lock checks")
 *     cheat_flag             == 1  <=>  cheat ON  ("flag is set")
 *
 *   lbz r0, 0x10C8(rA)   ->  lbz r0, -0x77C2(r13)   (0x880D883E)
 *   beq/bne following     ->  branch polarity flipped (top byte XOR 1)
 *
 * Net effect: the in-game "Unlock Everything" toggle drives the gating
 * decision directly -- toggling it ON unlocks all campaign missions, OFF
 * restores normal per-profile locking. Same UX as Dolphin; we just
 * bypass the broken propagation. Gated by RSBE3*. */
void swbf3_unlock_cheat_fix(u8 *addr, u32 len)
{
    struct site {
        const u32 *sig;
        u32        sig_words;
        u32        lbz_off;
        u32        branch_off;
        const char *name;
    };
    static const u32 S1[] = { 0x881610C8, 0x2C000000, 0x4182001C };                         /* writer @ 0x800BD020 */
    static const u32 S2[] = { 0x881910C8, 0x807910B0, 0x2C000000, 0x9079104C, 0x41820038 }; /* gate #1 @ 0x800BED0C */
    static const u32 S3[] = { 0x881910C8, 0x807910B0, 0x2C000000, 0x9079104C, 0x41820040 }; /* gate #2 @ 0x800BF044 */
    static const u32 S4[] = { 0x881910C8, 0x2C000000, 0x4082005C };                         /* gate #3 @ 0x800BF35C */
    static const struct site SITES[] = {
        { S1, sizeof(S1)/4, 0,  8, "writer-loop @ +0x10C8(r22)"  },
        { S2, sizeof(S2)/4, 0, 16, "gate-check #1 (beq +56)"     },
        { S3, sizeof(S3)/4, 0, 16, "gate-check #2 (beq +64)"     },
        { S4, sizeof(S4)/4, 0,  8, "gate-check #3 (bne +92)"     },
    };
    static const u32 LBZ_R0_CHEATFLAG = 0x880D883E; /* lbz r0, -0x77C2(r13) */

    int patched = 0;
    for (size_t s = 0; s < sizeof(SITES)/sizeof(SITES[0]); s++)
    {
        const struct site *S = &SITES[s];
        u32 siglen = S->sig_words * 4;
        if (len < siglen) continue;
        u8 *p = addr;
        u8 *end = addr + len - siglen;
        while (p <= end)
        {
            if (memcmp(p, S->sig, siglen) == 0)
            {
                *((u32 *)(p + S->lbz_off)) = LBZ_R0_CHEATFLAG;
                p[S->branch_off] ^= 0x01;
                gprintf("SWBF3 unlock-cheat-fix: %s patched at %p\n", S->name, (void *)p);
                patched++;
                p += siglen;
                break;
            }
            p += 4;
        }
    }
    if (patched > 0)
        gprintf("SWBF3 unlock-cheat-fix: %d/%d sites patched; debug-menu \"Unlock Everything\" toggle now drives gating.\n",
                patched, (int)(sizeof(SITES)/sizeof(SITES[0])));
}

/* Dev-disc support: SWBF3 r2.91120a Havok physics OOM crash chain.
 *
 * Observed on retail Wii loading Dantooine (in-DOL crash dumper):
 *   ERROR 10 : Context 0x809750a8
 *   LR  = 0x80534764       SRR0 = 0x805347f0       SRR1 = 0x0000b432
 *   DSISR = 0           DAR  = 0           (==> Program-FPU vector)
 *
 * CHavokPhysicsMgr's 8 KB slab refill (fn 0x80534688) requests an 8256-
 * byte, 64-aligned block via the named-heap wrapper (source-stamped
 * "CHavokPhysicsMgr.cpp" line 178).  Under tight memory the alloc returns
 * NULL.  fn loads mgr.failureHandler at +0xD3C; when also NULL it falls
 * into a pair of MSR.FE1-toggle "FP-exception serialize" mtmsr sequences:
 *
 *     mfmsr r0                ; save MSR
 *     ori   r3, r0, 0x0400    ; ori MSR.FE1
 *     mtmsr r3                ; arm: precise FP exception reporting on
 *     mtmsr r0       <-- SRR0 ; restore: pending FP exception detonates
 *
 * Patch (per-site, 3 words in place): zero mgr.curBlock + mgr.field_A20
 * and redirect the no-handler beq to the function epilogue, so the slab
 * refill returns gracefully when both the alloc and the recovery handler
 * are absent.  Validation: prev word = `bne +X`, mid = lwz r0,0xD3C(r25)
 * + cmpwi r0,0, beq target lands on `mfmsr r0` (FE1 sig).  Epilogue
 * located by 5-word signature, branch offset computed at patch time.
 *
 * The three-word in-place patch:
 *   lwz   r0, 0xD3C(r25)   ->  stw r24, 0xA18(r25)  (0x93190A18)
 *   cmpwi r0, 0             ->  stw r24, 0xA20(r25)  (0x93190A20)
 *   beq   +0x70 (-> FE1)    ->  b   +epilog          (per-site offset)
 *
 * r24 is already 0 here (the failed-alloc return value stored by the
 * upstream `or. r24, r3, r3`), so the stws zero the manager fields.
 *
 * Gated RSBE3*.  No-op on any disc whose dol doesn't contain the exact
 * pattern. */
void swbf3_havok_alloc_fail_fix(u8 *addr, u32 len)
{
    static const u32 EPILOG[5] = {
        0xBB010010,  /* lmw   r24, 16(r1)  */
        0x80010034,  /* lwz   r0,  52(r1)  */
        0x7C0803A6,  /* mtlr  r0           */
        0x38210030,  /* addi  r1, r1, 48   */
        0x4E800020,  /* blr                */
    };
    static const u32 SIG_MID[2] = {
        0x80190D3C,  /* lwz   r0, 0xD3C(r25)  */
        0x2C000000,  /* cmpwi r0, 0           */
    };
    static const u32 FE1_MFMSR = 0x7C0000A6;

    if (len < sizeof(EPILOG)) return;

    u8 *epilog = NULL;
    {
        u8 *end = addr + len - sizeof(EPILOG);
        for (u8 *p = addr; p <= end; p += 4)
        {
            if (memcmp(p, EPILOG, sizeof(EPILOG)) == 0)
            {
                epilog = p;
                break;
            }
        }
    }
    if (!epilog)
    {
        /* silent: most DOL sections don't contain this epilogue */
        return;
    }

    int patched = 0;
    if (len >= sizeof(SIG_MID) + 8)
    {
        u8 *end = addr + len - sizeof(SIG_MID) - 4;
        u8 *p = addr + 4;
        while (p <= end)
        {
            if (memcmp(p, SIG_MID, sizeof(SIG_MID)) == 0)
            {
                u32 prev = *((u32 *)(p - 4));
                u32 beq  = *((u32 *)(p + 8));
                bool prev_ok = (prev & 0xFFFF0000) == 0x40820000;
                bool beq_ok  = (beq  & 0xFFFF0000) == 0x41820000;
                bool fe1_ok = false;
                if (beq_ok)
                {
                    s32 disp = (s32)(beq & 0x0000FFFC);
                    if (disp & 0x8000) disp |= 0xFFFF0000;
                    u8 *beq_site = p + 8;
                    u8 *tgt = beq_site + disp;
                    if (tgt >= addr && (tgt + 4) <= (addr + len))
                        fe1_ok = (*((u32 *)tgt) == FE1_MFMSR);
                }
                if (prev_ok && beq_ok && fe1_ok)
                {
                    u8 *beq_site = p + 8;
                    s32 offset = (s32)((u32)epilog - (u32)beq_site);
                    if (offset >= -0x02000000 && offset < 0x02000000)
                    {
                        /* v3d: do NOT zero curBlock/A20.  Earlier zeroing
                         * broke the cutscene init at fn 0x806067b4 which
                         * reads those fields and expects stale-but-valid
                         * state.  Just branch to epilogue, leaving the
                         * fields with whatever the prior successful
                         * iteration set. */
                        u32 br = 0x48000000 | (((u32)offset) & 0x03FFFFFC);
                        *((u32 *)beq_site) = br;
                        gprintf("SWBF3 havok-alloc-fail-fix: site %p -> b +0x%08x (epilog %p)\n",
                                (void *)beq_site, offset, (void *)epilog);
                        patched++;
                        p += sizeof(SIG_MID) + 4;
                        continue;
                    }
                }
            }
            p += 4;
        }
    }

    if (patched == 0)
        gprintf("SWBF3 havok-alloc-fail-fix: no failure-fork sites matched (build differs from r2.91120a?)\n");
    else
        gprintf("SWBF3 havok-alloc-fail-fix: %d sites patched\n", patched);
}

/* Pairs with swbf3_havok_alloc_fail_fix.  Second crash after v1 of that
 * patch surfaced inside fn 0x8053b3D8 (a slab freelist refiller higher
 * up the Havok chain):
 *
 *   ERROR 2 (DSI) SRR0 = 0x8053b604  DAR = 0x4006d9d8  (garbage ptr)
 *
 *   0x8053b5ec  lwz   r5, 0(r6)         # load from stack-buf
 *   0x8053b604  stw   r0, 0(r5)         <-- DSI (r5 = uninit stack mem)
 *
 * That fn calls a vfn via bctr at 0x8053b45c, passing the count r31 and
 * a stack-local buffer at r1+8.  The vfn was supposed to fill all r31
 * slots, then this prepend loop iterates them.  When the vfn's internal
 * cascading slab refill (= our patched fn 0x80534688) exits gracefully,
 * only some slots get filled; the rest contain stale stack contents.
 *
 * Patch (1 word in place): existing `cmpwi cr1, r31, 1` at 0x8053b460
 * (right after the bctr return) feeds a `bc 4,5, +416` at 0x8053b470
 * that already skips both prepend loops + falls to the function epilogue
 * when count <= 1.  Repurpose the cmpwi to test bctr return r3 against
 * 0, so the existing skip branch fires when the vfn signals failure.
 * The next two instructions clobber r3 with `add r3, r29, r0` -- they'd
 * have stomped any meaningful return value anyway, so testing r3 first
 * is correct.
 *
 * Signature anchored on the unique 5-word post-bctr setup:
 *   cmpwi  cr1, r31, 1            0x2C9F0001    <-- patch target
 *   rlwinm r0, r30, 3, 0, 28      0x57C01838
 *   add    r3, r29, r0            0x7C7D0214
 *   li     r4, 1                  0x38800001
 *   bc     4, 5, +416             0x408501A0
 *
 * Patch: replace the cmpwi with `cmpwi cr1, r3, 0` (0x2C830000). */
void swbf3_havok_slab_loop_skip_fix(u8 *addr, u32 len)
{
    static const u32 SIG[5] = {
        0x2C9F0001,
        0x57C01838,
        0x7C7D0214,
        0x38800001,
        0x408501A0,
    };
    static const u32 PATCH = 0x2C830000;  /* cmpwi cr1, r3, 0 */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            *((u32 *)p) = PATCH;
            gprintf("SWBF3 havok-slab-loop-skip-fix: patched cmpwi at %p\n", (void *)p);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 havok-slab-loop-skip-fix: %d site(s) patched\n", hits);
}

/* Third crash in the Havok OOM chain.  After v2 of both patches above,
 * caller of fn 0x8053b3D8 crashed at:
 *
 *   ERROR 2 (DSI) SRR0 = 0x805d8f3c  DAR = 0x3cbeea8f  (garbage ptr)
 *   LR  = 0x805d8f2c       GPR03 = GPR18 = 0x3cbeea8f
 *
 *   0x805d8f28  bl    0x8053b3D8       # call slab refill
 *   0x805d8f2c  or    r18, r3, r3       # save return
 *   0x805d8f30  cmpwi r18, 0            # NULL check
 *   0x805d8f34  beq   +12               # if NULL, skip write
 *   0x805d8f38  li    r0, 0
 *   0x805d8f3c  stw   r0, 0(r18)        <-- DSI (r18 = 0x3cbeea8f)
 *   0x805d8f40  addi  r31, r18, 16
 *   0x805d8f44  b     -0xf10            # retry loop
 *
 * fn 0x8053b3D8's epilogue returns `*(r1+8)` (the first slot of its
 * stack-local buffer).  When the vfn fails to fill that slot, the
 * function returns whatever garbage was on the stack -- not NULL.  The
 * caller's NULL check only matches r18==0, so garbage sneaks past and
 * crashes on the store.
 *
 * Fix (2 words in place):
 *   1) `cmpwi r18, 0`         ->  `rlwinm. r0, r18, 1, 31, 31`  (0x56400FFF)
 *      Extracts r18's top bit into r0 bit 31 and sets cr0.  cr0[EQ] = 1
 *      iff top bit is 0 -- catches NULL AND any non-kernel-mapped
 *      pointer (top nibble 0x0-0x7), including 0x3cbeea8f.  Valid Wii
 *      pointers (0x8xxxxxxx MEM1, 0x9xxxxxxx MEM2) keep the top bit set
 *      and bypass the beq, so the write happens as normal.
 *
 *   2) `addi r31, r18, 16`    ->  `nop`                         (0x60000000)
 *      After the skip we'd otherwise compute r31 = r18 + 16, which is
 *      garbage+16 if r18 was garbage.  v3a tried `li r31, 0` here but
 *      that exposed a downstream crash inside the same fn 0x805d7d50:
 *      r31 is the destination of a 16-byte memcpy loop at 0x805d80b0..
 *      0x805d80dc (and again at 0x805d8044..0x805d8078), and is also
 *      the pointer dereferenced at the loop-continuation jump-table
 *      dispatcher (lbz r0, 0(r31) at 0x805d831C).  Zeroing it just
 *      moved the crash 0xE84 bytes earlier in the function.
 *      Nop'ing the addi keeps r31 at whatever the prior successful
 *      iteration left it -- the memcpy then overwrites previously-
 *      written data (subtle staleness, no crash) and the dispatcher
 *      reads a valid type byte from the old destination.
 *
 * Signature is 7 words centered on the or/cmpwi/beq/li/stw/addi/b
 * sequence -- specific enough that false matches are unlikely. */
void swbf3_havok_caller_ptr_guard_fix(u8 *addr, u32 len)
{
    /* v5e: TRAMPOLINE-based scrub.  Previous v3e only patched the cmpwi
     * (turned `cmpwi r18,0` into `rlwinm. r0, r18, 1, 31, 31`), which
     * SKIPPED the immediate writes via r18 at +12 / +16.  But downstream
     * code (e.g., `addi r31, r18, 16` at 0x805d7fec) still propagated
     * the GARBAGE pointer (e.g., 0x41ec0596) into r31, causing a later
     * crash in a memcpy loop at 0x805d80b8 with DAR = garbage+16.
     *
     * New approach: REPLACE the `or r18, r3, r3` at +0 with `bl tramp`.
     * Trampoline:
     *   or     r18, r3, r3                ; original instruction
     *   rlwinm. r0, r18, 1, 31, 31         ; test bit-0 (MSB) of r18
     *   bnelr                               ; if MSB=1 (valid MEM1 ptr), return
     *   lis    r18, 0x8000                  ; otherwise scrub r18 to scratch...
     *   ori    r18, r18, 0x3D00             ; ...= 0x80003D00 (256-byte lowmem buf)
     *   blr
     *
     * After return:
     *   - Valid r18: untouched.  cmpwi at p+4 sees non-zero, beq fails,
     *     writes happen normally at *r18.
     *   - Garbage r18: replaced with 0x80003D00 scratch.  cmpwi at p+4
     *     sees non-zero, beq fails, writes happen at lowmem scratch
     *     (safe).  Downstream `addi r31, r18, 16` = 0x80003D10, also safe.
     *
     * 3 patch sites share the trampoline (each `bl` returns to its own
     * caller via LR). */
    static const u32 SIG[5] = {
        0x7C721B78,  /* or    r18, r3, r3   <- patch target (replaced w/ bl) */
        0x2C120000,  /* cmpwi r18, 0                                          */
        0x4182000C,  /* beq   +12                                            */
        0x38000000,  /* li    r0, 0                                          */
        0x90120000,  /* stw   r0, 0(r18)                                     */
    };

    /* v5k: moved tramp from 0x80003C60 to 0x80003F60.  Same problem as
     * psq_stx had at 0x80003BFC — 0x80003C60 area is corrupted by
     * game/OS state during runtime, causing the trampoline to either
     * not execute or execute with stale data.  v5i proved 0x80003F00+
     * area is SAFE (psq_stx tramp at 0x80003FA0 now works reliably).
     * Place caller_ptr_guard tramp at 0x80003F60 (32 bytes before psq_stx
     * tramp at 0x80003FA0).  28-byte tramp fits with 4 bytes pad. */
    const u32 TRAMPOLINE_VA = 0x80003F60;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    int wrote_tramp = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p;  /* the `or r18, r3, r3` */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 havok-caller-ptr-guard-fix: disp out of range; skipping site %p\n",
                        (void *)p);
                continue;
            }

            /* Write trampoline once (shared by all sites). */
            if (!wrote_tramp)
            {
                volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
                /* v5g: scratch moved OUT of lowmem into the BSS gap
                 * between sec 12 (ends 0x80AAEEA0) and sec 13
                 * (starts 0x80AAF940).  Lowmem scratch in v5e/v5f
                 * broke psq_stx tramp downstream — IOS may stash
                 * state in 0x80003D00 region. */
                /* v5j: STRICTER validity check.  v5i's MSB-only check
                 * accepted 0xF4AA0000 as "valid" (MSB=1) when it's
                 * actually invalid garbage.  Now extract upper 7 bits
                 * and require r18 in [0x80000000, 0x81FFFFFF] (MEM1).
                 * Trampoline (7 instr = 28 bytes): */
                /* v5M DIAGNOSTIC: always-scrub trampoline.  No conditional.
                 * Every call to bl 0x80003F60 sets r18 = 0xCAFE5555 (magic
                 * invalid address).  If next Dantooine crash shows
                 * DAR=0xCAFE5555 → trampoline IS running.  If still
                 * 0xF4AA0000 → bl at site got reverted to original or
                 * trampoline at 0x80003F60 got wiped.  This WILL break
                 * Tatooine because every r18 becomes magic. */
                /* v5R: expanded check to accept BOTH MEM1 (0x8X) and MEM2 (0x9X).
                 * v5P/v5Q only accepted MEM1, possibly scrubbing valid MEM2
                 * pointers and causing cascading corruption.  Use upper-nibble
                 * check: 8 = MEM1, 9 = MEM2, anything else = scrub. */
                tramp[0] = 0x7C721B78;  /* or    r18, r3, r3                       */
                tramp[1] = 0x5640273E;  /* rlwinm r0, r18, 4, 28, 31 (upper nibble) */
                tramp[2] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                     */
                tramp[3] = 0x4D820020;  /* beqlr                                    */
                tramp[4] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                     */
                tramp[5] = 0x4D820020;  /* beqlr                                    */
                tramp[6] = 0x3E4080AA;  /* lis    r18, 0x80AA                      */
                /* v5X: FIX critical encoding bug — was using 0x6652_XXXX
                 * which is oris (opcode 25), not ori (opcode 24).  oris
                 * ORs into UPPER 16 bits of rA, so r18 = 0x80AA0000 | (0xF800<<16)
                 * = 0xF8AA0000 — exactly matches the bogus DAR we saw!
                 * Correct ori r18, r18, UIMM = 0x6252_UIMM (byte 0 = 0x62). */
                tramp[7] = 0x6252F800;  /* ori    r18, r18, 0xF800 (=0x80AAF800 scratch) */
                tramp[8] = 0x4E800020;  /* blr                                      */
                DCFlushRange((void *)TRAMPOLINE_VA, 36);
                ICInvalidateRange((void *)TRAMPOLINE_VA, 36);
                /* v5W: read back ALL 9 tramp words to verify install */
                gprintf("SWBF3 caller-tramp readback: [0]=0x%08x [1]=0x%08x\n",
                    tramp[0], tramp[1]);
                gprintf("SWBF3 caller-tramp readback: [2]=0x%08x [3]=0x%08x\n",
                    tramp[2], tramp[3]);
                gprintf("SWBF3 caller-tramp readback: [4]=0x%08x [5]=0x%08x\n",
                    tramp[4], tramp[5]);
                gprintf("SWBF3 caller-tramp readback: [6]=0x%08x [7]=0x%08x [8]=0x%08x\n",
                    tramp[6], tramp[7], tramp[8]);
                wrote_tramp = 1;
            }

            /* Patch the crash site to bl trampoline */
            u32 bl = 0x48000001 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = bl;
            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);

            gprintf("SWBF3 havok-caller-ptr-guard-fix: patched %p -> bl 0x%08x\n",
                    (void *)p, TRAMPOLINE_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 havok-caller-ptr-guard-fix: %d site(s) patched (tramp 0x%08x)\n",
                hits, TRAMPOLINE_VA);
}

/* Dantooine cutscene NULL guard for fn 0x802f5a8c.
 *
 * Crash log (DSI):
 *   SRR0 = 0x802f5aa0  DAR = 0x00000000  GPR03 = 0x00000000
 *   LR  = 0x802f5bc4  (in caller fn 0x802f5b90)
 *
 *   fn 0x802f5a8c (vtable[3]==10 dispatcher) prologue:
 *     0x802f5a8c  stwu  r1, -16(r1)
 *     0x802f5a90  mflr  r0
 *     0x802f5a94  stw   r0, 20(r1)
 *     0x802f5a98  stw   r31, 12(r1)
 *     0x802f5a9c  or    r31, r4, r4
 *     0x802f5aa0  lwz   r4, 0(r3)        <-- DSI: r3 == NULL
 *
 *   Caller fn 0x802f5b90 calls fn 0x802f5a8c twice; the SECOND call
 *   passes r3 = r30 = caller-of-caller arg3 = NULL.  The NULL is a
 *   propagated symptom from the Havok OOM (one of the upstream Havok
 *   allocs gave NULL, that NULL got stashed in a manager field which
 *   was later passed as arg3 here).
 *
 * Naive in-place fix doesn't work: every code path in this function
 * dereferences r3 (including the "type != 10" path at 0x802f5aec which
 * also does `lwz r0, 12(r3)`).  Patching `lwz r4, 0(r3)` to `b epilogue`
 * is unconditional -- breaks valid r3 cases on other maps.
 *
 * Solution: TRAMPOLINE.  Place a small NULL-check helper at a fixed
 * lowmem scratch address (0x80003BC0, in the Wii OS reserved region
 * past BootInfo/IPL but before USBLGX's kenobi hook at 0x80001800
 * and the typical first-DOL-instruction at 0x80003100).  The helper:
 *   - tests r3
 *   - if NULL, runs the function epilogue (restore r31, LR, r1) and
 *     returns r3=0 to caller (caller's "no match" handling)
 *   - if non-NULL, runs the original `lwz r4, 0(r3)` and jumps back
 *     to the next instruction in the function
 *
 * Patch the original `lwz r4, 0(r3)` at 0x802f5aa0 to `b trampoline`.
 * Only the NULL case takes the trampoline's early-exit path; valid r3
 * cases behave EXACTLY as the original (lwz runs, function continues
 * with no behavior change).
 *
 * Safe for other maps: trampoline only fires the early-exit when r3
 * is NULL, which doesn't happen on other maps that don't trigger the
 * Havok OOM cascade. */
void swbf3_havok_cutscene_null_guard(u8 *addr, u32 len)
{
    /* 6-word signature for fn 0x802f5a8c's prologue + the crash insn */
    static const u32 SIG[6] = {
        0x9421FFF0,  /* stwu  r1, -16(r1)        */
        0x7C0802A6,  /* mflr  r0                  */
        0x90010014,  /* stw   r0, 20(r1)          */
        0x93E1000C,  /* stw   r31, 12(r1)         */
        0x7C9F2378,  /* or    r31, r4, r4         */
        0x80830000,  /* lwz   r4, 0(r3)   <-- patch */
    };

    /* Trampoline scratch address (Wii lowmem; past IPL reserved, before
     * USBLGX kenobi hook).  9 instructions = 36 bytes. */
    const u32 TRAMPOLINE_VA = 0x80003BC0;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p + 20;       /* offset to lwz r4, 0(r3) */
            u8 *return_site = p + 24;      /* the next instruction after lwz */

            /* Compute branch displacements */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            s32 disp_back     = (s32)((u32)return_site - (u32)(TRAMPOLINE_VA + 0x20));

            /* Both displacements fit in 26-bit signed (~+/-32MB) */
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000 ||
                disp_back     < -0x02000000 || disp_back     >= 0x02000000)
            {
                gprintf("SWBF3 cutscene-null-guard: displacement out of range; skipping\n");
                return;
            }

            /* Write the trampoline at TRAMPOLINE_VA via direct memory access.
             * The dol is already loaded at this point, and lowmem is mapped. */
            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x2C030000;                                          /* cmpwi r3, 0       */
            tramp[1] = 0x40820018;                                          /* bne +0x18 (->+1C) */
            tramp[2] = 0x83E1000C;                                          /* lwz r31, 12(r1)   */
            tramp[3] = 0x80010014;                                          /* lwz r0, 20(r1)    */
            tramp[4] = 0x7C0803A6;                                          /* mtlr r0           */
            tramp[5] = 0x38210010;                                          /* addi r1, r1, 16   */
            tramp[6] = 0x4E800020;                                          /* blr               */
            tramp[7] = 0x80830000;                                          /* lwz r4, 0(r3)     */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);        /* b back            */

            /* Patch the crash site to branch to the trampoline */
            u32 br = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = br;

            /* Flush the patched code AND the trampoline so the CPU
             * sees the new instructions. */
            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);
            DCFlushRange((void *)TRAMPOLINE_VA, 36);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 36);

            gprintf("SWBF3 cutscene-null-guard: trampoline at 0x%08x, crash site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)crash_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 cutscene-null-guard: %d site(s) patched\n", hits);
}

/* Dantooine cutscene: psq_stx NULL deref guard for fn 0x806067b4.
 *
 * Crash log (DSI):
 *   SRR0 = 0x80606d2c  DAR = 0x00000000  GPR14 = 0x00000000  GPR25 = 0x80aeb550
 *
 *   0x80606d24  lwz   r14, 0(r25)        # r14 = *r25 = 0 (NULL deref)
 *   0x80606d28  lfd   f0, 0xC0(r1)
 *   0x80606d2c  psq_stx f0, 0, r14, 0, 0 <-- DSI (.long 0x1000700E -- opcode 4
 *                                            sub-op 7 = paired-single
 *                                            quantized store indexed; Dolphin
 *                                            disassembler doesn't decode it)
 *   ...                                  # 4 more r14-based stores follow
 *
 * `*r25` is NULL because of a cascading effect from our other Havok OOM
 * patches that let alloc failure propagate gracefully.  r25 (= 0x80aeb550
 * = on stack) is valid; the value AT r25 is NULL.
 *
 * Trampoline approach (same idea as cutscene_null_guard):
 *   - intercept the `lwz r14, 0(r25)` at 0x80606d24
 *   - if `*r25` is non-NULL: behave exactly like the original (r14 gets
 *     the loaded value, return)
 *   - if NULL: redirect r14 to a safe scratch address (0x80650000, in
 *     the unused inter-section gap between Text 1 end at 0x80620220 and
 *     Data 5 start at 0x80654600).  Subsequent r14-based stores hit
 *     scratch instead of address 0.  Game state gets garbage written
 *     to an unused region; no crash.
 *
 * Trampoline (5 instructions, 20 bytes) at 0x80003BFC (immediately
 * after the cutscene_null_guard trampoline at 0x80003BC0 + 36 bytes):
 *
 *   tramp:
 *     lwz   r14, 0(r25)        ; do the original load
 *     cmpwi r14, 0              ; check NULL
 *     bnelr                      ; if non-NULL, just return (LR=back)
 *     lis   r14, 0x8065         ; else r14 = 0x80650000 (scratch)
 *     blr                        ; return
 *
 * Patch at 0x80606d24: `lwz r14, 0(r25)` -> `bl tramp`.  After bl, LR
 * is set to 0x80606d28 (next instruction); trampoline's blr returns
 * there.  Function's own saved LR is unchanged on the stack.
 *
 * 6-word signature anchored on the lwz + the surrounding distinctive
 * sequence: bl(target_varies) + lwz r14 + lfd 0xC0(r1) + psq_stx +
 * lwz r0, 656(r1) + lfd 0xC8(r1).  bl target is at offset varies so
 * we match only the lwz onward. */
void swbf3_havok_psq_stx_null_guard(u8 *addr, u32 len)
{
    /* 6-word signature: lwz r14 + lfd + psq_stx + lwz + lfd + stfd r14+8 */
    static const u32 SIG[6] = {
        0x81D90000,  /* lwz   r14, 0(r25)  <-- patch target */
        0xE00100C0,  /* lfd   f0, 0xC0(r1)                  */
        0x1000700E,  /* psq_stx f0, 0, r14, 0, 0            */
        0x80010290,  /* lwz   r0, 656(r1)                   */
        0xE00100C8,  /* lfd   f0, 0xC8(r1)                  */
        0xF00E0008,  /* stfd  f0, 8(r14)                    */
    };

    /* v5i: try end-of-lowmem location 0x80003FA0 instead of 0x80003BFC.
     * v5g's tramp at 0x80003BFC FAILED at runtime (DAR=0) even though
     * patch was installed.  Suspect game uses 0x80003BC0-0x80003BFC
     * region for transient state.  0x80003FA0 is just before Text 0
     * section at 0x80004000 — closer to game-reserved region. */
    const u32 TRAMPOLINE_VA = 0x80003FA0;
    /* scratch at 0x80003FC0 (40 bytes), tramp 24 bytes ends at 0x80003FB8 */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p;  /* the lwz r14, 0(r25) */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 psq-stx-null-guard: disp out of range; skipping\n");
                return;
            }

            /* v5V: MEM1/MEM2 check + move scratch to BSS gap 0x80AAF000.
             * NULL-only check let r14 = 0x30 through (DAR=0x30 crash).
             * 9-instr tramp = 36 bytes at 0x80003FA0-0x80003FC3.
             * Old scratch at 0x80003FC0 would overlap, so move to BSS. */
            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x81D90000;  /* lwz    r14, 0(r25)                  */
            tramp[1] = 0x55C0273E;  /* rlwinm r0, r14, 4, 28, 31 (upper nibble) */
            tramp[2] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                  */
            tramp[3] = 0x4D820020;  /* beqlr                                 */
            tramp[4] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                  */
            tramp[5] = 0x4D820020;  /* beqlr                                 */
            tramp[6] = 0x3DC080AA;  /* lis    r14, 0x80AA                   */
            tramp[7] = 0x61CEF000;  /* ori    r14, r14, 0xF000 (=0x80AAF000 BSS scratch) */
            tramp[8] = 0x4E800020;  /* blr                                   */

            /* Patch the crash site to bl trampoline */
            u32 bl = 0x48000001 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = bl;

            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);
            DCFlushRange((void *)TRAMPOLINE_VA, 36);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 36);

            gprintf("SWBF3 psq-stx-null-guard: trampoline at 0x%08x, crash site %p -> bl +%d\n",
                    TRAMPOLINE_VA, (void *)crash_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 psq-stx-null-guard: %d site(s) patched\n", hits);
}

/* v5S: string-scan early-return guard for fn 0x80391F34.
 *
 * After our caller_ptr_guard scrub fixes the Havok OOM cascade at
 * fn 0x805d7d50, Dantooine progresses past the cutscene world load
 * but crashes downstream in fn 0x80391F34 (a string-scan that looks
 * for '/' or '\') with input r3 = 0x02F1556X (garbage pointer left
 * over from corrupted Havok state).
 *
 * fn 0x80391F34 is a LEAF function (no prologue/epilogue, LR preserved
 * across the call).  We can replace its first instruction with `b tramp`,
 * test r3 validity (MEM1 or MEM2), and either continue with original
 * code path OR return -1 immediately.
 *
 * Caller code:
 *   bl 0x80391F34          (returns r3 = index or -1)
 *   cmpwi r3, 0
 *   blt (handle "not found")
 *
 * Forcing r3 = -1 takes the "not found" branch, skipping the bad
 * downstream cutscene processing.  For Tatooine with valid r3, the
 * original code path runs unchanged.
 *
 * Trampoline (9 instr = 36 bytes) at lowmem 0x80003D00. */
void swbf3_dantooine_string_scan_guard(u8 *addr, u32 len)
{
    /* Match fn 0x80391F34 start: "or r5, r4, r4; li r7, 0; b +0x58" */
    static const u32 SIG[3] = {
        0x7C852378,  /* or r5, r4, r4  <- patch target (replaced w/ b tramp) */
        0x38E00000,  /* li r7, 0                                              */
        0x48000058,  /* b +0x58 → 0x80391F94                                  */
    };

    const u32 TRAMPOLINE_VA = 0x80003D00;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p;  /* the `or r5, r4, r4` at fn entry */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-string-scan-guard: disp out of range\n");
                return;
            }

            /* Compute b back to crash_site+4 (i.e., the second instr li r7, 0) */
            u32 back_va = (u32)crash_site + 4;
            s32 disp_back = (s32)((u32)back_va - (u32)(TRAMPOLINE_VA + 32));
            if (disp_back < -0x02000000 || disp_back >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-string-scan-guard: back disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x7C852378;  /* or    r5, r4, r4                       */
            tramp[1] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31 (upper nibble) */
            tramp[2] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                     */
            tramp[3] = 0x41820014;  /* beq +20 → tramp[8] (valid path)         */
            tramp[4] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                     */
            tramp[5] = 0x4182000C;  /* beq +12 → tramp[8] (valid path)         */
            tramp[6] = 0x3860FFFF;  /* li    r3, -1 (invalid: return "not found") */
            tramp[7] = 0x4E800020;  /* blr (back to caller of fn 0x80391F34)   */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);  /* b back */
            DCFlushRange((void *)TRAMPOLINE_VA, 36);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 36);

            /* Patch the crash site to b tramp (no link - LR preserved) */
            u32 br = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = br;
            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);

            gprintf("SWBF3 dantooine-string-scan-guard: patched fn entry %p -> b 0x%08x\n",
                    (void *)crash_site, TRAMPOLINE_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-string-scan-guard: %d site(s) patched\n", hits);
}

/* v5T: r3-validity guard at fn 0x805d80a0 entry.
 *
 * After v5S, Dantooine progresses further into cutscene init but crashes
 * at SRR0=0x805d8160 with DAR=0x00000112 (= r3 + 4 with r3=0x10E).  This
 * is INSIDE fn 0x805d80a0 (a virtual function called via bctrl from the
 * cutscene rendering code, with garbage r3 input from corrupted Havok
 * state).
 *
 * fn 0x805d80a0 prologue:
 *   mr r12, r1           <-- patch here with `b tramp` (no link, LR preserved)
 *   subfic r11, r11, -0x60
 *   stwux r1, r1, r11
 *   mflr r0
 *   ...
 *
 * Trampoline tests r3.  If invalid (upper nibble not 0x8/0x9), returns
 * NULL immediately to caller (no frame allocated yet, LR untouched).
 * If valid, continues with original prologue.
 *
 * For Tatooine with valid r3, behavior unchanged.  For Dantooine garbage r3,
 * function returns NULL.  Caller's downstream code presumably handles
 * NULL return gracefully (or at worst hits a NULL deref instead of a
 * DSI on 0x112). */
void swbf3_dantooine_vfn_r3_guard(u8 *addr, u32 len)
{
    /* v5T crisis fix: 3-instruction SIG matched 50 unrelated functions
     * with the same CodeWarrior prologue, breaking the entire game.
     * Extend SIG to 7 instructions including Broadway-specific psq_st
     * and psq_l with EXACT operand encodings to uniquely identify
     * fn 0x805d80a0. */
    static const u32 SIG[7] = {
        0x7C2C0B78,  /* mr     r12, r1                 <-- patch target */
        0x216BFFA0,  /* subfic r11, r11, -0x60                          */
        0x7C21596E,  /* stwux  r1, r1, r11                              */
        0x7C0802A6,  /* mflr   r0                                       */
        0x1020200C,  /* psq_st (Broadway-specific, exact encoding)      */
        0x900C0004,  /* stw    r0, 4(r12)                               */
        0xE0040008,  /* psq_l  (Broadway-specific, exact encoding)      */
    };

    const u32 TRAMPOLINE_VA = 0x80003D40;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p;  /* the `mr r12, r1` at fn entry */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-vfn-r3-guard: disp out of range\n");
                return;
            }

            u32 back_va = (u32)crash_site + 4;  /* back to subfic */
            s32 disp_back = (s32)((u32)back_va - (u32)(TRAMPOLINE_VA + 32));
            if (disp_back < -0x02000000 || disp_back >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-vfn-r3-guard: back disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x7C2C0B78;  /* mr     r12, r1                          */
            tramp[1] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31 (upper nibble)  */
            tramp[2] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                      */
            tramp[3] = 0x41820014;  /* beq +20 → tramp[8] (valid path)          */
            tramp[4] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                      */
            tramp[5] = 0x4182000C;  /* beq +12 → tramp[8] (valid path)          */
            tramp[6] = 0x38600000;  /* li    r3, 0 (NULL return)                */
            tramp[7] = 0x4E800020;  /* blr (back to caller of fn 0x805d80a0)    */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);  /* b back */
            DCFlushRange((void *)TRAMPOLINE_VA, 36);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 36);

            /* Patch crash site to b tramp (no link - LR preserved for early return) */
            u32 br = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = br;
            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);

            gprintf("SWBF3 dantooine-vfn-r3-guard: patched fn entry %p -> b 0x%08x\n",
                    (void *)crash_site, TRAMPOLINE_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-vfn-r3-guard: %d site(s) patched\n", hits);
}

/* v5Z: hkArray data-pointer corruption guard.
 *
 * After the v5X chain progresses past psq_stx and string-scan, Dantooine
 * now crashes at:
 *
 *   ERROR 2 (DSI) SRR0 = 0x805d6718  DAR = 0x00000004
 *
 *   0x805d6708  lwz  r5, 0(r31)      # r5 = *a1 = array data pointer
 *   0x805d670c  li   r4, 0x10
 *   0x805d6710  li   r3, 1
 *   0x805d6714  li   r0, -1
 *   0x805d6718  stw  r7, 0(r5)       # CRASH: r5 == 4 (corrupted ptr)
 *
 * Inside sub_805D6514 ("reset hkArray then push one new node").  The
 * array data pointer should be a fresh allocation from sub_8053B97C
 * (small slab allocator) — instead it's the literal value 4.  This
 * happens because the slab class-0 freelist head got overwritten with
 * 4 by a prior consumer that used the freelist node as data storage
 * (consumer's first u32 was 4, the bookkeeping count for the node).
 *
 * Patch: trampoline at 0x805d6708 that loads *r31 and validates it.
 * If r5 isn't in MEM1 (0x8xxx) or MEM2 (0x9xxx), substitute scratch
 * BSS pointer at 0x80AAF400.  The function continues writing the new
 * node + init fields into scratch (discarded) and returns cleanly.
 * The new slab node (r7) becomes "leaked" since *a1 no longer holds
 * it, but the function completes without crash, letting the cutscene
 * progress past this point.
 *
 * 5-instruction SIG covers the lwz + 3 lis + stw — distinctive enough
 * to be unique in the .text section. */
void swbf3_dantooine_array_data_guard(u8 *addr, u32 len)
{
    static const u32 SIG[5] = {
        0x80BF0000,  /* lwz r5, 0(r31)   <-- patch target                 */
        0x38800010,  /* li  r4, 0x10                                       */
        0x38600001,  /* li  r3, 1                                          */
        0x3800FFFF,  /* li  r0, -1                                         */
        0x90E50000,  /* stw r7, 0(r5)    <-- the crash                     */
    };

    const u32 TRAMPOLINE_VA = 0x80003E00;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *crash_site = p;  /* the `lwz r5, 0(r31)` */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)crash_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-array-data-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x80BF0000;  /* lwz    r5, 0(r31)                       */
            tramp[1] = 0x54A0273E;  /* rlwinm r0, r5, 4, 28, 31 (upper nibble) */
            tramp[2] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                      */
            tramp[3] = 0x4D820020;  /* beqlr                                    */
            tramp[4] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                      */
            tramp[5] = 0x4D820020;  /* beqlr                                    */
            tramp[6] = 0x3CA080AA;  /* lis    r5, 0x80AA                       */
            tramp[7] = 0x60A5F000;  /* ori    r5, r5, 0xF000 (=0x80AAF000 BSS, shared w/ psq-stx) */
            tramp[8] = 0x4E800020;  /* blr                                      */

            /* Patch the crash site to bl trampoline (sets LR to next instr) */
            u32 bl = 0x48000001 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)crash_site) = bl;

            DCFlushRange(crash_site, 4);
            ICInvalidateRange(crash_site, 4);
            DCFlushRange((void *)TRAMPOLINE_VA, 36);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 36);

            gprintf("SWBF3 dantooine-array-data-guard: trampoline at 0x%08x, crash site %p -> bl +%d\n",
                    TRAMPOLINE_VA, (void *)crash_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-array-data-guard: %d site(s) patched\n", hits);
}

/* v6B: ROOT-CAUSE FIX — grow the Havok slab pool (atomic version).
 *
 * v6A's SIG-based approach found only 3 of 8 alloc sites at runtime for
 * unknown reasons (all 8 sites have identical bytes in IDA — possibly a
 * cache or loop-iteration quirk in the patch). The partial state caused
 * buffer overflow: 3 slabs allocated as 16KB but the fill-loop wrote
 * 16KB worth of nodes into the OTHER slabs that were still 8KB, corrupting
 * adjacent memory.
 *
 * v6B uses HARDCODED VAs for ALL 12 sites across BOTH allocator functions
 * (sub_80534174 + sub_80534684), with an atomic verify-all-then-patch-all
 * approach. If any site doesn't have the expected original bytes, the
 * entire patch aborts — no partial state possible.
 *
 * Bumps 8 KB slabs → 16 KB slabs, doubling freelist nodes per refill,
 * halving allocation pressure on the Havok memory system. */
void swbf3_havok_grow_slab_pool(u8 *addr, u32 len)
{
    /* Run-once guard. The gamepatches dispatcher calls this per DOL
     * section, but we use absolute VAs so we only need to apply once. */
    static int already_applied = 0;
    if (already_applied) return;

    /* Gate on disc ID — already gated upstream but double-check. */
    if (memcmp((const void *)0x80000000, "RSBE3", 5) != 0) return;

    /* Each entry: (VA, expected original DWORD, replacement DWORD). */
    static const u32 PATCHES[][3] = {
        /* sub_80534174 alloc-size sites (4) */
        { 0x8053425C, 0x38602040, 0x38604040 },
        { 0x805342A4, 0x38602040, 0x38604040 },
        { 0x8053446C, 0x38602040, 0x38604040 },
        { 0x805344B4, 0x38602040, 0x38604040 },
        /* sub_80534684 alloc-size sites (4) */
        { 0x80534750, 0x38602040, 0x38604040 },
        { 0x80534798, 0x38602040, 0x38604040 },
        { 0x80534960, 0x38602040, 0x38604040 },
        { 0x805349A8, 0x38602040, 0x38604040 },
        /* sub_80534174 slab-end ptr (addi r0, r3, 0x2000 -> 0x4000) */
        { 0x8053430C, 0x38032000, 0x38034000 },
        /* sub_80534174 body fill-loop terminator (cmpwi r8, 0x2000 -> 0x4000) */
        { 0x805345A4, 0x2C082000, 0x2C084000 },
        /* sub_80534684 slab-end ptr */
        { 0x80534800, 0x38032000, 0x38034000 },
        /* sub_80534684 body fill-loop terminator (cmpwi r9, 0x2000 -> 0x4000) */
        { 0x80534A98, 0x2C092000, 0x2C094000 },
    };
    const int N = sizeof(PATCHES) / sizeof(PATCHES[0]);

    /* Pass 1: verify ALL sites have expected original bytes. */
    for (int i = 0; i < N; i++)
    {
        u32 actual = *(volatile u32 *)PATCHES[i][0];
        if (actual != PATCHES[i][1])
        {
            gprintf("SWBF3 grow-slab-pool: site %d (0x%08x) verify FAILED: got 0x%08x, expected 0x%08x — aborting all\n",
                    i, PATCHES[i][0], actual, PATCHES[i][1]);
            return;
        }
    }

    /* Pass 2: all verified — apply all patches atomically. */
    for (int i = 0; i < N; i++)
    {
        volatile u32 *target = (volatile u32 *)PATCHES[i][0];
        *target = PATCHES[i][2];
        DCFlushRange((void *)target, 4);
        ICInvalidateRange((void *)target, 4);
    }

    already_applied = 1;
    gprintf("SWBF3 grow-slab-pool: ATOMIC patch applied — %d sites (8 alloc-size + 2 slab-end + 2 fill-loop) patched. Heap doubled to 16KB slabs.\n", N);
}

/* v6C: Havok freelist validator at sub_8053B97C.
 *
 * Even with the heap doubled (v6B), the slab allocator still returns
 * garbage (DAR=0x3B5194D1 from sub_80592FBC after bl sub_8053B97C).
 * The freelist HEAD itself is corrupted to a non-MEM pointer — some
 * upstream use-after-free wrote into a node that was still on the
 * freelist, then re-allocated. Doubling the slab pool reduces but
 * doesn't eliminate the race.
 *
 * Fix: insert a validator before sub_8053B97C pops the freelist head.
 *
 * Original code at 0x8053b9b8:
 *   8053b9b8  lwz   r8, 0x28(r7)     ; r8 = freelist head (could be junk)
 *   8053b9bc  cmpwi r8, 0             <-- patch site
 *   8053b9c0  beq   loc_8053B9E0     ; fallback if 0
 *   8053b9c4  ...pop r8 from freelist...
 *   8053b9ec  mr    r3, r8
 *   8053b9f0  blr
 *   8053b9f4  b     sub_8053B3D8     ; the refill fallback
 *
 * Replace cmpwi with `bl tramp`. Tramp:
 *   - If r8 is 0:           set CR0=EQ → beq fires → fallback to refill
 *   - If r8 in MEM1/MEM2:   set CR0=NE → fall through → pop normally
 *   - If r8 is garbage:     ZERO r7[0x28] (clear bad head), set CR0=EQ →
 *                            fallback to refill, freelist now clean
 *
 * Tramp at 0x80003E40, 13 instructions = 52 bytes. */
void swbf3_havok_freelist_guard(u8 *addr, u32 len)
{
    /* 4-instr SIG, unique in dol — verified */
    static const u32 SIG[4] = {
        0x81070028,  /* lwz r8, 0x28(r7)               */
        0x2C080000,  /* cmpwi r8, 0   <-- patch this   */
        0x41820020,  /* beq +0x20 (to 0x8053B9E0)      */
        0x80C7002C,  /* lwz r6, 0x2C(r7)               */
    };

    const u32 TRAMPOLINE_VA = 0x80003E40;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;  /* the cmpwi r8, 0 */
            s32 disp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            if (disp < -0x02000000 || disp >= 0x02000000)
            {
                gprintf("SWBF3 freelist-guard: disp out of range\n");
                return;
            }

            /* sub_8053B97C is a LEAF function (no prologue) — `bl` would
             * clobber LR and break the function's blr return. Use `b`
             * (no link) and end the tramp with a branch back to the
             * next instruction in the function. */
            u32 back_va = (u32)patch_site + 4;  /* = 0x8053B9C0, the beq after */
            s32 disp_back = (s32)(back_va - (u32)(TRAMPOLINE_VA + 12 * 4));
            if (disp_back < -0x02000000 || disp_back >= 0x02000000)
            {
                gprintf("SWBF3 freelist-guard: back disp out of range\n");
                return;
            }
            u32 b_back = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            /* Build tramp — all flow converges to tramp[12] which is
             * `b back_va`. No bl/blr, so LR is preserved across the
             * trampoline (critical for leaf functions). */
            tramp[0]  = 0x2C080000;  /* cmpwi r8, 0                              */
            tramp[1]  = 0x4182002C;  /* beq +0x2C → tramp[12] (CR0=EQ stays)     */
            tramp[2]  = 0x5500273E;  /* rlwinm r0, r8, 4, 28, 31 (upper nibble)  */
            tramp[3]  = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                       */
            tramp[4]  = 0x4182001C;  /* beq +0x1C → tramp[11] (mark_valid)        */
            tramp[5]  = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                       */
            tramp[6]  = 0x41820014;  /* beq +0x14 → tramp[11]                     */
            /* fallthrough: garbage upper nibble — zero head, force CR0=EQ */
            tramp[7]  = 0x38000000;  /* li r0, 0                                  */
            tramp[8]  = 0x90070028;  /* stw r0, 0x28(r7) (zero bad head)          */
            tramp[9]  = 0x2C000000;  /* cmpwi r0, 0 (CR0=EQ)                       */
            tramp[10] = 0x48000008;  /* b +0x08 → tramp[12] (skip valid block)    */
            /* mark_valid (tramp[11]): force CR0=NE */
            tramp[11] = 0x2C080000;  /* cmpwi r8, 0 (r8 ≠ 0 → CR0=NE)             */
            /* tramp[12]: branch back to the beq at 0x8053B9C0 */
            tramp[12] = b_back;

            DCFlushRange((void *)TRAMPOLINE_VA, 13 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 13 * 4);

            /* Patch the crash site to `b trampoline` (no link — keep LR). */
            u32 b_to_tramp = 0x48000000 | (((u32)disp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 freelist-guard: trampoline at 0x%08x, patch site %p -> b +%d (back=+%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp, disp_back);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 freelist-guard: %d site(s) patched\n", hits);
}

/* v6D: Large-alloc-path guard for sub_8053B97C.
 *
 * v6C's freelist-guard at 0x8053B9BC ONLY validates the SMALL-alloc
 * path (size <= 0x2000). For size > 0x2000, sub_8053B97C takes the
 * LARGE path at 0x8053B9F8 which tail-calls (bctr) the parent
 * allocator's vfn[5]:
 *   8053b9f8  lwz r3, 4(r3)        ; r3 = mgr.parent
 *   8053b9fc  lwz r12, 0(r3)       ; vtable
 *   8053ba00  lwz r12, 0x14(r12)   ; vfn[5] = large_alloc
 *   8053ba04  mtctr r12
 *   8053ba08  bctr                  <-- tail call, never returns to here
 *
 * sub_80592F6C computes an alloc size of (a1 * (4*(a2+a3)+52) + 63) & ~0xF
 * which, for cutscene animation data, is typically > 0x2000 → LARGE path.
 * If the parent's vfn[5] returns garbage (which it does after corruption),
 * sub_80592F6C's `sth r30, 2(r3)` crashes.
 *
 * Fix: replace the `bctr` at 0x8053BA08 with `b tramp`. The trampoline:
 *   1. Saves LR (sub_8053B97C's return addr) into r12
 *   2. bctrl — calls vfn[5] (was the tail call); clobbers LR
 *   3. Restores LR from r12
 *   4. Validates r3 result (NULL, MEM1, MEM2 — anything else is garbage)
 *   5. If valid: blr (returns r3 to original caller)
 *   6. If invalid: r3 = 0x80AAF200 (BSS scratch buffer), blr
 *
 * The scratch buffer absorbs corrupt-allocator returns. The caller writes
 * its node init data into scratch and returns. Downstream code reads
 * stale scratch (wrong data) but doesn't crash — preferable to DSI. */
void swbf3_havok_large_alloc_guard(u8 *addr, u32 len)
{
    /* 5-instr SIG = the entire large-alloc path. Patch site is bctr at
     * SIG offset +0x10 (last instruction). */
    static const u32 SIG[5] = {
        0x80630004,  /* lwz   r3, 4(r3)       */
        0x81830000,  /* lwz   r12, 0(r3)      */
        0x818C0014,  /* lwz   r12, 0x14(r12)  */
        0x7D8903A6,  /* mtctr r12             */
        0x4E800420,  /* bctr  <-- patch here  */
    };

    const u32 TRAMPOLINE_VA = 0x80003E80;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 16;  /* the bctr at sig+0x10 */
            s32 disp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            if (disp < -0x02000000 || disp >= 0x02000000)
            {
                gprintf("SWBF3 large-alloc-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            /* CRITICAL: r12 is VOLATILE in PPC EABI — vfn[5] may clobber
             * it during the bctrl. We must save LR on the stack, not in
             * a register. Allocate a 16-byte frame, use the reserved
             * saved-LR slot at SP+20 (= caller_r1+4, EABI convention). */
            tramp[0]  = 0x9421FFF0;  /* stwu  r1, -16(r1) (alloc small frame) */
            tramp[1]  = 0x7C0802A6;  /* mflr  r0                              */
            tramp[2]  = 0x90010014;  /* stw   r0, 20(r1) (save LR to caller's reserved slot) */
            tramp[3]  = 0x4E800421;  /* bctrl (call vfn[5] — CTR set by mtctr at 0x8053BA04) */
            tramp[4]  = 0x80010014;  /* lwz   r0, 20(r1) (restore LR)         */
            tramp[5]  = 0x7C0803A6;  /* mtlr  r0                              */
            tramp[6]  = 0x38210010;  /* addi  r1, r1, 16 (free frame)         */
            tramp[7]  = 0x2C030000;  /* cmpwi r3, 0                            */
            tramp[8]  = 0x41820018;  /* beq +0x18 → inv_path (tramp[14])     */
            tramp[9]  = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31              */
            tramp[10] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)                    */
            tramp[11] = 0x4D820020;  /* beqlr (valid, return r3)               */
            tramp[12] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)                    */
            tramp[13] = 0x4D820020;  /* beqlr (valid)                          */
            /* inv_path: invalid result, substitute scratch */
            tramp[14] = 0x3C6080AA;  /* lis r3, 0x80AA                         */
            tramp[15] = 0x6063F200;  /* ori r3, r3, 0xF200 (=0x80AAF200 BSS)   */
            tramp[16] = 0x4E800020;  /* blr                                    */

            DCFlushRange((void *)TRAMPOLINE_VA, 17 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 17 * 4);

            /* Replace bctr with `b tramp` (no link). LR is preserved from
             * the original `bl sub_8053B97C` caller; trampoline does the
             * actual call+validate+return on caller's behalf. */
            u32 b_to_tramp = 0x48000000 | (((u32)disp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 large-alloc-guard: trampoline at 0x%08x, bctr site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 large-alloc-guard: %d site(s) patched\n", hits);
}

/* v6E: SMALL-fallback guard at sub_8053B9F4.
 *
 * Even with v6D (LARGE path guard) and v6C (small-path freelist
 * validator), the crash at sub_80592FBC persists with DAR=0x3B5194D1.
 * The path being taken is:
 *   sub_80592F6C → bl sub_8053B97C
 *     sub_8053B97C SMALL path
 *     v6C zeros bad freelist head, sets CR0=EQ
 *     beq → 0x8053B9E0 → li r8,0; cmpwi; beq → 0x8053B9F4
 *     b sub_8053B3D8  (TAIL CALL — return value propagates to sub_80592F6C)
 *       sub_8053B3D8 → bctrl vfn[5] → b epilogue → return r3 to OUR caller
 *
 * sub_8053B3D8's vfn[5] return is NOT validated and can be garbage.
 *
 * Fix: replace the `b sub_8053B3D8` at 0x8053B9F4 with a trampoline
 * that bl's sub_8053B3D8 (with link), validates r3 result, substitutes
 * 0x80AAF300 scratch if invalid, then blr's to the original caller of
 * sub_8053B97C.
 *
 * Critical: must save LR on the stack since the bl clobbers it and
 * vfn[5] / sub_8053B3D8 can clobber any volatile reg. */
void swbf3_havok_small_fallback_guard(u8 *addr, u32 len)
{
    /* 3-instr SIG anchored on the blr before, the tail-call b, and the
     * first instruction of the LARGE path that follows. */
    static const u32 SIG[3] = {
        0x4E800020,  /* blr (end of small-path success)         */
        0x4BFFF9E4,  /* b sub_8053B3D8  <-- patch this          */
        0x80630004,  /* lwz r3, 4(r3) (start of large path)     */
    };

    const u32 TRAMPOLINE_VA = 0x80003ED0;
    const u32 TARGET_VA = 0x8053B3D8;  /* sub_8053B3D8 */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;  /* the b sub_8053B3D8 */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 small-fallback-guard: disp out of range\n");
                return;
            }

            /* bl from tramp[3] to sub_8053B3D8. tramp[3] is at TRAMP+0x0C. */
            u32 bl_addr = TRAMPOLINE_VA + 12;
            s32 disp_bl = (s32)(TARGET_VA - bl_addr);
            u32 bl_instr = 0x48000001 | (((u32)disp_bl) & 0x03FFFFFC);

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0]  = 0x9421FFF0;  /* stwu r1, -16(r1)                       */
            tramp[1]  = 0x7C0802A6;  /* mflr r0                                */
            tramp[2]  = 0x90010014;  /* stw r0, 20(r1) (LR in EABI slot)       */
            tramp[3]  = bl_instr;    /* bl sub_8053B3D8                        */
            tramp[4]  = 0x80010014;  /* lwz r0, 20(r1) (restore LR)            */
            tramp[5]  = 0x7C0803A6;  /* mtlr r0                                */
            tramp[6]  = 0x38210010;  /* addi r1, r1, 16 (free frame)           */
            tramp[7]  = 0x2C030000;  /* cmpwi r3, 0                            */
            tramp[8]  = 0x41820018;  /* beq +0x18 → inv_path (tramp[14])      */
            tramp[9]  = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31               */
            tramp[10] = 0x2C000008;  /* cmpwi r0, 8                            */
            tramp[11] = 0x4D820020;  /* beqlr                                  */
            tramp[12] = 0x2C000009;  /* cmpwi r0, 9                            */
            tramp[13] = 0x4D820020;  /* beqlr                                  */
            tramp[14] = 0x3C6080AA;  /* lis r3, 0x80AA                         */
            tramp[15] = 0x6063F300;  /* ori r3, r3, 0xF300 (=0x80AAF300 BSS)   */
            tramp[16] = 0x4E800020;  /* blr                                    */

            DCFlushRange((void *)TRAMPOLINE_VA, 17 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 17 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 small-fallback-guard: tramp at 0x%08x, site %p -> b +%d (bl disp=+%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, disp_bl);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 small-fallback-guard: %d site(s) patched\n", hits);
}

/* v6F: lbz r0, 0x12B0(r3) guard at 0x805D81F4.
 *
 * After v6E got us past 0x80592FBC, the new crash is at this lbz where
 * r3 has cascaded into the NDEV-only 0xA0000000+ MEM2 mirror range
 * (retail Wii only has 0x90000000+).  DAR=0xA12AE674 means r3=0xA12AD3C4.
 *
 * The chain:
 *   805d81b0  bctrl                     ; vfn returns r3
 *   805d81dc  lwz r3, 0xC(r3)            ; r3 = *(vfn_result + 0xC)
 *   805d81e8  rlwinm r0, r0, 5, 0, 26
 *   805d81ec  add r0, r20, r0
 *   805d81f0  add r3, r0, r3
 *   805d81f4  lbz r0, 0x12B0(r3)         <-- CRASH (r3 in NDEV-only region)
 *
 * Replace lbz with `b tramp_v6F`:
 *   - If r3 upper nibble == 8 (MEM1) or 9 (MEM2): do the lbz normally
 *   - Else: r0 = 0 (safe placeholder), continue
 * Then branch back to next instruction (0x805D81F8). */
void swbf3_dantooine_lbz_guard(u8 *addr, u32 len)
{
    /* 3-instr SIG: add r3, r0, r3 | lbz r0, 0x12B0(r3) | b +0x14 (uniquely
     * identifies this site — the b takes the function to the post-lbz
     * processing). */
    static const u32 SIG[3] = {
        0x7C601A14,  /* add r3, r0, r3                              */
        0x880312B0,  /* lbz r0, 0x12B0(r3)   <-- patch this site    */
        0x48000014,  /* b +0x14 (skip the alt-offset path)          */
    };

    const u32 TRAMPOLINE_VA = 0x80003F20;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;  /* the lbz */
            u32 back_va = (u32)patch_site + 4;  /* = 0x805D81F8 */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-lbz-guard: tramp disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31     */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                   */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7] do_load */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                   */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]          */
            tramp[5] = 0x38000000;  /* li r0, 0 (invalid → placeholder) */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x880312B0;  /* lbz r0, 0x12B0(r3)             */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);  /* b back */

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-lbz-guard: tramp at 0x%08x, site %p -> b +%d (back +%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, disp_back);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-lbz-guard: %d site(s) patched\n", hits);
}

/* v6G: NDEV memory mirror BAT alias — ROOT-CAUSE fix.
 *
 * SWBF3 r2.91120a was built for NDEV dev kit which had MEM2 mirrored at
 * 0xA0000000-0xA3FFFFFF (extra 64 MB virtual range aliasing same physical
 * MEM2). Retail Wii only has 0x90000000-0x93FFFFFF cached MEM2; the 0xA0
 * mirror doesn't exist. Game code computes pointers in 0xA0xxxxxx range
 * → DSI fault on retail. Confirmed by crash log #21 (DAR=0xA12AE674) and
 * by Dolphin MCP rejecting 0xA0000000 as "not in MEM1/MEM2".
 *
 * Fix: program Broadway DBAT5 to alias 0xA0000000-0xA3FFFFFF → physical
 * MEM2 (0x10000000-0x13FFFFFF, same as 0x90000000+). Two virtual ranges
 * map to the same physical 64 MB. Game's NDEV pointer math works
 * transparently without changing any data.
 *
 * DBAT5U = 0xA00007FF  (BEPI=0x5000=0xA0000000>>17, BL=0x1FF=64MB, Vs=Vp=1)
 * DBAT5L = 0x10000012  (BRPN=0x0800, WIMG=0010 (M only, cached), PP=10 RW)
 *
 * Hook: replace first instruction at game entry (0x800060A4) with
 * `b tramp`. Trampoline programs the BAT, then calls original target
 * (0x80006210) and branches back to 0x800060A8.
 *
 * Encoded mtspr SPR-field-swap (PowerPC quirk):
 *   mtspr 570 (DBAT5U), r0 = 0x7C1A8BA6
 *   mtspr 571 (DBAT5L), r0 = 0x7C1B8BA6
 *
 * Risk: relies on HID4[H4A]=1 (extended BATs active). Libogc enables this
 * on Wii so most retail games already have it on. If game's __start later
 * resets BAT5, our alias vanishes — would need a later hook point. */
void swbf3_ndev_bat_setup_fix(u8 *addr, u32 len)
{
    /* Run-once guard. */
    static int already_applied = 0;
    if (already_applied) return;
    if (memcmp((const void *)0x80000000, "RSBE3", 5) != 0) return;

    const u32 ENTRY_VA = 0x800060A4;       /* game entry, current first instr = bl +0x16C */
    const u32 ORIG_FIRST_INSTR = 0x4800016D;  /* bl 0x80006210 — the bl we replace */
    const u32 ORIG_BL_TARGET = 0x80006210;    /* what the bl was calling */
    const u32 BACK_VA = ENTRY_VA + 4;         /* return point = 0x800060A8 */
    /* v6X: relocated from 0x80003B40 → 0x80003200 to fit extended BAT tramp
     * (22 instr = 88 bytes). Lowmem 0x80003100-0x80003B00 is large unused gap. */
    const u32 TRAMPOLINE_VA = 0x80003200;

    /* Verify the entry point still holds the expected bl (so we abort
     * cleanly if the build differs). */
    u32 actual = *(volatile u32 *)ENTRY_VA;
    if (actual != ORIG_FIRST_INSTR)
    {
        gprintf("SWBF3 ndev-bat-setup: entry @0x%08x has 0x%08x (expected 0x%08x) — aborting\n",
                ENTRY_VA, actual, ORIG_FIRST_INSTR);
        return;
    }

    /* Trampoline at 0x80003200 (22 instr = 88 bytes).
     * v6X: setup THREE BAT aliases for NDEV MEM2 mirroring on retail:
     *   DBAT5: 0xA0000000-0xA3FFFFFF → phys MEM2 (first 64MB of NDEV mirror)
     *   DBAT6: 0xA4000000-0xA7FFFFFF → phys MEM2 (aliases first 64MB — NDEV's
     *          extra 32MB; on retail aliases back since we lack physical)
     *   DBAT7: 0x94000000-0x97FFFFFF → phys MEM2 (alias for direct MEM2 alloc
     *          beyond first 64MB; same aliasing strategy)
     *
     * The aliasing creates data overlap (writes via 0xA4xxxxxx clobber
     * 0xA0xxxxxx contents) but PREVENTS BUS FAULTS on retail when game code
     * built for NDEV's 96MB MEM2 derefs pointers in the extra 32MB range.
     * Better data overlap than crash. */
    u32 bl_addr = TRAMPOLINE_VA + 20 * 4;   /* tramp[20] = the bl */
    u32 b_addr  = TRAMPOLINE_VA + 21 * 4;   /* tramp[21] = the b back */
    s32 disp_bl = (s32)(ORIG_BL_TARGET - bl_addr);
    s32 disp_b  = (s32)(BACK_VA - b_addr);
    if (disp_bl < -0x02000000 || disp_bl >= 0x02000000 ||
        disp_b  < -0x02000000 || disp_b  >= 0x02000000)
    {
        gprintf("SWBF3 ndev-bat-setup: branch disp out of range — aborting\n");
        return;
    }
    u32 bl_instr = 0x48000001 | (((u32)disp_bl) & 0x03FFFFFC);
    u32 b_instr  = 0x48000000 | (((u32)disp_b)  & 0x03FFFFFC);

    volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
    /* DBAT5 (SPR 570/571): virtual 0xA0000000-0xA3FFFFFF → MEM2 phys */
    tramp[0]  = 0x3C00A000;  /* lis r0, 0xA000                           */
    tramp[1]  = 0x600007FF;  /* ori r0, r0, 0x07FF (DBAT5U=0xA00007FF)   */
    tramp[2]  = 0x7C1A8BA6;  /* mtspr 570, r0 (DBAT5U)                   */
    tramp[3]  = 0x3C001000;  /* lis r0, 0x1000                           */
    tramp[4]  = 0x60000012;  /* ori r0, r0, 0x0012 (DBAT5L=0x10000012)   */
    tramp[5]  = 0x7C1B8BA6;  /* mtspr 571, r0 (DBAT5L)                   */
    /* DBAT6 (SPR 572/573): virtual 0xA4000000-0xA7FFFFFF → MEM2 phys (alias) */
    tramp[6]  = 0x3C00A400;  /* lis r0, 0xA400                           */
    tramp[7]  = 0x600007FF;  /* ori r0, r0, 0x07FF (DBAT6U=0xA40007FF)   */
    tramp[8]  = 0x7C1C8BA6;  /* mtspr 572, r0 (DBAT6U)                   */
    tramp[9]  = 0x3C001000;  /* lis r0, 0x1000                           */
    tramp[10] = 0x60000012;  /* ori r0, r0, 0x0012 (DBAT6L=0x10000012)   */
    tramp[11] = 0x7C1D8BA6;  /* mtspr 573, r0 (DBAT6L)                   */
    /* DBAT7 (SPR 574/575): virtual 0x94000000-0x97FFFFFF → MEM2 phys (alias) */
    tramp[12] = 0x3C009400;  /* lis r0, 0x9400                           */
    tramp[13] = 0x600007FF;  /* ori r0, r0, 0x07FF (DBAT7U=0x940007FF)   */
    tramp[14] = 0x7C1E8BA6;  /* mtspr 574, r0 (DBAT7U)                   */
    tramp[15] = 0x3C001000;  /* lis r0, 0x1000                           */
    tramp[16] = 0x60000012;  /* ori r0, r0, 0x0012 (DBAT7L=0x10000012)   */
    tramp[17] = 0x7C1F8BA6;  /* mtspr 575, r0 (DBAT7L)                   */
    tramp[18] = 0x7C0004AC;  /* sync                                      */
    tramp[19] = 0x4C00012C;  /* isync                                     */
    tramp[20] = bl_instr;    /* bl original_target (= 0x80006210)        */
    tramp[21] = b_instr;     /* b back_to_0x800060A8                     */

    DCFlushRange((void *)TRAMPOLINE_VA, 22 * 4);
    ICInvalidateRange((void *)TRAMPOLINE_VA, 22 * 4);

    /* Patch game entry point: bl → b tramp (no link). */
    s32 disp_entry = (s32)(TRAMPOLINE_VA - ENTRY_VA);
    u32 b_to_tramp = 0x48000000 | (((u32)disp_entry) & 0x03FFFFFC);
    *((volatile u32 *)ENTRY_VA) = b_to_tramp;
    DCFlushRange((void *)ENTRY_VA, 4);
    ICInvalidateRange((void *)ENTRY_VA, 4);

    already_applied = 1;
    gprintf("SWBF3 ndev-bat-setup(v6X): tramp at 0x%08x. DBAT5(0xA0-0xA3) + DBAT6(0xA4-0xA7) + DBAT7(0x94-0x97) all -> phys MEM2 (NDEV 96MB alias).\n",
            TRAMPOLINE_VA);
}

/* v6H: lwz r6, 4(r3) guard at 0x805D8160.
 *
 * After v6G's BAT alias got us past the 0xA1xxxxxx crash, FPS dropped
 * from 54→40 (game doing MORE work, progressing further). New crash at
 * 0x805D8160 `lwz r6, 4(r3)` with DAR=0x00A10004 → r3=0x00A10000.
 *
 * r3 was loaded from stack (sp+0x10) at 0x805D8150. Some earlier code
 * stored 0x00A10000 there. 0x00A10000 is below MEM1's mapped 0x80xxxxxx
 * range — no BAT covers it. Probably a NDEV-style raw physical pointer
 * (NDEV apps could sometimes access raw phys with translation off).
 *
 * Patch: replace lwz r6, 4(r3) with `b tramp`; tramp validates r3 in
 * MEM1/MEM2; if invalid, r6 = 0 (placeholder), continue. */
void swbf3_dantooine_lwz_r6_guard(u8 *addr, u32 len)
{
    static const u32 SIG[5] = {
        0x80610010,  /* lwz r3, 0x10(r1)                            */
        0x7EE8BB78,  /* mr  r8, r23                                  */
        0x80810278,  /* lwz r4, 0x278(r1)                            */
        0x7F87E378,  /* mr  r7, r28                                  */
        0x80C30004,  /* lwz r6, 4(r3)   <-- patch this              */
    };

    const u32 TRAMPOLINE_VA = 0x80003B00;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 16;  /* the lwz r6, 4(r3) */
            u32 back_va = (u32)patch_site + 4;  /* = 0x805D8164 */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-lwz-r6-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31      */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8 (MEM1?)            */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7] do_load  */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9 (MEM2?)            */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38C00000;  /* li r6, 0 (invalid placeholder) */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x80C30004;  /* lwz r6, 4(r3)                  */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-lwz-r6-guard: tramp at 0x%08x, site %p -> b +%d (back +%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, disp_back);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-lwz-r6-guard: %d site(s) patched\n", hits);
}

/* v6I: NULL guard at 0x805A8EA8 lwz r0, 0x28(r8).
 *
 * After v6G+v6H pushed us past prior crashes, the previously-masked
 * crash at 0x805A8EA8 reappears: lwz r0, 0x28(r8) with r8=0 (loaded
 * from r25+0x24 NULL field). DAR=0x28.
 *
 * Lucky: next instr is `cmpwi r0, 0; beq +0xB0`. If we substitute r0=0,
 * cmpwi sets CR0=EQ, beq fires, control branches over the would-be
 * downstream use. Safe NULL-path substitution.
 *
 * Tramp at 0x80003B70. Replace lwz with `b tramp`. */
void swbf3_dantooine_lwz_r0_null_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x81190024,  /* lwz r8, 0x24(r25)                          */
        0x80080028,  /* lwz r0, 0x28(r8)   <-- patch site         */
        0x2C000000,  /* cmpwi r0, 0                                 */
        0x418200B0,  /* beq +0xB0 (NULL-path branch)               */
    };

    const u32 TRAMPOLINE_VA = 0x80003B70;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;  /* the lwz r0, 0x28(r8) */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-lwz-r0-null-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5500273E;  /* rlwinm r0, r8, 4, 28, 31      */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                    */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7]           */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                    */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38000000;  /* li r0, 0 (NULL-path placeholder) */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x80080028;  /* lwz r0, 0x28(r8)               */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-lwz-r0-null-guard: tramp at 0x%08x, site %p -> b +%d (back +%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, disp_back);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-lwz-r0-null-guard: %d site(s) patched\n", hits);
}

/* v6J: NULL-r3 entry guard at sub_805BF554.
 *
 * Leaf setter function called via bl from 0x805A8F88 (sub_805A8xxx).
 * Takes r3 as struct ptr and does:
 *   lwz r8, 0x10(r3)
 *   lwz r7, 0x28(r3)
 *   ...conditional sets of r3->[0x10], r7->[0xC], r7->[0x10]
 * If r3=NULL, lwz r8, 0x10(r3) crashes (DAR=0x10).
 *
 * Fix: replace first lwz with `b tramp`. Tramp checks r3 in MEM1/MEM2;
 * if invalid, blr (LR is caller's bl-return, since this is a leaf —
 * skipping the function entirely is safe). If valid, do the lwz and
 * continue. */
void swbf3_dantooine_setter_null_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x81030010,  /* lwz r8, 0x10(r3)   <-- patch site (fn entry) */
        0x80E30028,  /* lwz r7, 0x28(r3)                              */
        0x2C080000,  /* cmpwi r8, 0                                   */
        0x40820018,  /* bne +0x18                                     */
    };

    const u32 TRAMPOLINE_VA = 0x80003B98;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p;  /* the lwz r8, 0x10(r3) at fn entry */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 6 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-setter-null-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31         */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                       */
            tramp[2] = 0x4182000C;  /* beq +0x0C → tramp[5] do_load     */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                       */
            tramp[4] = 0x40820008;  /* bne +0x08 → tramp[6] invalid-return */
            tramp[5] = 0x81030010;  /* lwz r8, 0x10(r3) (do original)   */
            /* fallthrough to back-branch if valid */
            tramp[6] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);   /* b back */
            tramp[7] = 0x4E800020;  /* blr (invalid path: return now)   */

            /* Wait — the flow: valid path falls from tramp[5] (lwz) into
             * tramp[6] (b back). Invalid path: tramp[4] beq jumps to
             * tramp[6]? No — beq fires when r0==9 (valid MEM2). Let me
             * reconsider the logic. */

            /* Redo: */
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31         */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                       */
            tramp[2] = 0x41820010;  /* beq +0x10 → tramp[6] do_load     */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                       */
            tramp[4] = 0x41820008;  /* beq +0x08 → tramp[6] do_load     */
            tramp[5] = 0x4E800020;  /* invalid: blr (return immediately) */
            tramp[6] = 0x81030010;  /* lwz r8, 0x10(r3)                  */
            tramp[7] = 0x48000000 | (((s32)(back_va - (TRAMPOLINE_VA + 7 * 4))) & 0x03FFFFFC);  /* b back */

            DCFlushRange((void *)TRAMPOLINE_VA, 8 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 8 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-setter-null-guard: tramp at 0x%08x, site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-setter-null-guard: %d site(s) patched\n", hits);
}

/* v6K: NULL guard at 0x80495334 lhz r0, 0x18(r3).
 *
 * After v6J got past the cutscene-loader chain, we leapt forward to a
 * completely different code region. Crash at 0x80495334:
 *   80495330  lwz r3, -0x72AC(r13)  ; SDA-rel global ptr — NULL
 *   80495334  lhz r0, 0x18(r3)      ; CRASH: DAR=0x18
 *
 * SRR1 IR=0 → this is real-mode code, possibly interrupt handler or
 * early init. The SDA global at r13-0x72AC is uninitialized.
 *
 * Substitute r0=0 if r3 invalid. The next instr is rlwinm/addi/cmplw
 * which work fine with r0=0 (gives r3=0, r0=1, comparison fails →
 * default branch path). */
void swbf3_dantooine_lhz_sda_null_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x806D8D54,  /* lwz r3, -0x72AC(r13)                       */
        0xA0030018,  /* lhz r0, 0x18(r3)   <-- patch site         */
        0x5403F87E,  /* rlwinm r3, r0, 31, 1, 31                   */
        0x38030001,  /* addi r0, r3, 1                             */
    };

    const u32 TRAMPOLINE_VA = 0x80003C00;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;  /* the lhz */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-lhz-sda-null-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31      */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                    */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7] do_load  */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                    */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38000000;  /* li r0, 0 (invalid placeholder) */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0xA0030018;  /* lhz r0, 0x18(r3)               */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-lhz-sda-null-guard: tramp at 0x%08x, site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-lhz-sda-null-guard: %d site(s) patched\n", hits);
}

/* v6L: lwz r5, 0(r5) guard at 0x805D8170.
 *
 * Post-v6K: back in cutscene-loader. Crash at 0x805D8170 lwz r5, 0(r5)
 * with DAR=0x99 → r5=0x99 (junk). r5 came from sp+0x10 (loaded just
 * before by lwz r5, 0x10(r1)). Some upstream code stored 0x99 there.
 *
 * Code context:
 *   805d8168 lwz r5, 0x10(r1)
 *   805d816c lwz r12, 0(r3)        ; vtable
 *   805d8170 lwz r5, 0(r5)         <-- CRASH
 *   805d8174 lwz r12, 8(r12)       ; vfn[2]
 *   805d8178 mtctr r12
 *   805d817c bctrl
 *
 * Substitute r5=0 if invalid. The subsequent vfn[2] gets called with
 * r5=0 — likely handled as NULL arg (or no-op). */
void swbf3_dantooine_lwz_r5_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x80A10010,  /* lwz r5, 0x10(r1)                           */
        0x81830000,  /* lwz r12, 0(r3)                              */
        0x80A50000,  /* lwz r5, 0(r5)   <-- patch site             */
        0x818C0008,  /* lwz r12, 8(r12)                             */
    };

    const u32 TRAMPOLINE_VA = 0x80003C40;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 8;  /* the lwz r5, 0(r5) */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 dantooine-lwz-r5-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5500273E;  /* rlwinm r0, r5, 4, 28, 31      */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                    */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7] do_load  */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                    */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38A00000;  /* li r5, 0 (NULL placeholder)    */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x80A50000;  /* lwz r5, 0(r5)                  */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 dantooine-lwz-r5-guard: tramp at 0x%08x, site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 dantooine-lwz-r5-guard: %d site(s) patched\n", hits);
}

/* v6M: r27+r28 double-NULL-guard at 0x805A7F5C/60.
 *
 * Crash at 0x805A7F64 lfs f4, 0x84(r27) where r27 from r19+0x58 = NULL.
 * The following lfs f0, 0x84(r28) at 0x805A7F68 has the same risk
 * (r28 from r19+0x5C). Patch both at once: replace `lwz r27, 0x58(r19)`
 * with a trampoline that loads BOTH r27 and r28, validates each, and
 * substitutes 0x80AAEEC0 (a zero-initialized BSS buffer) for invalid
 * ones. Reads from offset 0x84 of zero-buffer give 0.0, which is safe
 * for the downstream fmuls.
 *
 * Branches back to 0x805A7F64 (the first lfs). */
void swbf3_dantooine_r27_r28_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x83730058,  /* lwz r27, 0x58(r19)  <-- patch site (replaced w/ b tramp) */
        0x83930050 | 0xC,  /* lwz r28, 0x5C(r19) — encoded as 0x8393005C */
        0xC09B0084,  /* lfs f4, 0x84(r27)                                        */
        0xC01C0084,  /* lfs f0, 0x84(r28)                                        */
    };

    const u32 TRAMPOLINE_VA = 0x80003C80;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p;  /* the lwz r27, 0x58(r19) */
            u32 back_va = (u32)patch_site + 8;  /* = 0x805A7F64 (the lfs) */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 16 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 r27-r28-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            /* Validate r27 */
            tramp[0]  = 0x83730058;  /* lwz r27, 0x58(r19)                                       */
            tramp[1]  = 0x5760273E;  /* rlwinm r0, r27, 4, 28, 31 (rS=27, rA=0)                  */
            tramp[2]  = 0x2C000008;  /* cmpwi r0, 8                                              */
            tramp[3]  = 0x41820014;  /* beq +0x14 → tramp[8] r27_ok                              */
            tramp[4]  = 0x2C000009;  /* cmpwi r0, 9                                              */
            tramp[5]  = 0x4182000C;  /* beq +0x0C → tramp[8]                                     */
            tramp[6]  = 0x3F6080AA;  /* lis r27, 0x80AA                                          */
            tramp[7]  = 0x637BEEC0;  /* ori r27, r27, 0xEEC0 (=0x80AAEEC0 zero buffer)           */
            /* Validate r28 — CRITICAL FIX from prior version: rA must be 0, not 28. */
            tramp[8]  = 0x8393005C;  /* lwz r28, 0x5C(r19)                                       */
            tramp[9]  = 0x5780273E;  /* rlwinm r0, r28, 4, 28, 31 (rS=28, rA=0) — was 0x579C273E
                                       which was rlwinm r28, r28, ... clobbering r28!            */
            tramp[10] = 0x2C000008;  /* cmpwi r0, 8                                              */
            tramp[11] = 0x41820014;  /* beq +0x14 → tramp[15] r28_ok                             */
            tramp[12] = 0x2C000009;  /* cmpwi r0, 9                                              */
            tramp[13] = 0x4182000C;  /* beq +0x0C → tramp[15]                                    */
            tramp[14] = 0x3F8080AA;  /* lis r28, 0x80AA                                          */
            tramp[15] = 0x639CEEC0;  /* ori r28, r28, 0xEEC0                                     */
            /* Back to 0x805A7F64 (the lfs that uses r27) */
            tramp[16] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 17 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 17 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 r27-r28-guard: tramp at 0x%08x, site %p -> b +%d (back +%d)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, disp_back);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 r27-r28-guard: %d site(s) patched\n", hits);
}

/* v6N: NULL guard at 0x805BCDEC lwz r5, 0x178(r3).
 *
 * After v6M fixed, new crash at 0x805BCDEC where r3=NULL → DAR=0x178.
 * Same outer cutscene-loader call chain (LR=0x805A8290, then 0x806094D8/
 * 0x80606DEC/0x80610D24). Substitute r5=0 if r3 invalid. */
void swbf3_dantooine_lwz_r3_178_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x9004000C,  /* stw r0, 0xC(r4)                              */
        0x80A30178,  /* lwz r5, 0x178(r3)  <-- patch site            */
        0x3B85FFFF,  /* addi r28, r5, -1                              */
        0x579D103A,  /* rlwinm r29, r28, 2, 0, 29                     */
    };

    const u32 TRAMPOLINE_VA = 0x80003CD0;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 4;
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 lwz-r3-178-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5460273E;  /* rlwinm r0, r3, 4, 28, 31      */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                    */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7]           */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                    */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38A00000;  /* li r5, 0 (NULL placeholder)    */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x80A30178;  /* lwz r5, 0x178(r3)              */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 lwz-r3-178-guard: tramp at 0x%08x, site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 lwz-r3-178-guard: %d site(s) patched\n", hits);
}

/* v6O: NULL guard at 4 lwz r3, 0x178(r26) sites (0x805BCEAC, D024,
 * D19C, D314 — copy-pasted code blocks for different data categories).
 *
 * After v6N got us into the cutscene fade-in (screen dimmed!), r26 is
 * NULL at these post-loop accesses. Substitute r3=0; subsequent code
 * uses CR from earlier cmpwi r28 (still LT after v6N), so blt fires
 * and jumps over the bad code path.
 *
 * 4 separate trampolines packed at 0x80003D70-0x80003E00 (36 bytes ea). */
void swbf3_dantooine_lwz_r26_178_guard(u8 *addr, u32 len)
{
    /* SIG: bge -0xAC (loop end) | lwz r3, 0x178(r26) | subfic | addi
     * — distinctive enough to find all 4 copies. */
    static const u32 SIG[4] = {
        0x4080FF54,  /* bge -0xAC (loop back)                         */
        0x807A0178,  /* lwz r3, 0x178(r26)   <-- patch site           */
        0x34A3FFFF,  /* addic. r5, r3, -1                              */
        0x38050001,  /* addi r0, r5, 1                                 */
    };

    /* Per-site trampolines */
    const u32 TRAMP_BASES[4] = {
        0x80003D70, 0x80003D94, 0x80003DB8, 0x80003DDC,
    };

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            if (hits >= 4)
            {
                gprintf("SWBF3 lwz-r26-178-guard: more than 4 hits — slots exhausted\n");
                break;
            }
            const u32 TRAMPOLINE_VA = TRAMP_BASES[hits];
            u8 *patch_site = p + 4;  /* the lwz */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 8 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 lwz-r26-178-guard: disp out of range\n");
                return;
            }

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5740273E;  /* rlwinm r0, r26, 4, 28, 31     */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                    */
            tramp[2] = 0x41820014;  /* beq +0x14 → tramp[7]           */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                    */
            tramp[4] = 0x4182000C;  /* beq +0x0C → tramp[7]           */
            tramp[5] = 0x38600000;  /* li r3, 0                       */
            tramp[6] = 0x48000008;  /* b +0x08 → tramp[8] back        */
            tramp[7] = 0x807A0178;  /* lwz r3, 0x178(r26)             */
            tramp[8] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);

            DCFlushRange((void *)TRAMPOLINE_VA, 9 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 9 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 lwz-r26-178-guard: tramp at 0x%08x, site %p -> b +%d\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 lwz-r26-178-guard: %d site(s) patched\n", hits);
}

/* v6P: NULL vtable guard at 0x805D6A20 lwz r12, 0x14(r12).
 *
 * After v6O cleared the cutscene fade-in chain. New crash at vfn call:
 *   805d6a18  lwz r12, 0(r3)     ; r12 = *(r3) = vtable ptr (= NULL)
 *   805d6a1c  lwz r6, 8(r15)
 *   805d6a20  lwz r12, 0x14(r12) ; CRASH: r12=NULL, DAR=0x14
 *   805d6a24  lwz r4, 0(r15)
 *   805d6a28  mtctr r12
 *   805d6a2c  bctrl
 *
 * Substitute r12 with an embedded `blr` stub address. The subsequent
 * bctrl calls the stub, which immediately returns — LR is preserved
 * (caller's bl-return), r3 unchanged. Function continues as if the
 * vfn was a no-op returning input r3.
 *
 * Tramp at 0x80003FC8 — last available lowmem slot before section 4
 * end at 0x80004000. The stub blr lives at TRAMPOLINE_VA + 0x28 =
 * 0x80003FF0. */
void swbf3_dantooine_vtable_null_guard(u8 *addr, u32 len)
{
    static const u32 SIG[4] = {
        0x81830000,  /* lwz r12, 0(r3) (vtable load)                  */
        0x80CF0008,  /* lwz r6, 8(r15)                                 */
        0x818C0014,  /* lwz r12, 0x14(r12)  <-- patch site            */
        0x808F0000,  /* lwz r4, 0(r15)                                 */
    };

    const u32 TRAMPOLINE_VA = 0x80003FC8;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 8;  /* the lwz r12, 0x14(r12) */
            u32 back_va = (u32)patch_site + 4;
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 9 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 vtable-null-guard: disp out of range\n");
                return;
            }

            /* Compute stub address: tramp[10] at TRAMPOLINE_VA + 0x28.
             * lis r12, hi(stub) ; ori r12, r12, lo(stub) */
            u32 stub_va = TRAMPOLINE_VA + 10 * 4;
            u32 stub_hi = (stub_va + 0x8000) >> 16;  /* round for sign-extend */
            u32 stub_lo = stub_va & 0xFFFF;

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x5580273E;  /* rlwinm r0, r12, 4, 28, 31 (rS=12, rA=0) */
            tramp[1] = 0x2C000008;  /* cmpwi r0, 8                              */
            tramp[2] = 0x41820018;  /* beq +0x18 → tramp[8] do_load             */
            tramp[3] = 0x2C000009;  /* cmpwi r0, 9                              */
            tramp[4] = 0x41820010;  /* beq +0x10 → tramp[8]                     */
            tramp[5] = 0x3D800000 | (stub_hi & 0xFFFF);  /* lis r12, stub_hi    */
            tramp[6] = 0x618C0000 | (stub_lo & 0xFFFF);  /* ori r12, r12, stub_lo */
            tramp[7] = 0x48000008;  /* b +0x08 → tramp[9] done                  */
            tramp[8] = 0x818C0014;  /* lwz r12, 0x14(r12) (do_load)             */
            tramp[9] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);  /* b back  */
            tramp[10] = 0x4E800020; /* blr (stub: called via bctrl, returns)    */

            DCFlushRange((void *)TRAMPOLINE_VA, 11 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 11 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 vtable-null-guard: tramp at 0x%08x, site %p -> b +%d, stub at 0x%08x\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, stub_va);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 vtable-null-guard: %d site(s) patched\n", hits);
}

/* v6Q: inline `b +0x10` skip past NULL-vtable vfn block at 2 sites.
 *
 * After v6P guarded 0x805D6A20 (first vfn, offset 0x14), crashes
 * cascaded to 0x805D6A50 (second vfn, offset 0x18 — same NULL vtable).
 * 2 instances exist in the dol (0x805D6A44 and 0x805D6E3C).
 *
 * Each block:
 *   lwz r3, 0xC(r15)        ; r3 = struct ptr
 *   li r4, -1
 *   lwz r12, 0(r3)          ; r12 = vtable  <-- patch site (replace)
 *   lwz r12, 0x18(r12)      ; r12 = vfn[6]  (would crash)
 *   mtctr r12
 *   bctrl                    ; call vfn
 *                            ; <-- branch target (+0x10 from patch site)
 *
 * Replace `lwz r12, 0(r3)` with `b +0x10` (0x48000010). Skips the
 * entire 4-instruction vfn call when r3 is unreliable. */
void swbf3_dantooine_skip_vfn_block_fix(u8 *addr, u32 len)
{
    static const u32 SIG[6] = {
        0x806F000C,  /* lwz r3, 0xC(r15)                              */
        0x3880FFFF,  /* li r4, -1                                      */
        0x81830000,  /* lwz r12, 0(r3)   <-- patch site                */
        0x818C0018,  /* lwz r12, 0x18(r12)                             */
        0x7D8903A6,  /* mtctr r12                                       */
        0x4E800421,  /* bctrl                                           */
    };

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + 8;  /* the lwz r12, 0(r3) */
            *((u32 *)patch_site) = 0x48000010;  /* b +0x10 (skip 4 instr) */
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);
            gprintf("SWBF3 skip-vfn-block: site %p -> b +0x10 (skips NULL vtable vfn call)\n",
                    (void *)patch_site);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 skip-vfn-block: %d site(s) patched\n", hits);
}

/* v6R: NULL/garbage guard at 0x804219E0 lwz r0, 0x3C(r3).
 *
 * Far-from-cutscene crash. r3 = 0x3FD76E40 (looks like an IEEE float
 * value being misinterpreted as a pointer — cascade from our scratch
 * substitutions returning float-encoded "data" as object pointers).
 *
 * Code:
 *   804219e0  lwz r0, 0x3C(r3)         <-- patch site
 *   804219e4  mr r26, r3
 *   ...
 *   804219f0  rlwinm r0, r0, 0, 29, 29 ; isolate bit
 *   804219f4  beq +0x2C                 ; if bit clear, branch over
 *   804219f8  lwz r6, 0x50(r3)          ; would also crash
 *
 * Substitute r0=0 if r3 invalid → rlwinm gives 0 → beq fires → safe path. */
void swbf3_dantooine_lwz_r3_3C_guard(u8 *addr, u32 len)
{
    /* v6U — ROOT-CAUSE FIX. sub_804219CC is Havok hkAllocator::alloc(size).
     * It is called via sub_8036877C (and sub_801901D4) with `r3 = *(0x807526D0)`,
     * which is a GLOBAL POINTER to the allocator object instance (it lives at
     * 0x8075EBA0 in .data, vtable 0x8070A5C4 in .rodata).
     *
     * On Dolphin (unpatched, working): *(0x807526D0) = 0x8075EBA0 (correct).
     * On retail (broken):              *(0x807526D0) = 0x3FD76E40 (float garbage).
     *
     * The global is being clobbered by something (likely an NDEV-mirror write
     * landing in the wrong place; could also be one of our earlier scratch
     * substitutions). Symptom: when sub_804219CC tries to alloc 0x38 bytes for
     * the Dantooine cutscene, it dereferences 0x3FD76E40 as if it were the
     * allocator object → crash at lwz r0, 0x3C(r3) (DAR=0x3FD76E7C).
     *
     * Earlier attempts (all wrong direction — symptom-treating):
     *   v6R: r0=0 then fall through → cascades to lwz at 0x80420EF8
     *   v6S: li r3,0; blr at entry → caller polls forever (freeze)
     *   v6T: li r3,0; b epilogue   → caller can't get allocated memory (freeze)
     *
     * v6U: when r3 is positive (= garbage, since all valid pointers are negative
     * as signed int), SUBSTITUTE r3 with the known allocator address 0x8075EBA0
     * and continue. Function runs the real allocator, returns a real allocated
     * block, cutscene loader gets memory, cutscene plays.
     */
    /* SIG still matches the post-prologue body; patch site = match_addr - 0x14
     * (= function entry, the stwu r1, -0x30(r1) instruction). */
    static const u32 SIG[4] = {
        0x8003003C,  /* lwz r0, 0x3C(r3)   (matches at fn_entry + 0x14)  */
        0x7C7A1B78,  /* mr r26, r3                                       */
        0x7C9B2378,  /* mr r27, r4                                       */
        0x7CBC2B78,  /* mr r28, r5                                       */
    };

    /* Reuse same 24-byte slot at 0x80003F44 (v6R's old location). */
    const u32 TRAMPOLINE_VA  = 0x80003F44;
    const u32 PROLOGUE_OFFSET = 0x14;        /* SIG[0] is 0x14 past fn entry */
    /* Known address of the hkAllocator object (in .data, lives in BSS-init region).
     * Confirmed via Dolphin live trace: sub_804219CC called with r3 = 0x8075EBA0
     * during Dantooine cutscene playback. Vtable = 0x8070A5C4 ("ObAl" magic at +0xBC). */
    const u32 ALLOCATOR_VA   = 0x8075EBA0;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p - PROLOGUE_OFFSET;  /* fn entry: stwu r1, -0x30(r1) */
            u32 back_va = (u32)patch_site + 4;     /* mflr r0 (continue prologue) */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back5    = (s32)(back_va - (TRAMPOLINE_VA + 5 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 lwz-r3-3C-guard: disp out of range\n");
                return;
            }

            /* Tramp (24 bytes, 6 instr):
             *   [0] cmpwi  r3, 0          ; valid MEM1/MEM2 ptrs are NEGATIVE (top bit set)
             *   [1] blt    +0x0C → [4]    ; if negative, r3 OK — skip substitution
             *   [2] lis    r3, 0x8075     ; r3 = 0x80750000  (garbage path)
             *   [3] ori    r3, r3, 0xEBA0 ; r3 = 0x8075EBA0  (the real allocator)
             *   [4] stwu   r1, -0x30(r1)  ; original entry instr
             *   [5] b      back (0x804219D0 = mflr r0)
             */
            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x2C030000;                          /* cmpwi r3, 0           */
            tramp[1] = 0x4180000C;                          /* blt +0x0C → tramp[4]  */
            tramp[2] = 0x3C600000 | ((ALLOCATOR_VA >> 16) & 0xFFFF);  /* lis r3, 0x8075 */
            tramp[3] = 0x60630000 | (ALLOCATOR_VA & 0xFFFF);          /* ori r3, r3, 0xEBA0 */
            tramp[4] = 0x9421FFD0;                          /* stwu r1, -0x30(r1)    */
            tramp[5] = 0x48000000 | (((u32)disp_back5) & 0x03FFFFFC);  /* b back     */

            DCFlushRange((void *)TRAMPOLINE_VA, 6 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 6 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 lwz-r3-3C-guard(v6U): tramp at 0x%08x, fn-entry %p -> b +%d (substitute r3=0x%08x on garbage)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, ALLOCATOR_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 lwz-r3-3C-guard: %d site(s) patched\n", hits);
}

/* v6V — bail-on-r10=0 guard at sub_805D7D50's epilogue (0x805D9044).
 *
 * Context: sub_805D7D50 is the Dantooine cutscene loader function. Its
 * prologue (at 0x805D7D40) does `stwux r1, r1, r11` which sets stack[0] =
 * caller's SP, then saves FPRs at r12-X offsets. Its epilogue (at 0x805D9040)
 * does `lwz r10, 0(r1); psq_l f31, -8(r10); ...` to restore those FPRs.
 *
 * When the function is entered via a code path that BYPASSES the prologue
 * (Havok fiber-resume, longjmp, or a wrong-vfn-table jump from v6P/v6Q),
 * stack[0] is left at 0. Epilogue then loads r10=0 and `psq_l f31, -8(r10)`
 * accesses 0xFFFFFFF8 -> DSI on retail. Dolphin's lenient memory emulation
 * silently returns garbage, so this crash only manifests on retail.
 *
 * Confirmed via Dolphin live trace at 0x805D9044: r10=0 captured (matches
 * retail crash GPR10=0). The function reaches the epilogue with bogus state.
 *
 * v6V: patch 0x805D9044 to a tramp that checks r10. If r10=0, do `blr` —
 * return immediately using whatever LR/r1 currently hold (which were set by
 * the prologue-bypassing caller, so they're consistent with how we got here).
 * If r10!=0 (normal call), run the original psq_l and continue restore. */
void swbf3_dantooine_epilogue_r10_guard(u8 *addr, u32 len)
{
    /* SIG anchored on 3 words ending at the patch site. The 2-word version
     * (lwz r10,0(r1) + psq_l) matched 89 sites across the binary because
     * that pair is the standard EABI FPR-restore epilogue. Adding the
     * preceding `stw r0, 0x3080(r30)` (writing field 12416 = 0x3080 of the
     * cutscene-loader's r30 struct) makes it specific to sub_805D7D50.
     * Patch site = matched_addr + 8 (the psq_l). */
    static const u32 SIG[3] = {
        0x901E3080,  /* stw   r0, 0x3080(r30)   (unique to sub_805D7D50) */
        0x81410000,  /* lwz   r10, 0(r1)         (standard epilogue)     */
        0xE3EA0FF8,  /* psq_l f31, -8(r10), 0,0  (patch site at +8)      */
    };
    const u32 SIG_TO_PATCH_OFFSET = 8;  /* psq_l is the 3rd word */

    /* 20-byte slot at 0x80003F00 (verified free on retail). */
    const u32 TRAMPOLINE_VA = 0x80003F00;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p + SIG_TO_PATCH_OFFSET;   /* the psq_l (= 3rd word) */
            u32 back_va = (u32)patch_site + 4;   /* next instr after psq_l */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back3    = (s32)(back_va - (TRAMPOLINE_VA + 3 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 epilogue-r10-guard: disp out of range\n");
                return;
            }

            /* Tramp (20 bytes, 5 instr):
             *   [0] cmpwi  r10, 0          ; set CR0
             *   [1] beq    +0x0C → [4]     ; r10=0 → bail to blr
             *   [2] psq_l  f31, -8(r10)    ; original (valid r10 path)
             *   [3] b      back            ; continue normal FPR restore
             *   [4] blr                    ; bail: return with current LR
             */
            /* v7D: when bailing, ALSO restore r14-r31 from scratch (saved by
             * the entry-tramp at function start) so caller sees its callee-
             * saved registers intact. Tramp expanded from 5 to 8 instructions
             * (32 bytes, fits in 0x80003F00-0x80003F1F slot). Scratch buffer
             * at 0x80003800 (72 bytes for r14-r31, written by entry-tramp at
             * 0x80003260). */
            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x2C0A0000;  /* cmpwi r10, 0                          */
            tramp[1] = 0x4080000C;  /* bge +0x0C → tramp[4] (r10 >= 0 = bad) */
            tramp[2] = 0xE3EA0FF8;  /* psq_l f31, -8(r10) (only if r10 < 0)  */
            tramp[3] = 0x48000000 | (((u32)disp_back3) & 0x03FFFFFC);  /* b back */
            /* Bail path: restore r14-r31 from scratch, then blr.
             * v7F BUGFIX: was 0xBDCC0000 (stmw — WRONG: would store, not load).
             * lmw opcode is 46 (= 0xB8000000); stmw is 47 (= 0xBC000000). */
            tramp[4] = 0x3D808000;  /* lis r12, 0x8000                       */
            tramp[5] = 0x618C3800;  /* ori r12, r12, 0x3800 (scratch addr)   */
            tramp[6] = 0xB9CC0000;  /* lmw r14, 0(r12) — load r14..r31       */
            tramp[7] = 0x4E800020;  /* blr (with caller's r14-r31 restored)  */

            DCFlushRange((void *)TRAMPOLINE_VA, 8 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 8 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 epilogue-r10-guard(v7C): tramp at 0x%08x, site %p -> b +%d (blr on r10>=0 = any non-pointer)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 epilogue-r10-guard: %d site(s) patched\n", hits);
}

/* v7D: Entry-tramp for sub_805D7D50 that saves r14-r31 to a scratch buffer
 * at 0x80003800 BEFORE the function's prologue runs. The v6V bail at
 * 0x805D9044 reads from the same scratch and restores r14-r31 before blr.
 * This preserves EABI callee-saved registers across the bail, so caller's
 * r23 (and r14-r31 generally) stays intact and the r23=4 crash at the
 * caller's stb at 0x805D96D0 is prevented.
 *
 * SIG matches sub_805D7D50's unique 4-instruction prologue. Patch site is
 * the matched address (= function entry 0x805D7D40). */
void swbf3_dantooine_entry_save_callee_regs(u8 *addr, u32 len)
{
    /* SIG: the 4-instruction prologue of sub_805D7D50. Highly specific
     * (uses immediate -0x1700 = 0xE900 in subfic which is very unusual). */
    static const u32 SIG[4] = {
        0x542B073E,  /* rlwinm r11, r1, 0, 28, 31 */
        0x7C2C0B78,  /* mr r12, r1                 */
        0x216BE900,  /* subfic r11, r11, -0x1700   */
        0x7C21596E,  /* stwux r1, r1, r11          */
    };

    const u32 TRAMPOLINE_VA = 0x80003260;  /* free area after BAT tramp */
    const u32 SCRATCH_VA    = 0x80003800;  /* 72 bytes for r14-r31 */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            u8 *patch_site = p;  /* function entry (= first instr of prologue) */
            u32 back_va = (u32)patch_site + 4;  /* continue with original 2nd instr */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 6 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 entry-save-callee-regs: disp out of range\n");
                return;
            }

            /* Tramp (28 bytes, 7 instr):
             *   [0] lis  r12, 0x8000           ; r12 = 0x80000000
             *   [1] ori  r12, r12, 0x3800      ; r12 = 0x80003800 (scratch)
             *   [2] stmw r14, 0(r12)           ; store r14..r31 to scratch
             *   [3] rlwinm r11, r1, 0, 28, 31  ; original instr #1 (we replaced it)
             *   [4] (already runs back at patch_site+4 — wait we patched [0])
             * Actually we patched patch_site with `b tramp`. So orig instr at
             * patch_site is GONE. Tramp must execute that instr too.
             *   [0] lis  r12, 0x8000
             *   [1] ori  r12, r12, 0x3800
             *   [2] stmw r14, 0(r12)
             *   [3] rlwinm r11, r1, 0, 28, 31  ; original (the one we displaced)
             *   [4] mr r12, r1                  ; original prologue continues
             *   [5] subfic r11, r11, -0x1700    ; original
             *   [6] stwux r1, r1, r11           ; original
             *   [7] b back (to 0x805D7D50 = patch_site + 0x10)
             *
             * Wait — if patch_site is 0x805D7D40 (rlwinm), back_va should be
             * 0x805D7D50 (after all 4 prologue instructions we duplicated).
             * Recomputing disp_back below.
             */
            (void)disp_back;  /* recomputed below */
            u32 back_va_full = (u32)patch_site + 4 * 4;  /* skip all 4 prologue instrs */
            s32 disp_back7 = (s32)(back_va_full - (TRAMPOLINE_VA + 7 * 4));

            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x3D808000;  /* lis r12, 0x8000                  */
            tramp[1] = 0x618C3800;  /* ori r12, r12, 0x3800             */
            tramp[2] = 0xBDCC0000;  /* stmw r14, 0(r12) — save r14..r31 */
            tramp[3] = 0x542B073E;  /* rlwinm r11, r1, 0, 28, 31 (orig) */
            tramp[4] = 0x7C2C0B78;  /* mr r12, r1 (orig)                */
            tramp[5] = 0x216BE900;  /* subfic r11, r11, -0x1700 (orig)  */
            tramp[6] = 0x7C21596E;  /* stwux r1, r1, r11 (orig)         */
            tramp[7] = 0x48000000 | (((u32)disp_back7) & 0x03FFFFFC);  /* b back */

            DCFlushRange((void *)TRAMPOLINE_VA, 8 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 8 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 entry-save-callee-regs(v7D): tramp at 0x%08x, fn-entry %p -> b +%d (scratch at 0x%08x)\n",
                    TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, SCRATCH_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 entry-save-callee-regs: %d site(s) patched\n", hits);
}

/* v7E: wrap the bcctrl at 0x805D96C8 so r23 is preserved across whatever vfn
 * gets invoked. The vfn is loaded from a vtable-array indexed by a byte at
 * r23+1, so different calls invoke different functions — patching any single
 * called function (like v7D's sub_805D7D50 hook) misses other paths.
 *
 * SIG anchors on the 3-instruction sequence ending at the bcctrl:
 *   mtctr r12 ; bcctrl ; subf r0, r23, r3
 * Combined with the preceding lwz r12, 0x16B0(r6) this is highly unique. */
void swbf3_dantooine_callsite_r23_preserve(u8 *addr, u32 len)
{
    /* SIG: lwz r12, 0x16B0(r6); mtctr r12; bcctrl; subf r0, r23, r3 */
    static const u32 SIG[4] = {
        0x818616B0,  /* lwz r12, 0x16B0(r6)   */
        0x7D8903A6,  /* mtctr r12              */
        0x4E800421,  /* bcctrl                 */
        0x7C171850,  /* subf r0, r23, r3       */
    };
    const u32 SIG_TO_BCCTRL_OFFSET = 8;  /* bcctrl is SIG[2] = matched_addr + 8 */

    /* v7F: allocate per-site tramps so each site's `b back` returns to its
     * own next instruction. v7E shared one tramp; the `b back` only worked
     * for the last matched site. With ≥2 sites we need ≥2 tramps. */
    const u32 TRAMPOLINE_BASE   = 0x80003290;
    const u32 TRAMPOLINE_STRIDE = 32;  /* 8 instr per tramp */
    const u32 MAX_TRAMPS = 4;
    const u32 SCRATCH_VA = 0x80003880;  /* shared scratch (only one in flight at a time) */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            if ((u32)hits >= MAX_TRAMPS)
            {
                gprintf("SWBF3 callsite-r23-preserve: too many sites (>%d) — skipping\n", MAX_TRAMPS);
                continue;
            }
            u32 TRAMPOLINE_VA = TRAMPOLINE_BASE + hits * TRAMPOLINE_STRIDE;
            u8 *patch_site = p + SIG_TO_BCCTRL_OFFSET;  /* the bcctrl */
            u32 back_va = (u32)patch_site + 4;  /* after bcctrl = subf */
            s32 disp_to_tramp = (s32)((u32)TRAMPOLINE_VA - (u32)patch_site);
            s32 disp_back     = (s32)(back_va - (TRAMPOLINE_VA + 7 * 4));
            if (disp_to_tramp < -0x02000000 || disp_to_tramp >= 0x02000000)
            {
                gprintf("SWBF3 callsite-r23-preserve: disp out of range\n");
                return;
            }

            /* Tramp (32 bytes, 8 instr):
             *   [0] lis  r11, 0x8000          ; r11 = 0x80000000 (NOT r0!
             *                                    PowerPC RA=0 in load/store
             *                                    treats r0 as literal 0)
             *   [1] ori  r11, r11, 0x3880     ; r11 = SCRATCH_VA
             *   [2] stw  r23, 0(r11)           ; save caller's r23
             *   [3] bcctrl                      ; original — call the vfn
             *   [4] lis  r11, 0x8000
             *   [5] ori  r11, r11, 0x3880
             *   [6] lwz  r23, 0(r11)           ; restore caller's r23
             *   [7] b    back (per-site)       ; continue at this site's subf
             */
            volatile u32 *tramp = (volatile u32 *)TRAMPOLINE_VA;
            tramp[0] = 0x3D608000;  /* lis r11, 0x8000           */
            tramp[1] = 0x616B3880;  /* ori r11, r11, 0x3880      */
            tramp[2] = 0x92EB0000;  /* stw r23, 0(r11)           */
            tramp[3] = 0x4E800421;  /* bcctrl (original)          */
            tramp[4] = 0x3D608000;  /* lis r11, 0x8000           */
            tramp[5] = 0x616B3880;  /* ori r11, r11, 0x3880      */
            tramp[6] = 0x82EB0000;  /* lwz r23, 0(r11)           */
            tramp[7] = 0x48000000 | (((u32)disp_back) & 0x03FFFFFC);  /* b back */

            DCFlushRange((void *)TRAMPOLINE_VA, 8 * 4);
            ICInvalidateRange((void *)TRAMPOLINE_VA, 8 * 4);

            u32 b_to_tramp = 0x48000000 | (((u32)disp_to_tramp) & 0x03FFFFFC);
            *((u32 *)patch_site) = b_to_tramp;
            DCFlushRange(patch_site, 4);
            ICInvalidateRange(patch_site, 4);

            gprintf("SWBF3 callsite-r23-preserve(v7F): tramp #%d at 0x%08x, bcctrl %p -> b +%d (scratch 0x%08x)\n",
                    hits, TRAMPOLINE_VA, (void *)patch_site, disp_to_tramp, SCRATCH_VA);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 callsite-r23-preserve: %d site(s) patched\n", hits);
}

/* Fourth crash in the chain.  After v3b (NOP'd addi), Dantooine loaded
 * the mesh briefly then crashed before the cutscene:
 *
 *   ERROR 2 (DSI) SRR0 = 0x805d81c0  DAR = 0x00000004
 *   LR = 0x805d81b4  (== SRR0 - 12)
 *
 *   0x805d81b0  bctr (lk)             # vfn[7] call
 *   0x805d81b4  stw   r3, 112(r1)      # save vfn return
 *   0x805d81b8  li    r0, -1
 *   0x805d81bc  stw   r23, 116(r1)
 *   0x805d81c0  stw   r0, 4(r31)       <-- DSI: r31 = NULL, writing to 0x4
 *   0x805d81c4  stw   r23, 8(r31)      # would also crash if executed
 *
 * Same fn 0x805d7d50 as before, different code path -- entered during
 * cutscene init.  r31 is callee-saved (preserved by EABI helper at
 * 0x805d7d8c) and INHERITED FROM THE CALLER.  When this code path is
 * exercised after our earlier patches let the function progress further,
 * r31 was never initialized to a valid value -- it's still whatever the
 * recursive parent invocation had, which can be NULL on the cutscene
 * setup path.
 *
 * Patch (2 NOPs in place): NOP both stws to r31.  Skips the "init node
 * fields with -1 sentinel and r23 handle" pattern entirely.  When r31
 * IS valid (normal load), we lose the writes -- the cutscene's pre-roll
 * node may render with default-zero data instead of -1/r23, which may
 * be visually wrong but won't crash.  When r31 is NULL (our case), we
 * skip cleanly past the deref.
 *
 * Better fix (TODO if NOP isn't enough): emit a trampoline elsewhere
 * that does `cmpwi r31, 0; beqlr; stws; blr`, then patch 0x805d81c0 to
 * `bl trampoline; nop`.  Requires reserving DOL scratch space.
 *
 * 6-word signature: bctr + 4 surrounding stws + the 2 r31-based stws.
 * Unique anchor on the bctr-after-vfn[7]-call + (-1, r23) sentinel set. */
void swbf3_havok_node_init_null_skip(u8 *addr, u32 len)
{
    static const u32 SIG[6] = {
        0x4E800421,  /* bctr (lk)               */
        0x90610070,  /* stw   r3, 112(r1)        */
        0x3800FFFF,  /* li    r0, -1             */
        0x92E10074,  /* stw   r23, 116(r1)       */
        0x901F0004,  /* stw   r0, 4(r31)   <- NOP */
        0x92FF0008,  /* stw   r23, 8(r31)  <- NOP */
    };
    static const u32 NOP = 0x60000000;

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            *((u32 *)(p + 16)) = NOP;  /* stw r0, 4(r31)  -> nop */
            *((u32 *)(p + 20)) = NOP;  /* stw r23, 8(r31) -> nop */
            gprintf("SWBF3 havok-node-init-null-skip: patched at %p (NOPed stw r0,4(r31) + stw r23,8(r31))\n",
                    (void *)p);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 havok-node-init-null-skip: %d site(s) patched\n", hits);
}

/* Fifth crash in the chain.  After v4 NOPed two stws to r31, fn 0x805d7d50
 * advanced further but crashed at the very next r31 dereference:
 *
 *   ERROR 2 (DSI) SRR0 = 0x805d820c  DAR = 0x00000001
 *   0x805d820c  stb   r0, 1(r31)   <-- stores byte to (NULL + 1)
 *
 * Inspection shows the surrounding block (0x805d8198..0x805d8318) has
 * 10+ r31 dereferences -- stbs, stws, stfds, vfn-args using r31, and
 * the loop-continuation dispatcher at 0x805d831C does `lbz r0, 0(r31)`
 * for jump-table indexing.  NOPing each crash one at a time is whack-
 * a-mole; the cleanest in-place fix is to bypass the entire r31-using
 * block when r31 may be NULL on the cutscene-init path.
 *
 * Patch (2 words):
 *
 *   1) 0x805d8194  `beq +0x168` (-> 0x805d82FC, "safe init" path)
 *                   ->  `b +0x184`   (-> 0x805d8318, jump-table addi)
 *      Forces unconditional jump past ALL r31 dereferences, directly
 *      to the per-iteration counter advance at 0x805d8318
 *      (addi r29, r29, 4).
 *
 *   2) 0x805d831C  `lbz r0, 0(r31)` (load type byte for dispatcher)
 *                   ->  `li r0, 100` (force r0 > 6)
 *      The existing `cmplwi r0, 6; bgt +2000` at 0x805d8320/0x805d8324
 *      then fires the bgt unconditionally, exiting the jump-table
 *      dispatcher to the function's default continuation at
 *      0x805d8AF4 (where r31 is no longer dereferenced in the immediate
 *      neighborhood).
 *
 * Effect: this iteration of fn 0x805d7d50's processing loop runs as
 * a no-op for ANY r29 item (not just NULL-r31 cases).  The function's
 * normal work for these items is skipped -- cutscene visuals tied to
 * this code path may render with default state.  Loop still terminates
 * via the r29 advance.
 *
 * Trade-off: this is a destructive patch for cases where r31 IS valid
 * (we lose the legitimate processing).  Acceptable here because:
 *   - Without the patch, NULL r31 = guaranteed DSI = no boot at all.
 *   - With the patch, visually-degraded cutscene = better than crash.
 *
 * Signatures anchor on highly-distinctive surrounding instructions:
 *   site 1: rlwinm.0,0,1,31,31 + beq +0x168 + lwz r12,0(r28)
 *   site 2: addi r29,r29,4 + lbz r0,0(r31) + cmplwi r0,6 + bgt +0x7D0
 */
void swbf3_havok_r31_block_bypass_fix(u8 *addr, u32 len)
{
    /* Site 1: force-skip the r31-using block */
    static const u32 SIG1[3] = {
        0x54000FFF,  /* rlwinm r0, r0, 1, 31, 31              */
        0x41820168,  /* beq    +0x168    <- patch target #1    */
        0x819C0000,  /* lwz    r12, 0(r28)                    */
    };
    static const u32 PATCH1 = 0x48000184;  /* b +0x184 (to addi r29,r29,4) */

    /* Site 2: replace the dispatcher's lbz with li r0, 100 to force the
     * existing bgt +0x7D0 to exit-via-default */
    static const u32 SIG2[4] = {
        0x3BBD0004,  /* addi   r29, r29, 4                    */
        0x881F0000,  /* lbz    r0, 0(r31)    <- patch target #2 */
        0x28000006,  /* cmplwi r0, 6                          */
        0x418107D0,  /* bgt    +0x7D0 (-> default exit)        */
    };
    static const u32 PATCH2 = 0x38000064;  /* li r0, 100 (>6 to trigger bgt) */

    int hits1 = 0, hits2 = 0;

    if (len >= sizeof(SIG1))
    {
        u8 *end = addr + len - sizeof(SIG1);
        for (u8 *p = addr; p <= end; p += 4)
        {
            if (memcmp(p, SIG1, sizeof(SIG1)) == 0)
            {
                *((u32 *)(p + 4)) = PATCH1;
                gprintf("SWBF3 havok-r31-block-bypass-fix: site1 (skip-block) at %p\n", (void *)p);
                hits1++;
            }
        }
    }
    if (len >= sizeof(SIG2))
    {
        u8 *end = addr + len - sizeof(SIG2);
        for (u8 *p = addr; p <= end; p += 4)
        {
            if (memcmp(p, SIG2, sizeof(SIG2)) == 0)
            {
                *((u32 *)(p + 4)) = PATCH2;
                gprintf("SWBF3 havok-r31-block-bypass-fix: site2 (dispatcher-li) at %p\n", (void *)p);
                hits2++;
            }
        }
    }

    if (hits1 > 0 || hits2 > 0)
        gprintf("SWBF3 havok-r31-block-bypass-fix: site1=%d, site2=%d patched\n", hits1, hits2);
}

/* Sixth crash in the Havok OOM chain.  After v5 bypassed the r31 block,
 * Dantooine cutscene init crashed at:
 *
 *   ERROR 2 (DSI) SRR0 = 0x8053b66c  DAR = 0x00000001
 *   LR  = 0x805d0358
 *   DSISR = 0x04000000  (page protection violation)
 *   stack has corrupted LR=0x50 in one frame
 *
 *   0x8053b658  cmpwi r8, 0           # check this->field_24
 *   0x8053b65c  bne   +36             # if non-zero: alt path
 *   0x8053b660  lwz   r3, 4(r3)        # r3 = this->field_4 (= 1, garbage!)
 *   0x8053b664  or    r4, r31, r31
 *   0x8053b668  or    r5, r27, r27
 *   0x8053b66c  lwz   r12, 0(r3)      <-- DSI: read from address 1
 *
 * Inside fn 0x8053b634 -- a smaller single-entry slab refill that
 * mirrors fn 0x8053b3D8 but takes a different path.  Object pointed at
 * by `this` (r3) was partially initialized by our earlier patches:
 * field_4 was supposed to hold a vtable-like pointer but contains 1
 * (a small int sentinel left over from incomplete init).
 *
 * Patch (1 word): force the bne at 0x8053b65c to unconditional branch
 * (b +36).  Skips both the `lwz r3, 4(r3)` reload and the immediate
 * vtable deref, jumping directly to the alt path at 0x8053b680, which
 * uses r3 in `add r29, r3, r28` etc. -- safe because r3 at function
 * entry IS a valid this-pointer; only the reload at 0x8053b660 turned
 * it into garbage.
 *
 * Trade-off: when field_24 == 0 originally (the "load field_4" path),
 * we now take the field_24 != 0 alt path instead.  The two paths
 * compute slightly different things, so this is destructive when the
 * original logic was correct.  Acceptable: the crashing path is more
 * common after our other patches, and the alt path uses valid r3.
 *
 * Signature anchored on the lwz/cmpwi/bne+36 + the lwz r3,4(r3) +
 * or r4,r31,r31 + or r5,r27,r27 sequence -- unique to this site. */
void swbf3_havok_field4_force_alt_fix(u8 *addr, u32 len)
{
    static const u32 SIG[7] = {
        0x81030024,  /* lwz   r8, 36(r3)             */
        0x836700B0,  /* lwz   r27, 176(r7)           */
        0x2C080000,  /* cmpwi r8, 0                  */
        0x40820024,  /* bne   +36              <- patch target */
        0x80630004,  /* lwz   r3, 4(r3)              */
        0x7FE4FB78,  /* or    r4, r31, r31           */
        0x7F65DB78,  /* or    r5, r27, r27           */
    };
    static const u32 PATCH = 0x48000024;  /* b +36 (unconditional take alt path) */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            *((u32 *)(p + 12)) = PATCH;  /* patch the bne at offset +12 */
            gprintf("SWBF3 havok-field4-force-alt-fix: patched at %p\n", (void *)p);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 havok-field4-force-alt-fix: %d site(s) patched\n", hits);
}

/* Seventh crash.  After v6's bne->b force-alt, the alt path at
 * 0x8053b680 ran further but hit ANOTHER `lwz r3, 4(r30); lwz r12,
 * 0(r3)` vfn dispatch with the same garbage field_4 = 1:
 *
 *   ERROR 2 (DSI) SRR0 = 0x8053b708  DAR = 0x00000001
 *
 *   0x8053b6f8  lwz   r3, 4(r30)        # r3 = r30->field_4 (=1, garbage)
 *   0x8053b6fc  or    r6, r27, r27
 *   0x8053b700  subf  r26, r5, r26
 *   0x8053b704  addi  r4, r1, 8
 *   0x8053b708  lwz   r12, 0(r3)         <-- DSI: read from addr 1
 *   0x8053b70c  lwz   r12, 16(r12)
 *   0x8053b710  mtctr r12
 *   0x8053b714  bctr (lk)                # vfn[4]
 *
 * Same garbage-field_4 pattern as v6 but in a different code path
 * (alt-path tail, not main-path head).
 *
 * Patch (1 word): replace the `lwz r3, 4(r30)` with `b +0x20`, jumping
 * past the entire vfn dispatch + bctr, landing on the `cmpw r26, r25`
 * at 0x8053b718.  Post-bctr code uses r30 (still valid), not the
 * vfn-return r3, so skipping is safe.
 *
 * Signature is 6 words anchored on `addi r6,r6,4; bdnz +-52; lwz r3,
 * 4(r30); or r6,r27,r27; subf r26,r5,r26; addi r4,r1,8` -- the
 * memzero-then-vfn-call sequence specific to this site. */
void swbf3_havok_field4_vfn_skip_fix(u8 *addr, u32 len)
{
    static const u32 SIG[6] = {
        0x38C60004,  /* addi  r6, r6, 4                  */
        0x4200FFCC,  /* bc    16,0, +-52  (bdnz)         */
        0x807E0004,  /* lwz   r3, 4(r30)  <- patch target */
        0x7F66DB78,  /* or    r6, r27, r27               */
        0x7F45D050,  /* subf  r26, r5, r26               */
        0x38810008,  /* addi  r4, r1, 8                  */
    };
    static const u32 PATCH = 0x48000020;  /* b +0x20 (skip vfn dispatch) */

    if (len < sizeof(SIG)) return;
    u8 *end = addr + len - sizeof(SIG);
    int hits = 0;
    for (u8 *p = addr; p <= end; p += 4)
    {
        if (memcmp(p, SIG, sizeof(SIG)) == 0)
        {
            *((u32 *)(p + 8)) = PATCH;
            gprintf("SWBF3 havok-field4-vfn-skip-fix: patched at %p\n", (void *)p);
            hits++;
        }
    }
    if (hits > 0)
        gprintf("SWBF3 havok-field4-vfn-skip-fix: %d site(s) patched\n", hits);
}

/* Dev-disc support: SWBF3 r2.91120a vec3-copy destination guard.
 * Flips two `beq +16` guards in the campaign loop body to `bge +16` so
 * signed-non-negative addresses (NULL, small-int garbage like 0xC) skip
 * the vec3_copy call while real Wii pointers (0x8xxxxxxx, negative
 * under signed compare) still fall through. */
void swbf3_campaign_vec3_guard_fix(u8 *addr, u32 len)
{
    static const u32 GuardA[4] = {
        0x2C1E0000, 0x41820010, 0x7FC3F378, 0x7EC4B378,
    };
    static const u32 GuardB[5] = {
        0x2C1F0000, 0x41820010, 0x7FE3FB78, 0x3896000C, 0x4BC6D3CD,
    };
    if (len < sizeof(GuardB)) return;
    u8 *p;
    u8 *end_a = addr + len - sizeof(GuardA);
    u8 *end_b = addr + len - sizeof(GuardB);
    for (p = addr; p <= end_a; p += 4)
    {
        if (memcmp(p, GuardA, sizeof(GuardA)) == 0)
        {
            *((u32 *)(p + 4)) = 0x40800010;
            gprintf("SWBF3 vec3 guard A hardened at %p\n", (void *)p);
            break;
        }
    }
    for (p = addr; p <= end_b; p += 4)
    {
        if (memcmp(p, GuardB, sizeof(GuardB)) == 0)
        {
            *((u32 *)(p + 4)) = 0x40800010;
            gprintf("SWBF3 vec3 guard B hardened at %p\n", (void *)p);
            break;
        }
    }
}

u8 *find_safe_space(u8 *addr, u32 len)
{
    u8 SearchPatternA[] = {0x80, 0x1E, 0x00, 0x00, 0x3C, 0x60, 0x80, 0x00, 0x83}; // Most games
    u8 SearchPatternB[] = {0x80, 0x1F, 0x00, 0x00, 0x3C, 0x60, 0x80, 0x00, 0x83}; // Mortal Kombat
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SearchPatternA);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SearchPatternA, sizeof(SearchPatternA)) == 0)
        {
            if (*(u32*)(addr_start + 36) == 0x38000001)
            {
                gprintf("Found safe space (A)\n");
                return addr_start + 36;
            }
        }
        else if (memcmp(addr_start, SearchPatternB, sizeof(SearchPatternB)) == 0)
        {
            if (*(u32*)(addr_start + 36) == 0x38000001)
            {
                gprintf("Found safe space (B)\n");
                return addr_start + 36;
            }
        }
        addr_start += 4;
    }
    return NULL;
}

void patch_width(u8 *addr, u32 len)
{
    u8 SearchPattern[32] = {
        0x40, 0x82, 0x00, 0x08, 0x48, 0x00, 0x00, 0x1C,
        0x28, 0x09, 0x00, 0x03, 0x40, 0x82, 0x00, 0x08,
        0x48, 0x00, 0x00, 0x10, 0x2C, 0x03, 0x00, 0x00,
        0x40, 0x82, 0x00, 0x08, 0x54, 0xA5, 0x0C, 0x3C};
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SearchPattern);
    u8 *patch = find_safe_space(addr, len);

    if (patch)
        patch += 12; // Puts us at the first crclr
    else
        return;

    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            if (addr_start[-0x70] == 0xA0 && addr_start[-0x6E] == 0x00 && addr_start[-0x6D] == 0x0A)
            {
                if (addr_start[-0x44] == 0xA0 && addr_start[-0x42] == 0x00 && addr_start[-0x41] == 0x0E)
                {
                    u8 reg_a = (addr_start[-0x6F] >> 5);
                    u8 reg_b = (addr_start[-0x43] >> 5);

                    // Patch to the framebuffer resolution
                    addr_start[-0x41] = 0x04;

                    // Center the image
                    void *offset = addr_start - 0x70;
    
                    u32 org_address = (addr_start[-0x70] << 24) | (addr_start[-0x6F] << 16);
                    *(u32 *)(patch + 0x00) = org_address | 4;
                    *(u32 *)(patch + 0x04) = 0x200002D0 | (reg_b << 21) | (reg_a << 16);
                    *(u32 *)(patch + 0x08) = 0x38000002 | (reg_a << 21);
                    *(u32 *)(patch + 0x0C) = 0x7C000396 | (reg_a << 21) | (reg_b << 16) | (reg_a << 11);

                    *(u32 *)offset = 0x48000000 + (((u32)patch - (u32)offset) & 0x3ffffff);
                    *(u32 *)(patch + 0x10) = 0x48000000 + ((((u32)offset + 0x04) - ((u32)patch + 16)) & 0x3ffffff);
                    gprintf("Patched resolution. Branched from 0x%x to 0x%x\n", offset, patch);
                    //hexdump((void *)patch - 32, 92);
                    return;
                }
            }
        }
        addr_start += 4;
    }
}

/** Patch GXSetCopyFilter to disable the deflicker filter **/
void deflicker_patch(u8 *addr, u32 len)
{
    u32 SearchPattern[18] = {
        0x3D20CC01, 0x39400061, 0x99498000,
        0x2C050000, 0x38800053, 0x39600000,
        0x90098000, 0x38000054, 0x39800000,
        0x508BC00E, 0x99498000, 0x500CC00E,
        0x90698000, 0x99498000, 0x90E98000,
        0x99498000, 0x91098000, 0x41820040};
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SearchPattern);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            *((u32 *)addr_start + 17) = 0x48000040; // Change beq to b
            gprintf("Patched GXSetCopyFilter @ %p\n", addr_start);
            return;
        }
        addr_start += 4;
    }
}


/** Patch GXSetDither to disable dithering **/
/*
// Not offered because it causes banding and posterization
void dithering_patch(u8 *addr, u32 len)
{
    u32 SearchPattern[10] = {
        0x3C80CC01, 0x38A00061,
        0x38000000, 0x80C70220,
        0x5066177A, 0x98A48000,
        0x90C48000, 0x90C70220,
        0xB0070002, 0x4E800020};
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SearchPattern);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SearchPattern, sizeof(SearchPattern)) == 0)
        {
            *((u32 *)addr_start - 1) = 0x48000028;
            gprintf("Patched GXSetDither @ %p\n", addr_start);
            return;
        }
        addr_start += 4;
    }
}
*/

/** Patch WPADControlSpeaker **/
void speaker_patch(u8 *addr, u32 len)
{
    u32 SpeakerPattern[4] = {0x9421FA00, 0x7C0802A6, 0x90010604, 0x39610600};

    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(SpeakerPattern);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, SpeakerPattern, sizeof(SpeakerPattern)) == 0)
        {
            *((u32 *)addr_start) = 0x4E800020;
            gprintf("Patched speaker @ %p\n", addr_start);
            //hexdump(addr_start, 20);
            return;
        }
        addr_start += 4;
    }
}

/** Patch WPADControlMotor **/
void motor_patch(u8 *addr, u32 len)
{
    u32 MotorPatternA[2] = {0x9421FFF0, 0x7C0802A6};
    u32 MotorPatternB[4] = {0x2C000000, 0x40820020, 0x2C1E0000, 0x40820010};
    u32 MotorPatternC[5] = {0x48000020, 0x7C9E00D0, 0x38000001, 0x7C84F378, 0x54840FFE};
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - sizeof(MotorPatternA) - sizeof(MotorPatternB) - sizeof(MotorPatternC);
    while (addr_start <= addr_end)
    {
        if (memcmp(addr_start, MotorPatternA, sizeof(MotorPatternA)) == 0)
        {
            if (memcmp(addr_start + 68, MotorPatternB, sizeof(MotorPatternB)) == 0)
            {
                if (memcmp(addr_start + 148, MotorPatternC, sizeof(MotorPatternC)) == 0)
                {
                    *(u32 *)addr_start = 0x4E800020;
                    gprintf("Patched motor @ %p\n", addr_start);
                    //hexdump(addr_start, 20);
                    return;
                }
            }
        }
        addr_start += 4;
    }
}

/**
    480p Pixel Fix Patch by leseratte
    Fix for a Nintendo Revolution SDK bug found by Extrems affecting early Wii console when using 480p video mode.
    https://shmups.system11.org/viewtopic.php?p=1361158#p1361158
    https://github.com/ExtremsCorner/libogc-rice/commit/941d687e271fada68c359bbed98bed1fbb454448
**/
void PatchFix480p()
{
    u8 prefix[2] = {0x4b, 0xff};

    ///         Patch offset: ----------VVVVVVVV
    u32 Pattern_MKW[8] = {0x38000065, 0x9b810019, 0x38810018, 0x386000e0, 0x98010018, 0x38a00002};
    u32 patches_MKW[2] = {0x38600003, 0x98610019};
    /// Used by: MKWii, Wii Play, Need for Speed Nitro, Wii Sports, ...

    ///         Patch offset: -----------------------------------------------VVVVVVVV
    u32 Pattern_NSMB[8] = {0x38000065, 0x9801001c, 0x3881001c, 0x386000e0, 0x9b81001d, 0x38a00002};
    u32 patches_NSMB[2] = {0x38a00003, 0x98a1001d};
    /// Used by: New Super Mario Bros, ...

    /*
     * Code block that is being patched (in MKW):
     *
     * 4bffe30d: bl WaitMicroTime
     * 38000065: li r0, 0x65
     * 9b810019: stb r28, 25(r1) // store the wrong value (1)
     * 38810018: addi r4, r1, 0x18
     * 386000e0: li r3, 0xe0
     * 98010018: stb r0, 24(r1)
     * 38a00002: li r5, 2
     * 4bffe73d: bl __VISendI2CData
     *
     * r28 is a register that is set to 1 at the beginning of the function.
     * However, its contents are used elsewhere as well, so we can't just modify this one function.
     *
     * The following code first searches for one of the patterns above, then replaces the
     * "stb r28, 25(r1)" instruction that stores the wrong value on the stack with a branch instead
     * That branch branches to the injected custom code ("li r3, 3; stb r3, 25(r1)") that stores the
     * correct value (3) instead. At the end of the injected code will be another branch that branches
     * back to the instruction after the one that has been replaced (so, to "addi r4, r1, 0x18").
     * r3 can safely be used as a temporary register because its contents will be replaced immediately
     * afterwards anyways.
     *
     */

    void *offset = NULL;
    void *addr = (void *)0x80000000;
    u32 len = 0x900000;

    void *patch_ptr = 0;
    void *a = addr;

    while ((char *)a < ((char *)addr + len))
    {
        if (memcmp(a, &Pattern_MKW, 6 * 4) == 0)
        {
            // Found pattern?
            if (memcmp(a - 4, &prefix, 2) == 0)
            {
                if (memcmp(a + 8 * 4, &prefix, 2) == 0)
                {
                    offset = a + 4;
                    //hexdump(a, 30);
                    patch_ptr = &patches_MKW;
                    break;
                }
            }
        }
        else if (memcmp(a, &Pattern_NSMB, 6 * 4) == 0)
        {
            // Found pattern?
            if (memcmp(a - 4, &prefix, 2) == 0)
            {
                if (memcmp(a + 8 * 4, &prefix, 2) == 0)
                {
                    offset = a + 16;
                    //hexdump(a, 30);
                    patch_ptr = &patches_NSMB;
                    break;
                }
            }
        }
        a += 4;
    }

    if (offset == 0)
    {
        // offset is still 0, we didn't find the pattern, return
        gprintf("Didn't find offset for 480p patch!\n");
        return;
    }

    u8 *patch = find_safe_space(addr, len);
    if (patch)
        patch += 32; // Puts us at "This TV format"
    else
        return;

    memcpy((void *)patch, patch_ptr, 8);

    *(u32 *)offset = 0x48000000 + (((u32)patch - (u32)offset) & 0x3ffffff);
    *(u32 *)(patch + 8) = 0x48000000 + ((((u32)offset + 4) - ((u32)patch + 8)) & 0x3ffffff);
    gprintf("Applied 480p patch. Branched from 0x%x to 0x%x\n", offset, patch);
    //hexdump((void *)patch - 32, 92);
    return;
}

/** Patch URLs for private Servers - Thanks to ToadKing/wiilauncher-nossl **/
void PrivateServerPatcher(void *addr, u32 len, u8 privateServer, const char *serverAddr)
{

    // Patch protocol https -> http
    char *cur = (char *)addr;
    const char *end = cur + len - 8;
    do
    {
        if (memcmp(cur, "https://", 8) == 0 && cur[8] != 0)
        {
            int len = strlen(cur);
            memmove(cur + 4, cur + 5, len - 5);
            cur[len - 1] = 0;
            cur += len;
        }
    } while (++cur < end);
    // Patch nintendowifi.net -> private server domain
    if (privateServer == PRIVSERV_WIIMMFI)
        domainpatcher(addr, len, "wiimmfi.de");
    else if (privateServer == PRIVSERV_ALTWFC)
        domainpatcher(addr, len, "zwei.moe");
    else if (privateServer == PRIVSERV_CUSTOM && strlen(serverAddr) > 3)
        domainpatcher(addr, len, serverAddr);
}

static inline int GetOpcode(unsigned int *instructionAddr)
{
    return ((*instructionAddr >> 26) & 0x3f);
}

static inline int GetImmediateDataVal(unsigned int *instructionAddr)
{
    return (*instructionAddr & 0xffff);
}

static inline int GetLoadTargetReg(unsigned int *instructionAddr)
{
    return (int)((*instructionAddr >> 21) & 0x1f);
}

static inline int GetComparisonTargetReg(unsigned int *instructionAddr)
{
    return (int)((*instructionAddr >> 16) & 0x1f);
}

s8 do_new_wiimmfi_nonMKWii(void *addr, u32 len)
{
    // As of February 2021, Wiimmfi requires a special Wiimmfi patcher
    // update which does a bit more than just patch the server addresses.
    // This function is being called by apploader.c, right before
    // jumping to the entry point (only for non-MKWii games on Wiimmfi),
    // and applies all the necessary security fixes to the game.

    // This function has been implemented by Leseratte. Please don't
    // try to modify it without speaking to the Wiimmfi team because
    // doing so could have unintended side effects.

    // This function MUST not run for MKWii, that would break stuff.

    int hasGT2Error = 0;
    char gt2locator[] = {0x38, 0x61, 0x00, 0x08, 0x38, 0xA0, 0x00, 0x14};
    // Opcode list for p2p:
    unsigned char opCodeChainP2P_v1[22] = {32, 32, 21, 21, 21, 21, 20, 20, 31, 40, 21, 20, 20, 31, 31, 10, 20, 36, 21, 44, 36, 16};
    unsigned char opCodeChainP2P_v2[22] = {32, 32, 21, 21, 20, 21, 20, 21, 31, 40, 21, 20, 20, 31, 31, 10, 20, 36, 21, 44, 36, 16};

    // Opcode list for MASTER:
    unsigned char opCodeChainMASTER_v1[22] = {21, 21, 21, 21, 40, 20, 20, 20, 20, 31, 31, 14, 31, 20, 21, 44, 21, 36, 36, 18, 11, 16};
    unsigned char opCodeChainMASTER_v2[22] = {21, 21, 21, 21, 40, 20, 20, 20, 20, 31, 31, 14, 31, 20, 21, 36, 21, 44, 36, 18, 11, 16};

    int MASTERopcodeChainOffset = 0;

    char *cur = addr;
    const char *end = addr + len;

    // Check if the game needs the new patch.
    do
    {
        if (memcmp(cur, "<GT2> RECV-0x%02x <- [--------:-----] [pid=%u]", 0x2e) == 0)
        {
            hasGT2Error++;
        }
    } while (++cur < end);

    cur = addr;

    if (hasGT2Error > 1)
        return 1; // error, this either doesn't exist, or exists once. Can't exist multiple times.

    int successful_patch_p2p = 0;
    int successful_patch_master = 0;

    do
    {
        // Patch the User-Agent so Wiimmfi knows this game has been patched.
        // This also identifies patcher (G=USB-Loader GX) and patch version (=3), please
        // do not change this without talking to Leseratte first.
        if (memcmp(cur, "User-Agent\x00\x00RVL SDK/", 20) == 0)
        {
            if (hasGT2Error)
                memcpy(cur + 12, "G-3-1\x00", 6);
            else
                memcpy(cur + 12, "G-3-0\x00", 6);
        }

        if (hasGT2Error)
        {
            if (memcmp(cur, &gt2locator, 8) == 0)
            {
                int found_opcode_chain_P2P_v1 = 1;
                int found_opcode_chain_P2P_v2 = 1;

                for (int i = 0; i < 22; i++)
                {
                    int offset = (i * 4) + 12;
                    if (opCodeChainP2P_v1[i] != (unsigned char)(GetOpcode((unsigned int *)(cur + offset))))
                        found_opcode_chain_P2P_v1 = 0;

                    if (opCodeChainP2P_v2[i] != (unsigned char)(GetOpcode((unsigned int *)(cur + offset))))
                        found_opcode_chain_P2P_v2 = 0;
                }
                int found_opcode_chain_MASTER;
                for (int dynamic = 0; dynamic < 40; dynamic += 4)
                {
                    found_opcode_chain_MASTER = 1;
                    int offset = 0;
                    for (int i = 0; i < 22; i++)
                    {
                        offset = (i * 4) + 12 + dynamic;
                        if (
                            (opCodeChainMASTER_v1[i] != (unsigned char)(GetOpcode((unsigned int *)(cur + offset)))) &&
                            (opCodeChainMASTER_v2[i] != (unsigned char)(GetOpcode((unsigned int *)(cur + offset))))
                        )
                        {
                            found_opcode_chain_MASTER = 0;
                        }
                    }

                    if (found_opcode_chain_MASTER)
                    {
                        //gprintf("found master opcode chain\n");
                        // We found the opcode chain, let's take a note of the offset.
                        MASTERopcodeChainOffset = (int)(cur + 12 + dynamic);
                        break;
                    }
                }
                if (found_opcode_chain_P2P_v1 || found_opcode_chain_P2P_v2)
                {
                    // Match found
                    // Now compare all the immediate values:
                    if (
                        GetImmediateDataVal((unsigned int *)(cur + 0x0c)) == 0x0c &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x10)) == 0x18 &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x30)) == 0x12 &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x48)) == 0x5a &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x50)) == 0x0c &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x58)) == 0x12 &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x5c)) == 0x18 &&
                        GetImmediateDataVal((unsigned int *)(cur + 0x60)) == 0x18
                    )
                    {
                        //gprintf("Patching P2P...\n");
                        int loadedDataReg = GetLoadTargetReg((unsigned int *)(cur + 0x14));
                        int comparisonDataReg = GetComparisonTargetReg((unsigned int *)(cur + 0x48));

                        if (found_opcode_chain_P2P_v1)
                        {
                            *(int *)(cur + 0x14) = (0x88010011 | (comparisonDataReg << 21));
                            *(int *)(cur + 0x18) = (0x28000080 | (comparisonDataReg << 16));
                            *(int *)(cur + 0x24) = 0x41810064;
                            *(int *)(cur + 0x28) = 0x60000000;
                            *(int *)(cur + 0x2c) = 0x60000000;
                            *(int *)(cur + 0x34) = (0x3C005A00 | (comparisonDataReg << 21));
                            *(int *)(cur + 0x48) = (0x7C000000 | (comparisonDataReg << 16) | (loadedDataReg << 11));
                            successful_patch_p2p++;
                        }
                        if (found_opcode_chain_P2P_v2)
                        {
                            loadedDataReg = 12;

                            *(int *)(cur + 0x14) = (0x88010011 | (comparisonDataReg << 21));
                            *(int *)(cur + 0x18) = (0x28000080 | (comparisonDataReg << 16));
                            *(int *)(cur + 0x1c) = 0x41810070;
                            *(int *)(cur + 0x24) = *(int *)(cur + 0x28);
                            *(int *)(cur + 0x28) = (0x8001000c | (loadedDataReg << 21));
                            *(int *)(cur + 0x2c) = (0x3C005A00 | (comparisonDataReg << 21));
                            *(int *)(cur + 0x34) = (0x7c000000 | (comparisonDataReg << 16) | (loadedDataReg << 11));
                            *(int *)(cur + 0x48) = 0x60000000;
                            successful_patch_p2p++;
                        }
                    }
                }
                else if (found_opcode_chain_MASTER)
                {
                    if (
                        GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x10)) == 0x12 &&
                        GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x2c)) == 0x04 &&

                        GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x48)) == 0x18 &&
                        GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x50)) == 0x00 &&
                        GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x54)) == 0x18
                    )
                    {
                        int master_patch_version = 0;

                        // Check which version we have:
                        if (
                            GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x3c)) == 0x12 &&
                            GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x44)) == 0x0c
                        )
                            master_patch_version = 1;

                        else if (
                            GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x3c)) == 0x0c &&
                            GetImmediateDataVal((unsigned int *)(MASTERopcodeChainOffset + 0x44)) == 0x12
                        )
                            master_patch_version = 2;

                        if (master_patch_version == 2)
                        {
                            // Different opcode order...
                            *(int *)(MASTERopcodeChainOffset + 0x3c) = *(int *)(MASTERopcodeChainOffset + 0x44);
                        }

                        if (master_patch_version != 0)
                        {
                            int rY = GetComparisonTargetReg((unsigned int *)MASTERopcodeChainOffset);
                            int rX = GetLoadTargetReg((unsigned int *)MASTERopcodeChainOffset);

                            *(int *)(MASTERopcodeChainOffset + 0x00) = 0x38000004 | (rX << 21);
                            *(int *)(MASTERopcodeChainOffset + 0x04) = 0x7c00042c | (rY << 21) | (3 << 16) | (rX << 11);
                            *(int *)(MASTERopcodeChainOffset + 0x14) = 0x9000000c | (rY << 21) | (1 << 16);
                            *(int *)(MASTERopcodeChainOffset + 0x18) = 0x88000011 | (rY << 21) | (1 << 16);
                            *(int *)(MASTERopcodeChainOffset + 0x28) = 0x28000080 | (rY << 16);
                            *(int *)(MASTERopcodeChainOffset + 0x38) = 0x60000000;
                            *(int *)(MASTERopcodeChainOffset + 0x44) = 0x41810014;
                            successful_patch_master++;
                        }
                    }
                }
            }
        }
    } while (++cur < end);

    if (hasGT2Error)
    {
        if (successful_patch_master == 0 || successful_patch_p2p == 0)
            return 2;
    }

    return 0;
}

s8 do_new_wiimmfi()
{
    // As of November 2018, Wiimmfi requires a special Wiimmfi patcher
    // update which does a bit more than just patch the server addresses.
    // This function is being called by GameBooter.cpp, right before
    // jumping to the entry point (only for Mario Kart Wii & Wiimmfi),
    // and applies all the necessary new patches to the game.
    // This includes support for the new patcher update plus
    // support for StaticR.rel patching.

    // This function has been implemented by Leseratte. Please don't
    // try to modify it without speaking to the Wiimmfi team because
    // doing so could have unintended side effects.

    // Updated in 2021 to add the 51420 error fix.

    // check region:
    char region = *((char *)(0x80000003));
    char *patched;
    void *patch1_offset, *patch2_offset, *patch3_offset;
    void *errorfix_offset;

    // define some offsets and variables depending on the region:
    switch (region)
    {
        case 'P':
            patched = (char *)0x80276054;
            patch1_offset = (void *)0x800ee3a0;
            patch2_offset = (void *)0x801d4efc;
            patch3_offset = (void *)0x801A72E0;
            errorfix_offset = (void *)0x80658ce4;
            break;
        case 'E':
            patched = (char *)0x80271d14;
            patch1_offset = (void *)0x800ee300;
            patch2_offset = (void *)0x801d4e5c;
            patch3_offset = (void *)0x801A7240;
            errorfix_offset = (void *)0x8065485c;
            break;
        case 'J':
            patched = (char *)0x802759f4;
            patch1_offset = (void *)0x800ee2c0;
            patch2_offset = (void *)0x801d4e1c;
            patch3_offset = (void *)0x801A7200;
            errorfix_offset = (void *)0x80658360;
            break;
        case 'K':
            patched = (char *)0x80263E34;
            patch1_offset = (void *)0x800ee418;
            patch2_offset = (void *)0x801d5258;
            patch3_offset = (void *)0x801A763c;
            errorfix_offset = (void *)0x80646ffc;
            break;
        default:
            return -1;
    }

    if (*patched != '*')
        return -2; // ISO already patched

    // This RAM address is set (no asterisk) by all officially
    // updated patchers, so if it is modified, the image is already
    // patched with a new patcher and we don't need to patch anything.

    // For statistics and easier debugging in case of problems, Wiimmfi
    // wants to know what patcher a game has been patched with, thus,
    // let the game know the exact USB-Loader version.
    char *fmt = "USB-Loader GX v3.0 R%-21s";
    char patcher[42] = {0};
    snprintf((char *)&patcher, 42, fmt, LOADER_REV);
    strncpy(patched, (char *)&patcher, 42);

    // Do the plain old patching with the string search
    PrivateServerPatcher((void *)0x80004000, 0x385200, PRIVSERV_WIIMMFI, NULL);

    // Replace some URLs for Wiimmfi's new update system
    char newURL1[] = "http://ca.nas.wiimmfi.de/ca";
    char newURL2[] = "http://naswii.wiimmfi.de/ac";
    char newURL3P[] = "https://main.nas.wiimmfi.de/pp";
    char newURL3E[] = "https://main.nas.wiimmfi.de/pe";
    char newURL3J[] = "https://main.nas.wiimmfi.de/pj";
    char newURL3K[] = "https://main.nas.wiimmfi.de/pk";

    // Write the URLs to the proper place and do some other patching.
    switch (region)
    {
        case 'P':
            memcpy((void *)0x8027A400, newURL1, sizeof(newURL1));
            memcpy((void *)0x8027A400 + 0x28, newURL2, sizeof(newURL2));
            memcpy((void *)0x8027A400 + 0x4C, newURL3P, sizeof(newURL3P));
            *(u32 *)0x802a146c = 0x733a2f2f;
            *(u32 *)0x800ecaac = 0x3bc00000;
            break;
        case 'E':
            memcpy((void *)0x802760C0, newURL1, sizeof(newURL1));
            memcpy((void *)0x802760C0 + 0x28, newURL2, sizeof(newURL2));
            memcpy((void *)0x802760C0 + 0x4C, newURL3E, sizeof(newURL3E));
            *(u32 *)0x8029D12C = 0x733a2f2f;
            *(u32 *)0x800ECA0C = 0x3bc00000;
            break;
        case 'J':
            memcpy((void *)0x80279DA0, newURL1, sizeof(newURL1));
            memcpy((void *)0x80279DA0 + 0x28, newURL2, sizeof(newURL2));
            memcpy((void *)0x80279DA0 + 0x4C, newURL3J, sizeof(newURL3J));
            *(u32 *)0x802A0E0C = 0x733a2f2f;
            *(u32 *)0x800EC9CC = 0x3bc00000;
            break;
        case 'K':
            memcpy((void *)0x802682B0, newURL1, sizeof(newURL1));
            memcpy((void *)0x802682B0 + 0x28, newURL2, sizeof(newURL2));
            memcpy((void *)0x802682B0 + 0x4C, newURL3K, sizeof(newURL3K));
            *(u32 *)0x8028F474 = 0x733a2f2f;
            *(u32 *)0x800ECB24 = 0x3bc00000;
            break;
    }

    // Make some space on heap (0x500) for our custom code.
    u32 old_heap_ptr = *(u32 *)0x80003110;
    *((u32 *)0x80003110) = (old_heap_ptr - 0x500);
    u32 heap_space = old_heap_ptr - 0x500;
    memset((void *)old_heap_ptr - 0x500, 0xed, 0x500);

    // Binary blobs with Wiimmfi patches. Do not modify.
    // Provided by Leseratte on 2018-12-14.
    u32 binary[] = {
        0x37C849A2, 0x8BC32FA4, 0xC9A34B71, 0x1BCB49A2,
        0x2F119304, 0x5F402684, 0x3E4FDA29, 0x50849A21,
        0xB88B3452, 0x627FC9C1, 0xDC24D119, 0x5844350F,
        0xD893444F, 0x19A588DC, 0x16C91184, 0x0C3E237C,
        0x75906CED, 0x6E68A55E, 0x58791842, 0x072237E9,
        0xAB24906F, 0x0A8BDF21, 0x4D11BE42, 0x1AAEDDC8,
        0x1C42F908, 0x280CF2B2, 0x453A1BA4, 0x9A56C869,
        0x786F108E, 0xE8DF05D2, 0x6DB641EB, 0x6DFC84BB,
        0x7E980914, 0x0D7FB324, 0x23442185, 0xA7744966,
        0x53901359, 0xBF2103CC, 0xC24A4EB7, 0x32049A02,
        0xC1683466, 0xCA93689D, 0xD8245106, 0xA84987CF,
        0xEC9B47C9, 0x6FA688FE, 0x0A4D11A6, 0x8B653C7B,
        0x09D27E30, 0x5B936208, 0x5DD336DE, 0xCD092487,
        0xEF2C6D36, 0x1E09DF2D, 0x75B1BE47, 0xE68A7F22,
        0xB0E5F90D, 0xEC49F216, 0xAD1DCC24, 0xE2B5C841,
        0x066F6F63, 0xF4D90926, 0x299F42CD, 0xA3F125D6,
        0x077B093C, 0xB5721268, 0x1BE424D1, 0xEBC30BF0,
        0x77867BED, 0x4F0C9BCA, 0x3E195930, 0xDC32DE2C,
        0x1865D189, 0x70C67E7A, 0x71FA7329, 0x532233D3,
        0x06D2E87B, 0x6CBEBA7F, 0x99F08532, 0x52FA601C,
        0x05F4B82C, 0x4B64839C, 0xB5C65009, 0x1B8396E3,
        0x0A8B2DAF, 0x0DB85BE6, 0x12F1B71D, 0x186F6E4D,
        0x2870DC2E, 0x5960B8E6, 0x8F4D71BD, 0x0614E3C3,
        0x05E8C725, 0x365D8E3D, 0x74351CDE, 0xE1AB3930,
        0xFEDA721B, 0xE53AE4E9, 0xC3B4C9A6, 0xBAE59346,
        0x6D45269D, 0x634E4D1A, 0x2FD99A30, 0x26393449,
        0xE49768D1, 0x81E1D1A1, 0xFCE1A34A, 0x7EB44697,
        0xEB2F8D2D, 0xCECFE5AF, 0x81BD34B6, 0xB1F1696E,
        0x5E6ED2B2, 0xA473A4A0, 0x41664B70, 0xBF40968A,
        0x662F2CCB, 0xC5DF5B8C, 0xB632B772, 0x74EB6F39,
        0xE017DC71, 0xFDA3B890, 0xE3C9713D, 0xCE53E397,
        0xA12BC743, 0x5AD98EA5, 0xBC721C9F, 0x4568395A,
        0x925E72B4, 0x2D7DE4D7, 0x6777C9C7, 0xD6619396,
        0xA502268A, 0x77884D75, 0xF79E9AF0, 0xE6FC3461,
        0xF07468A5, 0xF866D11D, 0xF90CA342, 0xCF9546FF,
        0x87A48D81, 0x06881A51, 0x309C34D1, 0x79B669CE,
        0xFAADD2D7, 0xC8D7A5D1, 0x89214BE5, 0x1B8396EF,
        0x0A8B2DE9, 0x0D985B06, 0x12F1B711, 0x186F6E57,
        0x2850DC0E, 0x5960B8EA, 0x8F4D71AC, 0x0614E3E3,
        0x05E8C729, 0x365D8E39, 0x74351CFE, 0x518E3943,
        0x4A397268, 0x9D58E4B8, 0xD394C9A2, 0x0E069344,
        0xB522268B, 0x636E4D77, 0x2FF99A37, 0xF6DC346D,
        0xE49268B4, 0x2001D1A0, 0x4929A365, 0x7B764691,
        0xFFC68D49, 0x16A81A53, 0x247A34D2, 0xA1D16967,
        0x4B6DD2D5, 0xDDF4A5B7, 0x454A4B70, 0x0FAE96E2,
        0x0A8A2DC7, 0x0D98A47A, 0x06DCB71D, 0x0CCC6E38,
        0x55F25CFB, 0xB08C1E88, 0xDF4259C9, 0x0714E387,
        0xB00D47AF, 0x7B722975, 0x48BE349A, 0x29CC393C,
        0xEA797228, 0x98986471, 0x3778E1A3, 0xD7626D06,
        0x1567268D, 0x668ECD00, 0xD614F5C8, 0x133037CF,
        0x92F26CF2, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000};

    // Fix for error 51420:
    int patchCodeFix51420[] = {
        0x4800000d, 0x00000000,
        0x00000000, 0x7cc803a6,
        0x80860000, 0x7c041800,
        0x4182004c, 0x80a60004,
        0x38a50001, 0x2c050003,
        0x4182003c, 0x90a60004,
        0x90660000, 0x38610010,
        0x3ca08066, 0x38a58418,
        0x3c808066, 0x38848498,
        0x90a10010, 0x90810014,
        0x3ce08066, 0x38e78ce4,
        0x38e7fef0, 0x7ce903a6,
        0x4e800420, 0x3c80801d,
        0x388415f4, 0x7c8803a6,
        0x4e800021, 0x00000000};

    // Prepare patching process...
    int i = 3;
    int idx = 0;
    for (; i < 202; i++)
    {
        if (i == 67 || i == 82)
            idx++;
        binary[i] = binary[i] ^ binary[idx];
        binary[idx] = ((binary[idx] << 1) | ((binary[idx] >> (32 - 1)) & ~(0xfffffffe)));
    }

    // Binary blob needs some changes for regions other than PAL...
    switch (region)
    {
        case 'E':
            binary[29] = binary[67];
            binary[37] = binary[68];
            binary[43] = binary[69];
            binary[185] = 0x61295C74;
            binary[189] = 0x61295D40;
            binary[198] = 0x61086F5C;
            patchCodeFix51420[14] = 0x3ca08065;
            patchCodeFix51420[15] = 0x38a53f90;
            patchCodeFix51420[16] = 0x3c808065;
            patchCodeFix51420[17] = 0x38844010;
            patchCodeFix51420[20] = 0x3ce08065;
            patchCodeFix51420[21] = 0x38e7485c;
            patchCodeFix51420[26] = 0x38841554;
            break;
        case 'J':
            binary[29] = binary[70];
            binary[37] = binary[71];
            binary[43] = binary[72];
            binary[185] = 0x612997CC;
            binary[189] = 0x61299898;
            binary[198] = 0x61086F1C;
            patchCodeFix51420[14] = 0x3ca08065;
            patchCodeFix51420[15] = 0x38a57a84;
            patchCodeFix51420[16] = 0x3c808065;
            patchCodeFix51420[17] = 0x38847b04;
            patchCodeFix51420[20] = 0x3ce08065;
            patchCodeFix51420[21] = 0x38e78350;
            patchCodeFix51420[26] = 0x38841514;
            break;
        case 'K':
            binary[6] = binary[73];
            binary[9] = binary[74];
            binary[11] = binary[75];
            binary[23] = binary[76];
            binary[29] = binary[77];
            binary[33] = binary[78];
            binary[37] = binary[79];
            binary[43] = binary[80];
            binary[63] = binary[81];
            binary[184] = 0x3D208088;
            binary[185] = 0x61298AA4;
            binary[188] = 0x3D208088;
            binary[189] = 0x61298B58;
            binary[198] = 0x61087358;
            patchCodeFix51420[14] = 0x3ca08064;
            patchCodeFix51420[15] = 0x38a56730;
            patchCodeFix51420[16] = 0x3c808064;
            patchCodeFix51420[17] = 0x388467b0;
            patchCodeFix51420[20] = 0x3ce08064;
            patchCodeFix51420[21] = 0x38e76ffc;
            patchCodeFix51420[26] = 0x38841950;
            break;
    }

    // Installing all the patches.
    memcpy((void *)heap_space, (void *)binary, 820);
    u32 code_offset_1 = heap_space + 12;
    u32 code_offset_2 = heap_space + 88;
    u32 code_offset_3 = heap_space + 92;
    u32 code_offset_4 = heap_space + 264;
    u32 code_offset_5 = heap_space + 328;

    *((u32 *)patch1_offset) = 0x48000000 + (((u32)(code_offset_1) - ((u32)(patch1_offset))) & 0x3ffffff);
    *((u32 *)code_offset_2) = 0x48000000 + (((u32)(patch1_offset + 4) - ((u32)(code_offset_2))) & 0x3ffffff);
    *((u32 *)patch2_offset) = 0x48000000 + (((u32)(code_offset_3) - ((u32)(patch2_offset))) & 0x3ffffff);
    *((u32 *)code_offset_4) = 0x48000000 + (((u32)(patch2_offset + 4) - ((u32)(code_offset_4))) & 0x3ffffff);
    *((u32 *)patch3_offset) = 0x48000000 + (((u32)(code_offset_5) - ((u32)(patch3_offset))) & 0x3ffffff);

    // Add the 51420 fix:
    memcpy((void *)heap_space + 0x400, (void *)patchCodeFix51420, 0x78);
    *((u32 *)errorfix_offset) = 0x48000000 + (((u32)(heap_space + 0x400) - ((u32)(errorfix_offset))) & 0x3ffffff);
    *((u32 *)heap_space + 0x400 + 0x74) = 0x48000000 + (((u32)(errorfix_offset + 4) - ((u32)(heap_space + 0x400 + 0x74))) & 0x3ffffff);

    // Patches successfully installed
    // returns 0 when all patching is done and game is ready to be booted.
    return 0;
}

void domainpatcher(void *addr, u32 len, const char *domain)
{
    if (strlen("nintendowifi.net") < strlen(domain))
        return;

    char *cur = (char *)addr;
    const char *end = cur + len - 16;

    do
    {
        if (memcmp(cur, "nintendowifi.net", 16) == 0)
        {
            int len = strlen(cur);
            u8 i;
            memcpy(cur, domain, strlen(domain));
            memmove(cur + strlen(domain), cur + 16, len - 16);
            for (i = 16 - strlen(domain); i > 0; i--)
                cur[len - i] = 0;
            cur += len;
        }
    } while (++cur < end);
}

void patch_re4(u8 *gameid)
{
    if (memcmp(gameid, "RB4E08", 6) == 0)
        *(u32 *)0x8016B260 = 0x38600001;
    else if (memcmp(gameid, "RB4P08", 6) == 0)
        *(u32 *)0x8016B094 = 0x38600001;
    else if (memcmp(gameid, "RB4X08", 6) == 0)
        *(u32 *)0x8016B0C8 = 0x38600001;
}

bool patch_nsmb(u8 *gameid)
{
    WIP_Code *CodeList = NULL;

    if (memcmp(gameid, "SMNE01", 6) == 0)
    {
        CodeList = MEM2_alloc(3 * sizeof(WIP_Code));
        if (!CodeList)
            return false;

        CodeList[0].offset = 0x001AB610;
        CodeList[0].srcaddress = 0x9421FFD0;
        CodeList[0].dstaddress = 0x4E800020;
        CodeList[1].offset = 0x001CED53;
        CodeList[1].srcaddress = 0xDA000000;
        CodeList[1].dstaddress = 0x71000000;
        CodeList[2].offset = 0x001CED6B;
        CodeList[2].srcaddress = 0xDA000000;
        CodeList[2].dstaddress = 0x71000000;
    }
    else if (memcmp(gameid, "SMNP01", 6) == 0)
    {
        CodeList = MEM2_alloc(3 * sizeof(WIP_Code));
        if (!CodeList)
            return false;

        CodeList[0].offset = 0x001AB750;
        CodeList[0].srcaddress = 0x9421FFD0;
        CodeList[0].dstaddress = 0x4E800020;
        CodeList[1].offset = 0x001CEE90;
        CodeList[1].srcaddress = 0x38A000DA;
        CodeList[1].dstaddress = 0x38A00071;
        CodeList[2].offset = 0x001CEEA8;
        CodeList[2].srcaddress = 0x388000DA;
        CodeList[2].dstaddress = 0x38800071;
    }
    else if (memcmp(gameid, "SMNJ01", 6) == 0)
    {
        CodeList = MEM2_alloc(3 * sizeof(WIP_Code));
        if (!CodeList)
            return false;

        CodeList[0].offset = 0x001AB420;
        CodeList[0].srcaddress = 0x9421FFD0;
        CodeList[0].dstaddress = 0x4E800020;
        CodeList[1].offset = 0x001CEB63;
        CodeList[1].srcaddress = 0xDA000000;
        CodeList[1].dstaddress = 0x71000000;
        CodeList[2].offset = 0x001CEB7B;
        CodeList[2].srcaddress = 0xDA000000;
        CodeList[2].dstaddress = 0x71000000;
    }

    if (CodeList && set_wip_list(CodeList, 3) == false)
    {
        MEM2_free(CodeList);
        CodeList = NULL;
        return false;
    }
    if (CodeList)
        gprintf("Patched New Super Mario Bros\n");
    return CodeList != NULL;
}

bool patch_pop(u8 *gameid)
{
    if (memcmp(gameid, "SPX", 3) != 0 && memcmp(gameid, "RPW", 3) != 0)
        return false;

    WIP_Code *CodeList = MEM2_alloc(5 * sizeof(WIP_Code));
    CodeList[0].offset = 0x007AAC6A;
    CodeList[0].srcaddress = 0x7A6B6F6A;
    CodeList[0].dstaddress = 0x6F6A7A6B;
    CodeList[1].offset = 0x007AAC75;
    CodeList[1].srcaddress = 0x7C7A6939;
    CodeList[1].dstaddress = 0x69397C7A;
    CodeList[2].offset = 0x007AAC82;
    CodeList[2].srcaddress = 0x7376686B;
    CodeList[2].dstaddress = 0x686B7376;
    CodeList[3].offset = 0x007AAC92;
    CodeList[3].srcaddress = 0x80717570;
    CodeList[3].dstaddress = 0x75708071;
    CodeList[4].offset = 0x007AAC9D;
    CodeList[4].srcaddress = 0x82806F3F;
    CodeList[4].dstaddress = 0x6F3F8280;

    if (set_wip_list(CodeList, 5) == false)
    {
        MEM2_free(CodeList);
        CodeList = NULL;
        return false;
    }
    if (CodeList)
        gprintf("Patched Prince of Persia\n");
    return CodeList != NULL;
}

void patch_sdcard(u8 *gameid)
{
    // I might patch this at the cIOS level at some point, but this works for now

    // Excite Truck
    if (memcmp(gameid, "REXE01", 6) == 0)
        *(u32 *)0x800b9e48 = 0x4800014c;
    else if (memcmp(gameid, "REXP01", 6) == 0)
        *(u32 *)0x800ba358 = 0x4800014c;
    else if (memcmp(gameid, "REXJ01", 6) == 0)
        *(u32 *)0x800ba404 = 0x4800014c;

    // Kirby's Return to Dream Land
    else if (memcmp(gameid, "SUKE01", 6) == 0)
    {
        *(u32 *)0x8022da10 = 0x60000000;
        *(u32 *)0x8022da48 = 0x60000000;
    }
    else if (memcmp(gameid, "SUKP01", 6) == 0)
    {
        *(u32 *)0x8022e800 = 0x60000000;
        *(u32 *)0x8022e838 = 0x60000000;
    }
    else if (memcmp(gameid, "SUKJ01", 6) == 0)
    {
        *(u32 *)0x8022c66c = 0x60000000;
        *(u32 *)0x8022c6a4 = 0x60000000;
    }
    else if (memcmp(gameid, "SUKK01", 6) == 0)
    {
        *(u32 *)0x8022dfc4 = 0x60000000;
        *(u32 *)0x8022dffc = 0x60000000;
    }
}

void patch_error_codes(u8 *gameid)
{
    // Thanks to Seeky for the MKWii gecko codes
    // Reimplemented by Leseratte without the need for a code handler.
    u32 *patch_addr = 0;
    u32 *patched = 0;

    // Patch RCE vulnerability in MKWii.
    if (memcmp(gameid, "RMC", 3) == 0)
    {
        switch (gameid[3])
        {
            case 'P':
                patched = (u32 *)0x80276054;
                patch_addr = (u32 *)0x8089a194;
                break;
            case 'E':
                patched = (u32 *)0x80271d14;
                patch_addr = (u32 *)0x80895ac4;
                break;
            case 'J':
                patched = (u32 *)0x802759f4;
                patch_addr = (u32 *)0x808992f4;
                break;
            case 'K':
                patched = (u32 *)0x80263E34;
                patch_addr = (u32 *)0x808885cc;
                break;
            default:
                gprintf("NOT patching RCE vulnerability due to invalid game ID: %s\n", gameid);
                return;
        }

        if (*patched != '*')
            gprintf("Game is already Wiimmfi-patched, don't apply the RCE fix\n");
        else
        {
            gprintf("Patching RCE vulnerability for %s\n", gameid);

            for (int i = 0; i < 7; i++)
                *patch_addr++ = 0xff;
        }
    }
}

/** Insert the individual gamepatches above with the patterns and patch data **/
/** Following is only the VideoPatcher **/

// viYOrigin is calculated as (576 - 528)/2 in libogc 2.0.0 for the following render modes.
// But we need to use (574 - 528)/2 so that the render modes match the Revolution SDK.

// An RGB byte was included in libogc 2.11.0, but it might be removed in a future version.

GXRModeObj TVPal528Prog_RVL = {
    6,             // viDisplayMode
    640,           // fbWidth
    528,           // efbHeight
    528,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVPal528ProgSoft_RVL = {
    6,             // viDisplayMode
    640,           // fbWidth
    528,           // efbHeight
    528,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        8,  // line n-1
        8,  // line n-1
        10, // line n
        12, // line n
        10, // line n
        8,  // line n+1
        8   // line n+1
    }
};

GXRModeObj TVPal524ProgAa_RVL = {
    6,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    524,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    524,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        4,  // line n-1
        8,  // line n-1
        12, // line n
        16, // line n
        12, // line n
        8,  // line n+1
        4   // line n+1
    }
};

GXRModeObj TVPal528Int_RVL = {
    4,             // viDisplayMode
    640,           // fbWidth
    528,           // efbHeight
    528,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_DF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVPal528IntDf_RVL = {
    4,             // viDisplayMode
    640,           // fbWidth
    528,           // efbHeight
    528,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_DF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        8,  // line n-1
        8,  // line n-1
        10, // line n
        12, // line n
        10, // line n
        8,  // line n+1
        8   // line n+1
    }
};

GXRModeObj TVEurgb60Hz480Prog_RVL = {
    22,            // viDisplayMode
    640,           // fbWidth
    480,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_TRUE,       // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVEurgb60Hz480ProgSoft_RVL = {
    22,            // viDisplayMode
    640,           // fbWidth
    480,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_TRUE,       // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        8,  // line n-1
        8,  // line n-1
        10, // line n
        12, // line n
        10, // line n
        8,  // line n+1
        8   // line n+1
    }
};

GXRModeObj TVEurgb60Hz480ProgAa_RVL = {
    22,            // viDisplayMode
    640,           // fbWidth
    242,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_TRUE,       // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        4,  // line n-1
        8,  // line n-1
        12, // line n
        16, // line n
        12, // line n
        8,  // line n+1
        4   // line n+1
    }
};

GXRModeObj TVPal524IntAa_RVL = {
    4,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    524,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2 //574 instead of 576
    640,           // viWidth
    524,           // viHeight
    VI_XFBMODE_DF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        4,  // line n-1
        8,  // line n-1
        12, // line n
        16, // line n
        12, // line n
        8,  // line n+1
        4   // line n+1
    }
};

GXRModeObj TVPal264Int_RVL = {
    4,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    264,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_TRUE,       // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVPal264IntAa_RVL = {
    4,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    264,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    23,            // viYOrigin (574 - 528)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_TRUE,       // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVPal264Ds_RVL = {
    5,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    264,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    11,            // viYOrigin (572/2 - 528/2)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVPal264DsAa_RVL = {
    5,             // viDisplayMode
    640,           // fbWidth
    264,           // efbHeight
    264,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    11,            // viYOrigin (572/2 - 528/2)/2
    640,           // viWidth
    528,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVMpal240Int_RVL = {
    8,             // viDisplayMode
    640,           // fbWidth
    240,           // efbHeight
    240,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_TRUE,       // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVMpal240IntAa_RVL = {
    8,             // viDisplayMode
    640,           // fbWidth
    240,           // efbHeight
    240,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_TRUE,       // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVMpal480Int_RVL = {
    8,             // viDisplayMode
    640,           // fbWidth
    480,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_DF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        0,  // line n-1
        0,  // line n-1
        21, // line n
        22, // line n
        21, // line n
        0,  // line n+1
        0   // line n+1
    }
};

GXRModeObj TVMpal480ProgSoft_RVL = {
    10,            // viDisplayMode
    640,           // fbWidth
    480,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_FALSE,      // aa

    // sample points arranged in increasing Y order
    {
        {6, 6}, {6, 6}, {6, 6}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {6, 6}, {6, 6}, {6, 6}, // pix 1
        {6, 6}, {6, 6}, {6, 6}, // pix 2
        {6, 6}, {6, 6}, {6, 6}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        8,  // line n-1
        8,  // line n-1
        10, // line n
        12, // line n
        10, // line n
        8,  // line n+1
        8   // line n+1
    }
};

GXRModeObj TVMpal480ProgAa_RVL = {
    10,            // viDisplayMode
    640,           // fbWidth
    242,           // efbHeight
    480,           // xfbHeight
    40,            // viXOrigin (720 - 640)/2
    0,             // viYOrigin (480 - 480)/2
    640,           // viWidth
    480,           // viHeight
    VI_XFBMODE_SF, // xFBmode
#if OGC_VERSION == 21100
    GX_FALSE,      // rgb
#endif
    GX_FALSE,      // field_rendering
    GX_TRUE,       // aa

    // sample points arranged in increasing Y order
    {
        {3, 2}, {9, 6}, {3, 10}, // pix 0, 3 sample points, 1/12 units, 4 bits each
        {3, 2}, {9, 6}, {3, 10}, // pix 1
        {9, 2}, {3, 6}, {9, 10}, // pix 2
        {9, 2}, {3, 6}, {9, 10}  // pix 3
    },

    // vertical filter[7], 1/64 units, 6 bits each
    {
        4,  // line n-1
        8,  // line n-1
        12, // line n
        16, // line n
        12, // line n
        8,  // line n+1
        4   // line n+1
    }
};

static GXRModeObj *vmodes[] = {
    &TVNtsc240Ds,
    &TVNtsc240DsAa,
    &TVNtsc240Int,
    &TVNtsc240IntAa,
    &TVNtsc480Int,
    &TVNtsc480IntAa,
    &TVNtsc480IntDf,
    &TVNtsc480Prog,
    &TVNtsc480ProgSoft,
    &TVNtsc480ProgAa,
    &TVMpal240Int_RVL,
    &TVMpal240IntAa_RVL,
    &TVMpal240Ds,
    &TVMpal240DsAa,
    &TVMpal480Int_RVL,
    &TVMpal480IntAa,
    &TVMpal480IntDf,
    &TVMpal480Prog,
    &TVMpal480ProgSoft_RVL,
    &TVMpal480ProgAa_RVL,
    &TVPal264Ds_RVL,
    &TVPal264DsAa_RVL,
    &TVPal264Int_RVL,
    &TVPal264IntAa_RVL,
    &TVPal524IntAa_RVL,
    &TVPal524ProgAa_RVL,
    &TVPal528Int_RVL,
    &TVPal528IntDf_RVL,
    &TVPal528Prog_RVL,
    &TVPal528ProgSoft_RVL,
    &TVPal576IntDfScale,
    &TVEurgb60Hz240Ds,
    &TVEurgb60Hz240DsAa,
    &TVEurgb60Hz240Int,
    &TVEurgb60Hz240IntAa,
    &TVEurgb60Hz480Int,
    &TVEurgb60Hz480IntDf,
    &TVEurgb60Hz480IntAa,
    &TVEurgb60Hz480Prog_RVL,
    &TVEurgb60Hz480ProgSoft_RVL,
    &TVEurgb60Hz480ProgAa_RVL};

static const char *vmodes_name[] = {
    "TVNtsc240Ds",
    "TVNtsc240DsAa",
    "TVNtsc240Int",
    "TVNtsc240IntAa",
    "TVNtsc480Int",
    "TVNtsc480IntAa",
    "TVNtsc480IntDf",
    "TVNtsc480Prog",
    "TVNtsc480ProgSoft",
    "TVNtsc480ProgAa",
    "TVMpal240Int",
    "TVMpal240IntAa",
    "TVMpal240Ds",
    "TVMpal240DsAa",
    "TVMpal480Int",
    "TVMpal480IntAa",
    "TVMpal480IntDf",
    "TVMpal480Prog",
    "TVMpal480ProgSoft",
    "TVMpal480ProgAa",
    "TVPal264Ds",
    "TVPal264DsAa",
    "TVPal264Int",
    "TVPal264IntAa",
    "TVPal524IntAa",
    "TVPal524ProgAa",
    "TVPal528Int",
    "TVPal528IntDf",
    "TVPal528Prog",
    "TVPal528ProgSoft",
    "TVPal576IntDfScale",
    "TVEurgb60Hz240Ds",
    "TVEurgb60Hz240DsAa",
    "TVEurgb60Hz240Int",
    "TVEurgb60Hz240IntAa",
    "TVEurgb60Hz480Int",
    "TVEurgb60Hz480IntDf",
    "TVEurgb60Hz480IntAa",
    "TVEurgb60Hz480Prog",
    "TVEurgb60Hz480ProgSoft",
    "TVEurgb60Hz480ProgAa"};

static GXRModeObj *PAL2NTSC[] = {
    &TVMpal240Int_RVL, &TVNtsc240Int,
    &TVMpal240IntAa_RVL, &TVNtsc240IntAa,
    &TVMpal240Ds, &TVNtsc240Ds,
    &TVMpal240DsAa, &TVNtsc240DsAa,
    &TVMpal480Int_RVL, &TVNtsc480Int,
    &TVMpal480IntAa, &TVNtsc480IntAa,
    &TVMpal480IntDf, &TVNtsc480IntDf,
    &TVMpal480Prog, &TVNtsc480Prog,
    &TVMpal480ProgSoft_RVL, &TVNtsc480ProgSoft,
    &TVMpal480ProgAa_RVL, &TVNtsc480ProgAa,
    &TVPal264Ds_RVL, &TVNtsc240Ds,
    &TVPal264DsAa_RVL, &TVNtsc240DsAa,
    &TVPal264Int_RVL, &TVNtsc240Int,
    &TVPal264IntAa_RVL, &TVNtsc240IntAa,
    &TVPal524IntAa_RVL, &TVNtsc480IntAa,
    &TVPal524ProgAa_RVL, &TVNtsc480ProgAa,
    &TVPal528Int_RVL, &TVNtsc480Int,
    &TVPal528IntDf_RVL, &TVNtsc480IntDf,
    &TVPal528Prog_RVL, &TVNtsc480Prog,
    &TVPal528ProgSoft_RVL, &TVNtsc480ProgSoft,
    &TVPal576IntDfScale, &TVNtsc480IntDf,
    &TVEurgb60Hz240Ds, &TVNtsc240Ds,
    &TVEurgb60Hz240DsAa, &TVNtsc240DsAa,
    &TVEurgb60Hz240Int, &TVNtsc240Int,
    &TVEurgb60Hz240IntAa, &TVNtsc240IntAa,
    &TVEurgb60Hz480Int, &TVNtsc480Int,
    &TVEurgb60Hz480IntDf, &TVNtsc480IntDf,
    &TVEurgb60Hz480IntAa, &TVNtsc480IntAa,
    &TVEurgb60Hz480Prog_RVL, &TVNtsc480Prog,
    &TVEurgb60Hz480ProgSoft_RVL, &TVNtsc480ProgSoft,
    &TVEurgb60Hz480ProgAa_RVL, &TVNtsc480ProgAa,
    0, 0};

static GXRModeObj *NTSC2PAL[] = {
    &TVNtsc240Ds, &TVPal264Ds_RVL,
    &TVNtsc240DsAa, &TVPal264DsAa_RVL,
    &TVNtsc240Int, &TVPal264Int_RVL,
    &TVNtsc240IntAa, &TVPal264IntAa_RVL,
    &TVNtsc480Int, &TVPal528Int_RVL,
    &TVNtsc480IntAa, &TVPal524IntAa_RVL,
    &TVNtsc480IntDf, &TVPal528IntDf_RVL,
    &TVNtsc480Prog, &TVPal528Prog_RVL,
    &TVNtsc480ProgSoft, &TVPal528ProgSoft_RVL,
    &TVNtsc480ProgAa, &TVPal524ProgAa_RVL,
    0, 0};

static GXRModeObj *NTSC2PAL60[] = {
    &TVNtsc240Ds, &TVEurgb60Hz240Ds,
    &TVNtsc240DsAa, &TVEurgb60Hz240DsAa,
    &TVNtsc240Int, &TVEurgb60Hz240Int,
    &TVNtsc240IntAa, &TVEurgb60Hz240IntAa,
    &TVNtsc480Int, &TVEurgb60Hz480Int,
    &TVNtsc480IntAa, &TVEurgb60Hz480IntAa,
    &TVNtsc480IntDf, &TVEurgb60Hz480IntDf,
    &TVNtsc480Prog, &TVEurgb60Hz480Prog_RVL,
    &TVNtsc480ProgSoft, &TVEurgb60Hz480ProgSoft_RVL,
    &TVNtsc480ProgAa, &TVEurgb60Hz480ProgAa_RVL,
    0, 0};

static u8 PATTERN[12][2] = {
    {6, 6}, {6, 6}, {6, 6},
    {6, 6}, {6, 6}, {6, 6},
    {6, 6}, {6, 6}, {6, 6},
    {6, 6}, {6, 6}, {6, 6}
};

static u8 PATTERN_AA[12][2] = {
    {3, 2}, {9, 6}, {3, 10},
    {3, 2}, {9, 6}, {3, 10},
    {9, 2}, {3, 6}, {9, 10},
    {9, 2}, {3, 6}, {9, 10}
};

static bool compare_videomodes_rvl(GXRModeObj *mode1, GXRModeObjRVL *mode2)
{
    if (mode1->viTVMode != mode2->viTVMode || mode1->fbWidth != mode2->fbWidth || mode1->efbHeight != mode2->efbHeight
            || mode1->xfbHeight != mode2->xfbHeight || mode1->viXOrigin != mode2->viXOrigin || mode1->viYOrigin
            != mode2->viYOrigin || mode1->viWidth != mode2->viWidth || mode1->viHeight != mode2->viHeight
            || mode1->xfbMode != mode2->xfbMode || mode1->field_rendering != mode2->field_rendering || mode1->aa
            != mode2->aa || mode1->sample_pattern[0][0] != mode2->sample_pattern[0][0] || mode1->sample_pattern[1][0]
            != mode2->sample_pattern[1][0] || mode1->sample_pattern[2][0] != mode2->sample_pattern[2][0]
            || mode1->sample_pattern[3][0] != mode2->sample_pattern[3][0] || mode1->sample_pattern[4][0]
            != mode2->sample_pattern[4][0] || mode1->sample_pattern[5][0] != mode2->sample_pattern[5][0]
            || mode1->sample_pattern[6][0] != mode2->sample_pattern[6][0] || mode1->sample_pattern[7][0]
            != mode2->sample_pattern[7][0] || mode1->sample_pattern[8][0] != mode2->sample_pattern[8][0]
            || mode1->sample_pattern[9][0] != mode2->sample_pattern[9][0] || mode1->sample_pattern[10][0]
            != mode2->sample_pattern[10][0] || mode1->sample_pattern[11][0] != mode2->sample_pattern[11][0]
            || mode1->sample_pattern[0][1] != mode2->sample_pattern[0][1] || mode1->sample_pattern[1][1]
            != mode2->sample_pattern[1][1] || mode1->sample_pattern[2][1] != mode2->sample_pattern[2][1]
            || mode1->sample_pattern[3][1] != mode2->sample_pattern[3][1] || mode1->sample_pattern[4][1]
            != mode2->sample_pattern[4][1] || mode1->sample_pattern[5][1] != mode2->sample_pattern[5][1]
            || mode1->sample_pattern[6][1] != mode2->sample_pattern[6][1] || mode1->sample_pattern[7][1]
            != mode2->sample_pattern[7][1] || mode1->sample_pattern[8][1] != mode2->sample_pattern[8][1]
            || mode1->sample_pattern[9][1] != mode2->sample_pattern[9][1] || mode1->sample_pattern[10][1]
            != mode2->sample_pattern[10][1] || mode1->sample_pattern[11][1] != mode2->sample_pattern[11][1]
            || mode1->vfilter[0] != mode2->vfilter[0] || mode1->vfilter[1] != mode2->vfilter[1] || mode1->vfilter[2]
            != mode2->vfilter[2] || mode1->vfilter[3] != mode2->vfilter[3] || mode1->vfilter[4] != mode2->vfilter[4]
            || mode1->vfilter[5] != mode2->vfilter[5] || mode1->vfilter[6] != mode2->vfilter[6])
    {
        return false;
    }
    else
    {
        return true;
    }
}

static bool compare_videomodes_ogc(GXRModeObj *mode1, GXRModeObj *mode2)
{
    if (mode1->viTVMode != mode2->viTVMode || mode1->fbWidth != mode2->fbWidth || mode1->efbHeight != mode2->efbHeight
            || mode1->xfbHeight != mode2->xfbHeight || mode1->viXOrigin != mode2->viXOrigin || mode1->viYOrigin
            != mode2->viYOrigin || mode1->viWidth != mode2->viWidth || mode1->viHeight != mode2->viHeight
            || mode1->xfbMode != mode2->xfbMode || mode1->field_rendering != mode2->field_rendering || mode1->aa
            != mode2->aa || mode1->sample_pattern[0][0] != mode2->sample_pattern[0][0] || mode1->sample_pattern[1][0]
            != mode2->sample_pattern[1][0] || mode1->sample_pattern[2][0] != mode2->sample_pattern[2][0]
            || mode1->sample_pattern[3][0] != mode2->sample_pattern[3][0] || mode1->sample_pattern[4][0]
            != mode2->sample_pattern[4][0] || mode1->sample_pattern[5][0] != mode2->sample_pattern[5][0]
            || mode1->sample_pattern[6][0] != mode2->sample_pattern[6][0] || mode1->sample_pattern[7][0]
            != mode2->sample_pattern[7][0] || mode1->sample_pattern[8][0] != mode2->sample_pattern[8][0]
            || mode1->sample_pattern[9][0] != mode2->sample_pattern[9][0] || mode1->sample_pattern[10][0]
            != mode2->sample_pattern[10][0] || mode1->sample_pattern[11][0] != mode2->sample_pattern[11][0]
            || mode1->sample_pattern[0][1] != mode2->sample_pattern[0][1] || mode1->sample_pattern[1][1]
            != mode2->sample_pattern[1][1] || mode1->sample_pattern[2][1] != mode2->sample_pattern[2][1]
            || mode1->sample_pattern[3][1] != mode2->sample_pattern[3][1] || mode1->sample_pattern[4][1]
            != mode2->sample_pattern[4][1] || mode1->sample_pattern[5][1] != mode2->sample_pattern[5][1]
            || mode1->sample_pattern[6][1] != mode2->sample_pattern[6][1] || mode1->sample_pattern[7][1]
            != mode2->sample_pattern[7][1] || mode1->sample_pattern[8][1] != mode2->sample_pattern[8][1]
            || mode1->sample_pattern[9][1] != mode2->sample_pattern[9][1] || mode1->sample_pattern[10][1]
            != mode2->sample_pattern[10][1] || mode1->sample_pattern[11][1] != mode2->sample_pattern[11][1]
            || mode1->vfilter[0] != mode2->vfilter[0] || mode1->vfilter[1] != mode2->vfilter[1] || mode1->vfilter[2]
            != mode2->vfilter[2] || mode1->vfilter[3] != mode2->vfilter[3] || mode1->vfilter[4] != mode2->vfilter[4]
            || mode1->vfilter[5] != mode2->vfilter[5] || mode1->vfilter[6] != mode2->vfilter[6])
    {
        return false;
    }
    else
    {
        return true;
    }
}

static void patch_videomode(GXRModeObjRVL *mode1, GXRModeObj *mode2)
{
    mode1->viTVMode = mode2->viTVMode;

    /* Dev-disc support: SWBF3 r2.91120a PAL crash workaround.
     * Forcing PAL rewrites SWBF3's NTSC rmode dimensions (efbHeight/xfbHeight
     * 480 -> 528). PAL528 framebuffer + Z-buffer overflow the ~0x190000-byte
     * envelope between the gui/nav heap tops (~0x97E2FFE0) and the IPC
     * region at 0x97FC0000. The overflow zeros the nav-heap descriptor's
     * MEM2 backing pointer at 0x8082CCD4, and the heap allocator at
     * SRR0=0x803a3800 dies (DSI, DAR=0xFFFFFFF8) under PAL50/60/480p alike.
     * For RSBE3* keep all dimensional + sample/vfilter fields at the disc
     * NTSC defaults; only viTVMode is forced so VI still scans at PAL
     * refresh (480-line content shown letterboxed in PAL frame). */
    if (memcmp((const void *)0x80000000, "RSBE3", 5) == 0)
    {
        gprintf("SWBF3 RSBE3* detected: keeping rmode dimensions at NTSC defaults; only viTVMode forced to 0x%x to avoid nav-heap MEM2 overflow at 0x803a3800.\n", mode2->viTVMode);
        return;
    }

    if (mode1->viWidth == 640 || mode1->viWidth == 708)
    {
        mode1->fbWidth = mode2->fbWidth;
        mode1->efbHeight = mode2->efbHeight;
        mode1->xfbHeight = mode2->xfbHeight;
        mode1->viXOrigin = mode2->viXOrigin;
        mode1->viYOrigin = mode2->viYOrigin;
        mode1->viWidth = mode2->viWidth;
        mode1->viHeight = mode2->viHeight;
    }
    else
        gprintf("Skipped patching dimensions %d x %d\n", mode1->viWidth, mode1->viHeight);

    mode1->xfbMode = mode2->xfbMode;
    mode1->field_rendering = mode2->field_rendering;
    mode1->aa = mode2->aa;
    mode1->sample_pattern[0][0] = mode2->sample_pattern[0][0];
    mode1->sample_pattern[1][0] = mode2->sample_pattern[1][0];
    mode1->sample_pattern[2][0] = mode2->sample_pattern[2][0];
    mode1->sample_pattern[3][0] = mode2->sample_pattern[3][0];
    mode1->sample_pattern[4][0] = mode2->sample_pattern[4][0];
    mode1->sample_pattern[5][0] = mode2->sample_pattern[5][0];
    mode1->sample_pattern[6][0] = mode2->sample_pattern[6][0];
    mode1->sample_pattern[7][0] = mode2->sample_pattern[7][0];
    mode1->sample_pattern[8][0] = mode2->sample_pattern[8][0];
    mode1->sample_pattern[9][0] = mode2->sample_pattern[9][0];
    mode1->sample_pattern[10][0] = mode2->sample_pattern[10][0];
    mode1->sample_pattern[11][0] = mode2->sample_pattern[11][0];
    mode1->sample_pattern[0][1] = mode2->sample_pattern[0][1];
    mode1->sample_pattern[1][1] = mode2->sample_pattern[1][1];
    mode1->sample_pattern[2][1] = mode2->sample_pattern[2][1];
    mode1->sample_pattern[3][1] = mode2->sample_pattern[3][1];
    mode1->sample_pattern[4][1] = mode2->sample_pattern[4][1];
    mode1->sample_pattern[5][1] = mode2->sample_pattern[5][1];
    mode1->sample_pattern[6][1] = mode2->sample_pattern[6][1];
    mode1->sample_pattern[7][1] = mode2->sample_pattern[7][1];
    mode1->sample_pattern[8][1] = mode2->sample_pattern[8][1];
    mode1->sample_pattern[9][1] = mode2->sample_pattern[9][1];
    mode1->sample_pattern[10][1] = mode2->sample_pattern[10][1];
    mode1->sample_pattern[11][1] = mode2->sample_pattern[11][1];
    mode1->vfilter[0] = mode2->vfilter[0];
    mode1->vfilter[1] = mode2->vfilter[1];
    mode1->vfilter[2] = mode2->vfilter[2];
    mode1->vfilter[3] = mode2->vfilter[3];
    mode1->vfilter[4] = mode2->vfilter[4];
    mode1->vfilter[5] = mode2->vfilter[5];
    mode1->vfilter[6] = mode2->vfilter[6];
}

static bool Search_and_patch_Video_Modes(u8 *Address, u32 Size, GXRModeObj *Table[])
{
    u8 *Addr = (u8 *)Address;
    bool found = 0;
    u32 i, j;

    while (Size >= sizeof(GXRModeObjRVL))
    {
        for (i = 0; Table[i]; i += 2)
        {
            if (compare_videomodes_rvl(Table[i], (GXRModeObjRVL *)Addr))
            {
                u8 current_vmode = 0;
                u8 target_vmode = 0;
                for (j = 0; j < sizeof(vmodes) / sizeof(vmodes[0]); j++)
                {
                    if (compare_videomodes_ogc(Table[i], vmodes[j]))
                    {
                        current_vmode = j;
                        break;
                    }
                }
                for (j = 0; j < sizeof(vmodes) / sizeof(vmodes[0]); j++)
                {
                    if (compare_videomodes_ogc(Table[i + 1], vmodes[j]))
                    {
                        target_vmode = j;
                        break;
                    }
                }

                gprintf("%s replaced with %s \n", vmodes_name[current_vmode], vmodes_name[target_vmode]);
                found = 1;
                patch_videomode((GXRModeObjRVL *)Addr, Table[i + 1]);
                Addr += (sizeof(GXRModeObjRVL) - 4);
                Size -= (sizeof(GXRModeObjRVL) - 4);
                break;
            }
        }

        Addr += 4;
        Size -= 4;
    }

    return found;
}

// Patch known and unknown vfilters within GXRModeObj structures
void patch_vfilters(u8 *addr, u32 len, u8 *vfilter)
{
    u8 *addr_start = addr;
    while (len >= sizeof(GXRModeObjRVL))
    {
        GXRModeObjRVL *vidmode = (GXRModeObjRVL *)addr_start;
        if ((memcmp(vidmode->sample_pattern, PATTERN, 24) == 0 || memcmp(vidmode->sample_pattern, PATTERN_AA, 24) == 0) &&
            (vidmode->fbWidth == 640 || vidmode->fbWidth == 608 || vidmode->fbWidth == 512) &&
            (vidmode->field_rendering == 0 || vidmode->field_rendering == 1) &&
            (vidmode->aa == 0 || vidmode->aa == 1))
        {
            gprintf("Replaced vfilter %02x%02x%02x%02x%02x%02x%02x @ %p (GXRModeObj)\n",
                    vidmode->vfilter[0], vidmode->vfilter[1], vidmode->vfilter[2], vidmode->vfilter[3],
                    vidmode->vfilter[4], vidmode->vfilter[5], vidmode->vfilter[6], addr_start);
            memcpy(vidmode->vfilter, vfilter, 7);
            addr_start += (sizeof(GXRModeObjRVL) - 4);
            len -= (sizeof(GXRModeObjRVL) - 4);
        }
        addr_start += 4;
        len -= 4;
    }
}

// Patch rogue vfilters found in some games
void patch_vfilters_rogue(u8 *addr, u32 len, u8 *vfilter)
{
    u8 known_vfilters[7][7] = {
        {8, 8, 10, 12, 10, 8, 8},
        {4, 8, 12, 16, 12, 8, 4},
        {7, 7, 12, 12, 12, 7, 7},
        {5, 5, 15, 14, 15, 5, 5},
        {4, 4, 15, 18, 15, 4, 4},
        {4, 4, 16, 16, 16, 4, 4},
        {2, 2, 17, 22, 17, 2, 2}
    };
    u8 *addr_start = addr;
    u8 *addr_end = addr + len - 8;
    while (addr_start <= addr_end)
    {
        u8 known_vfilter[7];
        for (int i = 0; i < 7; i++)
        {
            for (int x = 0; x < 7; x++)
                known_vfilter[x] = known_vfilters[i][x];
            if (!addr_start[7] && memcmp(addr_start, known_vfilter, 7) == 0)
            {
                gprintf("Replaced vfilter %02x%02x%02x%02x%02x%02x%02x @ %p\n", addr_start[0], addr_start[1],
                        addr_start[2], addr_start[3], addr_start[4], addr_start[5], addr_start[6], addr_start);
                memcpy(addr_start, vfilter, 7);
                addr_start += 7;
                break;
            }
        }
        addr_start += 1;
    }
}

static bool Search_and_patch_Video_To(void *Address, u32 Size, GXRModeObj *Table[], GXRModeObj *rmode, bool patchAll)
{
    u8 *Addr = (u8 *)Address;
    bool found = 0;
    u32 i;
    u8 target_vmode = 0;
    for (i = 0; i < sizeof(vmodes) / sizeof(vmodes[0]); i++)
    {
        if (compare_videomodes_ogc(Table[i], rmode))
        {
            target_vmode = i;
            break;
        }
    }

    while (Size >= sizeof(GXRModeObjRVL))
    {
        if ((memcmp(((GXRModeObjRVL *)Addr)->sample_pattern, PATTERN, 24) == 0 || memcmp(((GXRModeObjRVL *)Addr)->sample_pattern, PATTERN_AA, 24) == 0) &&
            (((GXRModeObjRVL *)Addr)->fbWidth == 640 || ((GXRModeObjRVL *)Addr)->fbWidth == 608 || ((GXRModeObjRVL *)Addr)->fbWidth == 512) &&
            (((GXRModeObjRVL *)Addr)->field_rendering == 0 || ((GXRModeObjRVL *)Addr)->field_rendering == 1) &&
            (((GXRModeObjRVL *)Addr)->aa == 0 || ((GXRModeObjRVL *)Addr)->aa == 1)
        )
        {
            // display found video mode patterns
            GXRModeObjRVL *vidmode = (GXRModeObjRVL *)Addr;
            gprintf("GXRModeObj \t%08x %04x %04x %04x %04x %04x %04x %04x %08x %04x %04x "
                    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
                    "%02x%02x%02x%02x%02x%02x%02x \n",
                    vidmode->viTVMode, vidmode->fbWidth, vidmode->efbHeight, vidmode->xfbHeight, vidmode->viXOrigin, vidmode->viYOrigin,
                    vidmode->viWidth, vidmode->viHeight, vidmode->xfbMode, vidmode->field_rendering, vidmode->aa,
                    vidmode->sample_pattern[0][0], vidmode->sample_pattern[1][0], vidmode->sample_pattern[2][0], vidmode->sample_pattern[3][0], vidmode->sample_pattern[4][0],
                    vidmode->sample_pattern[5][0], vidmode->sample_pattern[6][0], vidmode->sample_pattern[7][0], vidmode->sample_pattern[8][0], vidmode->sample_pattern[9][0],
                    vidmode->sample_pattern[10][0], vidmode->sample_pattern[11][0], vidmode->sample_pattern[0][1], vidmode->sample_pattern[1][1], vidmode->sample_pattern[2][1],
                    vidmode->sample_pattern[3][1], vidmode->sample_pattern[4][1], vidmode->sample_pattern[5][1], vidmode->sample_pattern[6][1], vidmode->sample_pattern[7][1],
                    vidmode->sample_pattern[8][1], vidmode->sample_pattern[9][1], vidmode->sample_pattern[10][1], vidmode->sample_pattern[11][1],
                    vidmode->vfilter[0], vidmode->vfilter[1], vidmode->vfilter[2], vidmode->vfilter[3], vidmode->vfilter[4], vidmode->vfilter[5], vidmode->vfilter[6]);

            found = 0;
            for (i = 0; i < sizeof(vmodes) / sizeof(vmodes[0]); i++)
            {
                if (compare_videomodes_rvl(Table[i], (GXRModeObjRVL *)Addr))
                {
                    found = 1;
                    gprintf("%s replaced with %s \n", vmodes_name[i], vmodes_name[target_vmode]);
                    patch_videomode((GXRModeObjRVL *)Addr, rmode);
                    Addr += (sizeof(GXRModeObjRVL) - 4);
                    Size -= (sizeof(GXRModeObjRVL) - 4);
                    break;
                }
            }
            if (patchAll && !found)
            {
                gprintf("Unknown replaced with %s \n", vmodes_name[target_vmode]);
                patch_videomode((GXRModeObjRVL *)Addr, rmode);
                Addr += (sizeof(GXRModeObjRVL) - 4);
                Size -= (sizeof(GXRModeObjRVL) - 4);
            }
        }
        Addr += 4;
        Size -= 4;
    }

    return found;
}

void VideoModePatcher(u8 *dst, int len, u8 videoSelected, u8 VideoPatchDol)
{
    GXRModeObj **table = NULL;
    if (videoSelected == VIDEO_MODE_PATCH) // patch enum'd in cfg.h
    {
        switch (CONF_GetVideo())
        {
            case CONF_VIDEO_PAL:
                table = CONF_GetEuRGB60() > 0 ? NTSC2PAL60 : NTSC2PAL;
                break;
            case CONF_VIDEO_MPAL:
                table = NTSC2PAL;
                break;
            default:
                table = PAL2NTSC;
                break;
        }
        Search_and_patch_Video_Modes(dst, len, table);
    }
    else if (VideoPatchDol == VIDEO_PATCH_DOL_REGION) //&& rmode != NULL)
    {
        switch (rmode->viTVMode >> 2)
        {
            case VI_PAL:
            case VI_MPAL:
                table = NTSC2PAL;
                break;
            case VI_EURGB60:
                table = NTSC2PAL60;
                break;
            default:
                table = PAL2NTSC;
        }
        Search_and_patch_Video_Modes(dst, len, table);
    }
    else if (VideoPatchDol == VIDEO_PATCH_DOL_ON && rmode != NULL)
    {
        Search_and_patch_Video_To(dst, len, vmodes, rmode, false);
    }
    else if (VideoPatchDol == VIDEO_PATCH_DOL_ALL && rmode != NULL)
    {
        Search_and_patch_Video_To(dst, len, vmodes, rmode, true);
    }
}

void sneek_video_patch(void *addr, u32 len)
{
    u8 *addr_start = addr;
    u8 *addr_end = addr + len;

    while (addr_start < addr_end)
    {
        if (*(vu32 *)(addr_start) == 0x3C608000)
        {
            if (((*(vu32 *)(addr_start + 4) & 0xFC1FFFFF) == 0x800300CC) && ((*(vu32 *)(addr_start + 8) >> 24) == 0x54))
            {
                *(vu32 *)(addr_start + 4) = 0x5400F0BE | ((*(vu32 *)(addr_start + 4) & 0x3E00000) >> 5);
            }
        }
        addr_start += 4;
    }
}

// giantpune's magic super patch to return to channels

static u32 ad[4] = {0, 0, 0, 0}; // these variables are global on the off chance the different parts needed
static u8 found = 0;             // to find in the dol are found in different sections of the dol
static u8 returnToPatched = 0;

bool PatchReturnTo(void *Address, int Size, u32 id)
{
    if (!id || returnToPatched)
        return 0;
    //gprintf("PatchReturnTo( %p, %08x, %08x )\n", Address, Size, id );

    // new __OSLoadMenu() (SM2.0 and higher)
    u8 SearchPattern[12] = {0x38, 0x80, 0x00, 0x02, 0x38, 0x60, 0x00, 0x01, 0x38, 0xa0, 0x00, 0x00}; // li r4,2
    // li r3,1
    // li r5,0
    // old _OSLoadMenu() (used in launch games)
    u8 SearchPatternB[12] = {0x38, 0xC0, 0x00, 0x02, 0x38, 0xA0, 0x00, 0x01, 0x38, 0xE0, 0x00, 0x00}; // li r6,2
    // li r5,1
    // li r7,0
    // identifier for the safe place
    u8 SearchPattern2[12] = {0x4D, 0x65, 0x74, 0x72, 0x6F, 0x77, 0x65, 0x72, 0x6B, 0x73, 0x20, 0x54}; // "Metrowerks T"

    u8 oldSDK = 0;
    found = 0;

    void *Addr = Address;
    void *Addr_end = Address + Size;

    while (Addr <= Addr_end - 12)
    {
        // find a safe place for the patch to hang out
        if (!ad[3] && memcmp(Addr, SearchPattern2, 12) == 0)
        {
            ad[3] = (u32)Addr + 0x30;
        }
        // find __OSLaunchMenu() and remember some addresses in it
        else if (memcmp(Addr, SearchPattern, 12) == 0)
        {
            ad[found++] = (u32)Addr;
        }
        else if (ad[0] && memcmp(Addr, SearchPattern, 8) == 0) // after the first match is found, only search the first 8 bytes for the other 2
        {
            if (!ad[1])
                ad[found++] = (u32)Addr;
            else if (!ad[2])
                ad[found++] = (u32)Addr;
            if (found >= 3)
                break;
        }
        Addr += 4;
    }
    // check for the older-ass version of the SDK
    if (found < 3 && ad[3])
    {
        Addr = Address;
        ad[0] = 0;
        ad[1] = 0;
        ad[2] = 0;
        found = 0;
        oldSDK = 1;

        while (Addr <= Addr_end - 12)
        {
            // find __OSLaunchMenu() and remember some addresses in it
            if (memcmp(Addr, SearchPatternB, 12) == 0)
            {
                ad[found++] = (u32)Addr;
            }
            else if (ad[0] && memcmp(Addr, SearchPatternB, 8) == 0) // after the first match is found, only search the first 8 bytes for the other 2
            {
                if (!ad[1])
                    ad[found++] = (u32)Addr;
                else if (!ad[2])
                    ad[found++] = (u32)Addr;
                if (found >= 3)
                    break;
            }
            Addr += 4;
        }
    }

    // if the function is found
    if (found == 3 && ad[3])
    {
        //gprintf("patch __OSLaunchMenu( 0x00010001, 0x%08x )\n", id);
        u32 nop = 0x60000000;

        // the magic that writes the TID to the registers
        u8 jump[20] = {
            0x3C, 0x60, 0x00, 0x01,                     // lis r3,1
            0x60, 0x63, 0x00, 0x01,                     // ori r3,r3,1
            0x3C, 0x80, (u8)(id >> 24), (u8)(id >> 16), // lis r4,(u16)(tid >> 16)
            0x60, 0x84, (u8)(id >> 8), (u8)id,          // ori r4,r4,(u16)(tid)
            0x4E, 0x80, 0x00, 0x20};                    // blr

        if (oldSDK)
        {
            jump[1] = 0xA0;  // 3CA00001 // lis r5,1
            jump[5] = 0xA5;  // 60A50001 // ori r5,r5,1
            jump[9] = 0xC0;  // 3CC0AF1B // lis r6,(u16)(tid >> 16)
            jump[13] = 0xC6; // 60C6F516 // ori r6,r6,(u16)(tid)
        }

        void *addr = (u32 *)ad[3];

        // write new stuff to in a unused part of the main.dol
        memcpy(addr, jump, sizeof(jump));

        // ES_GetTicketViews()
        u32 newval = (ad[3] - ad[0]);
        newval &= 0x03FFFFFC;
        newval |= 0x48000001;
        addr = (u32 *)ad[0];
        memcpy(addr, &newval, sizeof(u32));  // bl ad[ 3 ]
        memcpy(addr + 4, &nop, sizeof(u32)); // nop
        // gprintf("\t%08x -> %08x\n", addr, newval );

        // ES_GetTicketViews() again
        newval = (ad[3] - ad[1]);
        newval &= 0x03FFFFFC;
        newval |= 0x48000001;
        addr = (u32 *)ad[1];
        memcpy(addr, &newval, sizeof(u32));  // bl ad[ 3 ]
        memcpy(addr + 4, &nop, sizeof(u32)); // nop
        // gprintf("\t%08x -> %08x\n", addr, newval );

        // ES_LaunchTitle()
        newval = (ad[3] - ad[2]);
        newval &= 0x03FFFFFC;
        newval |= 0x48000001;
        addr = (u32 *)ad[2];
        memcpy(addr, &newval, sizeof(u32));  // bl ad[ 3 ]
        memcpy(addr + 4, &nop, sizeof(u32)); // nop
        // gprintf("\t%08x -> %08x\n", addr, newval );

        returnToPatched = 1;
    }

    if (returnToPatched)
        gprintf("Return to %08X patched with old method.\n", (u32)id);

    return returnToPatched;
}

int PatchNewReturnTo(int es_fd, u64 title)
{
    if (es_fd < 0 || title == 0)
        return -1;

    //! this is here for test purpose only and needs be moved later
    static u64 sm_title_id ATTRIBUTE_ALIGN(32);
    ioctlv *vector = (ioctlv *)memalign(32, sizeof(ioctlv));
    if (!vector)
        return -1;

    sm_title_id = title;
    vector[0].data = &sm_title_id;
    vector[0].len = sizeof(sm_title_id);

    int result = -1;

    if (es_fd >= 0)
        result = IOS_Ioctlv(es_fd, 0xA1, 1, 0, vector);

    if (result >= 0)
        gprintf("Return to %08X patched with d2x method.\n", (u32)title);

    free(vector);

    return result;
}

int BlockIOSReload(int es_fd, u32 gameIOS)
{
    if (es_fd < 0)
        return -1;

    static int mode ATTRIBUTE_ALIGN(32);
    static int ios ATTRIBUTE_ALIGN(32);
    ioctlv *vector = (ioctlv *)memalign(32, sizeof(ioctlv) * 2);
    if (!vector)
        return -1;

    int inlen = 2;
    mode = 2;
    ios = gameIOS; // ios to be reloaded in place of the requested one
    vector[0].data = &mode;
    vector[0].len = 4;
    vector[1].data = &ios;
    vector[1].len = 4;

    int result = -1;

    if (es_fd >= 0)
        result = IOS_Ioctlv(es_fd, 0xA0, inlen, 0, vector);

    if (result >= 0)
        gprintf("Block IOS Reload patched with d2x method to IOS%i; result: %i\n", gameIOS, result);

    free(vector);

    return result;
}

void PatchAspectRatio(void *addr, u32 len, u8 aspect)
{
    if (aspect > 1)
        return;

    static const u32 aspect_searchpattern1[5] = {
        0x9421FFF0, 0x7C0802A6, 0x38800001, 0x90010014, 0x38610008};

    static const u32 aspect_searchpattern2[15] = {
        0x2C030000, 0x40820010, 0x38000000, 0x98010008, 0x48000018,
        0x88010008, 0x28000001, 0x4182000C, 0x38000000, 0x98010008,
        0x80010014, 0x88610008, 0x7C0803A6, 0x38210010, 0x4E800020};

    u8 *addr_start = (u8 *)addr;
    u8 *addr_end = addr_start + len - sizeof(aspect_searchpattern1) - 4 - sizeof(aspect_searchpattern2);

    while (addr_start < addr_end)
    {
        if ((memcmp(addr_start, aspect_searchpattern1, sizeof(aspect_searchpattern1)) == 0) &&
            (memcmp(addr_start + 4 + sizeof(aspect_searchpattern1), aspect_searchpattern2, sizeof(aspect_searchpattern2)) == 0))
        {
            *((u32 *)(addr_start + 0x44)) = (0x38600000 | aspect);
            gprintf("Aspect ratio patched to: %s\n", aspect ? "16:9" : "4:3");
            break;
        }
        addr_start += 4;
    }
}
