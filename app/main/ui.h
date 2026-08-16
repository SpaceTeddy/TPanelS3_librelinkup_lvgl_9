/**
 * @file ui.h
 * @brief LVGL User Interface header for ESP32 LibreLinkUp Client
 * @version 1.0
 * @date 2025
 * 
 * This header file declares all UI elements, screens, and functions
 * for the glucose monitoring system interface.
 * 
 * @note LVGL Version: 9.2.2
 * @note Project: ESP32 LibreLinkUp Client
 */

#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

///////////////////// INCLUDES ////////////////////

#include "lvgl.h"
#include "ui_fonts.h"

///////////////////// CONFIGURATION CONSTANTS ////////////////////
#define TOTAL_CHART_POINTS          (142)    ///< Number of data points in glucose chart
#define MAX_BAR_WIDTH               (400)    ///< Maximum width of progress bars in pixels
#define SCREEN_WIDTH                (480)    ///< Display width in pixels
#define SCREEN_HEIGHT               (480)    ///< Display height in pixels
#define WELCOME_MESSAGE_WIDTH       (400)    ///< Welcome message width in pixels
#define DEBUG_MESSAGE_WIDTH         (400)    ///< Debug message width in pixels
#define LOGIN_MESSAGE_WIDTH         (400)    ///< Login message width in pixels
#define FWUPDATE_MESSAGE_WIDTH      (400)    ///< Fwupdate message width in pixels
#define API_ACTIVITY_WIDTH          (20)     ///< Api activity width in pixels
#define CHART_WIDTH                 (400)    ///< Glucose chart width in pixels
#define CHART_HEIGHT                (200)    ///< Glucose chart height in pixels
#define GLUCOSE_VALUE_WIDTH         (200)    ///< Glucose value width in pixels
#define GLUCOSE_DELTA_WIDTH         (300)    ///< Glucose delta width in pixels
#define GLUCOSE_TREND_WIDTH         (100)    ///< Glucose trend width in pixels
#define GLUCOSE_MESSAGE_WIDTH       (400)    ///< Glucose trend message width in pixels

// Layout offsets
#define GLUCOSE_VALUE_Y_OFFSET      (-140)   ///< Y-offset for main glucose value
#define GLUCOSE_DELTA_Y_OFFSET      (-60)    ///< Y-offset for glucose delta value
#define GLUCOSE_ARROW_X_OFFSET      (130)    ///< X-offset for trend arrow
#define GLUCOSE_ARROW_Y_OFFSET      (-100)   ///< Y-offset for trend arrow
#define SENSOR_BAR_Y_OFFSET         (170)    ///< Y-offset for sensor validity bar

// Colors
#define UI_COLOR_WHITE                 (0xFFFFFF)
#define UI_COLOR_RED                   (0xFF0000)
#define UI_COLOR_GREEN                 (0x00FF00)
#define UI_COLOR_BLACK                 (0x000000)

/// Block sizes for progress bars
#define MIN_BLOCK_SIZE              (5)      ///< Minimum block size for visual clarity
#define BLOCK_SPACING               (2)      ///< Spacing between blocks in pixels

/// Maximum number of blocks in a progress bar
#define MAX_BLOCKS                  (60)

/// Number of blocks for 14-day sensor display
#define BLOCKS_VALID_14DAYS         (14)

/// Number of blocks for 15-day sensor display  
#define BLOCKS_VALID_15DAYS         (15)

/// Number of blocks for hourly sensor display (24 hours)
#define BLOCKS_VALID_HOURS          (24)

/// Number of blocks for minute sensor display (60 minutes)
#define BLOCKS_VALID_MINUTES        (60)

///////////////////// DATA STRUCTURES ////////////////////

/**
 * @struct ProgressBarUI
 * @brief Structure for managing a visual progress bar
 * 
 * Contains all LVGL objects and state information needed to display
 * and update a block-based progress bar for sensor validity.
 */
typedef struct {
    lv_obj_t *bar;                      ///< Container object for the progress bar
    lv_obj_t *blocks[MAX_BLOCKS];       ///< Array of individual block objects
    lv_obj_t *label;                    ///< Text label for remaining time display
    int total_blocks;                   ///< Total number of blocks in this bar
} ProgressBarUI;

///////////////////// SCREEN OBJECTS ////////////////////

/// @name Screen Objects
/// @{
extern lv_obj_t * ui_Welcome_screen;    ///< Welcome/startup screen
extern lv_obj_t * ui_Main_screen;       ///< Main glucose monitoring screen
extern lv_obj_t * ui_Debug_screen;      ///< Debug information screen
extern lv_obj_t * ui_FWUpdate_screen;   ///< Firmware update progress screen
extern lv_obj_t * ui_Login_screen;      ///< Login credentials input screen
/// @}

///////////////////// WELCOME SCREEN ELEMENTS ////////////////////

/// @name Welcome Screen Elements
/// @{
extern lv_obj_t * ui_Label_WelcomeInfo;     ///< Welcome message label
extern lv_obj_t * ui_Label_WelcomeWifiInfo; ///< WiFi connection status label
/// @}

///////////////////// MAIN SCREEN ELEMENTS ////////////////////

/// @name Glucose Display Elements
/// @{
extern lv_obj_t * ui_Label_GlucoseValue;        ///< Current glucose value (large display)
extern lv_obj_t * ui_Label_GlucoseDelta;        ///< Glucose change delta
extern lv_obj_t * ui_Label_GlucoseTrendArrow;   ///< Trend direction arrow
extern lv_obj_t * ui_Label_GlucoseTrendMessage; ///< Alert/warning message
/// @}

/// @name Status Indicators
/// @{
extern lv_obj_t * ui_Label_ESP32Connectivity;     ///< API activity indicator (reused status label)
extern lv_obj_t * ui_Label_FWUpdateHint;          ///< Firmware update available hint
/// @}

/// @name Chart Elements
/// @{
extern lv_obj_t * ui_Chart_Glucose_5Min;                  ///< Main glucose history chart
extern lv_obj_t * ui_Chart_Valid_Sensor;                  ///< Sensor validity chart
extern lv_obj_t * ui_Label_Chart_GlucoseLimitLow;         ///< Low limit label
extern lv_obj_t * ui_Label_Chart_GlucoseLimitHigh;        ///< High limit label
extern lv_obj_t * ui_Chart_Glucose_5Min_last_point_marker;///< Marker for latest value
extern lv_obj_t * ui_Chart_x_label_start;                 ///< X-axis start time label
extern lv_obj_t * ui_Chart_x_label_middle;                ///< X-axis middle time label
extern lv_obj_t * ui_Chart_x_label_end;                   ///< X-axis end time label
/// @}

/// @name Chart Data Series
/// @{
extern lv_chart_series_t * glucoseValueSeries_upperlimit; ///< Upper limit line
extern lv_chart_series_t * glucoseValueSeries_lowerlimit; ///< Lower limit line
extern lv_chart_series_t * glucoseValueSeries_5Min;       ///< Main glucose data
extern lv_chart_series_t * glucoseValueSeries_alert;      ///< Alert range data
extern lv_chart_series_t * glucoseValueSeries_last;       ///< Latest value marker
extern lv_chart_series_t * sensorValidDaysSeries_yellow;  ///< Sensor validity (warning)
extern lv_chart_series_t * sensorValidDaysSeries_grey;    ///< Sensor validity (expired)
/// @}

///////////////////// DEBUG SCREEN ELEMENTS ////////////////////

/// @name Debug Screen Elements
/// @{
extern lv_obj_t * ui_Label_DebugInfo;           ///< Debug screen title
extern lv_obj_t * ui_Label_DebugDataRefresh;    ///< FSM state + next fetch countdown
extern lv_obj_t * ui_Label_DebugTime;           ///< System time display
extern lv_obj_t * ui_Label_DebugIP;             ///< WiFi IP + RSSI
extern lv_obj_t * ui_Label_DebugWG;             ///< WireGuard IP or off
extern lv_obj_t * ui_Label_DebugSensor;         ///< Sensor ID
extern lv_obj_t * ui_Label_DebugSensorTimestamp;///< MQTT connection status
extern lv_obj_t * ui_Label_DebugSensorState;    ///< Sensor state + validity
extern lv_obj_t * ui_Label_DebugSensorValue;    ///< Current sensor value + delta
extern lv_obj_t * ui_Label_DebugTest;           ///< FW version
extern lv_obj_t * ui_Label_DebugFsmReason;      ///< FSM last transition reason + failure count
extern lv_obj_t * ui_Label_DebugHeap;           ///< Free heap (bottom of screen)
extern lv_obj_t * ui_Label_DebugFwStatus;       ///< FW update status
/// @}

/// @name Debug Screen Buttons
/// @{
extern lv_obj_t * ui_btn_wireguard;         ///< WireGuard toggle button
extern lv_obj_t * ui_btn_label_wireguard;   ///< WireGuard button label
extern lv_obj_t * ui_btn_mqtt;              ///< MQTT toggle button
extern lv_obj_t * ui_btn_label_mqtt;        ///< MQTT button label
extern lv_obj_t * ui_btn_ota_update;        ///< OTA update button
extern lv_obj_t * ui_btn_label_ota_update;  ///< OTA update button label
extern lv_obj_t * ui_btn_fw_check;          ///< FW check/install button
extern lv_obj_t * ui_btn_label_fw_check;    ///< FW check/install button label
/// @}

///////////////////// LOGIN SCREEN ELEMENTS ////////////////////

/// @name Login Screen Elements
/// @{
extern lv_obj_t * ui_Label_LoginInfo;   ///< Login screen title
extern lv_obj_t * ui_kb;                ///< On-screen keyboard
extern lv_obj_t * ui_ta_email;          ///< Email input text area
extern lv_obj_t * ui_ta_password;       ///< Password input text area
extern lv_obj_t * btn_login;            ///< Login button
extern lv_obj_t * btn_label;            ///< Login button label
/// @}

///////////////////// FIRMWARE UPDATE SCREEN ELEMENTS ////////////////////

/// @name Firmware Update Screen Elements
/// @{
extern lv_obj_t * ui_Label_FWUpdateInfo;            ///< Update status message
extern lv_obj_t * ui_Label_FWUpdateProgress_percent;///< Progress percentage display
extern lv_obj_t * ui_Bar_FWUpdateProgress;          ///< Progress bar
/// @}

///////////////////// PROGRESS BAR INSTANCES ////////////////////

/// @name Sensor Validity Progress Bars
/// @{
extern ProgressBarUI dayBar14;      ///< 14-day sensor validity bar
extern ProgressBarUI dayBar15;      ///< 15-day sensor validity bar
extern ProgressBarUI hourBar;       ///< Hourly countdown bar
extern ProgressBarUI minuteBar;     ///< Minute countdown bar
/// @}

///////////////////// FUNCTION PROTOTYPES ////////////////////

/**
 * @defgroup ui_init_functions UI Initialization Functions
 * @brief Functions for initializing UI screens and elements
 * @{
 */

/**
 * @brief Main UI initialization function
 * 
 * Initializes the complete user interface including all screens,
 * themes, and interactive elements. Should be called once during
 * system startup after LVGL initialization.
 * 
 * @note Loads ui_Welcome_screen as default screen
 */
void ui_init(void);

/**
 * @brief Initializes the welcome screen
 * 
 * Creates the initial screen shown at startup with welcome message
 * and WiFi connection status.
 */
void ui_Welcome_screen_init(void);

/**
 * @brief Initializes the main glucose monitoring screen
 * 
 * Creates the main screen with glucose value display, trend indicators,
 * history chart, and sensor validity progress bar.
 */
void ui_Main_screen_init(void);

/**
 * @brief Initializes the debug information screen
 * 
 * Creates debug screen showing system information, timestamps,
 * sensor data, and control buttons.
 */
void ui_Debug_screen_init(void);

/**
 * @brief Initializes the login screen
 * 
 * Creates login screen with email/password inputs and on-screen keyboard.
 */
void ui_Login_screen_init(void);

/**
 * @brief Initializes the firmware update screen
 * 
 * Creates screen for displaying OTA firmware update progress.
 */
void ui_FWUpdate_screen_init(void);

/**
 * @brief Initializes debug screen control buttons
 * 
 * Creates toggle buttons for WireGuard, MQTT, and OTA update
 * on the debug screen.
 * 
 * @param[in] parent Parent screen object (should be ui_Debug_screen)
 */
void ui_btn_debug_screen_init(lv_obj_t * parent);

/** @} */ // end of ui_init_functions

/**
 * @defgroup progress_bar_functions Progress Bar Functions
 * @brief Functions for managing sensor validity progress bars
 * @{
 */

/**
 * @brief Creates a visual progress bar for sensor validity display
 * 
 * Allocates and configures a horizontal block-based progress bar
 * to visualize remaining sensor lifetime (days, hours, or minutes).
 * The function automatically calculates optimal block sizes based
 * on the number of blocks and available screen width.
 * 
 * @param[out] ui          Pointer to ProgressBarUI structure to initialize
 * @param[in]  parent      Parent LVGL object to attach the bar to
 * @param[in]  num_blocks  Number of blocks to display (clamped to MAX_BLOCKS)
 * @param[in]  x           X-axis offset from center alignment (pixels)
 * @param[in]  y           Y-axis offset from center alignment (pixels)
 * @param[in]  label_text  Initial text for the label (usually empty string)
 * 
 * @note Cleans up existing objects in the ProgressBarUI structure before creating new ones
 * @note Block size is dynamically calculated but never smaller than MIN_BLOCK_SIZE
 * @warning num_blocks values exceeding MAX_BLOCKS are automatically clamped
 * @warning NULL parameters will cause the function to return without action
 * 
 * @par Example:
 * @code
 * ProgressBarUI my_bar = {0};
 * create_sensor_valid_progress_bar(&my_bar, ui_Main_screen, 14, 0, 170, "");
 * @endcode
 */
void create_sensor_valid_progress_bar(ProgressBarUI *ui, lv_obj_t *parent, 
                                      int num_blocks, int x, int y, 
                                      const char *label_text);

/**
 * @brief Updates the visual state of a progress bar
 * 
 * Changes the color and number of active blocks based on remaining time.
 * Automatically adjusts the label text and applies color coding:
 * - Green: Normal operation (day mode with >3 days remaining)
 * - Yellow: Warning (day mode with ≤3 days remaining)
 * - Red: Critical (hour/minute mode)
 * - Gray: Inactive/expired blocks
 * 
 * @param[in,out] ui        Pointer to ProgressBarUI structure to update
 * @param[in]     remaining Number of remaining units (days/hours/minutes)
 * 
 * @note Values are automatically clamped to valid range [0, total_blocks]
 * @note Label is automatically hidden when remaining reaches 0
 * @note NULL parameters will cause the function to return without action
 * 
 * @par Example:
 * @code
 * update_sensor_valid_progress_bar(&dayBar14, 5); // Shows 5 days remaining
 * @endcode
 */
void update_sensor_valid_progress_bar(ProgressBarUI *ui, int remaining);

/**
 * @brief Switches between different sensor validity display modes
 * 
 * Hides all progress bars and shows only the specified one.
 * Used to transition between day/hour/minute countdown displays
 * based on remaining sensor lifetime.
 * 
 * @param[in] ui   Pointer to the ProgressBarUI to make visible
 * 
 * @note All bars are hidden first, then only the selected one is shown
 * @warning NULL ui parameter or NULL ui->bar will cause function to return early
 * 
 * @par Example:
 * @code
 * switch_sensor_valid_progress_bar(&hourBar); // Switch to hour display
 * @endcode
 */
void switch_sensor_valid_progress_bar(ProgressBarUI *ui);

/**
 * @brief Creates all sensor validity progress bar variants
 * 
 * Initializes all four progress bar types (14-day, 15-day, hourly, minute).
 * Day bars are always recreated to allow switching between 14/15 day modes,
 * while hour and minute bars are only created if they don't exist yet.
 * 
 * @note Should be called during main screen initialization
 * @note Hour and minute bars are created only once to prevent memory leaks
 * @note Requires ui_Main_screen to be initialized first
 * @warning Will return without action if ui_Main_screen is NULL
 * 
 * @par Example:
 * @code
 * ui_Main_screen_init();
 * create_all_sensor_valid_progress_bars();
 * @endcode
 */
void create_all_sensor_valid_progress_bars(void);

/**
 * @brief Updates the appropriate progress bar based on UI pointer
 * 
 * Determines which progress bar instance to update by comparing
 * the bar pointer in the ProgressBarUI structure and calls
 * the appropriate update function.
 * 
 * @param[in] ui    Pointer to ProgressBarUI structure to identify
 * @param[in] value New remaining value to display
 * 
 * @note Automatically routes to correct bar (day14/day15/hour/minute)
 * @note NULL parameters will cause function to return without action
 * 
 * @par Example:
 * @code
 * update_chart_valid_values(&dayBar14, 7); // Update to 7 days remaining
 * @endcode
 */
void update_chart_valid_values(ProgressBarUI *ui, int value);

/** @} */ // end of progress_bar_functions

///////////////////// INLINE HELPER DOCUMENTATION ////////////////////

/**
 * @defgroup helper_functions Helper Functions
 * @brief Internal helper functions for UI creation
 * @{
 */

/**
 * @brief Creates a styled label with common settings (static/internal)
 * 
 * This is an internal helper function used by screen initialization
 * functions to reduce code duplication when creating labels.
 * 
 * @param[in] parent     Parent LVGL object
 * @param[in] font       Font to use for the label
 * @param[in] color      Text color (hex value, e.g., 0xFFFFFF)
 * @param[in] width      Label width in pixels
 * @param[in] align      Alignment type (e.g., LV_ALIGN_CENTER)
 * @param[in] x_offset   X-axis offset from alignment point
 * @param[in] y_offset   Y-axis offset from alignment point
 * 
 * @return Pointer to created label object, NULL on failure
 * 
 * @note This function is declared static in ui.cpp and not exposed in header
 * @note Documented here for completeness of internal architecture
 */

/**
 * @brief Safely deletes an LVGL object and sets pointer to NULL (static/internal)
 * 
 * Internal helper that performs NULL-safe deletion of LVGL objects
 * and prevents dangling pointers by setting them to NULL.
 * 
 * @param[in,out] obj Pointer to LVGL object pointer
 * 
 * @note This function is declared static in ui.cpp and not exposed in header
 * @note Documented here for completeness of internal architecture
 */

/** @} */ // end of helper_functions

///////////////////// USAGE EXAMPLE ////////////////////

/**
 * @page usage_example Usage Example
 * 
 * @section init_section Initialization
 * @code
 * // Initialize LVGL first
 * lv_init();
 * 
 * // Initialize display driver
 * // ... display driver code ...
 * 
 * // Initialize UI
 * ui_init(); // This loads the welcome screen
 * @endcode
 * 
 * @section screen_switch Screen Switching
 * @code
 * // Switch to main screen
 * lv_disp_load_scr(ui_Main_screen);
 * 
 * // Update glucose value
 * lv_label_set_text_fmt(ui_Label_GlucoseValue, "%d", 120);
 * 
 * // Update trend arrow
 * lv_label_set_text(ui_Label_GlucoseTrendArrow, "→");
 * @endcode
 * 
 * @section progress_bar_section Progress Bar Usage
 * @code
 * // Update sensor validity (7 days remaining)
 * update_sensor_valid_progress_bar(&dayBar14, 7);
 * 
 * // Switch to hour display when less than 1 day
 * switch_sensor_valid_progress_bar(&hourBar);
 * update_sensor_valid_progress_bar(&hourBar, 18);
 * 
 * // Switch to minute display in final hour
 * switch_sensor_valid_progress_bar(&minuteBar);
 * update_sensor_valid_progress_bar(&minuteBar, 45);
 * @endcode
 * 
 * @section chart_section Chart Updates
 * @code
 * // Add new glucose value to chart
 * lv_chart_set_next_value(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, 135);
 * 
 * // Refresh chart display
 * lv_chart_refresh(ui_Chart_Glucose_5Min);
 * @endcode
 */

///////////////////// VERSION HISTORY ////////////////////

/**
 * @page version_history Version History
 * 
 * @section v1_0 Version 1.0 (2025)
 * - Initial release with Doxygen documentation
 * - Added NULL pointer checks throughout
 * - Fixed operator precedence bug in update_sensor_valid_progress_bar
 * - Added memory leak prevention in create_sensor_valid_progress_bar
 * - Improved error handling and parameter validation
 * - Added helper functions to reduce code duplication
 * - Comprehensive documentation for all public functions
 * 
 * @section future_work Future Improvements
 * - Add return codes for error handling instead of silent failures
 * - Implement animation support for smooth transitions
 * - Add configuration structure for customizable colors and sizes
 * - Consider adding event callback registration for buttons
 */

///////////////////// CONFIGURATION NOTES ////////////////////

/**
 * @page config_notes Configuration Notes
 * 
 * @section color_config Color Configuration
 * The UI uses predefined colors that can be modified in ui.cpp:
 * - UI_COLOR_WHITE (0xFFFFFF): Text and borders
 * - UI_COLOR_RED (0xFF0000): Alerts and critical states
 * - UI_COLOR_GREEN (0x00FF00): Normal operation indicators
 * - UI_COLOR_BLACK (0x000000): Backgrounds
 * 
 * @section layout_config Layout Configuration
 * Screen layout uses offset constants defined in ui.cpp:
 * - GLUCOSE_VALUE_Y_OFFSET (-140): Main glucose reading position
 * - GLUCOSE_DELTA_Y_OFFSET (-60): Delta value position
 * - SENSOR_BAR_Y_OFFSET (170): Progress bar vertical position
 * 
 * @section font_config Font Configuration
 * The UI requires custom JetBrains Mono fonts at various sizes:
 * - JetBrainsMonoLight16: Small text and labels
 * - JetBrainsMonoLight20: Debug information
 * - JetBrainsMonoLight24: Secondary info
 * - JetBrainsMonoLight32: Section headers
 * - JetBrainsMonoLight36: Important messages
 * - JetBrainsMonoLight56: Welcome screen
 * - JetBrainsMonoLight72: Trend arrows and large symbols
 * - JetBrainsMonoLight100: Main glucose value
 * 
 * These fonts must be included via ui_fonts.h
 * 
 * @section lvgl_config LVGL Configuration Requirements
 * Required LVGL configuration (in lv_conf.h):
 * - LV_COLOR_DEPTH = 16 (mandated by SquareLine Studio)
 * - LV_USE_CHART = 1 (for glucose history charts)
 * - LV_USE_KEYBOARD = 1 (for login screen)
 * - LV_USE_TEXTAREA = 1 (for text input)
 */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // UI_H
