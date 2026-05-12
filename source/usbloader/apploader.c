#include <stdio.h>
#include <string.h>
#include <ogcsys.h>

#include "apploader.h"
#include "wdvd.h"
#include "wpad.h"
#include "alternatedol.h"
#include "fstfile.h"
#include "gecko.h"
#include "memory/memory.h"
#include "patches/gamepatches.h"
#include "patches/wip.h"
#include "settings/SettingsEnums.h"

/* Apploader function pointers */
typedef int (*app_main)(void **dst, int *size, int *offset);
typedef void (*app_init_cb)(const char *fmt, ...);
typedef void (*app_init)(app_init_cb);
typedef void *(*app_final)();
typedef void (*app_entry)(app_init *init, app_main *main, app_final *final);

/* Apploader pointers */
static u8 *appldr = (u8 *) 0x81200000;

/* Constants */
#define APPLDR_OFFSET   0x2440

/* Variables */
static u32 buffer[0x20] ATTRIBUTE_ALIGN( 32 );

/* ===========================================================================
 * Dev-disc support: optional DirectLoad bypass.
 *
 * Some prototype/dev discs ship an apploader that misbehaves when invoked
 * via cIOS's redirected disc interface (works in Dolphin, hangs on real
 * hardware).  As a debug escape hatch, we can read boot.bin/main.dol/FST
 * straight off the partition and set MEM1 up like a stock SDK apploader
 * would, then return the DOL entry point.
 *
 * In practice most dev discs need the apploader's resident OS runtime in
 * MEM1 to actually function (the game DOL calls into SDK code that the
 * apploader's trailer placed there), so DirectLoad rarely wins.  Kept
 * opt-in for diagnostic use.
 *
 * Activate by creating empty file `sd:/<gameid>_USE_DIRECT` (e.g.
 * `sd:/RABAZZ_USE_DIRECT`).  Default = on-disc apploader path.
 *
 * The DOL header layout we parse here is the standard Wii/GC format:
 *   0x00 7   text section file offsets
 *   0x1C 11  data section file offsets
 *   0x48 7   text section memory addresses
 *   0x64 11  data section memory addresses
 *   0x90 7   text section sizes
 *   0xAC 11  data section sizes
 *   0xD8     BSS address
 *   0xDC     BSS size
 *   0xE0     entry point
 * =========================================================================== */
static u8 g_boot_buf[0x440] ATTRIBUTE_ALIGN(32);
static u8 g_bi2_buf[0x2000] ATTRIBUTE_ALIGN(32);
static u8 g_dol_hdr[0x100] ATTRIBUTE_ALIGN(32);

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static s32 Apploader_DirectLoad(entry_point *entry)
{
	s32 ret;

	gprintf("DirectLoad: bypassing disc apploader\n");

	/* boot.bin holds DOL/FST offsets at 0x420+ (each field is bytes>>2) */
	ret = WDVD_Read(g_boot_buf, sizeof(g_boot_buf), 0);
	if (ret < 0) { gprintf("DirectLoad: boot.bin read fail %d\n", ret); return ret; }

	u32 dol_off = be32(g_boot_buf + 0x420) << 2;
	u32 fst_off = be32(g_boot_buf + 0x424) << 2;
	u32 fst_sz  = be32(g_boot_buf + 0x428) << 2;
	u32 fst_max = be32(g_boot_buf + 0x42C) << 2;
	gprintf("DirectLoad: dol@0x%08x fst@0x%08x fst_sz=%u fst_max=%u\n",
	        dol_off, fst_off, fst_sz, fst_max);

	/* Compute SDK-standard layout, top-down:
	 *   FST  at 0x81800000 - fst_max_aligned    (topmost block of MEM1)
	 *   BI2  at FST - 0x2000                     (immediately below FST)
	 * Place the BI2 data and publish its pointer at *0x800000F4. */
	u32 fst_max_aligned_pre = (fst_max + 31) & ~31u;
	void *bi2_dst = (void *)(0x81800000u - fst_max_aligned_pre - 0x2000u);
	ret = WDVD_Read(g_bi2_buf, sizeof(g_bi2_buf), 0x440);
	if (ret < 0) { gprintf("DirectLoad: bi2 read fail %d\n", ret); return ret; }
	memcpy(bi2_dst, g_bi2_buf, sizeof(g_bi2_buf));
	DCFlushRange(bi2_dst, sizeof(g_bi2_buf));
	*BI2 = (u32)bi2_dst;

	/* DOL header */
	ret = WDVD_Read(g_dol_hdr, sizeof(g_dol_hdr), dol_off);
	if (ret < 0) { gprintf("DirectLoad: dol_hdr read fail %d\n", ret); return ret; }

	u32 fileOffsets[18], memAddrs[18], sizes[18];
	for (int i = 0; i < 18; i++)
	{
		fileOffsets[i] = be32(g_dol_hdr + 0x00 + i * 4);
		memAddrs[i]    = be32(g_dol_hdr + 0x48 + i * 4);
		sizes[i]       = be32(g_dol_hdr + 0x90 + i * 4);
	}
	u32 bssAddr   = be32(g_dol_hdr + 0xD8);
	u32 bssSize   = be32(g_dol_hdr + 0xDC);
	u32 entryAddr = be32(g_dol_hdr + 0xE0);
	gprintf("DirectLoad: entry=0x%08x bss@0x%08x size=%u\n", entryAddr, bssAddr, bssSize);

	/* Load each section directly to its mapped MEM1 address. */
	for (int i = 0; i < 18; i++)
	{
		if (!sizes[i] || !fileOffsets[i] || !memAddrs[i])
			continue;
		void *dst = (void *)memAddrs[i];
		gprintf("DirectLoad: sec %2d  dst=%p len=%u file_off=0x%x\n",
		        i, dst, sizes[i], fileOffsets[i]);
		ret = WDVD_Read(dst, sizes[i], dol_off + fileOffsets[i]);
		if (ret < 0) { gprintf("  WDVD_Read fail %d\n", ret); return ret; }
		RegisterDOL((u8 *)dst, sizes[i]);
		DCFlushRange(dst, sizes[i]);
		ICInvalidateRange(dst, sizes[i]);
	}

	/* Clear BSS - the apploader normally does this implicitly. */
	if (bssSize && bssAddr)
	{
		memset((void *)bssAddr, 0, bssSize);
		DCFlushRange((void *)bssAddr, bssSize);
	}

	/* Place FST at top of MEM1 and publish FST pointer + max size.
	 * Set ArenaHi to the FST address (matches what the SDK apploader
	 * sets in the live in-menu Dolphin snapshot). */
	u32 fst_max_aligned = fst_max_aligned_pre;
	void *fst_dst = (void *)(0x81800000u - fst_max_aligned);
	ret = WDVD_Read(fst_dst, (fst_sz + 31) & ~31u, fst_off);
	if (ret < 0) { gprintf("DirectLoad: fst read fail %d\n", ret); return ret; }
	DCFlushRange(fst_dst, fst_max_aligned);
	*FST = (u32)fst_dst;
	*Max_FST = fst_max;
	*Arena_H = (u32)fst_dst;

	/* SDK heap top, set by the apploader.  Live-Dolphin snapshot shows
	 * *0x80003110 == FST address.  Used by libogc/SDK as the post-OS-init
	 * arena ceiling. */
	*(vu32 *)0x80003110 = (u32)fst_dst;

	/* Standard SDK low-memory globals.  The game's __OSInit checks
	 * Sys_Magic (0x80000020) and bails silently if it isn't 0x0D15EA5E.
	 * Bus/CPU speed are required by udelay/IPC timeouts.  Disc_SetLowMem()
	 * already populates these for normal apploader runs; DirectLoad needs
	 * to do it itself. */
	*(vu32 *)0x80000018 = 0x5D1C9EA3;  /* Disc_Magic    (Wii)        */
	*(vu32 *)0x80000020 = 0x0D15EA5E;  /* Sys_Magic                  */
	*(vu32 *)0x80000024 = 0x00000001;  /* Sys_Version                */
	*(vu32 *)0x80000028 = 0x01800000;  /* Mem_Size = 24 MB           */
	*(vu32 *)0x8000002C = 0x00000023;  /* Board_Model = retail prod  */
	*(vu32 *)0x800000F0 = 0x01800000;  /* Simulated_Mem              */
	*(vu32 *)0x800000F8 = 0x0E7BE2C0;  /* Bus_Speed  = 243 MHz       */
	*(vu32 *)0x800000FC = 0x2B73A840;  /* CPU_Speed  = 729 MHz       */

	DCFlushRange((void *)0x80000000, 0x100);
	DCFlushRange((void *)0x80003100, 0x20);

	gprintf("DirectLoad: BI2@%p  FST@%p sz=%u max=%u  ArenaHi=FST  "
	        "Sys_Magic=0x0D15EA5E  Mem_Size=24M  Bus=243MHz CPU=729MHz\n",
	        bi2_dst, fst_dst, fst_sz, fst_max);

	*entry = (entry_point)entryAddr;
	gprintf("DirectLoad: done, entry=0x%08x\n", entryAddr);
	return 0;
}

s32 Apploader_Run(entry_point *entry, char * dolpath, u8 alternatedol, u32 alternatedoloffset)
{
	app_entry appldr_entry = NULL;
	app_init appldr_init = NULL;
	app_main appldr_main = NULL;
	app_final appldr_final = NULL;

	/* Per-disc opt-in DirectLoad bypass.  Activate by creating an empty
	 * file `sd:/<gameid>_USE_DIRECT` where <gameid> is the 6-char disc ID
	 * GX shows in its menu (e.g. `sd:/RABAZZ_USE_DIRECT`).  Default
	 * (file absent) = normal on-disc apploader path. */
	{
		char toggle_path[40];
		char gameid[7];
		memcpy(gameid, (const void *)Disc_ID, 6);
		gameid[6] = '\0';
		snprintf(toggle_path, sizeof(toggle_path), "sd:/%s_USE_DIRECT", gameid);
		FILE *toggle = fopen(toggle_path, "rb");
		if (toggle) {
			fclose(toggle);
			gprintf("Apploader_Run: %s -> DirectLoad bypass (opt-in via %s)\n",
			        gameid, toggle_path);
			return Apploader_DirectLoad(entry);
		}
	}

	void *dst = NULL;
	int len = 0;
	int offset = 0;
	u32 appldr_len;
	s32 ret;
	gprintf("\nApploader_Run() started\n");

	/* Read apploader header */
	ret = WDVD_Read(buffer, 0x20, APPLDR_OFFSET);
	if (ret < 0) return ret;

	/* Calculate apploader length */
	appldr_len = buffer[5] + buffer[6];

	/* Read apploader code */
	ret = WDVD_Read(appldr, appldr_len, APPLDR_OFFSET + 0x20);
	if (ret < 0) return ret;

	/* Dev-disc apploader fix: production-mode DOL section boundary.
	 *
	 * Dev-SDK apploaders embed a "production mode" boundary check that
	 * rejects DOL sections above 0x80900000 (~9 MB into MEM1).  Some dev
	 * builds (e.g. Star Wars Battlefront 3 r2.91120a prototype) have
	 * sections above that and trigger:
	 *    "APPLOADER ERROR >>> ... should not exceed 0x80900000 (production mode)"
	 *
	 * Patch every `lis r?, 0x8090` to `lis r?, 0x8180` in the loaded
	 * apploader code, raising the boundary to the full MEM1 top
	 * (0x81800000) before any apploader callback runs.  Harmless for
	 * retail games (their sections are far below 0x80900000 so the
	 * check would have passed either way). */
	{
		u32 *p = (u32 *)appldr;
		u32 patches = 0;
		for (u32 i = 0; i < appldr_len / 4; i++) {
			u32 ins = p[i];
			/* lis rT, 0x8090  ==  addis rT, 0, 0x8090
			 *   opcode (bits 0-5)   = 15
			 *   rA     (bits 16-20) = 0
			 *   imm    (bits 16-31) = 0x8090
			 */
			if ((ins >> 26) == 15
			 && ((ins >> 16) & 0x1F) == 0
			 && (ins & 0xFFFF) == 0x8090) {
				p[i] = (ins & 0xFFFF0000) | 0x8180;
				patches++;
			}
		}
		if (patches)
			gprintf("Apploader_Run: raised production boundary check "
			        "(0x8090->0x8180) at %u call site(s)\n", patches);
	}

	/* Flush memory */
	DCFlushRange(appldr, appldr_len);
	ICInvalidateRange(appldr, appldr_len);

	/* Set apploader entry function */
	appldr_entry = (app_entry) buffer[4];

	/* Call apploader entry */
	appldr_entry(&appldr_init, &appldr_main, &appldr_final);

	/* Initialize apploader */
	appldr_init(gprintf);

	int sec_idx = 0;
	while(appldr_main(&dst, &len, &offset))
	{
		gprintf("appldr sec %d: dst=%p len=%d off=0x%llx\n",
		        sec_idx++, dst, len, (u64)(offset << 2));
		/* Read data from DVD */
		s32 rd = WDVD_Read(dst, len, (u64) (offset << 2));
		if (rd < 0)
			gprintf("  WDVD_Read FAILED ret=%d\n", rd);

		RegisterDOL((u8 *) dst, len);

		DCFlushRange(dst, len);
		ICInvalidateRange(dst, len);
	}
	gprintf("appldr_main loop done after %d sections\n", sec_idx);

	*entry = appldr_final();
	gprintf("appldr_final entry=%p\n", *entry);

	/* Dev-disc support: clamp SDK-reported MEM-size globals to actual
	 * retail-Wii hardware (24 MB MEM1 + 64 MB MEM2).  Some dev-disc
	 * apploaders (built for NDEV with extended RAM) leave these set to
	 * NDEV values like 96 MB / 128 MB.  When the game's __OSInit reads
	 * those it creates heap arenas larger than hardware, and the first
	 * allocation past the real boundary crashes.  Standard symptom is
	 * the SDK eject-error screen (e.g. "Error #001 - unauthorized
	 * device has been detected" -- the SDK uses that catch-all message
	 * for "this hardware cannot run me").
	 *
	 * Clamp is safe for retail games: their apploaders never set these
	 * above 24/64 MB so the clamp is a no-op for them. */
	{
		u32 mem1 = *(vu32 *)0x80000028;
		u32 mem2 = *(vu32 *)0x80003118;
		bool changed = false;
		if (mem1 > 0x01800000) {
			gprintf("Mem_Size  clamp: 0x%08x -> 0x01800000 (retail 24 MB)\n", mem1);
			*(vu32 *)0x80000028 = 0x01800000;
			*(vu32 *)0x800000F0 = 0x01800000;  /* Simulated_Mem  */
			changed = true;
		}
		if (mem2 > 0x04000000) {
			gprintf("Mem2_Size clamp: 0x%08x -> 0x04000000 (retail 64 MB)\n", mem2);
			*(vu32 *)0x80003118 = 0x04000000;
			changed = true;
		}
		if (changed) {
			DCFlushRange((void *)0x80000020, 0x100);
			DCFlushRange((void *)0x80003100, 0x40);
		}
	}

	/** Load alternate dol if set **/
	if (alternatedol == ALT_DOL_FROM_SD_USB)
	{
		ClearDOLList();
		wip_reset_counter();
		void *dolbuffer = NULL;
		int dollen = 0;

		bool dolloaded = Load_Dol(&dolbuffer, &dollen, dolpath);
		if (dolloaded)
			*entry = (entry_point) load_dol_image(dolbuffer);

		if (dolbuffer) free(dolbuffer);
	}
	else if (alternatedol == ALT_DOL_FROM_GAME && alternatedoloffset != 0)
	{
		ClearDOLList();
		wip_reset_counter();
		FST_ENTRY *fst = (FST_ENTRY *) *(u32 *) 0x80000038;

		if (!fst)
			return -1;
		//! Check if it's inside the limits
		if(alternatedoloffset >= fst[0].filelen)
			return 0;

		*entry = (entry_point) Load_Dol_from_disc(fst[alternatedoloffset].fileoffset);
	}

	return 0;
}
