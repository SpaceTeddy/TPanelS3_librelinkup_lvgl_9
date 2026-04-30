/**
 * @file ui.cpp
 * @brief LVGL User Interface implementation for ESP32 LibreLinkUp Client
 * @version 1.0
 * @date 2025
 * 
 * This file implements the complete UI for the glucose monitoring system,
 * including multiple screens (Welcome, Main, Debug, Login, FW Update) and
 * interactive elements like charts and progress bars.
 * 
 * @note LVGL Version: 9.2.2
 * @note Project: ESP32 LibreLinkUp Client
 */

#include "ui.h"

///////////////////// CONSTANTS ////////////////////

///////////////////// VARIABLES ////////////////////

// Screen objects
lv_obj_t * ui_Welcome_screen;
lv_obj_t * ui_Main_screen;
lv_obj_t * ui_Debug_screen;
lv_obj_t * ui_FWUpdate_screen;
lv_obj_t * ui_Login_screen;

// Welcome screen elements
lv_obj_t * ui_Label_WelcomeInfo;
lv_obj_t * ui_Label_WelcomeWifiInfo;

// Main screen - Glucose display elements
lv_obj_t * ui_Label_GlucoseValue;
lv_obj_t * ui_Label_GlucoseDelta;
lv_obj_t * ui_Label_GlucoseTrendArrow;
lv_obj_t * ui_Label_GlucoseTrendMessage;

// Main screen - Status indicators
lv_obj_t * ui_Label_LiebreViewAPIActivity;
lv_obj_t * ui_Label_ESP32Connectivity;
lv_obj_t * ui_Label_FWUpdateHint = NULL;

// Firmware update elements
lv_obj_t * ui_Label_FWUpdateInfo;
lv_obj_t * ui_Label_FWUpdateProgress_percent;
lv_obj_t * ui_Bar_FWUpdateProgress;

// Chart objects
lv_obj_t * ui_Chart_Glucose_5Min;
lv_obj_t * ui_Chart_Valid_Sensor;
lv_obj_t * ui_Label_Chart_GlucoseLimitLow;
lv_obj_t * ui_Label_Chart_GlucoseLimitHigh;
lv_obj_t * ui_Chart_Glucose_5Min_last_point_marker;

// Chart axis labels
lv_obj_t *ui_Chart_x_label_start;
lv_obj_t *ui_Chart_x_label_middle;
lv_obj_t *ui_Chart_x_label_end;

// Chart data series
lv_chart_series_t * glucoseValueSeries_upperlimit;
lv_chart_series_t * glucoseValueSeries_lowerlimit;
lv_chart_series_t * glucoseValueSeries_5Min;
lv_chart_series_t * glucoseValueSeries_alert;
lv_chart_series_t * glucoseValueSeries_last;

lv_chart_series_t * sensorValidDaysSeries_yellow;
lv_chart_series_t * sensorValidDaysSeries_grey;

// Debug screen elements
lv_obj_t * ui_Label_DebugInfo;
lv_obj_t * ui_Label_DebugDataRefresh;
lv_obj_t * ui_Label_DebugTime;
lv_obj_t * ui_Label_DebugIP;
lv_obj_t * ui_Label_DebugWG;
lv_obj_t * ui_Label_DebugSensor;
lv_obj_t * ui_Label_DebugSensorTimestamp;
lv_obj_t * ui_Label_DebugSensorState;
lv_obj_t * ui_Label_DebugSensorValue;
lv_obj_t * ui_Label_DebugTest;
lv_obj_t * ui_Label_DebugFsmReason;
lv_obj_t * ui_Label_DebugHeap;

// Login screen elements
lv_obj_t * ui_Label_LoginInfo;
lv_obj_t * ui_kb;
lv_obj_t * ui_ta_email;
lv_obj_t * ui_ta_password;
lv_obj_t * btn_login;
lv_obj_t * btn_label;

// Debug screen buttons
lv_obj_t * ui_btn_wireguard;
lv_obj_t * ui_btn_label_wireguard;
lv_obj_t * ui_btn_mqtt;
lv_obj_t * ui_btn_label_mqtt;
lv_obj_t * ui_btn_ota_update;
lv_obj_t * ui_btn_label_ota_update;

/// Progress bar definitions with C-linkage (as ui.h declares in extern "C")
ProgressBarUI dayBar14 = { 0 };
ProgressBarUI dayBar15 = { 0 };
ProgressBarUI hourBar = { 0 };
ProgressBarUI minuteBar = { 0 };

///////////////////// LVGL CONFIGURATION CHECKS ////////////////////

#if LV_COLOR_DEPTH != 16
    #error "LV_COLOR_DEPTH should be 16bit to match SquareLine Studio's settings"
#endif

///////////////////// HELPER FUNCTIONS ////////////////////

/**
 * @brief Creates a styled label with common settings
 * 
 * @param[in] parent     Parent LVGL object
 * @param[in] font       Font to use for the label
 * @param[in] color      Text color (hex value)
 * @param[in] width      Label width in pixels
 * @param[in] align      Alignment type
 * @param[in] x_offset   X-axis offset
 * @param[in] y_offset   Y-axis offset
 * 
 * @return Pointer to created label object, NULL on failure
 */
static lv_obj_t* create_styled_label(lv_obj_t* parent, const lv_font_t* font, 
                                      uint32_t color, lv_coord_t width,
                                      lv_align_t align, lv_coord_t x_offset, 
                                      lv_coord_t y_offset)
{
    if (parent == NULL || font == NULL) {
        return NULL;
    }

    lv_obj_t* label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }

    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(label, width);
    lv_obj_align(label, align, x_offset, y_offset);
    
    return label;
}

/**
 * @brief Safely deletes an LVGL object and sets pointer to NULL
 * 
 * @param[in,out] obj Pointer to LVGL object pointer
 */
static void safe_delete_obj(lv_obj_t** obj)
{
    if (obj != NULL && *obj != NULL) {
        lv_obj_del(*obj);
        *obj = NULL;
    }
}

///////////////////// SENSOR VALIDITY PROGRESS BAR FUNCTIONS ////////////////////

/**
 * @brief Creates a visual progress bar for sensor validity display
 * 
 * Creates a horizontal bar chart with individual blocks to visualize
 * remaining sensor lifetime. The bar can display days (14/15), hours (24),
 * or minutes (60) depending on the configuration.
 * 
 * @param[out] ui          Pointer to ProgressBarUI structure to initialize
 * @param[in]  parent      Parent LVGL object to attach the bar to
 * @param[in]  num_blocks  Number of blocks to display
 * @param[in]  x           X-axis offset from center alignment
 * @param[in]  y           Y-axis offset from center alignment
 * @param[in]  label_text  Initial text for the label (usually empty string)
 * 
 * @note The function dynamically calculates block size based on MAX_BAR_WIDTH
 * @warning If called multiple times without cleanup, may cause memory leaks
 * @warning num_blocks is clamped to MAX_BLOCKS if exceeded
 */
void create_sensor_valid_progress_bar(ProgressBarUI *ui, lv_obj_t *parent, 
                                      int num_blocks, int x, int y, 
                                      const char *label_text) 
{
    if (ui == NULL || parent == NULL || label_text == NULL) {
        return;
    }

    // Clean up existing objects to prevent memory leaks
    safe_delete_obj(&ui->bar);
    safe_delete_obj(&ui->label);
    
    // Clamp num_blocks to valid range
    if (num_blocks > MAX_BLOCKS) {
        num_blocks = MAX_BLOCKS;
    }
    if (num_blocks < 1) {
        num_blocks = 1;
    }
    
    ui->total_blocks = num_blocks;

    // Calculate dynamic block size (not wider than MAX_BAR_WIDTH)
    int block_size = MAX_BAR_WIDTH / num_blocks;
    if (block_size < MIN_BLOCK_SIZE) {
        block_size = MIN_BLOCK_SIZE;
    }
    
    // Calculate actual width (maximum MAX_BAR_WIDTH)
    int total_width = block_size * num_blocks;
    if (total_width > MAX_BAR_WIDTH) {
        total_width = MAX_BAR_WIDTH;
    }

    // Create main container for the display
    ui->bar = lv_obj_create(parent);
    if (ui->bar == NULL) {
        return;
    }
    
    lv_obj_set_size(ui->bar, total_width, block_size + 10);
    lv_obj_align(ui->bar, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(ui->bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(ui->bar, 0, 0);
    lv_obj_set_style_pad_all(ui->bar, 5, 0);

    // Create individual blocks within the container
    for (int i = 0; i < num_blocks; i++) {
        ui->blocks[i] = lv_obj_create(ui->bar);
        if (ui->blocks[i] == NULL) {
            continue;
        }
        
        lv_obj_set_size(ui->blocks[i], block_size - BLOCK_SPACING, block_size - BLOCK_SPACING);
        lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(80, 80, 80), 0);
        lv_obj_set_style_radius(ui->blocks[i], 2, 0);
        lv_obj_set_style_border_width(ui->blocks[i], 0, 0);
        lv_obj_align(ui->blocks[i], LV_ALIGN_LEFT_MID, i * block_size, 0);
    }

    // Create text label for the display
    ui->label = lv_label_create(parent);
    if (ui->label != NULL) {
        lv_label_set_text(ui->label, label_text);
        lv_obj_set_style_text_font(ui->label, &JetBrainsMonoLight24, 0);
        lv_obj_set_style_text_color(ui->label, lv_color_white(), 0);
        lv_obj_align(ui->label, LV_ALIGN_CENTER, x, y + block_size + 10);
    }
}

/**
 * @brief Updates the visual state of a sensor validity progress bar
 * 
 * Changes the color and number of active blocks based on remaining time.
 * Color scheme:
 * - Green: Normal operation (days mode with >3 days remaining)
 * - Yellow: Warning (days mode with ≤3 days remaining)
 * - Red: Critical (hours/minutes mode)
 * - Gray: Inactive blocks
 * 
 * @param[in,out] ui        Pointer to ProgressBarUI structure
 * @param[in]     remaining Number of remaining units (days/hours/minutes)
 * 
 * @note Automatically hides label when remaining reaches 0
 */
void update_sensor_valid_progress_bar(ProgressBarUI *ui, int remaining_raw)
{
    bool sensor_expired = (remaining_raw < 0);
    int remaining = remaining_raw;

    // Normal clamping, but keep the expired state via sensor_expired
    if (remaining > ui->total_blocks) remaining = ui->total_blocks;
    if (remaining < 0) remaining = 0;

    // Set colors
    for (int i = 0; i < ui->total_blocks; i++) {

        // Sensor expired -> ALL gray, independent of remaining
        if (sensor_expired) {
            lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(80, 80, 80), 0); // gray
            continue;
        }

        if (i < remaining) {
            // Days mode
            if ((ui->total_blocks == BLOCKS_VALID_14DAYS ||
                 ui->total_blocks == BLOCKS_VALID_15DAYS) &&
                remaining > 3)
            {
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(100, 200, 100), 0); // green
            }
            else if ((ui->total_blocks == BLOCKS_VALID_14DAYS ||
                      ui->total_blocks == BLOCKS_VALID_15DAYS) &&
                     remaining <= 3)
            {
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(200, 200, 0), 0); // yellow
            }
            else {
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(200, 0, 0), 0);   // red
            }
        } else {
            lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(80, 80, 80), 0);     // gray
        }
    }

    // Clear label first
    //lv_label_set_text_fmt(ui->label, "");
    //lv_timer_handler();

    // Sensor expired -> hide label and clear text
    if (sensor_expired) {
        lv_obj_add_flag(ui->label, LV_OBJ_FLAG_HIDDEN);
        lv_timer_handler();
        return;
    }

    // Normal labels
    if (ui->total_blocks == BLOCKS_VALID_14DAYS || ui->total_blocks == BLOCKS_VALID_15DAYS) {
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d days", remaining);
        lv_obj_add_flag(hourBar.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(minuteBar.label, LV_OBJ_FLAG_HIDDEN);
    } else if (ui->total_blocks == BLOCKS_VALID_HOURS) {
        lv_obj_add_flag(dayBar14.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dayBar15.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(minuteBar.label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d hours", remaining);
    } else if (ui->total_blocks == BLOCKS_VALID_MINUTES) {
        lv_obj_add_flag(dayBar14.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dayBar15.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hourBar.label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d minutes", remaining);
    }

    // remaining==0 but not expired (if you ever need that behavior)
    if (remaining <= 0) {
        lv_label_set_text(ui->label, "");
        lv_obj_add_flag(ui->label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui->label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_handler();
}

/**
 * @brief Switches between different sensor validity display modes
 * 
 * Hides all progress bars and shows only the specified one.
 * Used to switch between day/hour/minute views.
 * 
 * @param[in] ui   Pointer to the ProgressBarUI to make visible
 * 
 * @note All bars are hidden first, then the selected one is shown
 */
void switch_sensor_valid_progress_bar(ProgressBarUI *ui) 
{
    // Hide all bars and labels with NULL checks
    if (dayBar14.bar != NULL) {
        lv_obj_add_flag(dayBar14.bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (dayBar15.bar != NULL) {
        lv_obj_add_flag(dayBar15.bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (hourBar.bar != NULL) {
        lv_obj_add_flag(hourBar.bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (minuteBar.bar != NULL) {
        lv_obj_add_flag(minuteBar.bar, LV_OBJ_FLAG_HIDDEN);
    }

    if (dayBar14.label != NULL) {
        lv_obj_add_flag(dayBar14.label, LV_OBJ_FLAG_HIDDEN);
    }
    if (hourBar.label != NULL) {
        lv_obj_add_flag(hourBar.label, LV_OBJ_FLAG_HIDDEN);
    }
    if (minuteBar.label != NULL) {
        lv_obj_add_flag(minuteBar.label, LV_OBJ_FLAG_HIDDEN);
    }

    if (ui == NULL || ui->bar == NULL) {
        return;
    }

    // Show the selected bar and its label
    if (ui->bar == dayBar14.bar) {
        lv_obj_clear_flag(dayBar14.bar, LV_OBJ_FLAG_HIDDEN);
        if (dayBar14.label != NULL) {
            lv_obj_clear_flag(dayBar14.label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (ui->bar == dayBar15.bar) {
        lv_obj_clear_flag(dayBar15.bar, LV_OBJ_FLAG_HIDDEN);
        if (dayBar14.label != NULL) {
            lv_obj_clear_flag(dayBar14.label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (ui->bar == hourBar.bar) {
        lv_obj_clear_flag(hourBar.bar, LV_OBJ_FLAG_HIDDEN);
        if (hourBar.label != NULL) {
            lv_obj_clear_flag(hourBar.label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (ui->bar == minuteBar.bar) {
        lv_obj_clear_flag(minuteBar.bar, LV_OBJ_FLAG_HIDDEN);
        if (minuteBar.label != NULL) {
            lv_obj_clear_flag(minuteBar.label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief Creates all sensor validity progress bars
 * 
 * Initializes all progress bar variants (14-day, 15-day, hourly, and minute bars).
 * Only creates bars that don't already exist to prevent memory leaks.
 * 
 * @note Should be called once during UI initialization
 */
void create_all_sensor_valid_progress_bars() 
{
    if (ui_Main_screen == NULL) {
        return;
    }

    // Always recreate day bars (they might need to switch between 14/15 days)
    create_sensor_valid_progress_bar(&dayBar15, ui_Main_screen, BLOCKS_VALID_15DAYS, 
                                     0, SENSOR_BAR_Y_OFFSET, "");    
    create_sensor_valid_progress_bar(&dayBar14, ui_Main_screen, BLOCKS_VALID_14DAYS, 
                                     0, SENSOR_BAR_Y_OFFSET, "");

    // Create hour/minute bars only if they don't exist yet
    if (hourBar.bar == NULL && hourBar.label == NULL) {
        create_sensor_valid_progress_bar(&hourBar, ui_Main_screen, BLOCKS_VALID_HOURS, 
                                        0, SENSOR_BAR_Y_OFFSET, "");
    }
    if (minuteBar.bar == NULL && minuteBar.label == NULL) {
        create_sensor_valid_progress_bar(&minuteBar, ui_Main_screen, BLOCKS_VALID_MINUTES, 
                                        0, SENSOR_BAR_Y_OFFSET, "");
    }
}

/**
 * @brief Updates the appropriate progress bar based on UI pointer
 * 
 * Determines which progress bar to update by comparing the bar pointer
 * and calls the appropriate update function.
 * 
 * @param[in] ui    Pointer to ProgressBarUI structure
 * @param[in] value New remaining value to display
 */
void update_chart_valid_values(ProgressBarUI *ui, int value)
{
    bool expired = (value < 0);

    if (expired)
    {
        // Sensor expired -> all bars grey
        update_sensor_valid_progress_bar(&dayBar14,  -1);
        update_sensor_valid_progress_bar(&dayBar15,  -1);
        update_sensor_valid_progress_bar(&hourBar,   -1);
        update_sensor_valid_progress_bar(&minuteBar, -1);

        // optional: all labels hidden
        lv_obj_add_flag(dayBar14.label,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dayBar15.label,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hourBar.label,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(minuteBar.label,  LV_OBJ_FLAG_HIDDEN);

        return;
    }

    // normal mode → show active bar
    switch_sensor_valid_progress_bar(ui);

    // redirect to correct bar update
    if (ui->bar == dayBar14.bar) {
        update_sensor_valid_progress_bar(&dayBar14, value);
    }
    else if (ui->bar == dayBar15.bar) {
        update_sensor_valid_progress_bar(&dayBar15, value);
    }
    else if (ui->bar == hourBar.bar) {
        update_sensor_valid_progress_bar(&hourBar, value);
    }
    else if (ui->bar == minuteBar.bar) {
        update_sensor_valid_progress_bar(&minuteBar, value);
    }
}

///////////////////// SCREEN INITIALIZATION FUNCTIONS ////////////////////

/**
 * @brief Initializes the welcome screen
 * 
 * Creates the initial welcome screen displayed at startup.
 * Shows welcome message and WiFi connection information.
 */
void ui_Welcome_screen_init(void)
{
    ui_Welcome_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Welcome_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Welcome_screen, lv_color_black(), LV_PART_MAIN);

    ui_Label_WelcomeInfo = create_styled_label(ui_Welcome_screen, &JetBrainsMonoLight56,
                                                UI_COLOR_WHITE, WELCOME_MESSAGE_WIDTH, LV_ALIGN_CENTER, 0, -60);
    if (ui_Label_WelcomeInfo != NULL) {
        lv_obj_set_style_text_align(ui_Label_WelcomeInfo, LV_TEXT_ALIGN_CENTER, 0);
    }

    ui_Label_WelcomeWifiInfo = create_styled_label(ui_Welcome_screen, &JetBrainsMonoLight24,
                                                    UI_COLOR_WHITE, WELCOME_MESSAGE_WIDTH, LV_ALIGN_CENTER, 0, 150);
    if (ui_Label_WelcomeWifiInfo != NULL) {
        lv_obj_set_style_text_align(ui_Label_WelcomeWifiInfo, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "");
    }
}

/**
 * @brief Initializes the main screen
 * 
 * Creates the main glucose monitoring screen with:
 * - Current glucose value display
 * - Trend arrow and delta
 * - Glucose history chart
 * - Sensor validity progress bar
 * - Status indicators
 */
void ui_Main_screen_init(void)
{
    ui_Main_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Main_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Main_screen, lv_color_black(), LV_PART_MAIN);

    // Main glucose value display
    ui_Label_GlucoseValue = create_styled_label(ui_Main_screen, &JetBrainsMonoLight100,
                                                 UI_COLOR_WHITE, GLUCOSE_VALUE_WIDTH, LV_ALIGN_CENTER, 
                                                 0, GLUCOSE_VALUE_Y_OFFSET);
    if (ui_Label_GlucoseValue != NULL) {
        lv_label_set_text(ui_Label_GlucoseValue, "");
    }

    // Glucose delta display
    ui_Label_GlucoseDelta = create_styled_label(ui_Main_screen, &JetBrainsMonoLight36,
                                                 UI_COLOR_WHITE, GLUCOSE_DELTA_WIDTH, LV_ALIGN_CENTER, 
                                                 0, GLUCOSE_DELTA_Y_OFFSET);
    if (ui_Label_GlucoseDelta != NULL) {
        lv_label_set_text(ui_Label_GlucoseDelta, "");
    }
    
    // Trend arrow
    ui_Label_GlucoseTrendArrow = create_styled_label(ui_Main_screen, &JetBrainsMonoLight72,
                                                      UI_COLOR_WHITE, GLUCOSE_TREND_WIDTH, LV_ALIGN_CENTER, 
                                                      GLUCOSE_ARROW_X_OFFSET, GLUCOSE_ARROW_Y_OFFSET);
    if (ui_Label_GlucoseTrendArrow != NULL) {
        lv_label_set_text(ui_Label_GlucoseTrendArrow, "");
    }

    // Trend message
    ui_Label_GlucoseTrendMessage = create_styled_label(ui_Main_screen, &JetBrainsMonoLight36,
                                                        UI_COLOR_RED, GLUCOSE_MESSAGE_WIDTH, LV_ALIGN_CENTER, 0, 55);
    if (ui_Label_GlucoseTrendMessage != NULL) {
        lv_label_set_text(ui_Label_GlucoseTrendMessage, "");
    }

    // API Activity indicator
    ui_Label_LiebreViewAPIActivity = create_styled_label(ui_Main_screen, &JetBrainsMonoLight36,
                                                          UI_COLOR_WHITE, API_ACTIVITY_WIDTH, LV_ALIGN_CENTER, 100, -180);
    if (ui_Label_LiebreViewAPIActivity != NULL) {
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, " ");
    }

    // Connectivity indicator
    ui_Label_ESP32Connectivity = create_styled_label(ui_Main_screen, &JetBrainsMonoLight24,
                                                      UI_COLOR_GREEN, API_ACTIVITY_WIDTH, LV_ALIGN_CENTER, 100, -180);
    if (ui_Label_ESP32Connectivity != NULL) {
        lv_label_set_text(ui_Label_ESP32Connectivity, " ");
    }
    
    // Chart limit labels
    ui_Label_Chart_GlucoseLimitHigh = create_styled_label(ui_Main_screen, &JetBrainsMonoLight16,
                                                           UI_COLOR_WHITE, 40, LV_ALIGN_CENTER, -175, 0);
    if (ui_Label_Chart_GlucoseLimitHigh != NULL) {
        lv_label_set_text(ui_Label_Chart_GlucoseLimitHigh, "");
    }

    ui_Label_Chart_GlucoseLimitLow = create_styled_label(ui_Main_screen, &JetBrainsMonoLight16,
                                                          UI_COLOR_WHITE, 40, LV_ALIGN_CENTER, -180, 95);
    if (ui_Label_Chart_GlucoseLimitLow != NULL) {
        lv_label_set_text(ui_Label_Chart_GlucoseLimitLow, "");
    }

    // Glucose chart
    ui_Chart_Glucose_5Min = lv_chart_create(ui_Main_screen);
    lv_obj_set_size(ui_Chart_Glucose_5Min, CHART_WIDTH, CHART_HEIGHT);
    lv_obj_align(ui_Chart_Glucose_5Min, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_size(ui_Chart_Glucose_5Min, 1, 1, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ui_Chart_Glucose_5Min, 5, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ui_Chart_Glucose_5Min, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Chart_Glucose_5Min, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_chart_set_update_mode(ui_Chart_Glucose_5Min, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(ui_Chart_Glucose_5Min, LV_CHART_AXIS_PRIMARY_Y, 40, 225);
    lv_chart_set_point_count(ui_Chart_Glucose_5Min, TOTAL_CHART_POINTS);
    lv_chart_set_type(ui_Chart_Glucose_5Min, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(ui_Chart_Glucose_5Min, 5, 5);
    lv_obj_clear_flag(ui_Chart_Glucose_5Min, LV_OBJ_FLAG_SCROLLABLE);
    
    // Add chart data series
    glucoseValueSeries_upperlimit = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_lowerlimit = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_5Min       = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_alert      = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_last       = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_YELLOW), LV_CHART_AXIS_PRIMARY_Y);

    // Circle marker for last graph value
    ui_Chart_Glucose_5Min_last_point_marker = lv_obj_create(ui_Chart_Glucose_5Min);
    lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ui_Chart_Glucose_5Min_last_point_marker, 15, 15);
    lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_radius(ui_Chart_Glucose_5Min_last_point_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ui_Chart_Glucose_5Min_last_point_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(ui_Chart_Glucose_5Min_last_point_marker, 2, 0); 

    // Create X-axis labels
    ui_Chart_x_label_start = lv_label_create(ui_Main_screen);
    if (ui_Chart_x_label_start != NULL) {
        lv_label_set_text(ui_Chart_x_label_start, "");
        lv_obj_set_pos(ui_Chart_x_label_start, 50, 375);
    }

    ui_Chart_x_label_middle = lv_label_create(ui_Main_screen);
    if (ui_Chart_x_label_middle != NULL) {
        lv_label_set_text(ui_Chart_x_label_middle, "");
        lv_obj_set_pos(ui_Chart_x_label_middle, 225, 375);
    }

    ui_Chart_x_label_end = lv_label_create(ui_Main_screen);
    if (ui_Chart_x_label_end != NULL) {
        lv_label_set_text(ui_Chart_x_label_end, "");
        lv_obj_set_pos(ui_Chart_x_label_end, 395, 375);
    }

    // Initialize sensor validity progress bars
    create_all_sensor_valid_progress_bars();

    // Firmware update hint label (hidden by default, shown when update is available)
    ui_Label_FWUpdateHint = create_styled_label(ui_Main_screen, &JetBrainsMonoLight16,
                                                 0x4488FF, 400, LV_ALIGN_CENTER, 0, 228);
    if (ui_Label_FWUpdateHint != NULL) {
        lv_label_set_text(ui_Label_FWUpdateHint, "");
        lv_obj_add_flag(ui_Label_FWUpdateHint, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Initializes the debug screen
 * 
 * Creates a debug screen displaying:
 * - System time
 * - IP address
 * - Data refresh countdown
 * - Sensor information
 * - Debug values
 */
void ui_Debug_screen_init(void)
{
    ui_Debug_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Debug_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Debug_screen, lv_color_black(), LV_PART_MAIN);

    // ── Title ───────────────────────────────────────────────────────────────
    ui_Label_DebugInfo = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight32,
                                              UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 15);
    if (ui_Label_DebugInfo != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugInfo, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugInfo, "Debug");
    }

    // ── SYSTEM ──────────────────────────────────────────────────────────────
    ui_Label_DebugIP = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                            UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 65);
    if (ui_Label_DebugIP != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugIP, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugIP, "WiFi: -");
    }

    ui_Label_DebugWG = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                            UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 87);
    if (ui_Label_DebugWG != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugWG, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugWG, "WG: -");
    }

    ui_Label_DebugTime = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                              UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 109);
    if (ui_Label_DebugTime != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugTime, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugTime, "Time: -");
    }

    ui_Label_DebugTest = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                              UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 131);
    if (ui_Label_DebugTest != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugTest, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugTest, "FW: -");
    }

    // ── HEAP (bottom, above buttons) ────────────────────────────────────────
    ui_Label_DebugHeap = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                              UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 330);
    if (ui_Label_DebugHeap != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugHeap, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugHeap, "Heap: -");
    }

    // ── FSM ─────────────────────────────────────────────────────────────────
    ui_Label_DebugDataRefresh = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                     UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 158);
    if (ui_Label_DebugDataRefresh != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugDataRefresh, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugDataRefresh, "State: -");
    }

    ui_Label_DebugFsmReason = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                   UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 180);
    if (ui_Label_DebugFsmReason != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugFsmReason, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugFsmReason, "Reason: -");
    }

    // ── SENSOR ──────────────────────────────────────────────────────────────
    ui_Label_DebugSensor = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 207);
    if (ui_Label_DebugSensor != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugSensor, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugSensor, "Sensor: -");
    }

    ui_Label_DebugSensorState = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                     UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 229);
    if (ui_Label_DebugSensorState != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugSensorState, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugSensorState, "State: -");
    }

    ui_Label_DebugSensorValue = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                     UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 251);
    if (ui_Label_DebugSensorValue != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugSensorValue, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugSensorValue, "Value: -");
    }

    // ── MQTT ────────────────────────────────────────────────────────────────
    ui_Label_DebugSensorTimestamp = create_styled_label(ui_Debug_screen, &JetBrainsMonoLight20,
                                                         UI_COLOR_WHITE, DEBUG_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 10, 278);
    if (ui_Label_DebugSensorTimestamp != NULL) {
        lv_obj_set_style_text_align(ui_Label_DebugSensorTimestamp, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_DebugSensorTimestamp, "MQTT: -");
    }
}

/**
 * @brief Initializes the login screen
 * 
 * Creates login screen with:
 * - Email input field
 * - Password input field
 * - On-screen keyboard
 * - Login button
 */
void ui_Login_screen_init(void)
{
    ui_Login_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Login_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Login_screen, lv_color_black(), LV_PART_MAIN);

    // Login screen title
    ui_Label_LoginInfo = create_styled_label(ui_Login_screen, &JetBrainsMonoLight32,
                                              UI_COLOR_WHITE, LOGIN_MESSAGE_WIDTH, LV_TEXT_ALIGN_CENTER, 120, 30);
    if (ui_Label_LoginInfo != NULL) {
        lv_obj_set_style_text_align(ui_Label_LoginInfo, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(ui_Label_LoginInfo, "LLU Login");
    }

    // Create on-screen keyboard
    ui_kb = lv_keyboard_create(ui_Login_screen);
    lv_obj_set_size(ui_kb, 400, 180);
    lv_obj_align(ui_kb, LV_ALIGN_CENTER, 0, 40);

    // Email input text area
    ui_ta_email = lv_textarea_create(ui_Login_screen);
    lv_obj_align(ui_ta_email, LV_TEXT_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_text_font(ui_ta_email, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_textarea_set_placeholder_text(ui_ta_email, "user@example.com");
    lv_obj_set_size(ui_ta_email, 200, 40);

    // Password input text area
    ui_ta_password = lv_textarea_create(ui_Login_screen);
    lv_obj_align(ui_ta_password, LV_TEXT_ALIGN_CENTER, 0, 140);
    lv_obj_set_style_text_font(ui_ta_password, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_textarea_set_placeholder_text(ui_ta_password, "password");
    lv_textarea_set_password_mode(ui_ta_password, true);
    lv_obj_set_size(ui_ta_password, 200, 40);

    // Login button
    btn_login = lv_btn_create(ui_Login_screen);
    lv_obj_set_size(btn_login, 100, 50);
    lv_obj_align(btn_login, LV_ALIGN_CENTER, 0, 180);

    btn_label = lv_label_create(btn_login);
    if (btn_label != NULL) {
        lv_label_set_text(btn_label, "Login");
        lv_obj_center(btn_label);
    }
}

/**
 * @brief Initializes the firmware update screen
 * 
 * Creates screen for displaying firmware update progress with:
 * - Update status message
 * - Progress percentage display
 */
void ui_FWUpdate_screen_init(void)
{
    ui_FWUpdate_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_FWUpdate_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_FWUpdate_screen, lv_color_black(), LV_PART_MAIN);

    // Update info message
    ui_Label_FWUpdateInfo = create_styled_label(ui_FWUpdate_screen, &JetBrainsMonoLight36,
                                                 UI_COLOR_WHITE, FWUPDATE_MESSAGE_WIDTH, LV_ALIGN_CENTER, 0, -100);
    if (ui_Label_FWUpdateInfo != NULL) {
        lv_label_set_text(ui_Label_FWUpdateInfo, "FW Update ...");
    }

    // Progress percentage display
    ui_Label_FWUpdateProgress_percent = create_styled_label(ui_FWUpdate_screen, &JetBrainsMonoLight72,
                                                             UI_COLOR_WHITE, FWUPDATE_MESSAGE_WIDTH, LV_ALIGN_CENTER, 0, 50);
    if (ui_Label_FWUpdateProgress_percent != NULL) {
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, "0%");
    }
}

/**
 * @brief Initializes debug screen buttons
 * 
 * Creates control buttons on debug screen:
 * - WireGuard toggle button
 * - MQTT toggle button
 * - OTA update button
 * 
 * @param[in] parent Parent object to attach buttons to (should be ui_Debug_screen)
 */
void ui_btn_debug_screen_init(lv_obj_t * parent)
{
    if (parent == NULL) {
        return;
    }

    // WireGuard button
    ui_btn_wireguard = lv_btn_create(parent);
    lv_obj_align(ui_btn_wireguard, LV_ALIGN_CENTER, -115, 150);
    lv_obj_add_flag(ui_btn_wireguard, LV_OBJ_FLAG_CHECKABLE);

    ui_btn_label_wireguard = lv_label_create(ui_btn_wireguard);
    if (ui_btn_label_wireguard != NULL) {
        lv_obj_center(ui_btn_label_wireguard);
        lv_label_set_text(ui_btn_label_wireguard, "WG");
    }

    // MQTT button
    ui_btn_mqtt = lv_btn_create(parent);
    lv_obj_align(ui_btn_mqtt, LV_ALIGN_CENTER, 5, 150);
    lv_obj_add_flag(ui_btn_mqtt, LV_OBJ_FLAG_CHECKABLE);    
    
    ui_btn_label_mqtt = lv_label_create(ui_btn_mqtt);
    if (ui_btn_label_mqtt != NULL) {
        lv_obj_center(ui_btn_label_mqtt);
        lv_label_set_text(ui_btn_label_mqtt, "MQTT");
    }

    // OTA Update button
    ui_btn_ota_update = lv_btn_create(parent);
    lv_obj_align(ui_btn_ota_update, LV_ALIGN_CENTER, 130, 150);
    lv_obj_add_flag(ui_btn_ota_update, LV_OBJ_FLAG_CHECKABLE);    
    
    ui_btn_label_ota_update = lv_label_create(ui_btn_ota_update);
    if (ui_btn_label_ota_update != NULL) {
        lv_obj_center(ui_btn_label_ota_update);
        lv_label_set_text(ui_btn_label_ota_update, "OTA");
    }
}

///////////////////// MAIN INITIALIZATION ////////////////////

/**
 * @brief Main UI initialization function
 * 
 * Initializes the complete user interface:
 * - Sets up default theme
 * - Initializes all screens
 * - Initializes buttons
 * - Loads welcome screen as default
 * 
 * @note Should be called once during system startup after LVGL initialization
 */
void ui_init(void)
{
    // Setup default theme
    lv_disp_t * dispp = lv_disp_get_default();
    lv_theme_t * theme = lv_theme_default_init(dispp, 
                                                lv_palette_main(LV_PALETTE_GREY), 
                                                lv_palette_main(LV_PALETTE_GREY), 
                                                true, 
                                                LV_FONT_DEFAULT);
    
    lv_disp_set_theme(dispp, theme);

    // Initialize all screens
    ui_Welcome_screen_init();
    ui_Main_screen_init();
    ui_Debug_screen_init();
    ui_Login_screen_init();
    ui_FWUpdate_screen_init();
    
    // Initialize debug screen buttons
    ui_btn_debug_screen_init(ui_Debug_screen);

    // Load welcome screen as default
    lv_disp_load_scr(ui_Welcome_screen);
}
