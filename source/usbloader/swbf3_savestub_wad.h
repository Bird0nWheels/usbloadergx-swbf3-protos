#ifndef SWBF3_SAVESTUB_WAD_H_
#define SWBF3_SAVESTUB_WAD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <gccore.h>

extern const u8  swbf3_savestub_wad[];
extern const u32 swbf3_savestub_wad_size;
extern const u32 swbf3_savestub_cert_off;
extern const u32 swbf3_savestub_cert_size;
extern const u32 swbf3_savestub_ticket_off;
extern const u32 swbf3_savestub_ticket_size;
extern const u32 swbf3_savestub_tmd_off;
extern const u32 swbf3_savestub_tmd_size;
extern const u32 swbf3_savestub_content_off;
extern const u32 swbf3_savestub_content_size;

/* Returns 0 if the stub title 00010000-30303030 is already installed in NAND
 * (so no work to do) or was installed successfully this call.  Negative on
 * unrecoverable error.  Idempotent: safe to call on every disc boot. */
int swbf3_savestub_install_if_missing(void);

#ifdef __cplusplus
}
#endif

#endif
