#include "display.h"
#include "audio.h"

#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

static const char *TAG = "display";

// Set to true only after hardware is initialized and LVGL screens are created.
static bool display_ready = false;

// Screen sleep state
static bool display_sleeping = false;
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
static lv_obj_t *lbl_prompt = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *lbl_queue = NULL;
static lv_obj_t *lbl_battery = NULL;

// Recording screen widgets
static lv_obj_t *lbl_rec_time = NULL;
static lv_obj_t *lbl_rec_hint = NULL;
static lv_obj_t *obj_pulse = NULL;

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

// Style for list items and action buttons
static lv_style_t style_list_btn;
static lv_style_t style_list_btn_selected;
static lv_style_t style_action_btn;

// Style for sage green text on dark background
static lv_style_t style_sage;
static lv_style_t style_sage_large;
static lv_style_t style_dim;

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
    lv_style_set_text_font(&style_list_btn, &lv_font_montserrat_18);
    lv_style_set_border_width(&style_list_btn, 0);
    lv_style_set_radius(&style_list_btn, 8);
    lv_style_set_pad_ver(&style_list_btn, 12);
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
    lv_style_set_text_font(&style_action_btn, &lv_font_montserrat_18);
    lv_style_set_radius(&style_action_btn, 10);
    lv_style_set_border_width(&style_action_btn, 0);
    lv_style_set_pad_all(&style_action_btn, 10);
}

// --- Swipe gesture handler ---
static void gesture_event_cb(lv_event_t *e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_current_target(e);

    if (scr == screens[SCREEN_IDLE] && dir == LV_DIR_LEFT) {
        display_refresh_memo_list();
        display_show_screen(SCREEN_QUEUE);
    } else if (scr == screens[SCREEN_QUEUE] && dir == LV_DIR_RIGHT) {
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

        // Get file size
        struct stat st;
        long kb = 0;
        if (stat(path, &st) == 0) {
            kb = (long)(st.st_size / 1024);
        }
        snprintf(out, out_len, "%s %d, %02d:%02d  %ld KB", mstr, day, hour, min, kb);
    } else {
        snprintf(out, out_len, "%s", fname);
    }
}

// --- Memo list item delete handler — frees the strdup'd path stored as user_data ---
static void memo_item_delete_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}

// --- Memo list item tap handler ---
static void memo_item_click_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *path = (const char *)lv_event_get_user_data(e);

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

// --- Play button handler ---
static void play_btn_cb(lv_event_t *e) {
    (void)e;
    if (selected_memo_path[0] == '\0') return;
    audio_play(selected_memo_path);
    // Update button text to indicate playing
    lv_obj_t *lbl = lv_obj_get_child(btn_play, 0);
    if (lbl) lv_label_set_text(lbl, "Playing...");
}

// --- Delete button handler ---
static void delete_btn_cb(lv_event_t *e) {
    (void)e;
    if (selected_memo_path[0] == '\0') return;
    audio_delete_memo(selected_memo_path);
    selected_memo_path[0] = '\0';
    selected_btn = NULL;
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    // Refresh the list and update idle badge
    display_refresh_memo_list();
    display_update_queue_badge(audio_get_memo_count());
}

static void create_idle_screen(void) {
    lv_obj_t *scr = screens[SCREEN_IDLE] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lbl_time = lv_label_create(scr);
    lv_obj_add_style(lbl_time, &style_sage_large, 0);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 60);

    lbl_queue = lv_label_create(scr);
    lv_obj_add_style(lbl_queue, &style_sage, 0);
    lv_label_set_text(lbl_queue, "");
    lv_obj_align(lbl_queue, LV_ALIGN_CENTER, 0, -20);

    lbl_prompt = lv_label_create(scr);
    lv_obj_add_style(lbl_prompt, &style_dim, 0);
    lv_label_set_text(lbl_prompt, "Hold to record");
    lv_obj_align(lbl_prompt, LV_ALIGN_CENTER, 0, 40);

    lbl_wifi = lv_label_create(scr);
    lv_obj_add_style(lbl_wifi, &style_dim, 0);
    lv_label_set_text(lbl_wifi, "No Wi-Fi");
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_LEFT, 10, 10);

    lbl_battery = lv_label_create(scr);
    lv_obj_add_style(lbl_battery, &style_dim, 0);
    lv_label_set_text(lbl_battery, "-- %");
    lv_obj_align(lbl_battery, LV_ALIGN_TOP_RIGHT, -10, 10);

    // Swipe left → memo list
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

static void create_recording_screen(void) {
    lv_obj_t *scr = screens[SCREEN_RECORDING] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    obj_pulse = lv_obj_create(scr);
    lv_obj_set_size(obj_pulse, 80, 80);
    lv_obj_set_style_radius(obj_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj_pulse, lv_color_hex(0x5C997C), 0);
    lv_obj_set_style_bg_opa(obj_pulse, LV_OPA_70, 0);
    lv_obj_set_style_border_width(obj_pulse, 0, 0);
    lv_obj_align(obj_pulse, LV_ALIGN_CENTER, 0, -40);

    lbl_rec_time = lv_label_create(scr);
    lv_obj_add_style(lbl_rec_time, &style_sage_large, 0);
    lv_label_set_text(lbl_rec_time, "0:00");
    lv_obj_align(lbl_rec_time, LV_ALIGN_CENTER, 0, 40);

    lbl_rec_hint = lv_label_create(scr);
    lv_obj_add_style(lbl_rec_hint, &style_dim, 0);
    lv_label_set_text(lbl_rec_hint, "Release to stop");
    lv_obj_align(lbl_rec_hint, LV_ALIGN_CENTER, 0, 80);
}

static void create_queue_screen(void) {
    lv_obj_t *scr = screens[SCREEN_QUEUE] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Header
    lbl_queue_header = lv_label_create(scr);
    lv_obj_add_style(lbl_queue_header, &style_sage, 0);
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
    lv_obj_set_size(btn_play, 140, 48);
    lv_obj_align(btn_play, LV_ALIGN_BOTTOM_LEFT, 25, -15);
    lv_obj_add_flag(btn_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *lbl_p = lv_label_create(btn_play);
    lv_label_set_text(lbl_p, "Play");
    lv_obj_center(lbl_p);
    lv_obj_add_event_cb(btn_play, play_btn_cb, LV_EVENT_CLICKED, NULL);

    // Delete button
    btn_delete = lv_btn_create(scr);
    lv_obj_add_style(btn_delete, &style_action_btn, 0);
    lv_obj_set_size(btn_delete, 140, 48);
    lv_obj_align(btn_delete, LV_ALIGN_BOTTOM_RIGHT, -25, -15);
    lv_obj_add_flag(btn_delete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *lbl_d = lv_label_create(btn_delete);
    lv_label_set_text(lbl_d, "Delete");
    lv_obj_center(lbl_d);
    lv_obj_add_event_cb(btn_delete, delete_btn_cb, LV_EVENT_CLICKED, NULL);

    // Swipe right → back to idle
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

static void create_sync_screen(void) {
    lv_obj_t *scr = screens[SCREEN_SYNC_CONFIRM] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

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

static esp_err_t init_i2c(void) {
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
        ESP_LOGW(TAG, "Touch init failed (%s) — display will work without touch",
                 esp_err_to_name(touch_ret));
    }

    // 4. Create LVGL screens (must hold the port lock)
    lvgl_port_lock(0);

    init_styles();
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

void display_next_screen(void) {
    if (!display_ready) return;
    screen_id_t next = (current_screen + 1) % SCREEN_COUNT;
    if (next == SCREEN_SYNC_CONFIRM) next = SCREEN_IDLE;
    display_show_screen(next);
}

void display_update_time(int hour, int min) {
    if (!display_ready || !lbl_time) return;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(lbl_time, "%02d:%02d", hour, min);
    lvgl_port_unlock();
}

void display_update_wifi(bool connected) {
    if (!display_ready || !lbl_wifi) return;
    lvgl_port_lock(0);
    lv_label_set_text(lbl_wifi, connected ? "Wi-Fi" : "No Wi-Fi");
    lvgl_port_unlock();
}

void display_update_queue_badge(int count) {
    if (!display_ready || !lbl_queue) return;
    lvgl_port_lock(0);
    if (count > 0) {
        lv_label_set_text_fmt(lbl_queue, "%d memo%s queued", count, count > 1 ? "s" : "");
    } else {
        lv_label_set_text(lbl_queue, "");
    }
    lvgl_port_unlock();
}

void display_update_battery(int percent) {
    if (!display_ready || !lbl_battery) return;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(lbl_battery, "%d%%", percent);
    lvgl_port_unlock();
}

void display_update_recording(int elapsed_sec, int max_sec) {
    if (!display_ready) return;
    lvgl_port_lock(0);
    if (lbl_rec_time) {
        lv_label_set_text_fmt(lbl_rec_time, "%d:%02d", elapsed_sec / 60, elapsed_sec % 60);
    }
    if (obj_pulse) {
        int phase = (elapsed_sec * 2) % 3;
        lv_opa_t opa = (phase == 0) ? LV_OPA_50 : (phase == 1) ? LV_OPA_80 : LV_OPA_60;
        lv_obj_set_style_bg_opa(obj_pulse, opa, 0);
    }
    if (lbl_rec_hint && max_sec - elapsed_sec <= 10) {
        lv_label_set_text_fmt(lbl_rec_hint, "Auto-stop in %ds", max_sec - elapsed_sec);
    }
    lvgl_port_unlock();
}

void display_show_sync_result(const char *title, bool success) {
    if (!display_ready) return;
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
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x5C997C), 0);

        // Register delete callback to free the strdup'd path when the button is destroyed
        lv_obj_add_event_cb(btn, memo_item_delete_cb, LV_EVENT_DELETE, memos[i]);
        lv_obj_add_event_cb(btn, memo_item_click_cb, LV_EVENT_CLICKED, memos[i]);
        // memos[i] ownership transferred to the delete callback; do not free here
    }

    // Free the pointer array only — individual strings are freed via LV_EVENT_DELETE
    if (memos) free(memos);

    lvgl_port_unlock();
}

void display_memo_playback_done(void) {
    if (!display_ready || !btn_play) return;
    lvgl_port_lock(0);
    lv_obj_t *lbl = lv_obj_get_child(btn_play, 0);
    if (lbl) lv_label_set_text(lbl, "Play");
    lvgl_port_unlock();
}

void *display_get_i2c_handle(void) {
    return (void *)i2c_handle;
}

void display_sleep(void) {
    if (!display_ready || display_sleeping) return;
    esp_lcd_panel_disp_on_off(panel_handle, false);
    display_sleeping = true;
    ESP_LOGI(TAG, "Display sleeping");
}

void display_wake(void) {
    if (!display_ready || !display_sleeping) return;
    esp_lcd_panel_disp_on_off(panel_handle, true);
    display_sleeping = false;
    inactivity_seconds = 0;
    // Force a full redraw so the screen content is restored
    lvgl_port_lock(0);
    lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Display waking");
}

bool display_is_sleeping(void) {
    return display_sleeping;
}

void display_note_activity(void) {
    inactivity_seconds = 0;
    if (display_sleeping) {
        display_wake();
    }
}

void display_tick_inactivity(void) {
    if (display_sleeping) return;
    inactivity_seconds++;
    if (inactivity_seconds >= DISPLAY_SLEEP_TIMEOUT_SEC) {
        display_sleep();
    }
}
