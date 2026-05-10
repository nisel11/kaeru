//
// SPDX-FileCopyrightText: 2026 Nisel <nisel11good@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>

#define LK_FUNC(ret, name, addr, args) ret (*name) args = (ret (*) args)((addr) | 1)

LK_FUNC(void, lk_mdelay, 0x4C45E6E8, (uint32_t msec));
LK_FUNC(int, get_unlocked_status, 0x4C47441C, (void));
LK_FUNC(int, seccfg_set_lock_state, 0x4C472994, (int lock_state));
LK_FUNC(void, mtk_arch_reset, 0x4C45EF7C, (int));

static void cmd_flashing_unlock(const char *arg, void *data, unsigned sz) {
    if (get_unlocked_status()) {
        fastboot_fail("Device is already unlocked");
        return;
    }

    fastboot_okay("");

    seccfg_set_lock_state(3);

    lk_mdelay(400);
    mtk_arch_reset(1);
}

static void cmd_flashing_lock(const char *arg, void *data, unsigned sz) {
    if (!get_unlocked_status()) {
        fastboot_fail("Device is already locked");
        return;
    }

    fastboot_okay("");

    seccfg_set_lock_state(1);

    lk_mdelay(400);
    mtk_arch_reset(1);
}

static char *board_id_str_buf = (char *)0x4C63F010;
static char *hwc_buf = (char *)0x4C63F1B4;
static char *hwversion_buf = (char *)0x4C63F0C8;
static char *product_sku_buf = (char *)0x4C63F084;

static int is_poco_device(void) {
    size_t len = strlen(product_sku_buf);
    return len > 0 && product_sku_buf[len - 1] == 'p';
}

#define SET_BOARD(bid, h, hv, sku) do { \
    strcpy(board_id_str_buf, bid);      \
    strcpy(hwc_buf,          h);        \
    strcpy(hwversion_buf,    hv);       \
    strcpy(product_sku_buf,  sku);      \
} while (0)

#define LOGO_POCO_LOCKED_INDEX 45
#define LOGO_POCO_UNLOCKED_INDEX 46

static void handle_board_info_failure(void) {
    int adc = 0;
    ((void (*)(int, int *))(0x4C418B94 | 1))(2, &adc); // get_adc_value

    if (adc <= 225999) SET_BOARD("miel", "Global", "androidboot.hwversion=1.29.0", "miel");
    else if (adc <= 316999) SET_BOARD("miel",  "India",  "androidboot.hwversion=1.19.0", "miel");
    else if (adc <= 403999) SET_BOARD("fleur", "Global", "androidboot.hwversion=2.39.0", "fleur");
    else if (adc <= 495999) SET_BOARD("miel",  "Global", "androidboot.hwversion=1.49.0", "miel");
    else if (adc <= 584999) SET_BOARD("fleur", "Global", "androidboot.hwversion=2.29.0", "fleurp");
    else if (adc <= 673000) SET_BOARD("miel",  "India",  "androidboot.hwversion=1.19.0", "mielp");
    else return;

    // Patch mt_disp_show_boot_logo for use POCO logo if device is POCO
    // There's literally no good point for check is POCO logo exist
    // so let's just hope all poco users will have correct logo.bin
    if (is_poco_device()) {
        PATCH_MEM(0x4C404806, 0x2000 | (LOGO_POCO_UNLOCKED_INDEX & 0xFF)); // originally using 0x2b which is MI_UNLOCKED
        PATCH_MEM(0x4C4047D0, 0x2000 | (LOGO_POCO_LOCKED_INDEX & 0xFF)); // originally using 0x0 which is MI_LOCKED
    }
}

void board_early_init(void) {
    printf("Entering early init for Xiaomi Redmi Note 11S 4G/POCO M4 Pro 4G/Xiaomi Redmi Note 12S\n");

    uint32_t addr = 0;

    // Forcing get_vfy_policy to return 0 skips certificate verification for
    // all partitions and firmware images (boot, recovery, dtbo, SCP, etc.)
    // so the device can boot with modified or unsigned images.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF67, 0xF3C0);
    if (addr) {
        printf("Found get_vfy_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // Same idea but for download policy, forcing get_dl_policy to return
    // 0 ensures no partition is marked as download-forbidden, so flashing
    // via fastboot works for all partitions.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF61, 0xF000);
    if (addr) {
        printf("Found get_dl_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // This function handles certificate chain and hash verification for
    // modem-related images (md1rom, md3rom, etc.) during the modem loading
    // process. Same idea as above — force it to return 0 so modem images
    // can be loaded without passing signature verification.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x41F0, 0x460A, 0x4604);
    if (addr) {
        printf("Found ccci_ld_md_sec_ptr_hdr_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // Hook board identification to handle the failure path for
    // fleur/fleurp/miel/mielp devices.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0x4857, 0x4478, 0xF017, 0xF822);
    if (addr) {
        printf("Hooking board_info failure at 0x%08X\n", addr + 4);
        PATCH_CALL(addr + 4, (void *)handle_board_info_failure, TARGET_THUMB);
    }

    // NOP rechecking board_info call
    NOP(0x4C427A64, 2);

    // NOP calling FUN_4c43e1fc for not set androidboot.* to Unknown
    NOP(0x4C427AFA, 2);
    NOP(0x4C427B0C, 2);
    NOP(0x4C427B18, 2);
    NOP(0x4C427B24, 2);

    // Disable the custom_get_lock_state call in lock_state_check,
    // use only seccfg.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xF000, 0xFA28, 0xB9B8, 0x9B02);
    if (addr) {
        printf("Found custom_get_lock_state call at 0x%08X\n", addr);
        NOP(addr - 2, 6);
        PATCH_MEM(addr + 10, 0xE021);
    }

    // NOP the vendor_boot physical partition redirection to vb modem
    NOP(0x4C45FB7E, 1);
    NOP(0x4C459A70, 1);

    // Use custom commands for oem unlock/lock
    PATCH_MEM_ARM(0x4C50C870, (uint32_t)cmd_flashing_unlock | 1);
    PATCH_MEM_ARM(0x4C50C808, (uint32_t)cmd_flashing_lock | 1);
}

void board_late_init(void) {
    printf("Entering late init for Xiaomi Redmi Note 11S 4G/POCO M4 Pro 4G/Xiaomi Redmi Note 12S\n");

    uint32_t addr = 0;

    // Disables the dm-verity corruption warning shown during boot when
    // the device is unlocked. Without this patch, the user gets a scary
    // "Your device is corrupt" screen that waits for a power button
    // press and powers off after 5 seconds if ignored.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB530, 0xB083, 0xAB02, 0x2200);
    if (addr) {
        printf("Found dm_verity_corruption at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // On unlocked devices, LK shows an orange state warning during boot
    // that also introduces an unnecessary 5 second delay. Forcing the
    // function to return 0 skips both the warning and the delay.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0x4B0E, 0x447B);
    if (addr) {
        printf("Found orange_state_warning at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }
}
