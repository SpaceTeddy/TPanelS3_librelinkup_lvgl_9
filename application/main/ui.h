/**
 * @file ui.h
 * @brief LVGL UI definitions for ESP32 LibreLinkUp Client
 * @version 8.3
 * 
 * This header contains all UI element declarations and related structures
 * for the ESP32 LibreLinkUp client application using LVGL.
 */

#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

// LVGL inclusion handling
#if defined __has_include
#if __has_include("lvgl.h")
#include "lvgl.h"
#elif __has_include("lvgl/lvgl.h")
#include "lvgl/lvgl.h"
#else
#include "lvgl.h"
#endif
#else
#include "lvgl.h"
#endif

/**
 * @defgroup ui_screens UI Screens
 * @brief Main application screen containers
 * @{
 */
extern lv_obj_t * ui_Main_screen;       ///< Primary display screen
extern lv_obj_t * ui_Welcome_screen;    ///< Initial welcome/status screen
extern lv_obj_t * ui_Debug_screen;      ///< Debug information screen
extern lv_obj_t * ui_FWUpdate_screen;   ///< Firmware update progress screen
extern lv_obj_t * ui_Login_screen;      ///< User authentication screen
/** @} */

/**
 * @defgroup welcome_screen Welcome Screen Elements
 * @brief UI components for the welcome screen
 * @{
 */
extern lv_obj_t * ui_Label_WelcomeInfo;     ///< Welcome message text label
extern lv_obj_t * ui_Label_WelcomeWifiInfo; ///< WiFi connection status label
/** @} */

/**
 * @defgroup main_screen Main Screen Elements
 * @brief Glucose monitoring UI components
 * @{
 */
extern lv_obj_t * ui_Label_GlucoseValue;        ///< Current glucose value display
extern lv_obj_t * ui_Label_GlucoseDelta;        ///< Glucose change from last reading
extern lv_obj_t * ui_Label_GlucoseTrendArrow;   ///< Visual trend indicator (arrow)
extern lv_obj_t * ui_Label_GlucoseTrendMessage; ///< Textual trend description
/** @} */

/**
 * @defgroup status_indicators Status Indicators
 * @brief System status display elements
 * @{
 */
extern lv_obj_t * ui_Label_LiebreViewAPIActivity; ///< API connection status
extern lv_obj_t * ui_Label_ESP32Connectivity;     ///< Network connectivity status
/** @} */

/**
 * @defgroup firmware_update Firmware Update Elements
 * @brief OTA update interface components
 * @{
 */
extern lv_obj_t * ui_Label_FWUpdateInfo;          ///< Update status text
extern lv_obj_t * ui_Label_FWUpdateProgress_percent; ///< Percentage complete label
extern lv_obj_t * ui_Bar_FWUpdateProgress;        ///< Progress bar visualization
/** @} */

/**
 * @defgroup chart_elements Chart Components
 * @brief Data visualization elements
 * @{
 */
extern lv_obj_t * ui_Chart_Glucose_5Min;         ///< 5-minute history chart
extern lv_obj_t * ui_Chart_Valid_Sensor;         ///< Sensor validity timeline
extern lv_obj_t * ui_Label_Chart_GlucoseLimitLow;  ///< Lower glucose boundary label
extern lv_obj_t * ui_Label_Chart_GlucoseLimitHigh; ///< Upper glucose boundary label
extern lv_obj_t * ui_Chart_Glucose_5Min_last_point_marker; ///< Latest data point indicator
extern lv_obj_t *ui_Chart_x_label_start;
extern lv_obj_t *ui_Chart_x_label_middle;
extern lv_obj_t *ui_Chart_x_label_end;
/** @} */

/**
 * @defgroup chart_series Chart Data Series
 * @brief Data series for chart visualizations
 * @{
 */
extern lv_chart_series_t * glucoseValueSeries_upperlimit; ///< Upper limit reference
extern lv_chart_series_t * glucoseValueSeries_lowerlimit; ///< Lower limit reference
extern lv_chart_series_t * glucoseValueSeries_5Min;       ///< 5-minute interval data
extern lv_chart_series_t * glucoseValueSeries_alert;      ///< Alert threshold markers
extern lv_chart_series_t * glucoseValueSeries_last;       ///< Latest reading marker

extern lv_chart_series_t * sensorValidDaysSeries_yellow;  ///< Valid period indication
extern lv_chart_series_t * sensorValidDaysSeries_grey;    ///< Expired period indication
/** @} */

/**
 * @defgroup debug_screen Debug Screen Elements
 * @brief Diagnostic information components
 * @{
 */
extern lv_obj_t * ui_Label_DebugInfo;            ///< General debug text
extern lv_obj_t * ui_Label_DebugDataRefresh;     ///< Data refresh status
extern lv_obj_t * ui_Label_DebugTime;            ///< System time display
extern lv_obj_t * ui_Label_DebugIP;              ///< Network IP address
extern lv_obj_t * ui_Label_DebugSensor;          ///< Sensor status
extern lv_obj_t * ui_Label_DebugSensorTimestamp; ///< Last sensor reading time
extern lv_obj_t * ui_Label_DebugSensorState;     ///< Sensor operational state
extern lv_obj_t * ui_Label_DebugSensorValue;     ///< Raw sensor data
extern lv_obj_t * ui_Label_DebugTest;            ///< Testing/debugging label
/** @} */

/**
 * @defgroup login_screen Login Screen Elements
 * @brief User authentication interface
 * @{
 */
extern lv_obj_t * ui_Label_LoginInfo;  ///< Login instructions
extern lv_obj_t * ui_kb;               ///< Virtual keyboard
extern lv_obj_t * ui_ta_email;         ///< Email input field
extern lv_obj_t * ui_ta_password;      ///< Password input field
extern lv_obj_t * btn_login;           ///< Login action button
extern lv_obj_t * btn_label;           ///< Login button text
/** @} */

/**
 * @defgroup control_buttons Control Buttons
 * @brief System control interface elements
 * @{
 */
extern lv_obj_t * ui_btn_wireguard;        ///< VPN configuration button
extern lv_obj_t * ui_btn_label_wireguard;  ///< VPN button text

extern lv_obj_t * ui_btn_mqtt;             ///< MQTT settings button
extern lv_obj_t * ui_btn_label_mqtt;       ///< MQTT button text

extern lv_obj_t * ui_btn_ota_update;       ///< Firmware update trigger
extern lv_obj_t * ui_btn_label_ota_update; ///< Update button text
/** @} */

/**
 * @defgroup font_declarations Font Declarations
 * @brief Custom font assets for the UI
 * @{
 */
LV_FONT_DECLARE(JetBrainsMonoLight16);  ///< 16px Light font
LV_FONT_DECLARE(JetBrainsMonoLight18);  ///< 18px Light font
LV_FONT_DECLARE(JetBrainsMonoLight20);  ///< 20px Light font
LV_FONT_DECLARE(JetBrainsMonoLight22);  ///< 22px Light font
LV_FONT_DECLARE(JetBrainsMonoLight24);  ///< 24px Light font
LV_FONT_DECLARE(JetBrainsMonoLight28);  ///< 28px Light font
LV_FONT_DECLARE(JetBrainsMonoLight32);  ///< 32px Light font
LV_FONT_DECLARE(JetBrainsMonoLight36);  ///< 36px Light font
LV_FONT_DECLARE(JetBrainsMonoLight40);  ///< 40px Light font
LV_FONT_DECLARE(JetBrainsMonoLight44);  ///< 44px Light font
LV_FONT_DECLARE(JetBrainsMonoLight48);  ///< 48px Light font
LV_FONT_DECLARE(JetBrainsMonoLight56);  ///< 56px Light font
LV_FONT_DECLARE(JetBrainsMonoLight72);  ///< 72px Light font
LV_FONT_DECLARE(JetBrainsMonoLight90);  ///< 90px Light font
LV_FONT_DECLARE(JetBrainsMonoLight100); ///< 100px Light font
/** @} */

/**
 * @defgroup constants Constants
 * @brief UI configuration values
 * @{
 */
#define MAX_BLOCKS      60        ///< Maximum number of progress blocks (minutes)
#define MAX_BAR_WIDTH   310       ///< Maximum container width in pixels
/** @} */

/**
 * @struct ProgressBarUI
 * @brief Structure managing progress bar visualization
 * 
 * Contains all elements needed to display and manage a segmented progress bar
 */
typedef struct {
    lv_obj_t *bar;              ///< Main container object
    lv_obj_t *label;            ///< Description label
    lv_obj_t *blocks[MAX_BLOCKS]; ///< Individual block elements
    int total_blocks;           ///< Total number of blocks
} ProgressBarUI;

/**
 * @brief Global progress bar instances
 * 
 * Three progress bars representing different time resolutions:
 * - Days remaining
 * - Hours remaining
 * - Minutes remaining
 */
ProgressBarUI dayBar, hourBar, minuteBar;

/**
 * @enum ValidBlockModes
 * @brief Configuration modes for progress bars
 */
enum {
    BLOCKS_VALID_DAYS    = 14,  ///< 14-day sensor validity period
    BLOCKS_VALID_HOURS   = 24,  ///< 24-hour display mode
    BLOCKS_VALID_MINUTES = 60   ///< 60-minute display mode
};

/**
 * @brief Initialize the complete UI system
 */
void ui_init(void);

/**
 * @brief Create a sensor validity progress bar
 * @param ui Pointer to ProgressBarUI instance
 * @param parent Parent LVGL object
 * @param num_blocks Total number of segments
 * @param x Horizontal position
 * @param y Vertical position
 * @param label_text Description text
 */
void create_sensor_valid_progress_bar(ProgressBarUI *ui, lv_obj_t *parent, int num_blocks, int x, int y, const char *label_text);

/**
 * @brief Update progress bar display
 * @param ui Pointer to ProgressBarUI instance
 * @param remaining Number of active segments to show
 */
void update_sensor_valid_progress_bar(ProgressBarUI *ui, int remaining);

/**
 * @brief Switch progress bar display mode
 * @param ui Pointer to ProgressBarUI instance
 * @param mode Time resolution mode (from ValidBlockModes)
 */
void switch_sensor_valid_progress_bar(ProgressBarUI *ui, int mode);

/**
 * @brief Create all sensor validity progress bars
 */
void create_all_sensor_valid_progress_bars();

/**
 * @brief Update chart validation indicators
 * @param ui ProgressBarUI instance reference
 * @param value Current validity value
 */
void update_chart_valid_values(ProgressBarUI *ui, int value);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif