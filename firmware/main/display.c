#include "display.h"
#include "audio.h"
#include "axp2101.h"
#include "es8311.h"
#include "settings.h"

#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

static const char *TAG = "display";

// Set to true only after hardware is initialized and LVGL screens are created.
static bool display_ready = false;

// Screen sleep/dim state
static bool display_sleeping = false;
static bool display_dimmed = false;
static int inactivity_seconds = 0;

// Hardware handles
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static i2c_master_bus_handle_t i2c_handle = NULL;
static lv_display_t *lvgl_disp = NULL;

// LVGL objects — minimal widget tree to conserve SRAM
static lv_obj_t *screens[SCREEN_COUNT] = {0};
static screen_id_t current_screen = SCREEN_IDLE;

// Idle screen widgets
static lv_obj_t *lbl_time = NULL;
static lv_obj_t *lbl_date = NULL;           // date line inside clock card
static lv_obj_t *idle_clock_card = NULL;    // card containing clock + date
static lv_obj_t *idle_status_bar = NULL;    // transparent top status bar
static lv_obj_t *lbl_prompt = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *lbl_queue = NULL;
static lv_obj_t *lbl_battery = NULL;
static lv_obj_t *lbl_sync_progress = NULL;

// Recording screen widgets
static lv_obj_t *lbl_rec_time = NULL;
static lv_obj_t *lbl_rec_hint = NULL;
static lv_obj_t *obj_pulse = NULL;
static lv_obj_t *bar_vu = NULL;
static lv_anim_t pulse_anim;

// Sync confirmation widgets
static lv_obj_t *lbl_sync_title = NULL;
static lv_obj_t *lbl_sync_status = NULL;
static lv_timer_t *sync_return_timer = NULL;

// Queue/memo management widgets
static lv_obj_t *lbl_queue_header = NULL;
static lv_obj_t *lst_memos = NULL;
static lv_obj_t *btn_play = NULL;
static lv_obj_t *btn_delete = NULL;
static lv_obj_t *lbl_queue_empty = NULL;
static char selected_memo_path[280] = {0};
static lv_obj_t *selected_btn = NULL;

// Settings screen widgets
static lv_obj_t *lbl_vol_pct              = NULL;
static lv_obj_t *lbl_bri_pct             = NULL;
static lv_obj_t *sld_volume              = NULL;   // volume slider
static lv_obj_t *sld_brightness          = NULL;   // brightness slider
static lv_obj_t *settings_color_dots[6]  = {0};
static lv_obj_t *btn_clock_12h           = NULL;
static lv_obj_t *btn_clock_24h           = NULL;
static lv_obj_t *btn_font_normal         = NULL;
static lv_obj_t *btn_font_large          = NULL;

static const uint32_t ACCENT_PRESETS[6] = {
    0x5C997C,  // Sage green (default)
    0x4A7FA5,  // Steel blue
    0xA05CA0,  // Mauve
    0xC07840,  // Amber
    0xC04040,  // Red
    0x707070,  // Gray
};

// Playback progress widgets
static lv_obj_t *bar_playback = NULL;
static lv_obj_t *lbl_playback_time = NULL;
static lv_timer_t *playback_progress_timer = NULL;

// Delete confirmation state
static bool delete_confirming = false;
static lv_timer_t *delete_confirm_timer = NULL;

// Brief message state
static lv_timer_t *brief_msg_timer = NULL;

// Ambient clock mode — active when queue is empty
static bool ambient_clock_active = false;

// Style for list items and action buttons
static lv_style_t style_list_btn;
static lv_style_t style_list_btn_selected;
static lv_style_t style_action_btn;

// Style for sage green text on dark background
static lv_style_t style_sage;
static lv_style_t style_sage_large;
static lv_style_t style_dim;

// Floating card style — #141414 bg, 1px border, 12px radius
static lv_style_t style_card;

// Card and indicator color constants
#define COLOR_CARD_BG      lv_color_hex(0x141414)
#define COLOR_CARD_BORDER  lv_color_hex(0x2A2A2A)
#define COLOR_PAGE_DOT_OFF lv_color_hex(0x333333)

// SH8601 init commands for 368x448 (adapted from C6-2.06 BSP)
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},                      // SLPOUT
    {0xC4, (uint8_t[]){0x80}, 1, 0},                         // TE scan line
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},                   // TE scan line position
    {0x35, (uint8_t[]){0x00}, 1, 0},                          // TEON
    {0x53, (uint8_t[]){0x20}, 1, 10},                         // WRCTRLD
    {0x63, (uint8_t[]){0xFF}, 1, 10},                         // Brightness for HBM
    {0x51, (uint8_t[]){0x00}, 1, 10},                         // Set brightness (start dim)
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},       // CASET: 0-367
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},       // RASET: 0-447
    {0x29, (uint8_t[]){0x00}, 0, 10},                         // DISPON
    {0x51, (uint8_t[]){0xFF}, 1, 0},                          // Set brightness max
};

// LVGL v9 rounder callback — SH8601 requires 2-pixel aligned areas
static void rounder_event_cb(lv_event_t *e) {
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void init_styles(void) {
    lv_style_init(&style_sage);
    lv_style_set_text_color(&style_sage, lv_color_hex(0x5C997C));
    lv_style_set_text_font(&style_sage, &lv_font_montserrat_18);
    lv_style_set_bg_color(&style_sage, lv_color_black());

    lv_style_init(&style_sage_large);
    lv_style_set_text_color(&style_sage_large, lv_color_hex(0x5C997C));
    lv_style_set_text_font(&style_sage_large, &lv_font_montserrat_36);

    lv_style_init(&style_dim);
    lv_style_set_text_color(&style_dim, lv_color_hex(0x3A6B52));
    lv_style_set_text_font(&style_dim, &lv_font_montserrat_18);

    // Memo list item buttons
    lv_style_init(&style_list_btn);
    lv_style_set_bg_color(&style_list_btn, lv_color_hex(0x1A1A1A));
    lv_style_set_bg_opa(&style_list_btn, LV_OPA_COVER);
    lv_style_set_text_color(&style_list_btn, lv_color_hex(0x5C997C));
    lv_style_set_text_font(&style_list_btn, &lv_font_montserrat_20);
    lv_style_set_border_width(&style_list_btn, 0);
    lv_style_set_radius(&style_list_btn, 8);
    lv_style_set_pad_ver(&style_list_btn, 16);  // 20px text + 32px pad = 52px min touch target
    lv_style_set_pad_hor(&style_list_btn, 10);

    lv_style_init(&style_list_btn_selected);
    lv_style_set_bg_color(&style_list_btn_selected, lv_color_hex(0x2A4A3A));
    lv_style_set_bg_opa(&style_list_btn_selected, LV_OPA_COVER);
    lv_style_set_border_color(&style_list_btn_selected, lv_color_hex(0x5C997C));
    lv_style_set_border_width(&style_list_btn_selected, 2);

    // Action buttons (play/delete)
    lv_style_init(&style_action_btn);
    lv_style_set_bg_color(&style_action_btn, lv_color_hex(0x2A4A3A));
    lv_style_set_bg_opa(&style_action_btn, LV_OPA_COVER);
    lv_style_set_text_color(&style_action_btn, lv_color_hex(0x5C997C));
    lv_style_set_text_font(&style_action_btn, &lv_font_montserrat_20);
    lv_style_set_radius(&style_action_btn, 10);
    lv_style_set_border_width(&style_action_btn, 0);
    lv_style_set_pad_all(&style_action_btn, 12);

    // Floating card — dark bg, subtle border, rounded corners
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COLOR_CARD_BG);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, COLOR_CARD_BORDER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_pad_all(&style_card, 16);
}

// Page indicator dots — 3 circles at y=428, center dot = active screen
// active: 0=settings, 1=idle, 2=queue
static void create_page_dots(lv_obj_t *scr, int active) {
    const int cx      = DISP_WIDTH / 2;
    const int cy      = 428;
    const int dot_r   = 4;
    const int spacing = 14;
    lv_color_t accent = lv_color_hex(settings_get()->accent_color);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_set_size(dot, dot_r * 2, dot_r * 2);
        lv_obj_set_pos(dot, cx + (i - 1) * spacing - dot_r, cy - dot_r);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, (i == active) ? accent : COLOR_PAGE_DOT_OFF, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Forward declaration — defined after memo_item_delete_cb
static void reset_delete_confirm(void);

// --- Swipe gesture handler ---
static void gesture_event_cb(lv_event_t *e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_current_target(e);

    reset_delete_confirm();

    if (scr == screens[SCREEN_IDLE] && dir == LV_DIR_LEFT) {
        display_refresh_memo_list();
        display_show_screen(SCREEN_QUEUE);
    } else if (scr == screens[SCREEN_IDLE] && dir == LV_DIR_RIGHT) {
        display_show_screen(SCREEN_SETTINGS);
    } else if (scr == screens[SCREEN_QUEUE] && dir == LV_DIR_RIGHT) {
        display_show_screen(SCREEN_IDLE);
    } else if (scr == screens[SCREEN_SETTINGS] && dir == LV_DIR_LEFT) {
        settings_save();  // batch all settings changes to a single NVS write on exit
        display_show_screen(SCREEN_IDLE);
    }
}

// --- Sync auto-return timer callback ---
static void sync_return_timer_cb(lv_timer_t *timer) {
    (void)timer;
    sync_return_timer = NULL;
    display_show_screen(SCREEN_IDLE);
}

// --- Parse filename "YYYYMMDD_HHMMSS.wav" into display string ---
static void format_memo_label(const char *path, char *out, size_t out_len) {
    // Extract filename from path
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    // Parse YYYYMMDD_HHMMSS
    int year, mon, day, hour, min, sec;
    if (sscanf(fname, "%4d%2d%2d_%2d%2d%2d", &year, &mon, &day, &hour, &min, &sec) == 6) {
        static const char *months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        const char *mstr = (mon >= 1 && mon <= 12) ? months[mon - 1] : "???";

        // Get file size and calculate duration (16kHz, 16-bit, mono = 32000 bytes/sec)
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 44) {
            int dur_sec = (int)((st.st_size - 44) / 32000);
            if (dur_sec >= 60) {
                snprintf(out, out_len, "%s %d, %02d:%02d  %dm %ds",
                         mstr, day, hour, min, dur_sec / 60, dur_sec % 60);
            } else {
                snprintf(out, out_len, "%s %d, %02d:%02d  %ds",
                         mstr, day, hour, min, dur_sec > 0 ? dur_sec : 1);
            }
        } else {
            snprintf(out, out_len, "%s %d, %02d:%02d", mstr, day, hour, min);
        }
    } else {
        snprintf(out, out_len, "%s", fname);
    }
}

// --- Memo list item delete handler — frees the strdup'd path stored as user_data ---
static void memo_item_delete_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}

// --- Delete confirmation helpers ---
static void reset_delete_confirm(void) {
    if (delete_confirm_timer) {
        lv_timer_delete(delete_confirm_timer);
        delete_confirm_timer = NULL;
    }
    if (delete_confirming && btn_delete) {
        lv_obj_t *lbl = lv_obj_get_child(btn_delete, 0);
        if (lbl) lv_label_set_text(lbl, "Delete");
        lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0x2A4A3A), 0);
    }
    delete_confirming = false;
}

static void delete_confirm_timer_cb(lv_timer_t *timer) {
    (void)timer;
    delete_confirm_timer = NULL;
    reset_delete_confirm();
}

// --- Memo list item tap handler ---
static void memo_item_click_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *path = (const char *)lv_event_get_user_data(e);

    reset_delete_confirm();

    if (selected_btn == btn) {
        // Deselect
        lv_obj_remove_style(selected_btn, &style_list_btn_selected, 0);
        selected_btn = NULL;
        selected_memo_path[0] = '\0';
        lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Deselect previous
        if (selected_btn) {
            lv_obj_remove_style(selected_btn, &style_list_btn_selected, 0);
        }
        // Select new
        selected_btn = btn;
        lv_obj_add_style(btn, &style_list_btn_selected, 0);
        strncpy(selected_memo_path, path, sizeof(selected_memo_path) - 1);
        selected_memo_path[sizeof(selected_memo_path) - 1] = '\0';
        lv_obj_clear_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- Playback progress timer callback ---
static void playback_progress_timer_cb(lv_timer_t *timer) {
    (void)timer;
    int elapsed = 0, total = 0;
    if (audio_get_playback_progress(&elapsed, &total)) {
        if (bar_playback && total > 0) {
            lv_bar_set_value(bar_playback, (elapsed * 100) / total, LV_ANIM_ON);
        }
        if (lbl_playback_time) {
            lv_label_set_text_fmt(lbl_playback_time, "%d:%02d / %d:%02d",
                                  elapsed / 60, elapsed % 60, total / 60, total % 60);
        }
    }
}

static void stop_playback_progress(void) {
    if (playback_progress_timer) {
        lv_timer_delete(playback_progress_timer);
        playback_progress_timer = NULL;
    }
    if (bar_playback) lv_obj_add_flag(bar_playback, LV_OBJ_FLAG_HIDDEN);
    if (lbl_playback_time) lv_obj_add_flag(lbl_playback_time, LV_OBJ_FLAG_HIDDEN);
}

// --- Play button handler ---
static void play_btn_cb(lv_event_t *e) {
    (void)e;
    if (selected_memo_path[0] == '\0') return;

    if (audio_is_playing()) {
        // Stop playback
        audio_stop_playback();
        return;  // display_memo_playback_done() will reset the button
    }

    // Start playback
    if (audio_play(selected_memo_path) != ESP_OK) return;
    lv_obj_t *lbl = lv_obj_get_child(btn_play, 0);
    if (lbl) lv_label_set_text(lbl, "Stop");

    // Show and start progress tracking
    if (bar_playback) {
        lv_bar_set_value(bar_playback, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(bar_playback, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_playback_time) {
        lv_label_set_text(lbl_playback_time, "0:00 / 0:00");
        lv_obj_clear_flag(lbl_playback_time, LV_OBJ_FLAG_HIDDEN);
    }
    if (playback_progress_timer) lv_timer_delete(playback_progress_timer);
    playback_progress_timer = lv_timer_create(playback_progress_timer_cb, 250, NULL);
}

// --- Delete button handler (two-tap confirmation) ---
static void delete_btn_cb(lv_event_t *e) {
    (void)e;
    if (selected_memo_path[0] == '\0') return;

    if (!delete_confirming) {
        // First tap — enter confirmation state
        delete_confirming = true;
        lv_obj_t *lbl = lv_obj_get_child(btn_delete, 0);
        if (lbl) lv_label_set_text(lbl, "Confirm?");
        lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0x994040), 0);
        // Auto-reset after 2 seconds
        if (delete_confirm_timer) lv_timer_delete(delete_confirm_timer);
        delete_confirm_timer = lv_timer_create(delete_confirm_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(delete_confirm_timer, 1);
        return;
    }

    // Second tap — execute delete
    reset_delete_confirm();
    audio_stop_playback();  // stop playback if active on this memo
    audio_delete_memo(selected_memo_path);
    selected_memo_path[0] = '\0';
    selected_btn = NULL;
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    display_refresh_memo_list();
    display_update_queue_badge(audio_get_memo_count());
}

// Switch idle screen between ambient (large centered clock) and normal layout.
// Must be called with the LVGL port lock held.
static void set_ambient_mode(bool active) {
    if (!lbl_time || !idle_clock_card || !idle_status_bar) return;
    ambient_clock_active = active;
    if (active) {
        // Enlarge clock card and use biggest font
        lv_obj_set_size(idle_clock_card, 320, 160);
        lv_obj_set_pos(idle_clock_card, (DISP_WIDTH - 320) / 2, 100);
        lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
        lv_obj_add_flag(idle_status_bar,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_sync_progress,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_prompt,         LV_OBJ_FLAG_HIDDEN);
    } else {
        // Restore normal layout
        lv_obj_set_size(idle_clock_card, 320, 108);
        lv_obj_set_pos(idle_clock_card, (DISP_WIDTH - 320) / 2, 72);
        const lv_font_t *font_clock = settings_get()->font_large
            ? &lv_font_montserrat_44 : &lv_font_montserrat_36;
        lv_obj_set_style_text_font(lbl_time, font_clock, 0);
        lv_obj_clear_flag(idle_status_bar,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_sync_progress,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_prompt,         LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_idle_screen(void) {
    lv_obj_t *scr = screens[SCREEN_IDLE] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    lv_color_t accent = lv_color_hex(settings_get()->accent_color);

    // --- Status bar (transparent, 368×48, y=0) ---
    idle_status_bar = lv_obj_create(scr);
    lv_obj_set_size(idle_status_bar, DISP_WIDTH, 48);
    lv_obj_set_pos(idle_status_bar, 0, 0);
    lv_obj_set_style_bg_opa(idle_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_status_bar, 0, 0);
    lv_obj_set_style_pad_all(idle_status_bar, 0, 0);
    lv_obj_clear_flag(idle_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_wifi = lv_label_create(idle_status_bar);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0x3A6B52), 0);
    lv_label_set_text(lbl_wifi, "No Wi-Fi");
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_width(lbl_wifi, 200);
    lv_label_set_long_mode(lbl_wifi, LV_LABEL_LONG_DOT);

    lbl_battery = lv_label_create(idle_status_bar);
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0x3A6B52), 0);
    lv_label_set_text(lbl_battery, "--%");
    lv_obj_align(lbl_battery, LV_ALIGN_RIGHT_MID, -14, 0);

    // --- Clock card (320×108, y=72) ---
    idle_clock_card = lv_obj_create(scr);
    lv_obj_set_size(idle_clock_card, 320, 108);
    lv_obj_set_pos(idle_clock_card, (DISP_WIDTH - 320) / 2, 72);
    lv_obj_add_style(idle_clock_card, &style_card, 0);
    lv_obj_set_style_radius(idle_clock_card, 16, 0);
    lv_obj_clear_flag(idle_clock_card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_time = lv_label_create(idle_clock_card);
    lv_obj_set_style_text_color(lbl_time, accent, 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_44, 0);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 4);

    lbl_date = lv_label_create(idle_clock_card);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0x3A6B52), 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_BOTTOM_MID, 0, -4);

    // --- Queue badge pill (hidden when empty) ---
    lbl_queue = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_queue, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_queue, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(lbl_queue, accent, 0);
    lv_obj_set_style_bg_opa(lbl_queue, LV_OPA_80, 0);
    lv_obj_set_style_radius(lbl_queue, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(lbl_queue, 14, 0);
    lv_obj_set_style_pad_ver(lbl_queue, 6, 0);
    lv_label_set_text(lbl_queue, "");
    lv_obj_align(lbl_queue, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_add_flag(lbl_queue, LV_OBJ_FLAG_HIDDEN);

    // --- Sync progress ---
    lbl_sync_progress = lv_label_create(scr);
    lv_obj_add_style(lbl_sync_progress, &style_dim, 0);
    lv_label_set_text(lbl_sync_progress, "");
    lv_obj_align(lbl_sync_progress, LV_ALIGN_TOP_MID, 0, 256);

    // --- Hint / brief message ---
    lbl_prompt = lv_label_create(scr);
    lv_obj_add_style(lbl_prompt, &style_dim, 0);
    lv_obj_set_style_text_font(lbl_prompt, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_prompt, "Hold to record");
    lv_obj_align(lbl_prompt, LV_ALIGN_TOP_MID, 0, 302);

    // Page dots (idle = position 1)
    create_page_dots(scr, 1);

    // Swipe left → memo list, swipe right → settings
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

static void pulse_anim_cb(void *obj, int32_t value) {
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void create_recording_screen(void) {
    lv_obj_t *scr = screens[SCREEN_RECORDING] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    obj_pulse = lv_obj_create(scr);
    lv_obj_set_size(obj_pulse, 100, 100);
    lv_obj_set_style_radius(obj_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj_pulse, lv_color_hex(0x5C997C), 0);
    lv_obj_set_style_bg_opa(obj_pulse, LV_OPA_70, 0);
    lv_obj_set_style_border_width(obj_pulse, 0, 0);
    lv_obj_align(obj_pulse, LV_ALIGN_CENTER, 0, -50);

    // Smooth breathe animation on pulse circle opacity
    lv_anim_init(&pulse_anim);
    lv_anim_set_var(&pulse_anim, obj_pulse);
    lv_anim_set_exec_cb(&pulse_anim, pulse_anim_cb);
    lv_anim_set_values(&pulse_anim, LV_OPA_40, LV_OPA_90);
    lv_anim_set_duration(&pulse_anim, 1200);
    lv_anim_set_playback_duration(&pulse_anim, 1200);
    lv_anim_set_repeat_count(&pulse_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&pulse_anim);

    lbl_rec_time = lv_label_create(scr);
    lv_obj_add_style(lbl_rec_time, &style_sage_large, 0);
    lv_obj_set_style_text_font(lbl_rec_time, &lv_font_montserrat_44, 0);
    lv_label_set_text(lbl_rec_time, "0:00");
    lv_obj_align(lbl_rec_time, LV_ALIGN_CENTER, 0, 40);

    lbl_rec_hint = lv_label_create(scr);
    lv_obj_add_style(lbl_rec_hint, &style_dim, 0);
    lv_label_set_text(lbl_rec_hint, "Release to stop");
    lv_obj_align(lbl_rec_hint, LV_ALIGN_CENTER, 0, 86);

    // VU meter bar — 18px tall for better visibility
    bar_vu = lv_bar_create(scr);
    lv_obj_set_size(bar_vu, DISP_WIDTH - 48, 18);
    lv_obj_align(bar_vu, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar_vu, 0, 100);
    lv_bar_set_value(bar_vu, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(0x5C997C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_vu, LV_OPA_COVER, LV_PART_MAIN);
}

static void create_queue_screen(void) {
    lv_obj_t *scr = screens[SCREEN_QUEUE] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Header
    lbl_queue_header = lv_label_create(scr);
    lv_obj_add_style(lbl_queue_header, &style_sage, 0);
    lv_obj_set_style_text_font(lbl_queue_header, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_queue_header, "Memos");
    lv_obj_align(lbl_queue_header, LV_ALIGN_TOP_MID, 0, 10);

    // Empty state label
    lbl_queue_empty = lv_label_create(scr);
    lv_obj_add_style(lbl_queue_empty, &style_dim, 0);
    lv_label_set_text(lbl_queue_empty, "No memos");
    lv_obj_align(lbl_queue_empty, LV_ALIGN_CENTER, 0, 0);

    // Scrollable memo list
    lst_memos = lv_obj_create(scr);
    lv_obj_set_size(lst_memos, DISP_WIDTH - 20, 300);
    lv_obj_align(lst_memos, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_bg_opa(lst_memos, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lst_memos, 0, 0);
    lv_obj_set_style_pad_all(lst_memos, 0, 0);
    lv_obj_set_style_pad_row(lst_memos, 6, 0);
    lv_obj_set_flex_flow(lst_memos, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(lst_memos, LV_DIR_VER);

    // Play button
    btn_play = lv_btn_create(scr);
    lv_obj_add_style(btn_play, &style_action_btn, 0);
    lv_obj_set_size(btn_play, 160, 56);
    lv_obj_align(btn_play, LV_ALIGN_BOTTOM_LEFT, 15, -15);
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *lbl_p = lv_label_create(btn_play);
    lv_label_set_text(lbl_p, "Play");
    lv_obj_center(lbl_p);
    lv_obj_add_event_cb(btn_play, play_btn_cb, LV_EVENT_CLICKED, NULL);

    // Delete button
    btn_delete = lv_btn_create(scr);
    lv_obj_add_style(btn_delete, &style_action_btn, 0);
    lv_obj_set_size(btn_delete, 160, 56);
    lv_obj_align(btn_delete, LV_ALIGN_BOTTOM_RIGHT, -15, -15);
    lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *lbl_d = lv_label_create(btn_delete);
    lv_label_set_text(lbl_d, "Delete");
    lv_obj_center(lbl_d);
    lv_obj_add_event_cb(btn_delete, delete_btn_cb, LV_EVENT_CLICKED, NULL);

    // Playback progress bar (hidden by default, shown during playback)
    bar_playback = lv_bar_create(scr);
    lv_obj_set_size(bar_playback, DISP_WIDTH - 60, 8);
    lv_obj_align(bar_playback, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_bar_set_range(bar_playback, 0, 100);
    lv_bar_set_value(bar_playback, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_playback, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_playback, lv_color_hex(0x5C997C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_playback, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(bar_playback, LV_OBJ_FLAG_HIDDEN);

    // Playback time label (hidden by default)
    lbl_playback_time = lv_label_create(scr);
    lv_obj_add_style(lbl_playback_time, &style_dim, 0);
    lv_label_set_text(lbl_playback_time, "");
    lv_obj_align(lbl_playback_time, LV_ALIGN_BOTTOM_MID, 0, -78);
    lv_obj_add_flag(lbl_playback_time, LV_OBJ_FLAG_HIDDEN);

    // Page dots (queue = position 2)
    create_page_dots(scr, 2);

    // Swipe right → back to idle
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

static void sync_screen_tap_cb(lv_event_t *e) {
    (void)e;
    if (sync_return_timer) {
        lv_timer_delete(sync_return_timer);
        sync_return_timer = NULL;
    }
    display_show_screen(SCREEN_IDLE);
}

static void create_sync_screen(void) {
    lv_obj_t *scr = screens[SCREEN_SYNC_CONFIRM] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, sync_screen_tap_cb, LV_EVENT_CLICKED, NULL);

    lbl_sync_status = lv_label_create(scr);
    lv_obj_add_style(lbl_sync_status, &style_sage_large, 0);
    lv_label_set_text(lbl_sync_status, "");
    lv_obj_set_width(lbl_sync_status, DISP_WIDTH - 40);
    lv_obj_set_style_text_align(lbl_sync_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_sync_status, LV_ALIGN_CENTER, 0, -20);

    lbl_sync_title = lv_label_create(scr);
    lv_obj_add_style(lbl_sync_title, &style_sage, 0);
    lv_label_set_text(lbl_sync_title, "");
    lv_obj_set_width(lbl_sync_title, DISP_WIDTH - 40);
    lv_obj_set_style_text_align(lbl_sync_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_sync_title, LV_ALIGN_CENTER, 0, 20);
}

// --- Settings screen callbacks ---

static void slider_volume_cb(lv_event_t *e) {
    int val = lv_slider_get_value(lv_event_get_target(e));
    app_settings_t *s = settings_get();
    s->volume = (val * 255) / 100;
    es8311_set_dac_volume(s->volume);
    if (lbl_vol_pct) lv_label_set_text_fmt(lbl_vol_pct, "%d%%", val);
}

static void slider_brightness_cb(lv_event_t *e) {
    int val = lv_slider_get_value(lv_event_get_target(e));
    app_settings_t *s = settings_get();
    // Map 0-100 → 0x20-0xFF
    s->brightness = 0x20 + ((val * (0xFF - 0x20)) / 100);
    display_set_brightness(s->brightness);
    if (lbl_bri_pct) lv_label_set_text_fmt(lbl_bri_pct, "%d%%", val);
}

static void settings_color_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_get()->accent_color = ACCENT_PRESETS[idx];
    // Update dot borders to show selection
    for (int i = 0; i < 6; i++) {
        lv_obj_set_style_border_width(settings_color_dots[i], (i == idx) ? 3 : 0, 0);
    }
    display_apply_theme();
}

static void settings_clock_cb(lv_event_t *e) {
    bool is_24h = (bool)(intptr_t)lv_event_get_user_data(e);
    settings_get()->clock_24h = is_24h;
    lv_obj_set_style_bg_color(btn_clock_24h, lv_color_hex(is_24h  ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_bg_color(btn_clock_12h, lv_color_hex(!is_24h ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_clock_24h, is_24h  ? 2 : 0, 0);
    lv_obj_set_style_border_width(btn_clock_12h, !is_24h ? 2 : 0, 0);
}

static void settings_font_cb(lv_event_t *e) {
    bool large = (bool)(intptr_t)lv_event_get_user_data(e);
    settings_get()->font_large = large;
    lv_obj_set_style_bg_color(btn_font_large,  lv_color_hex(large  ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_bg_color(btn_font_normal, lv_color_hex(!large ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_font_large,  large  ? 2 : 0, 0);
    lv_obj_set_style_border_width(btn_font_normal, !large ? 2 : 0, 0);
    display_apply_theme();
}

// Create a small toggle-style button pair label
static lv_obj_t *make_seg_btn(lv_obj_t *parent, const char *text, int w, int h,
                               lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x5C997C), 0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x5C997C), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 8, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// Helper: apply standard slider styling to a newly created slider
static void style_slider(lv_obj_t *sld, lv_color_t accent) {
    lv_obj_set_style_bg_color(sld, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sld, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sld, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sld, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sld, accent, LV_PART_KNOB);
    lv_obj_set_style_radius(sld, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(sld, 4, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sld, LV_RADIUS_CIRCLE, LV_PART_KNOB);
}

static void create_settings_screen(void) {
    lv_obj_t *scr = screens[SCREEN_SETTINGS] = lv_obj_create(NULL);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    app_settings_t *s = settings_get();
    lv_color_t accent = lv_color_hex(s->accent_color);

    // Header
    lv_obj_t *hdr = lv_label_create(scr);
    lv_obj_set_style_text_color(hdr, accent, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_label_set_text(hdr, "Settings");
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 12);

    // Card layout: 320px wide, centered (x=24), vertically stacked with 8px gaps
    const int CARD_X = (DISP_WIDTH - 320) / 2;
    const int CARD_W = 320;
    const int PAD    = 12;  // inner padding for all cards
    int card_y = 48;

    // ── Card 1: Volume ──────────────────────────────────────────────────────
    // height = PAD + 18(label) + 8(gap) + 32(slider) + PAD = 82 → 84
    lv_obj_t *vol_card = lv_obj_create(scr);
    lv_obj_set_size(vol_card, CARD_W, 84);
    lv_obj_set_pos(vol_card, CARD_X, card_y);
    lv_obj_add_style(vol_card, &style_card, 0);
    lv_obj_set_style_pad_all(vol_card, PAD, 0);
    lv_obj_clear_flag(vol_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_vol_name = lv_label_create(vol_card);
    lv_obj_set_style_text_color(lbl_vol_name, accent, 0);
    lv_obj_set_style_text_font(lbl_vol_name, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_vol_name, "Volume");
    lv_obj_align(lbl_vol_name, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_vol_pct = lv_label_create(vol_card);
    lv_obj_set_style_text_color(lbl_vol_pct, accent, 0);
    lv_obj_set_style_text_font(lbl_vol_pct, &lv_font_montserrat_16, 0);
    int vol_pct = (s->volume * 100) / 255;
    lv_label_set_text_fmt(lbl_vol_pct, "%d%%", vol_pct);
    lv_obj_align(lbl_vol_pct, LV_ALIGN_TOP_RIGHT, 0, 0);

    sld_volume = lv_slider_create(vol_card);
    lv_slider_set_range(sld_volume, 0, 100);
    lv_slider_set_value(sld_volume, vol_pct, LV_ANIM_OFF);
    lv_obj_set_size(sld_volume, CARD_W - PAD * 2, 32);
    lv_obj_align(sld_volume, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_slider(sld_volume, accent);
    lv_obj_add_event_cb(sld_volume, slider_volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
    card_y += 84 + 8;

    // ── Card 2: Brightness ──────────────────────────────────────────────────
    lv_obj_t *bri_card = lv_obj_create(scr);
    lv_obj_set_size(bri_card, CARD_W, 84);
    lv_obj_set_pos(bri_card, CARD_X, card_y);
    lv_obj_add_style(bri_card, &style_card, 0);
    lv_obj_set_style_pad_all(bri_card, PAD, 0);
    lv_obj_clear_flag(bri_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_bri_name = lv_label_create(bri_card);
    lv_obj_set_style_text_color(lbl_bri_name, accent, 0);
    lv_obj_set_style_text_font(lbl_bri_name, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_bri_name, "Brightness");
    lv_obj_align(lbl_bri_name, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_bri_pct = lv_label_create(bri_card);
    lv_obj_set_style_text_color(lbl_bri_pct, accent, 0);
    lv_obj_set_style_text_font(lbl_bri_pct, &lv_font_montserrat_16, 0);
    int bri_pct = (s->brightness * 100) / 255;
    lv_label_set_text_fmt(lbl_bri_pct, "%d%%", bri_pct);
    lv_obj_align(lbl_bri_pct, LV_ALIGN_TOP_RIGHT, 0, 0);

    sld_brightness = lv_slider_create(bri_card);
    lv_slider_set_range(sld_brightness, 0, 100);
    lv_slider_set_value(sld_brightness, bri_pct, LV_ANIM_OFF);
    lv_obj_set_size(sld_brightness, CARD_W - PAD * 2, 32);
    lv_obj_align(sld_brightness, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_slider(sld_brightness, accent);
    lv_obj_add_event_cb(sld_brightness, slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    card_y += 84 + 8;

    // ── Card 3: Accent color ─────────────────────────────────────────────────
    // height = PAD + 18(label) + 8(gap) + 36(tiles) + PAD = 86 → 88
    lv_obj_t *col_card = lv_obj_create(scr);
    lv_obj_set_size(col_card, CARD_W, 88);
    lv_obj_set_pos(col_card, CARD_X, card_y);
    lv_obj_add_style(col_card, &style_card, 0);
    lv_obj_set_style_pad_all(col_card, PAD, 0);
    lv_obj_clear_flag(col_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_col = lv_label_create(col_card);
    lv_obj_set_style_text_color(lbl_col, accent, 0);
    lv_obj_set_style_text_font(lbl_col, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_col, "Color");
    lv_obj_align(lbl_col, LV_ALIGN_TOP_LEFT, 0, 0);

    // 6 color tiles, 36×36px, 8px gap → total = 6*36+5*8 = 256px
    // Center in content width (296px): start_x = (296-256)/2 = 20
    const int TILE_SIZE = 36;
    const int TILE_GAP  = 8;
    const int TILE_ROW_X = (CARD_W - PAD * 2 - (6 * TILE_SIZE + 5 * TILE_GAP)) / 2;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *dot = lv_btn_create(col_card);
        lv_obj_set_size(dot, TILE_SIZE, TILE_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(ACCENT_PRESETS[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(dot, (ACCENT_PRESETS[i] == s->accent_color) ? 3 : 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        // Position relative to col_card content area: x from left, y below the label
        lv_obj_set_pos(dot, TILE_ROW_X + i * (TILE_SIZE + TILE_GAP), 26);
        lv_obj_add_event_cb(dot, settings_color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        settings_color_dots[i] = dot;
    }
    card_y += 88 + 8;

    // ── Card 4: Clock format ─────────────────────────────────────────────────
    // height = PAD + 18(label) + 6(gap) + 52(buttons) + PAD = 100
    lv_obj_t *clk_card = lv_obj_create(scr);
    lv_obj_set_size(clk_card, CARD_W, 100);
    lv_obj_set_pos(clk_card, CARD_X, card_y);
    lv_obj_add_style(clk_card, &style_card, 0);
    lv_obj_set_style_pad_all(clk_card, PAD, 0);
    lv_obj_clear_flag(clk_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_clk = lv_label_create(clk_card);
    lv_obj_set_style_text_color(lbl_clk, accent, 0);
    lv_obj_set_style_text_font(lbl_clk, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_clk, "Clock");
    lv_obj_align(lbl_clk, LV_ALIGN_TOP_LEFT, 0, 0);

    // Two wide toggle buttons: (296 - 8gap) / 2 = 144px each
    btn_clock_24h = make_seg_btn(clk_card, "24h", 144, 52,
                                 settings_clock_cb, (void *)(intptr_t)1);
    lv_obj_align(btn_clock_24h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_clock_24h,
        lv_color_hex(s->clock_24h ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_clock_24h, s->clock_24h ? 2 : 0, 0);

    btn_clock_12h = make_seg_btn(clk_card, "12h", 144, 52,
                                 settings_clock_cb, (void *)(intptr_t)0);
    lv_obj_align(btn_clock_12h, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_clock_12h,
        lv_color_hex(!s->clock_24h ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_clock_12h, !s->clock_24h ? 2 : 0, 0);
    card_y += 100 + 8;

    // ── Card 5: Font size ────────────────────────────────────────────────────
    lv_obj_t *fnt_card = lv_obj_create(scr);
    lv_obj_set_size(fnt_card, CARD_W, 100);
    lv_obj_set_pos(fnt_card, CARD_X, card_y);
    lv_obj_add_style(fnt_card, &style_card, 0);
    lv_obj_set_style_pad_all(fnt_card, PAD, 0);
    lv_obj_clear_flag(fnt_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_fnt = lv_label_create(fnt_card);
    lv_obj_set_style_text_color(lbl_fnt, accent, 0);
    lv_obj_set_style_text_font(lbl_fnt, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_fnt, "Font");
    lv_obj_align(lbl_fnt, LV_ALIGN_TOP_LEFT, 0, 0);

    btn_font_normal = make_seg_btn(fnt_card, "Normal", 144, 52,
                                   settings_font_cb, (void *)(intptr_t)0);
    lv_obj_align(btn_font_normal, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_font_normal,
        lv_color_hex(!s->font_large ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_font_normal, !s->font_large ? 2 : 0, 0);

    btn_font_large = make_seg_btn(fnt_card, "Large", 144, 52,
                                  settings_font_cb, (void *)(intptr_t)1);
    lv_obj_align(btn_font_large, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_font_large,
        lv_color_hex(s->font_large ? 0x2A4A3A : 0x1A1A1A), 0);
    lv_obj_set_style_border_width(btn_font_large, s->font_large ? 2 : 0, 0);

    // Swipe left → back to idle (set on scr, not on cards, so gestures bubble up)
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

void display_apply_theme(void) {
    if (!display_ready) return;
    app_settings_t *s = settings_get();
    lv_color_t accent = lv_color_hex(s->accent_color);

    // Font table: normal vs large
    const lv_font_t *font_clock   = s->font_large ? &lv_font_montserrat_44 : &lv_font_montserrat_36;
    const lv_font_t *font_header  = s->font_large ? &lv_font_montserrat_28 : &lv_font_montserrat_24;
    const lv_font_t *font_list    = s->font_large ? &lv_font_montserrat_22 : &lv_font_montserrat_20;
    const lv_font_t *font_small   = s->font_large ? &lv_font_montserrat_16 : &lv_font_montserrat_18;

    lvgl_port_lock(0);

    // Update shared styles
    lv_style_set_text_color(&style_sage,      accent);
    lv_style_set_text_color(&style_sage_large, accent);
    lv_style_set_text_color(&style_list_btn,   accent);
    lv_style_set_text_color(&style_action_btn, accent);

    lv_style_set_text_font(&style_list_btn,   font_list);
    lv_style_set_text_font(&style_action_btn, font_list);

    // Idle screen
    if (lbl_time) {
        lv_obj_set_style_text_color(lbl_time, accent, 0);
        if (!ambient_clock_active) {
            lv_obj_set_style_text_font(lbl_time, font_clock, 0);
        }
    }
    // Queue pill: update bg color to new accent
    if (lbl_queue)         lv_obj_set_style_bg_color(lbl_queue, accent, 0);
    if (lbl_sync_progress) lv_obj_set_style_text_color(lbl_sync_progress, accent, 0);
    if (lbl_prompt)        lv_obj_set_style_text_color(lbl_prompt,        accent, 0);

    // Queue screen
    if (lbl_queue_header) {
        lv_obj_set_style_text_color(lbl_queue_header, accent, 0);
        lv_obj_set_style_text_font(lbl_queue_header, font_header, 0);
    }
    if (btn_play) {
        lv_obj_set_style_text_color(btn_play, accent, 0);
        lv_obj_set_style_text_font(btn_play, font_list, 0);
    }
    if (btn_delete) {
        lv_obj_set_style_text_color(btn_delete, accent, 0);
        lv_obj_set_style_text_font(btn_delete, font_list, 0);
    }
    if (lbl_playback_time) {
        lv_obj_set_style_text_color(lbl_playback_time, accent, 0);
        lv_obj_set_style_text_font(lbl_playback_time, font_small, 0);
    }

    // Recording screen
    if (lbl_rec_time) {
        lv_obj_set_style_text_color(lbl_rec_time, accent, 0);
        lv_obj_set_style_text_font(lbl_rec_time, font_clock, 0);
    }
    if (lbl_rec_hint) {
        lv_obj_set_style_text_color(lbl_rec_hint, accent, 0);
        lv_obj_set_style_text_font(lbl_rec_hint, font_small, 0);
    }
    if (obj_pulse) lv_obj_set_style_bg_color(obj_pulse, accent, 0);
    if (bar_vu)    lv_obj_set_style_bg_color(bar_vu, accent, LV_PART_INDICATOR);

    // Playback bar accent
    if (bar_playback) lv_obj_set_style_bg_color(bar_playback, accent, LV_PART_INDICATOR);

    // Settings sliders
    if (sld_volume) {
        lv_obj_set_style_bg_color(sld_volume, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sld_volume, accent, LV_PART_KNOB);
    }
    if (sld_brightness) {
        lv_obj_set_style_bg_color(sld_brightness, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sld_brightness, accent, LV_PART_KNOB);
    }

    // Force a full redraw
    lv_obj_invalidate(lv_scr_act());

    lvgl_port_unlock();
}

static esp_err_t init_i2c(void) {
    if (i2c_handle) return ESP_OK;  // already initialized — reuse existing bus
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = I2C_NUM_0,
    };
    return i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
}

static esp_err_t init_display_hw(void) {
    ESP_LOGI(TAG, "Initialize QSPI bus for SH8601");

    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
        BSP_LCD_DATA2, BSP_LCD_DATA3,
        DISP_WIDTH * DISP_HEIGHT * 2  // max transfer = full frame RGB565
    );
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        BSP_LCD_CS, NULL, NULL
    );
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM,
                        &io_config, &io_handle),
                        TAG, "Panel IO init failed");

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle),
                        TAG, "SH8601 panel create failed");

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    // No x_gap for 368-wide 1.8" panel (2.06 uses 0x16 for its 410-wide panel)
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    ESP_LOGI(TAG, "SH8601 AMOLED panel initialized");
    return ESP_OK;
}

static lv_display_t *init_lvgl_display(void) {
    // Initialize esp_lvgl_port (creates its own task and tick)
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_FALSE(lvgl_port_init(&lvgl_cfg) == ESP_OK, NULL, TAG, "LVGL port init failed");

    // Register display with esp_lvgl_port
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DISP_WIDTH * 20,  // 20-row strip buffer (~14.7KB)
        .monochrome = false,
        .hres = DISP_WIDTH,
        .vres = DISP_HEIGHT,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .sw_rotate = true,
            .buff_dma = true,
            .swap_bytes = true,  // SH8601 expects big-endian RGB565
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return NULL;
    }

    // SH8601 requires 2-pixel aligned draw areas
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    return disp;
}

static esp_err_t init_touch(void) {
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C init failed");

    /* Enable AXP2101 ALDO1+ALDO2 at 3.3V — these LDOs power the FT3168 touch
     * controller and are OFF by default after a cold boot. AXP2101 is normally
     * init'd after display_init(), so we do the minimal enable here directly. */
    i2c_master_dev_handle_t axp_temp = NULL;
    const i2c_device_config_t axp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(i2c_handle, &axp_cfg, &axp_temp) == ESP_OK) {
        uint8_t buf[2];
        esp_err_t wr;
        buf[0] = 0x92; buf[1] = 0x1C;  /* ALDO1 = 3.3V */
        wr = i2c_master_transmit(axp_temp, buf, 2, 100);
        ESP_LOGI(TAG, "AXP ALDO1 volt write: %s", esp_err_to_name(wr));
        buf[0] = 0x93; buf[1] = 0x1C;  /* ALDO2 = 3.3V */
        wr = i2c_master_transmit(axp_temp, buf, 2, 100);
        ESP_LOGI(TAG, "AXP ALDO2 volt write: %s", esp_err_to_name(wr));
        uint8_t reg90 = 0;
        uint8_t reg90_addr = 0x90;
        esp_err_t rd = i2c_master_transmit_receive(axp_temp, &reg90_addr, 1, &reg90, 1, 100);
        ESP_LOGI(TAG, "AXP reg0x90 read: %s val=0x%02X", esp_err_to_name(rd), reg90);
        buf[0] = 0x90; buf[1] = reg90 | 0x03;  /* Set ALDO1+ALDO2, preserve others */
        wr = i2c_master_transmit(axp_temp, buf, 2, 100);
        ESP_LOGI(TAG, "AXP ALDO enable write (0x%02X): %s", buf[1], esp_err_to_name(wr));
        i2c_master_bus_rm_device(axp_temp);

        /* Poll until FT3168 ACKs on I2C instead of a blind delay.
         * This confirms power is up AND the chip is ready. */
        ESP_LOGI(TAG, "Waiting for FT3168 at 0x38...");
        esp_err_t probe = ESP_ERR_NOT_FOUND;
        for (int ms = 0; ms < 5000 && probe != ESP_OK; ms += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
            probe = i2c_master_probe(i2c_handle, 0x38, 20);
        }
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "FT3168 responded on I2C");
        } else {
            ESP_LOGE(TAG, "FT3168 never responded after 5s — power or address issue");
        }
    } else {
        ESP_LOGW(TAG, "Could not reach AXP2101 — touch may fail");
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISP_WIDTH,
        .y_max = DISP_HEIGHT,
        .rst_gpio_num = BSP_TOUCH_RST,
        .int_gpio_num = BSP_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle),
                        TAG, "Touch panel IO init failed");

    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp),
                        TAG, "FT5x06 touch init failed");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);

    ESP_LOGI(TAG, "Touch initialized (FT3168 via FT5x06 driver)");
    return ESP_OK;
}

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Display init — SH8601 QSPI AMOLED + FT3168 touch");

    // 1. Initialize display hardware
    ESP_RETURN_ON_ERROR(init_display_hw(), TAG, "Display hardware init failed");

    // 2. Initialize LVGL and register display
    lvgl_disp = init_lvgl_display();
    if (!lvgl_disp) {
        return ESP_FAIL;
    }

    // 3. Initialize touch (needs I2C + LVGL display)
    esp_err_t touch_ret = init_touch();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (%s) — retrying in 2s", esp_err_to_name(touch_ret));
        vTaskDelay(pdMS_TO_TICKS(2000));
        touch_ret = init_touch();
        if (touch_ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch init failed again (%s) — display will work without touch",
                     esp_err_to_name(touch_ret));
        }
    }

    // 4. Create LVGL screens (must hold the port lock)
    lvgl_port_lock(0);

    init_styles();
    create_settings_screen();
    create_idle_screen();
    create_recording_screen();
    create_queue_screen();
    create_sync_screen();
    lv_scr_load(screens[SCREEN_IDLE]);

    lvgl_port_unlock();

    display_ready = true;

    ESP_LOGI(TAG, "Display ready — %dx%d AMOLED", DISP_WIDTH, DISP_HEIGHT);
    ESP_LOGI(TAG, "Free heap after display init: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

void display_show_screen(screen_id_t screen) {
    if (!display_ready) return;
    if (screen < SCREEN_COUNT && screens[screen]) {
        lvgl_port_lock(0);
        lv_scr_load_anim(screens[screen], LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
        current_screen = screen;
        lvgl_port_unlock();
    }
}

void display_update_time(int hour, int min) {
    if (!display_ready || !lbl_time) return;
    lvgl_port_lock(0);
    if (settings_get()->clock_24h) {
        lv_label_set_text_fmt(lbl_time, "%02d:%02d", hour, min);
    } else {
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;
        lv_label_set_text_fmt(lbl_time, "%d:%02d %s", h12, min, hour < 12 ? "AM" : "PM");
    }
    // Update date line using current wall clock
    if (lbl_date) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        char date_buf[32];
        strftime(date_buf, sizeof(date_buf), "%a, %b %d", &tm_info);
        lv_label_set_text(lbl_date, date_buf);
    }
    lvgl_port_unlock();
}

void display_update_wifi(wifi_display_state_t state, const char *ssid) {
    if (!display_ready || !lbl_wifi) return;
    lvgl_port_lock(0);
    switch (state) {
        case WIFI_DISPLAY_CONNECTED:
            lv_label_set_text(lbl_wifi, (ssid && ssid[0]) ? ssid : "Wi-Fi");
            break;
        case WIFI_DISPLAY_CONNECTING:
            lv_label_set_text(lbl_wifi, "Connecting...");
            break;
        case WIFI_DISPLAY_SUSPENDED:
            lv_label_set_text(lbl_wifi, "Wi-Fi idle");
            break;
        default:
            lv_label_set_text(lbl_wifi, "No Wi-Fi");
            break;
    }
    lvgl_port_unlock();
}

void display_update_queue_badge(int count) {
    if (!display_ready || !lbl_queue) return;
    lvgl_port_lock(0);
    if (count > 0) {
        lv_label_set_text_fmt(lbl_queue, "%d memo%s queued", count, count > 1 ? "s" : "");
        lv_obj_clear_flag(lbl_queue, LV_OBJ_FLAG_HIDDEN);
        if (ambient_clock_active) set_ambient_mode(false);
    } else {
        lv_obj_add_flag(lbl_queue, LV_OBJ_FLAG_HIDDEN);
        if (!ambient_clock_active) set_ambient_mode(true);
    }
    lvgl_port_unlock();
}

void display_update_battery(int percent, bool charging) {
    if (!display_ready || !lbl_battery) return;
    lvgl_port_lock(0);
    if (charging) {
        lv_label_set_text_fmt(lbl_battery, "%d%% +", percent);
    } else {
        lv_label_set_text_fmt(lbl_battery, "%d%%", percent);
    }
    // Color: red <20%, yellow <40%, sage green otherwise
    lv_color_t color;
    if (percent < 20 && !charging) {
        color = lv_color_hex(0xCC4444);  // red
    } else if (percent < 40 && !charging) {
        color = lv_color_hex(0xCCAA44);  // yellow
    } else {
        color = lv_color_hex(0x3A6B52);  // dim sage (matches style_dim)
    }
    lv_obj_set_style_text_color(lbl_battery, color, 0);
    lvgl_port_unlock();
}

void display_update_sync_status(const char *status) {
    if (!display_ready || !lbl_sync_progress) return;
    lvgl_port_lock(0);
    lv_label_set_text(lbl_sync_progress, status ? status : "");
    lvgl_port_unlock();
}

void display_update_recording(int elapsed_sec, int max_sec) {
    if (!display_ready) return;
    lvgl_port_lock(0);
    if (lbl_rec_time) {
        lv_label_set_text_fmt(lbl_rec_time, "%d:%02d", elapsed_sec / 60, elapsed_sec % 60);
    }
    // Pulse animation is driven by lv_anim_t — no manual update needed
    if (lbl_rec_hint && max_sec - elapsed_sec <= 10) {
        lv_label_set_text_fmt(lbl_rec_hint, "Auto-stop in %ds", max_sec - elapsed_sec);
    }
    // Update VU meter — scale RMS (0-32767) to percentage with log-ish curve
    if (bar_vu) {
        int rms = audio_get_recording_rms();
        // Map RMS to 0-100 with compression: clamp at ~8000 for full scale
        int level = (rms > 8000) ? 100 : (rms * 100 / 8000);
        lv_bar_set_value(bar_vu, level, LV_ANIM_ON);
    }
    lvgl_port_unlock();
}

void display_show_sync_result(const char *title, bool success) {
    if (!display_ready) return;

    // Don't interrupt the queue screen — refresh the list in place instead
    if (current_screen == SCREEN_QUEUE) {
        display_refresh_memo_list();
        display_update_queue_badge(audio_get_memo_count());
        return;
    }

    lvgl_port_lock(0);
    if (lbl_sync_status) {
        lv_label_set_text(lbl_sync_status, success ? "Synced" : "Sync Failed");
    }
    if (lbl_sync_title) {
        lv_label_set_text(lbl_sync_title, title ? title : "");
    }
    // Auto-return to idle after 3 seconds
    if (sync_return_timer) {
        lv_timer_delete(sync_return_timer);
    }
    sync_return_timer = lv_timer_create(sync_return_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(sync_return_timer, 1);
    lvgl_port_unlock();
    display_show_screen(SCREEN_SYNC_CONFIRM);
}

void display_refresh_memo_list(void) {
    if (!display_ready || !lst_memos) return;
    lvgl_port_lock(0);

    reset_delete_confirm();

    // Clear existing list children
    lv_obj_clean(lst_memos);
    selected_btn = NULL;
    selected_memo_path[0] = '\0';
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);

    // Get memos from flash
    int count = 0;
    char **memos = audio_list_memos(&count);

    // Update header
    if (count > 0) {
        lv_label_set_text_fmt(lbl_queue_header, "Memos (%d)", count);
        lv_obj_add_flag(lbl_queue_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_queue_header, "Memos");
        lv_obj_clear_flag(lbl_queue_empty, LV_OBJ_FLAG_HIDDEN);
    }

    // Populate list items
    for (int i = 0; i < count; i++) {
        char label_text[64];
        format_memo_label(memos[i], label_text, sizeof(label_text));

        lv_obj_t *btn = lv_btn_create(lst_memos);
        lv_obj_set_width(btn, DISP_WIDTH - 40);
        lv_obj_add_style(btn, &style_list_btn, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label_text);

        // Register delete callback to free the strdup'd path when the button is destroyed
        lv_obj_add_event_cb(btn, memo_item_delete_cb, LV_EVENT_DELETE, memos[i]);
        lv_obj_add_event_cb(btn, memo_item_click_cb, LV_EVENT_CLICKED, memos[i]);
        // memos[i] ownership transferred to the delete callback; do not free here
    }

    // Free the pointer array only — individual strings are freed via LV_EVENT_DELETE
    if (memos) free(memos);

    lvgl_port_unlock();
}

static void brief_msg_timer_cb(lv_timer_t *timer) {
    (void)timer;
    brief_msg_timer = NULL;
    lvgl_port_lock(0);
    if (lbl_prompt) lv_label_set_text(lbl_prompt, "Hold to record");
    lvgl_port_unlock();
}

void display_show_brief_message(const char *msg, int ms) {
    if (!display_ready || !lbl_prompt || current_screen != SCREEN_IDLE) return;
    lvgl_port_lock(0);
    lv_label_set_text(lbl_prompt, msg);
    if (brief_msg_timer) lv_timer_delete(brief_msg_timer);
    brief_msg_timer = lv_timer_create(brief_msg_timer_cb, (uint32_t)ms, NULL);
    lv_timer_set_repeat_count(brief_msg_timer, 1);
    lvgl_port_unlock();
}

void display_memo_playback_done(void) {
    if (!display_ready || !btn_play) return;
    lvgl_port_lock(0);
    lv_obj_t *lbl = lv_obj_get_child(btn_play, 0);
    if (lbl) lv_label_set_text(lbl, "Play");
    stop_playback_progress();
    lvgl_port_unlock();
}

void *display_get_i2c_handle(void) {
    return (void *)i2c_handle;
}

void display_sleep(void) {
    if (!display_ready || display_sleeping) return;
    if (current_screen == SCREEN_SETTINGS) {
        settings_save();
    }
    esp_lcd_panel_disp_on_off(panel_handle, false);       // DISPOFF
    esp_lcd_panel_io_tx_param(io_handle, 0x10, NULL, 0);  // SLPIN
    vTaskDelay(pdMS_TO_TICKS(120));  // SH8601 spec: >=120ms after SLPIN
    display_sleeping = true;
    ESP_LOGI(TAG, "Display sleeping");
}

void display_wake(void) {
    if (!display_ready || !display_sleeping) return;
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));  // Wait for oscillator stabilize
    esp_lcd_panel_disp_on_off(panel_handle, true);         // DISPON
    display_set_brightness(settings_get()->brightness);
    display_sleeping = false;
    display_dimmed = false;
    inactivity_seconds = 0;
    // Force full redraw to restore screen content after wake
    lvgl_port_lock(0);
    lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Display waking");
}

bool display_is_sleeping(void) {
    return display_sleeping;
}

void display_set_brightness(uint8_t level) {
    if (!panel_handle) return;
    // SH8601 brightness command 0x51 — set via panel IO
    uint8_t data = level;
    esp_lcd_panel_io_tx_param(io_handle, 0x51, &data, 1);
}

void display_note_activity(void) {
    inactivity_seconds = 0;
    if (display_sleeping) {
        display_wake();
    } else if (display_dimmed) {
        display_dimmed = false;
        display_set_brightness(settings_get()->brightness);
    }
}

void display_tick_inactivity(void) {
    if (display_sleeping) return;
    inactivity_seconds++;

    bool on_usb = axp2101_is_vbus_present();

    if (on_usb) {
        // On USB: dim after DIM_TIMEOUT, never sleep
        if (!display_dimmed && inactivity_seconds >= DISPLAY_DIM_TIMEOUT_SEC) {
            display_dimmed = true;
            display_set_brightness(0x4D);  // ~30% brightness
        }
    } else {
        // On battery: dim at half the sleep timeout, then sleep
        if (inactivity_seconds >= DISPLAY_SLEEP_TIMEOUT_SEC) {
            display_sleep();
        } else if (!display_dimmed && inactivity_seconds >= DISPLAY_SLEEP_TIMEOUT_SEC / 2) {
            display_dimmed = true;
            display_set_brightness(0x4D);  // ~30% brightness
        }
    }
}
