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
LK_FUNC(void, mt_disp_update, 0x4C400D4C, (uint32_t, uint32_t, uint32_t, uint32_t));
LK_FUNC(const char *, fastboot_get_var, 0x4C429B28, (const char *));
LK_FUNC(int, udc_stop, 0x4C45757C, (void));
LK_FUNC(void *, malloc, CONFIG_MALLOC_ADDRESS, (size_t));
LK_FUNC(void, free, CONFIG_FREE_ADDRESS, (void *));

#define LOGO_MI_LOCKED         38 // index is starting from 0
#define LOGO_MI_UNLOCKED       43
#define LOGO_ORIGINAL_FASTBOOT 42
#define LOGO_CUSTOM_FASTBOOT   45
#define LOGO_POCO_LOCKED       46
#define LOGO_POCO_UNLOCKED     47
#define LOGO_CONFIRM           48
#define FONT_MEDIUM_LOGO_INDEX 49
#define FONT_BOLD_LOGO_INDEX   50

static fb_font_t g_font_medium;
static fb_font_t g_font_bold;

static int load_fonts(void) {
    void *medium_buf = malloc(FONT_SHEET_BYTES);
    void *bold_buf = malloc(FONT_SHEET_BYTES);

    if (!medium_buf || !bold_buf) {
        printf("Failed to malloc fonts\n");
        goto cleanup;
    }

    if (fb_font_logo_load(&g_font_medium, FONT_MEDIUM_LOGO_INDEX, medium_buf, FONT_SHEET_BYTES) != 0) {
        printf("Failed to load medium font\n");
        goto cleanup;
    }

    if (fb_font_logo_load(&g_font_bold, FONT_BOLD_LOGO_INDEX, bold_buf, FONT_SHEET_BYTES) != 0) {
        printf("Failed to load bold font\n");
        goto cleanup;
    }

    return 0;

cleanup:
    if (medium_buf) free(medium_buf);
    if (bold_buf)   free(bold_buf);

    g_font_medium.data = NULL;
    g_font_bold.data = NULL;

    return -1;
}

#define UI_COLOR_HEADER 0xFFFFa500u // orange
#define UI_COLOR_TEXT   0xFF999999u // light gray

static const char *get_boot_reason(void) {
    switch (*(volatile uint32_t *)(*(volatile uint32_t *)0x4C50C5C0)) { // oem fbreason pointer
        case 0: return "volume down";
        case 1: return "reboot bootloader";
        case 2: return "boot menu";
        default: return "unknown";
    }
}

#define KEY_VOLUME_UP 17
#define KEY_VOLUME_DOWN 0
#define KEY_POWER 8

typedef enum {
    FB_OPTION_REBOOT = 0,
    FB_OPTION_RESTART_BOOTLOADER,
    FB_OPTION_RECOVERY_MODE,
    FB_OPTION_FASTBOOT_MODE,
    FB_OPTION_POWEROFF,
    FB_OPTION_COUNT,
} fb_option_t;

static const char *get_dram_size(void) {
    static char buf[16];

    uint64_t dram = ((unsigned long long (*)(void))(0x4C40256C | 1))(); // physical_memory_size
    unsigned int gb = (unsigned int)(dram / (1024ULL * 1024ULL * 1024ULL));

    npf_snprintf(buf, sizeof(buf), "%u GB", gb);
    return buf[0] ? buf : "N/A";
}

static const char *get_storage_size(void) {
    static char buf[16];

    uint64_t storage = ((unsigned long long (*)(void))(0x4C46EE68 | 1))(); // ufs_lk_get_device_size
    unsigned int gib = (unsigned int)(storage >> 30);
    unsigned int gb  = (gib * 1024u + 500u) / 1000u;

    static const unsigned int std_sizes[] = { 32, 64, 128, 256, 512, 1024 };
    for (int i = 0; i < 6; i++) {
        if (gb <= std_sizes[i]) { gb = std_sizes[i]; break; }
    }

    npf_snprintf(buf, sizeof(buf), "%u GB", gb);
    return buf[0] ? buf : "N/A";
}

static void render_line(int x, int *y, uint32_t color, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    npf_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fb_text(x, *y, buf, color);
    *y += fb_get_char_height();
}

static void fastboot_ui_render_info(void) {
    int x = 80, y = 1580;

    fb_font_select(&g_font_medium);
    fb_text(x, y, "Fastboot Mode", UI_COLOR_HEADER);
    y += fb_get_char_height() * 2;

    render_line(x, &y, UI_COLOR_TEXT, "Product revision: %s", fastboot_get_var("product"));
    render_line(x, &y, UI_COLOR_TEXT, "Kaeru version: %s %s", fastboot_get_var("kaeru-version"), __TIMESTAMP__);
    render_line(x, &y, UI_COLOR_TEXT, "Serial number: %s", fastboot_get_var("serialno"));
    render_line(x, &y, UI_COLOR_TEXT, "DRAM: %s", get_dram_size());
    render_line(x, &y, UI_COLOR_TEXT, "UFS: %s", get_storage_size());
    render_line(x, &y, UI_COLOR_TEXT, "Device state: %s", get_unlocked_status() ? "unlocked" : "locked");
    render_line(x, &y, UI_COLOR_TEXT, "Boot slot: %s", fastboot_get_var("current-slot"));
    render_line(x, &y, UI_COLOR_TEXT, "Enter reason: %s", get_boot_reason());

    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
}

static void fastboot_ui_render_mode(int sel) {
    const char *label;
    switch (sel) {
        case FB_OPTION_REBOOT: label = "Reboot"; break;
        case FB_OPTION_RESTART_BOOTLOADER: label = "Restart Bootloader"; break;
        case FB_OPTION_RECOVERY_MODE: label = "Recovery Mode"; break;
        case FB_OPTION_FASTBOOT_MODE: label = "Fastboot Mode"; break;
        case FB_OPTION_POWEROFF: label = "Power Off"; break;
        default: label = "unknown"; break;
    }

    fb_fill_rect(670, 1015, 340, 65, 0xFF000000u);
    fb_font_select(&g_font_bold);
    fb_text(1000 - fb_font_str_width(label, &g_font_bold), 1020, label, UI_COLOR_TEXT);
    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
}

int fastboot_ui_thread(void *arg) {
    if (load_fonts() != 0) {
        lk_mdelay(1); // logo is not showing without it for some reason
        fb_clear(FB_BLACK);
        fb_logo_show(LOGO_ORIGINAL_FASTBOOT, true);
        return 1;
    }

    int sel = 0;

    // Wait for fastboot vars to be populated before rendering
    for (int i = 0; i < 100; i++) {
        if (fastboot_get_var("serialno")[0])
            break;
        lk_mdelay(5);
    }

    fb_logo_show(LOGO_CUSTOM_FASTBOOT, false);

    fastboot_ui_render_info();
    fastboot_ui_render_mode(sel);

    // Delay before polling for don't immediately
    // start scrolling after run fastboot if forgot
    // to unpress vol down
    lk_mdelay(500);

    for (;;) {
        if (mtk_detect_key(KEY_VOLUME_UP)) {
            sel = (sel + FB_OPTION_COUNT - 1) % FB_OPTION_COUNT;
            fastboot_ui_render_mode(sel);
            lk_mdelay(150);
        } else if (mtk_detect_key(KEY_VOLUME_DOWN)) {
            sel = (sel + 1) % FB_OPTION_COUNT;
            fastboot_ui_render_mode(sel);
            lk_mdelay(150);
        } else if (mtk_detect_key(KEY_POWER)) {
            lk_mdelay(200);
            switch (sel) {
                case FB_OPTION_REBOOT:
                    ((void (*)(void))(0x4C42C1B4 | 1))(); // cmd_reboot
                    break;
                case FB_OPTION_RESTART_BOOTLOADER:
                    ((void (*)(void))(0x4C42C200 | 1))(); // cmd_reboot_bootloader
                    break;
                case FB_OPTION_RECOVERY_MODE:
                    ((void (*)(const char *, void *, unsigned))(0x4C42BD4C | 1))("", NULL, 0); // cmd_reboot_recovery
                    break;
                case FB_OPTION_FASTBOOT_MODE:
                    ((void (*)(const char *, void *, unsigned))(0x4C42BD88 | 1))("", NULL, 0); // cmd_reboot_fastboot
                    break;
                case FB_OPTION_POWEROFF:
                    ((void (*)(void))(0x4C400184 | 1))(); // mt_power_off
                    break;
            }
            return 0;
        }
        lk_mdelay(100);
    }
}

int show_confirm_prompt(const char *label) {
    if (!g_font_medium.data || !g_font_bold.data)
        return 1;

    fb_logo_show(LOGO_CONFIRM, false);

    fb_fill_rect(0, 1560, (uint32_t)CONFIG_FRAMEBUFFER_WIDTH, 65, 0xAA000000u);
    fb_font_draw_str(80, 1580, label, UI_COLOR_TEXT, &g_font_medium);
    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);

    lk_mdelay(500);

    while (1) {
        if (mtk_detect_key(KEY_VOLUME_UP)) {
            lk_mdelay(150);
            return 1;
        }
        if (mtk_detect_key(KEY_VOLUME_DOWN)) {
            lk_mdelay(150);
            return 0;
        }
        lk_mdelay(100);
    }
}

#define CONFIRM_LOCK_TEXT \
    "Relocking the bootloader restores the restriction on flashing\n" \
    "partitions. The device will no longer allow flashing custom\n" \
    "firmware through fastboot.\n" \
    "\n" \
    "If you have installed a custom ROM, modified the kernel, or\n" \
    "changed any AVB-verified partition your device may fail to\n" \
    "boot after relocking and may enter a permanent bootloop or\n" \
    "become unusable until the bootloader is unlocked again."

#define CONFIRM_UNLOCK_TEXT \
    "Unlocking the bootloader grants full access to device partitions\n" \
    "and allows flashing custom firmware, kernels, or recoveries.\n" \
    "\n" \
    "Ensure you only flash trusted images, as incorrect firmware\n" \
    "can lead to system instability or make the device fail to boot."

static void cmd_flashing_unlock(const char *arg, void *data, unsigned sz) {
    if (get_unlocked_status()) {
        fastboot_fail("Device is already unlocked");
        return;
    }

    fastboot_okay("");

    if (show_confirm_prompt(CONFIRM_UNLOCK_TEXT))
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

    if (show_confirm_prompt(CONFIRM_LOCK_TEXT))
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

static void cmd_read(const char *arg, void *data, unsigned int sz) {
    char buf[128];

    if (!arg || !arg[0]) {
        fastboot_fail("Usage: oem read <hex_addr> [size]");
        return;
    }

    const char *p = arg;
    while (*p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    uint32_t addr = 0;
    while (*p && *p != ' ') {
        char c = *p;
        addr <<= 4;
        if (c >= '0' && c <= '9') addr |= c - '0';
        else if (c >= 'a' && c <= 'f') addr |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') addr |= c - 'A' + 10;
        else break;
        p++;
    }

    int count = 64;
    while (*p == ' ') p++;
    if (*p) {
        count = 0;
        while (*p >= '0' && *p <= '9') {
            count = count * 10 + (*p - '0');
            p++;
        }
        if (count > 65536) count = 65536;
    }

    npf_snprintf(buf, sizeof(buf), "Read 0x%08X (%d bytes):", addr, count);
    fastboot_info(buf);

    uint8_t *mem = (uint8_t*)addr;

    for (int i = 0; i < count; i += 16) {
        char line[80];
        int pos = 0;

        pos += npf_snprintf(line + pos, sizeof(line) - pos, "%08X: ", addr + i);

        for (int j = 0; j < 16; j++) {
            if ((i + j) < count) {
                pos += npf_snprintf(line + pos, sizeof(line) - pos, "%02X ", mem[i + j]);
            } else {
                pos += npf_snprintf(line + pos, sizeof(line) - pos, "   ");
            }
        }

        pos += npf_snprintf(line + pos, sizeof(line) - pos, "|");
        for (int j = 0; j < 16 && (i + j) < count; j++) {
            char c = mem[i + j];
            line[pos++] = (c >= 32 && c <= 126) ? c : '.';
        }
        line[pos++] = '|';
        line[pos] = '\0';

        fastboot_info(line);
    }

    fastboot_okay("");
}

static void cmd_chainload(const char *arg, void *data, unsigned int sz) {
    uint32_t addr = 0x4E000000;

    if (arg && arg[0]) {
        const char *p = arg;
        while (*p == ' ') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

        addr = 0;
        while (*p && *p != ' ') {
            char c = *p;
            addr <<= 4;
            if (c >= '0' && c <= '9') addr |= c - '0';
            else if (c >= 'a' && c <= 'f') addr |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') addr |= c - 'A' + 10;
            else break;
            p++;
        }
    }

    char msg[64];
    npf_snprintf(msg, sizeof(msg), "Chainloading to 0x%08X...", addr);
    fastboot_info(msg);
    lk_mdelay(100);
    fastboot_okay("");

    /* Stop USB before chainloading */
    udc_stop();
    lk_mdelay(20);

    /* Prepare CPU for jump: Disable IRQ, MMU and Caches */
    __asm__ volatile(
        "cpsid i\n"
        "mrc p15, 0, r0, c1, c0, 0\n"
        "bic r0, r0, #0x0005\n" /* Disable MMU and D-Cache */
        "bic r0, r0, #0x1000\n" /* Disable I-Cache */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "dsb sy\n"
        "isb\n"
        ::: "r0", "memory"
    );

    /* Simple jump to entry point. Pass boottags addr in r2 */
    void (*entry)(uint32_t, uint32_t, uint32_t, uint32_t) = (void *)addr;
    entry(0, 0, 0x4C080000, 0);

    /* Should never reach here */
    while(1);
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

    // Suppress mt_disp_show_fastboot_logo in platform_init so we can
    // draw custom UI and text instead.
    PATCH_MEM(0x4C404830, 0x2001, 0x4770);

    fastboot_register("oem read", cmd_read, 1);
    fastboot_register("oem chainload", cmd_chainload, 1);
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

    // Run custom fastboot UI
    if (get_bootmode() == BOOTMODE_FASTBOOT) {
        thread_t *thr = thread_create("fastboot_ui", fastboot_ui_thread, NULL,
                                      LOW_PRIORITY, DEFAULT_STACK_SIZE);
        if (thr)
            thread_resume(thr);
    }
}
