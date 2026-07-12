#ifndef GAMEPATCHES_H_
#define GAMEPATCHES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <gccore.h>

bool exclude_game(u8 *gameid, bool checkEmuNAND);
void RegisterDOL(u8 *dst, int len);
void ClearDOLList();

/* Per-game "Gecko TTY (USB)" toggle. Set by GameBooter from the resolved
 * GameCFG/global setting before gamepatches() runs; when non-zero the game-side
 * OSReport/serial->USB-Gecko TTY hooks install (same as the legacy sd:/GECKO_TTY
 * file, which still works as a fallback). */
extern int GeckoTTYEnabled;
void gamepatches(u8 videoSelected, u8 videoPatchDol, u8 aspectForce, u8 languageChoice, u8 patchcountrystring,
                 u8 vipatch, u8 deflicker, u8 disableMotor, u8 disableSpeaker,
                 u8 sneekVideoPatch, u8 hooktype, u8 videoWidth, u64 returnTo, u8 privateServer, const char *serverAddr);
void anti_002_fix(u8 *addr, u32 len);
void swbf3_mem2_check_fix(u8 *addr, u32 len);
void swbf3_instant_action_null_fix(u8 *addr, u32 len);
void swbf3_campaign_lookup_fix(u8 *addr, u32 len);
void swbf3_campaign_loop_fix(u8 *addr, u32 len);
void swbf3_campaign_vec3_guard_fix(u8 *addr, u32 len);
void swbf3_campaign_transform_null_fix(u8 *addr, u32 len);
void swbf3_unlock_cheat_fix(u8 *addr, u32 len);
void swbf3_havok_alloc_fail_fix(u8 *addr, u32 len);
void swbf3_havok_slab_loop_skip_fix(u8 *addr, u32 len);
void swbf3_havok_caller_ptr_guard_fix(u8 *addr, u32 len);
void swbf3_havok_cutscene_null_guard(u8 *addr, u32 len);
void swbf3_havok_psq_stx_null_guard(u8 *addr, u32 len);
void swbf3_dantooine_string_scan_guard(u8 *addr, u32 len);
void swbf3_dantooine_vfn_r3_guard(u8 *addr, u32 len);
void swbf3_dantooine_array_data_guard(u8 *addr, u32 len);
void swbf3_havok_grow_slab_pool(u8 *addr, u32 len);
void swbf3_havok_freelist_guard(u8 *addr, u32 len);
void swbf3_havok_large_alloc_guard(u8 *addr, u32 len);
void swbf3_havok_small_fallback_guard(u8 *addr, u32 len);
void swbf3_dantooine_lbz_guard(u8 *addr, u32 len);
void swbf3_ndev_bat_setup_fix(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r6_guard(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r0_null_guard(u8 *addr, u32 len);
void swbf3_dantooine_setter_null_guard(u8 *addr, u32 len);
void swbf3_dantooine_lhz_sda_null_guard(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r5_guard(u8 *addr, u32 len);
void swbf3_dantooine_r27_r28_guard(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r3_178_guard(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r26_178_guard(u8 *addr, u32 len);
void swbf3_dantooine_vtable_null_guard(u8 *addr, u32 len);
void swbf3_dantooine_skip_vfn_block_fix(u8 *addr, u32 len);
void swbf3_dantooine_lwz_r3_3C_guard(u8 *addr, u32 len);
void swbf3_dantooine_epilogue_r10_guard(u8 *addr, u32 len);
void swbf3_dantooine_entry_save_callee_regs(u8 *addr, u32 len);
void swbf3_dantooine_callsite_r23_preserve(u8 *addr, u32 len);
void swbf3_havok_node_init_null_skip(u8 *addr, u32 len);
void swbf3_havok_r31_block_bypass_fix(u8 *addr, u32 len);
void swbf3_havok_field4_force_alt_fix(u8 *addr, u32 len);
void swbf3_havok_field4_vfn_skip_fix(u8 *addr, u32 len);
void deflicker_patch(u8 *addr, u32 len);
void patch_vfilters(u8 *addr, u32 len, u8 *vfilter);
void patch_vfilters_rogue(u8 *addr, u32 len, u8 *vfilter);
void patch_width(u8 *addr, u32 len);
void speaker_patch(u8 *addr, u32 len);
void motor_patch(u8 *addr, u32 len);
void PrivateServerPatcher(void *addr, u32 len, u8 privateServer, const char *serverAddr);
void PatchFix480p();
s8 do_new_wiimmfi();
s8 do_new_wiimmfi_nonMKWii(void *addr, u32 len);
void domainpatcher(void *addr, u32 len, const char *domain);
bool patch_nsmb(u8 *gameid);
bool patch_pop(u8 *gameid);
void patch_re4(u8 *gameid);
void patch_sdcard(u8 *gameid);
void patch_error_codes(u8 *gameid);
void VideoModePatcher(u8 *dst, int len, u8 videoSelected, u8 VideoPatchDol);
void sneek_video_patch(void *addr, u32 len);
bool PatchReturnTo(void *Address, int Size, u32 id);
int PatchNewReturnTo(int es_fd, u64 title);
int BlockIOSReload(int es_fd, u32 gameIOS);
void PatchAspectRatio(void *addr, u32 len, u8 aspect);

extern GXRModeObj TVPal528Prog_RVL;
extern GXRModeObj TVPal528ProgSoft_RVL;
extern GXRModeObj TVPal524ProgAa_RVL;
extern GXRModeObj TVPal528Int_RVL;
extern GXRModeObj TVPal528IntDf_RVL;
extern GXRModeObj TVEurgb60Hz480Prog_RVL;
extern GXRModeObj TVEurgb60Hz480ProgSoft_RVL;
extern GXRModeObj TVEurgb60Hz480ProgAa_RVL;
extern GXRModeObj TVPal524IntAa_RVL;
extern GXRModeObj TVPal264Int_RVL;
extern GXRModeObj TVPal264IntAa_RVL;
extern GXRModeObj TVPal264Ds_RVL;
extern GXRModeObj TVPal264DsAa_RVL;
extern GXRModeObj TVMpal240Int_RVL;
extern GXRModeObj TVMpal240IntAa_RVL;
extern GXRModeObj TVMpal480Int_RVL;
extern GXRModeObj TVMpal480ProgSoft_RVL;
extern GXRModeObj TVMpal480ProgAa_RVL;

#ifdef __cplusplus
}
#endif

#endif
