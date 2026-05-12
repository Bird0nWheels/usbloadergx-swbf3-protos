/* SWBF3 dev-disc save stub: install title 00010000-30303030 from an
 * embedded fakesigned WAD if it isn't already in NAND.
 *
 * This lets the GX boot.dol be self-contained on a freshly-set-up Wii or
 * Wii U / vWii -- no manual WAD-manager step required for the save dir to
 * exist before the running-TID switch in GameBooter::BootPartition.
 *
 * The trucha-bug fakesigned ticket+TMD are only accepted by cIOS d2x
 * (v11-beta3+ recommended).  On Wii U / vWii that means an installed
 * vWii d2x cIOS; on Wii, base IOS 37 or 38 with d2x.
 */

#include <gccore.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ogc/es.h>
#include <ogc/isfs.h>
#include <ogc/ipc.h>
#include <malloc.h>

#include "swbf3_savestub_wad.h"
#include "gecko.h"

#define SWBF3_STUB_TITLE_UPPER 0x00010000u
#define SWBF3_STUB_TITLE_LOWER 0x30303030u
#define SWBF3_STUB_TITLEID     0x0001000030303030ULL

static bool stub_tmd_present_in_nand(void)
{
    s32 fd = ISFS_Open(
        "/title/00010000/30303030/content/title.tmd", ISFS_OPEN_READ);
    if (fd >= 0) { ISFS_Close(fd); return true; }
    return false;
}

/* Read content_id and content_size out of the 1-content TMD we built. */
static void parse_first_content_record(const u8 *tmd, u32 *out_cid, u32 *out_size)
{
    const u8 *rec = tmd + 0x1E4;
    *out_cid  = (rec[0] << 24) | (rec[1] << 16) | (rec[2] << 8) | rec[3];
    /* 8-byte BE content size, but our content is 64 bytes, top 4 are zero */
    *out_size = (rec[12] << 24) | (rec[13] << 16) | (rec[14] << 8) | rec[15];
}

int swbf3_savestub_install_if_missing(void)
{
    s32 r;

    r = ISFS_Initialize();
    gprintf("savestub: ISFS_Initialize ret=%d\n", r);

    if (stub_tmd_present_in_nand())
    {
        gprintf("savestub: title 00010000-30303030 already in NAND, skipping install\n");
        return 0;
    }

    gprintf("savestub: installing embedded WAD (%u bytes)\n",
            swbf3_savestub_wad_size);

    /* Aligned views into the embedded WAD.  IOS ioctl payloads need 32-byte
     * alignment; the wad-as-array is declared ATTRIBUTE_ALIGN(32) and each
     * 64-byte-aligned WAD section is therefore 32-aligned too. */
    const signed_blob *certs   = (const signed_blob *)(swbf3_savestub_wad + swbf3_savestub_cert_off);
    const signed_blob *ticket  = (const signed_blob *)(swbf3_savestub_wad + swbf3_savestub_ticket_off);
    const signed_blob *tmd     = (const signed_blob *)(swbf3_savestub_wad + swbf3_savestub_tmd_off);
    const u8          *content = swbf3_savestub_wad + swbf3_savestub_content_off;

    r = ES_AddTicket(ticket, swbf3_savestub_ticket_size,
                     certs, swbf3_savestub_cert_size, NULL, 0);
    gprintf("savestub: ES_AddTicket ret=%d\n", r);
    if (r < 0) return r;

    r = ES_AddTitleStart(tmd, swbf3_savestub_tmd_size,
                         certs, swbf3_savestub_cert_size, NULL, 0);
    gprintf("savestub: ES_AddTitleStart ret=%d\n", r);
    if (r < 0) return r;

    u32 cid, csize;
    parse_first_content_record(swbf3_savestub_wad + swbf3_savestub_tmd_off,
                               &cid, &csize);

    s32 cfd = ES_AddContentStart(SWBF3_STUB_TITLEID, cid);
    gprintf("savestub: ES_AddContentStart(cid=%u) ret=%d\n", cid, cfd);
    if (cfd < 0) { ES_AddTitleCancel(); return cfd; }

    /* ES_AddContentData requires the buffer be 32-byte aligned and a
     * multiple of 32 long.  Our content is 64 zero bytes, already aligned
     * because the embedded array is 32-aligned and starts at a 64-aligned
     * offset.  Copy to a fresh aligned buffer to be safe and survive any
     * future content-size changes. */
    void *aligned = memalign(32, (csize + 31) & ~31u);
    if (!aligned) { ES_AddContentFinish(cfd); ES_AddTitleCancel(); return IPC_ENOMEM; }
    memset(aligned, 0, (csize + 31) & ~31u);
    memcpy(aligned, content, csize);

    r = ES_AddContentData(cfd, (u8 *)aligned, csize);
    gprintf("savestub: ES_AddContentData(%u) ret=%d\n", csize, r);
    free(aligned);
    if (r < 0) { ES_AddContentFinish(cfd); ES_AddTitleCancel(); return r; }

    r = ES_AddContentFinish(cfd);
    gprintf("savestub: ES_AddContentFinish ret=%d\n", r);
    if (r < 0) { ES_AddTitleCancel(); return r; }

    r = ES_AddTitleFinish();
    gprintf("savestub: ES_AddTitleFinish ret=%d\n", r);
    if (r < 0) return r;

    /* Verify install: TMD should now be readable from NAND. */
    if (stub_tmd_present_in_nand())
    {
        gprintf("savestub: install OK, TMD now present in NAND\n");
        return 0;
    }
    gprintf("savestub: install reported OK but TMD not in NAND -- unexpected\n");
    return -1;
}
