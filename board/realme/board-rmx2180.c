//
// SPDX-FileCopyrightText: 2026 fantom3031 <mpiven69@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>

#define LK_FUNC(ret, name, addr, args) ret (*name) args = (ret (*) args)((addr) | 1)

LK_FUNC(void, lk_mdelay, 0x48004CAC, (uint32_t msec));
LK_FUNC(int, get_unlocked_status, 0x48063B34, (void));
LK_FUNC(int, seccfg_set_lock_state, 0x4806353C, (int lock_state));
LK_FUNC(void, mtk_arch_reset, 0x4800716C, (int));
LK_FUNC(void, mt_disp_update, CONFIG_DISP_UPDATE_ADDR, (uint32_t, uint32_t, uint32_t, uint32_t));
LK_FUNC(const char *, fastboot_get_var, 0x4831b048, (const char *));
LK_FUNC(void *, malloc, CONFIG_MALLOC_ADDRESS, (size_t));
LK_FUNC(void, free, CONFIG_FREE_ADDRESS, (void *));
LK_FUNC(long, partition_read, CONFIG_PARTITION_READ_ADDRESS, (const char* part_name, long long offset, uint8_t* data, size_t size));
LK_FUNC(void, video_clean_screen, 0x48032038, (void));

#define LOGO_CUSTOM_FASTBOOT 88 // index is starting from 0
#define LOGO_CONFIRM         89
#define FONT_MEDIUM_LOGO_INDEX 90

static fb_font_t g_font_medium;

static int load_fonts(void) {
    void *medium_buf = malloc(FONT_SHEET_BYTES);

    if (!medium_buf) {
        printf("Failed to malloc font\n");
        goto cleanup;
    }

    if (fb_font_logo_load(&g_font_medium, FONT_MEDIUM_LOGO_INDEX, medium_buf, FONT_SHEET_BYTES) != 0) {
        printf("Failed to load medium font\n");
        goto cleanup;
    }

    return 0;

cleanup:
    if (medium_buf) free(medium_buf);
    g_font_medium.data = NULL;
    return -1;
}

#define UI_COLOR_HEADER 0xFFFFa500u // orange
#define UI_COLOR_TEXT 0xFF999999u // light gray
#define KEY_VOLUME_UP 17
#define KEY_VOLUME_DOWN 1
#define KEY_POWER 8

void play_simple_video(void) {
    #define PALETTE_SIZE (256 * 4)
    uint8_t *palette = (uint8_t*)(uintptr_t)CONFIG_LOGO_TEMPFB_ADDR;
    // palette + sizes: 1024 + 4 = 1028 bytes
    uint8_t *frame_buffer = palette + 1024 + 4;

    long long offset = 0;
    int frame_num = 0;

    fb_config_t *fb = fb_get_config();
    if (!fb || !fb->buffer) return;

    long bytes = partition_read("gz2", offset, palette, PALETTE_SIZE);
    if (bytes < PALETTE_SIZE) {
        video_printf("Failed to read palette!\n");
        return;
    }
    offset += PALETTE_SIZE;

    uint16_t video_width, video_height;
    bytes = partition_read("gz2", offset, (uint8_t*)&video_width, 2);
    bytes += partition_read("gz2", offset + 2, (uint8_t*)&video_height, 2);
    if (bytes < 4 || video_width == 0 || video_height == 0) {
        video_printf("Invalid video size!\n");
        return;
    }
    offset += 4;

    video_printf("Video: %dx%d\n", video_width, video_height);

    uint32_t max_frame_size = video_width * video_height;

    uint8_t *rle_buffer = frame_buffer + max_frame_size;

    int scale_x = fb->width / video_width;
    int scale_y = fb->height / video_height;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;

    int start_x = (fb->width - video_width * scale) / 2;
    int start_y = (fb->height - video_height * scale) / 2;
    video_clean_screen();

    while (1) {
        uint16_t rle_size;
        bytes = partition_read("gz2", offset, (uint8_t*)&rle_size, 2);
        if (bytes < 2 || rle_size == 0 || rle_size > 32768) break;
        offset += 2;

        bytes = partition_read("gz2", offset, rle_buffer, rle_size);
        if (bytes < rle_size) break;
        offset += rle_size;

        int out_pos = 0, in_pos = 0;
        while (in_pos < rle_size && out_pos < max_frame_size) {
            if (in_pos + 1 > rle_size) break;
            uint8_t count = rle_buffer[in_pos++];
            uint8_t value = rle_buffer[in_pos++];

            if (out_pos + count > max_frame_size)
                count = max_frame_size - out_pos;

            for (int i = 0; i < count && out_pos < max_frame_size; i++)
                frame_buffer[out_pos++] = value;
        }

        int idx = 0;
        for (int y = 0; y < video_height; y++) {
            for (int x = 0; x < video_width; x++) {
                uint8_t color_idx = frame_buffer[idx++];
                uint8_t *pal = palette + (color_idx * 4);
                uint32_t color = (pal[2] << 16) | (pal[1] << 8) | pal[0] | (pal[3] << 24);

                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int fb_x = start_x + x * scale + dx;
                        int fb_y = start_y + y * scale + dy;
                        if (fb_x >= 0 && fb_x < fb->width && fb_y >= 0 && fb_y < fb->height) {
                            fb_pixel(fb_x, fb_y, color);
                        }
                    }
                }
            }
        }
        mt_disp_update(0, 0, fb->width, fb->height);
        if (mtk_detect_key(KEY_POWER)) break;
        frame_num++;
        lk_mdelay(40);
    }
}

typedef enum {
    FB_OPTION_CONTINUE = 0,
    FB_OPTION_REBOOT,
    FB_OPTION_RESTART_BOOTLOADER,
    FB_OPTION_RECOVERY_MODE,
    FB_OPTION_FASTBOOT_MODE,
    FB_OPTION_POWEROFF,
    FB_OPTION_BAD_APPLE,
    FB_OPTION_COUNT,
} fb_option_t;

static const char *get_dram_size(void) {
    static char buf[16];

    uint64_t dram = ((unsigned long long (*)(void))(0x480027a0 | 1))(); // physical_memory_size
    unsigned int gb = (unsigned int)(dram / (1024ULL * 1024ULL * 1024ULL));

    npf_snprintf(buf, sizeof(buf), "%u GB", gb);
    return buf[0] ? buf : "N/A";
}

static const char *get_storage_size(void) {
    static char buf[16];

    uint64_t storage = ((unsigned long long (*)(void))(0x483085E8 | 1))(); // g_emmc_size addr
    unsigned int gib = (unsigned int)(storage >> 30);
    unsigned int gb = (gib * 1024u + 500u) / 1000u;

    static const unsigned int std_sizes[] = { 32, 64, 128, 256 };
    for (int i = 0; i < 4; i++) {
        if (gb <= std_sizes[i]) { gb = std_sizes[i]; break; }
    }

    npf_snprintf(buf, sizeof(buf), "%u GB", gb);
    return buf[0] ? buf : "N/A";
}

static const char *get_sbc_status(void) {
    uint32_t sboot_state = 1;

    int (*get_sboot_state)(uint32_t*) = (int (*)(uint32_t*))(0x480628b4 | 1); // get_sboot_state
    int ret = get_sboot_state(&sboot_state);

    if (ret != 0) {
        sboot_state = 1;
    }
    return sboot_state ? "enabled" : "disabled";
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
    int x = 50, y = 1100;

    const char *spoof_env = get_env(KAERU_ENV_BLDR_SPOOF);
    const char *spoof_str = "not set";
    if (spoof_env) {
        spoof_str = (strcmp(spoof_env, "1") == 0) ? "enabled" : "disabled";
    }

    fb_font_select(&g_font_medium);
    fb_text(x, y, "Fastboot Mode", UI_COLOR_HEADER);
    y += fb_get_char_height() * 2;

    render_line(x, &y, UI_COLOR_TEXT, "Product revision: %s", fastboot_get_var("product"));
    render_line(x, &y, UI_COLOR_TEXT, "Kaeru version: %s %s", fastboot_get_var("kaeru-version"), __TIMESTAMP__);
    render_line(x, &y, UI_COLOR_TEXT, "Bootloader version: %s %s", fastboot_get_var("version-bootloader"));
    render_line(x, &y, UI_COLOR_TEXT, "Baseband version: %s %s", fastboot_get_var("version-baseband"));
    render_line(x, &y, UI_COLOR_TEXT, "Serial number: %s", fastboot_get_var("serialno"));
    render_line(x, &y, UI_COLOR_TEXT, "Secure boot: %s", get_sbc_status());
    render_line(x, &y, UI_COLOR_TEXT, "DRAM: %s", get_dram_size());
    render_line(x, &y, UI_COLOR_TEXT, "EMMC: %s", get_storage_size());
    render_line(x, &y, UI_COLOR_TEXT, "Device state: %s", get_unlocked_status() ? "unlocked" : "locked");
    render_line(x, &y, UI_COLOR_TEXT, "Device state: %s", spoof_str);
    render_line(x, &y, UI_COLOR_TEXT, "Boot slot: %s", "device is not A/B");
    render_line(x, &y, UI_COLOR_TEXT, "Enter reason: %s", "not supported");

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
        case FB_OPTION_BAD_APPLE: label = "Bad Apple"; break;
        default: label = "unknown"; break;
    }

    fb_fill_rect(400, 660, 260, 90, 0xFF000000u);
    fb_text(CONFIG_FRAMEBUFFER_WIDTH - fb_font_str_width(label, &g_font_medium) - 65, 670, label, UI_COLOR_TEXT);
    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
}

int fastboot_ui_thread(void *arg) {
    if (load_fonts() != 0) {
        lk_mdelay(1);
        fb_clear(FB_BLACK);
        video_printf("font load failed");
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
                case FB_OPTION_CONTINUE:
                    fb_clear(FB_BLACK);
                    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
                    lk_mdelay(100);
                    ((void (*)(void))(0x4801ad28 | 1))(); // mt_disp_show_boot_logo
                    ((void (*)(const char *, void *, unsigned))(0x48027578 | 1))("", NULL, 0); // cmd_continue
                    return 0;
                case FB_OPTION_REBOOT:
                    ((void (*)(void))(0x48027b94 | 1))(); // cmd_reboot
                    break;
                case FB_OPTION_RESTART_BOOTLOADER:
                    ((void (*)(void))(0x48027bc4 | 1))(); // cmd_reboot_bootloader
                    break;
                case FB_OPTION_RECOVERY_MODE:
                    ((void (*)(const char *, void *, unsigned))(0x480277b4 | 1))("", NULL, 0); // cmd_reboot_recovery
                    break;
                case FB_OPTION_FASTBOOT_MODE:
                    ((void (*)(const char *, void *, unsigned))(0x480277f0 | 1))("", NULL, 0); // cmd_reboot_fastboot
                    break;
                case FB_OPTION_POWEROFF:
                    ((void (*)(void))(0x48006b34 | 1))(); // mt_power_off
                    break;
                case FB_OPTION_BAD_APPLE:
                    play_simple_video();
                    fastboot_ui_render_info();
                    fastboot_ui_render_mode(sel);
                    break;
            }
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

int show_confirm_prompt(const char *label) {
    if (!g_font_medium.data)
        return 1;

    fb_logo_show(LOGO_CONFIRM, false);

    fb_font_draw_str(50, 1080, label, UI_COLOR_TEXT, &g_font_medium);
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

void cmd_help(const char *arg, void *data, unsigned sz) {
    struct fastboot_cmd *cmd = (struct fastboot_cmd *)0x4831b048; // some pointer

    if (!cmd) {
        fastboot_fail("No commands found!");
        return;
    }

    fastboot_info("Available oem commands:");//nothing find, but wrote this
    while (cmd) {
        if (cmd->prefix) {
            if (strncmp(cmd->prefix, "oem", 3) == 0) {
                fastboot_info(cmd->prefix);
            }
        }
        cmd = cmd->next;
    }
    fastboot_okay("");
}

#define CMDLINE1_ADDR 0x48281190

static void patch_cmdline(char *cmdline) {
    cmdline_replace(cmdline, "androidboot.verifiedbootstate=",
                    "green", "orange");
    cmdline_replace(cmdline, "androidboot.secureboot=",
                    "1", "0");
    cmdline_replace(cmdline, "androidboot.vbmeta.device_state=",
                    "locked", "unlocked");
}

static void handle_recovery_boot(void) {
    if (get_bootmode() != BOOTMODE_RECOVERY || !is_spoofing_enabled())
        return;

    printf("Recovery boot detected, modifying cmdline for unlocked state.\n");

    patch_cmdline((char *)CMDLINE1_ADDR);
}

void spoof_lock_state(void) {
    uint32_t addr = 0;

    int spoofing = is_spoofing_enabled();
    fastboot_publish("is-spoofing", spoofing ? "1" : "0");

    if (!spoofing) {
        printf("Bootloader lock status spoofing disabled.\n");
        return;
    }

    printf("Bootloader lock status spoofing enabled, applying patches.\n");

    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB1D0, 0xB510, 0x4604, 0xF7FF, 0xFFDD);
    if (addr) {
        printf("Found seccfg_get_lock_state at 0x%08X\n", addr);
        PATCH_MEM(addr + 6,
            0x2301,  // movs r3, #0x1
            0x6023,  // str r3, [r4, #0x0]
            0x2002,  // movs r0, #0x2
            0xbd10   // pop {r4, pc}
        );
        }
    // AVB adds device state info to the kernel cmdline, but it
    // keeps showing "unlocked" even when we want it to say "locked".
    // This patch forces the cmdline to always use the "locked"
    // string instead of checking the actual device state.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4FF0, 0xB0A9, 0xF101);
    if (addr) {
        printf("Found AVB cmdline function at 0x%08X\n", addr);

        // NOP out the code that checks the actual device state,
        // forcing libavb to always use the "locked" string.
        NOP(addr + 0x9C, 4);
    }

    // When booting into recovery, we need to ensure verifiedbootstate
    // is set to "orange" so fastbootd detects the device as unlocked
    // and allows flashing. We also patch a few other cmdline params
    // (secureboot, device_state) as a precaution in case stock
    // recovery checks them as well.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xf00d, 0xfe26, 0xf001, 0xfbc2);
    if (addr) {
        printf("Found cmdline_pre_process at 0x%08X\n", addr);
        PATCH_CALL(addr, (void *)handle_recovery_boot, TARGET_THUMB);
    }

    // AVB verifies vbmeta public keys in two places: once for the main
    // vbmeta image (validate_vbmeta_public_key) and once for chained
    // vbmeta images (avb_safe_memcmp against the expected key). Both
    // reject the boot if the key doesn't match, causing the "Public key
    // used to sign data rejected" error. We patch both checks so any
    // key is accepted regardless.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xF47F, 0xAE6D, 0xE688, 0xF8DD);
    if (addr) {
        printf("Found load_and_verify_vbmeta at 0x%08X\n", addr);

        // The chain key check first compares key lengths before calling
        // memcmp. If lengths differ, it skips memcmp and falls straight
        // to the error path. Change "cmp r2, r3" to "cmp r3, r3" so the
        // length check always succeeds, allowing execution to reach the
        // memcmp path (which we NOP below).
        PATCH_MEM(addr - 0x328, 0x451B);

        // NOP the bne.w that rejects mismatched chained vbmeta keys,
        // falling through to the success path unconditionally.
        NOP(addr, 2);

        // Replace "cmp r3, #0" with "movs r3, #1" so key_is_trusted
        // is always nonzero and the following bne.w takes the success
        // branch.
        PATCH_MEM(addr + 0x6A, 0x2301);
    }
}

void board_early_init(void) {
    printf("Entering early init for Realme C15\n");

    uint32_t addr = 0;

    // Forcing get_vfy_policy to return 0 skips certificate
    // verification for all partitions and firmware images (boot,
    // recovery, dtbo, SCP, etc.) so the device can boot with
    // modified or unsigned images.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF75, 0xF3C0);
    if (addr) {
        printf("Found get_vfy_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // Same idea but for download policy, forcing get_dl_policy to return
    // 0 ensures no partition is marked as download-forbidden, so flashing
    // via fastboot works for all partitions.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF6F, 0xF000);
    if (addr) {
        printf("Found get_dl_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // On unlocked devices, LK shows an orange state warning during boot
    // that also introduces an unnecessary 5 second delay. Forcing the
    // function to return 0 skips both the warning and the delay.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0x4B0E, 0x447B, 0x681B);
    if (addr) {
        printf("Found orange_state_warning at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // Disables the dm-verity corruption warning shown during boot when
    // the device is unlocked. Without this patch, the user gets a scary
    // "Your device is corrupt" screen that waits for a power button
    // press and powers off after 5 seconds if ignored.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB530, 0xB083, 0xAB02, 0x2200, 0x4604);
    if (addr) {
        printf("Found dm_verity_corruption at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // LK has two security gates in the fastboot command processor that
    // reject commands with "not support on security" and "not allowed
    // in locked state" errors. When spoofing lock state, these would
    // block all fastboot operations despite the device being actually
    // unlocked underneath.
    //
    // Even without spoofing, we patch these out as a safety measure
    // since OEM-specific checks could still interfere with fastboot
    // commands in unexpected ways.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4880, 0xB087, 0x4D5A);
    if (addr) {
        printf("Found fastboot command processor at 0x%08X\n", addr);

        // "not support on security" call
        NOP(addr + 0x15A, 2);

        // "not allowed in locked state" call
        NOP(addr + 0x166, 2);

        // Jump directly to command handler
        PATCH_MEM(addr + 0xF0, 0xE006);
    }

    // BBK added a verification check to ensure the device was officially unlocked.
    // If the check fails, the bootloader exits fastboot mode and reboots.
    //
    // This is unnecessary, seccfg-based unlocks are already valid, so we patch
    // the check to always return true, ensuring fastboot remains accessible.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7C9, 0xF851);
    if (addr) {
        printf("Found fastboot_unlock_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // In cmd_flash function, there's a check for local_6c == 0 that rejects
    // flashing certain partitions with "download for partition is not allowed".
    // This is a software restriction, not a hardware/fuse limitation.
    //
    // We patch the conditional branch (beq) to an unconditional branch (b),
    // skipping the failure path and allowing the flash to proceed.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0x9b07, 0x2b00, 0xf000, 0x8091);
    if (addr) {
        printf("Found download restriction at 0x%08X\n", addr);
        PATCH_MEM(addr + 2, 0xe000);  // b instead of beq
    }

    // The stock cmd_erase function has a check "if (local_64 == 0)" that
    // rejects erasing partitions with "format for partition is not allowed".
    //
    // We NOP out the cbz instruction, so the check never branches to fail.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0x9b03, 0xb323);  // ldr r3, [sp, #0xc]; cbz r3
    if (addr) {
        printf("Found erase restriction at 0x%08X\n", addr);
        NOP(addr + 2, 2);
    }

    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x41F0, 0x460A, 0x4604);
    if (addr) {
        printf("Found ccci_ld_md_sec_ptr_hdr_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // This function handles certificate chain and hash verification for
    // modem-related images (md1rom, md3rom, etc.) during the modem loading
    // process. Same idea as above — force it to return 0 so modem images
    // can be loaded without passing signature verification.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x43F0, 0x460C, 0x4601);
    if (addr) {
        printf("Found ccci_ld_md_sec_image_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // The environment area isn't initialized yet when board_early_init
    // runs, so any get_env calls would return NULL at this stage. We
    // hook a printf call in platform_init that runs right after env
    // initialization completes, it's a convenient entry point since
    // the call itself is non-essential and we need the env to be ready
    // before applying our lock state patches.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xf02f, 0xfa8b, 0x6823, 0x2000);
    if (addr) {
        printf("Found env_init_done at 0x%08X\n", addr);
        PATCH_CALL(addr, (void *)spoof_lock_state, TARGET_THUMB);
    }

    PATCH_MEM_ARM(0x4827b608, (uint32_t)cmd_flashing_unlock | 1);
    PATCH_MEM_ARM(0x4827b78c, (uint32_t)cmd_flashing_lock   | 1);

    // Register help command with all available commands
    fastboot_register("oem help", cmd_help, 1);
    fastboot_register("oem bldr_spoof", cmd_spoof_bootloader_lock, 0);
}

void board_late_init(void) {
    printf("Entering late init for Realme C15\n");

    if (get_bootmode() != BOOTMODE_RECOVERY) {
        show_bootmode(get_bootmode());
    }

    if (mtk_detect_key(KEY_VOLUME_UP)) {
        set_bootmode(BOOTMODE_RECOVERY);
        show_bootmode(BOOTMODE_RECOVERY);
    } else if (mtk_detect_key(KEY_VOLUME_DOWN)) {
        set_bootmode(BOOTMODE_FASTBOOT);
    }

    // On locked Realme devices, volume key detection is broken, making it
    // difficult to enter fastboot or recovery mode through key combos.
    //
    if (mtk_detect_key(KEY_VOLUME_DOWN)) {
        ((void (*)(void))(0x48027bc4 | 1))(); // cmd_reboot_bootloader
    }

    // Run custom fastboot UI
    if (get_bootmode() == BOOTMODE_FASTBOOT) {
        thread_t *thr = thread_create("fastboot_ui", fastboot_ui_thread, NULL, LOW_PRIORITY, DEFAULT_STACK_SIZE);
        if (thr)
            thread_resume(thr);
    }
}
