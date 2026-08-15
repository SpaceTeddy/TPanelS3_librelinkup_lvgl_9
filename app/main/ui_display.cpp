/**
 * @file ui_display.cpp
 * @brief LVGL display rendering — chart, labels, and debug screen.
 *
 * Implements all visual output functions:
 * - Glucose chart drawing with mode selection (limits / data / both)
 * - Last-point highlight marker with range-based colour coding
 * - Sensor validity progress bar (days / hours / minutes)
 * - X-axis time label formatting and placement
 * - Glucose value, trend arrow, and delta label updates
 * - LCD API activity indicator (coloured asterisk)
 * - Debug information screen refresh
 *
 * @author Chris
 * @license GPL 3.0
 */

#include "ui_display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <librelinkup.h>
#include <uuid/log.h>

#include "lvgl.h"
#include "ui.h"
#include "app_fsm.h"
#include "helper.h"
#include "main.h"
#include "http_update.h"
#include "settings.h"

extern LIBRELINKUP  librelinkup;
extern HELPER       helper;
extern AppFsm       g_fsm;
extern SETTINGS     settings;
extern PubSubClient mqtt_client;
extern IPAddress    local_ip;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

///////////////////// CHART HELPER FUNCTIONS ////////////////////

/**
 * @brief Finds the last valid X position in a chart series
 *
 * Iterates backwards through the chart data to find the last
 * non-NONE value position.
 *
 * @param[in] chart  LVGL chart object
 * @param[in] series Chart data series to search
 *
 * @return X position (index) of last valid point
 * @retval -1 No valid point found in series
 *
 * @note Currently unused but kept for potential future use
 */
int16_t get_last_valid_x_position(lv_obj_t *chart, lv_chart_series_t *series)
{
    uint16_t point_count = lv_chart_get_point_count(chart);
    lv_coord_t *y_array = lv_chart_get_y_array(chart, series);

    for (int i = point_count - 1; i >= 0; i--)
    {
        if (y_array[i] != LV_CHART_POINT_NONE)
        {
            return lv_chart_get_x_start_point(chart, series) + i;
        }
    }

    return -1;
}

/**
 * @brief Highlights the most recent glucose value on the chart
 *
 * Displays a circular marker at the last valid data point.
 * The marker color changes based on glucose range:
 * - Red: Out of target range (high or low alarm)
 * - Green: Within target range
 *
 * Position is calculated by scaling the data index to chart dimensions.
 *
 * @note Automatically hides marker if no valid value exists
 * @note Uses fixed Y-axis range of 40-225 mg/dL
 */
static void highlight_last_point()
{
    const uint8_t x_pos_offset = 24; ///< X-axis offset for marker centering
    const uint8_t y_pos_offset = 12; ///< Y-axis offset for marker centering

    // Get last stored value from array
    int16_t last_value = librelinkup.sensor_history_data().graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1];

    // Hide marker if no valid value exists
    if (last_value == LV_CHART_POINT_NONE ||
        librelinkup.sensor_history_data().graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1] == 0)
    {
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    else
    {
        lv_obj_clear_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    }

    // Calculate X position (scale point index to chart width)
    lv_coord_t chart_width = lv_obj_get_width(ui_Chart_Glucose_5Min);
    uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1;

    lv_coord_t x_pos = ((chart_width * last_index) /
                        (librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE)) -
                       x_pos_offset;

    // Calculate Y position (scale Y value to chart height)
    lv_coord_t y_min = 40;
    lv_coord_t y_max = 225;
    lv_coord_t chart_height = lv_obj_get_height(ui_Chart_Glucose_5Min);
    lv_coord_t y_pos = (chart_height - ((last_value - y_min) * chart_height) / (y_max - y_min)) - y_pos_offset;

    // Set marker color based on glucose range
    if (last_value >= librelinkup.glucose_data().glucosetargetHigh ||
        last_value <= librelinkup.glucose_data().glucoseAlarmLow)
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_RED), 0);
    }
    else
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    // Set marker position
    lv_obj_set_pos(ui_Chart_Glucose_5Min_last_point_marker, x_pos, y_pos);

    // Move marker to foreground layer
    lv_obj_move_foreground(ui_Chart_Glucose_5Min_last_point_marker);
}

///////////////////// STATUS INDICATION ////////////////////

/**
 * @brief Shows/hides LCD API activity status indicator
 *
 * Displays a colored asterisk (*) in the corner to indicate LibreLinkUp
 * API activity and status.
 *
 * @param[in] on_off Enable/disable indicator
 *                   - 0: Hide indicator (blank)
 *                   - 1: Show indicator with color
 * @param[in] color  Color code for indicator
 *                   - 0: Yellow (0xFFFF00) - Warning/Processing
 *                   - 1: White (0xFFFFFF) - Normal
 *                   - 2: Red (0xFF0000) - Error
 *                   - Other: Default to white
 *
 * @note The main loop calls lv_timer_handler() and owns LVGL rendering.
 *       Do not render synchronously here; this function may be called while
 *       the fetch path is already updating the UI.
 */
void lcd_status_indication(bool on_off, uint8_t color)
{
    if (on_off == 0)
    {
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, " ");
    }
    else if (on_off == 1)
    {
        switch (color)
        {
        case 0:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case 1:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case 2:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;

        default:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        }
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, "*");
    }
}

///////////////////// LIBRELINKUP CHART FUNCTIONS ////////////////////

// Forward decl - defined further down with the rest of the warmup-arc widget
// code. Called from draw_chart_sensor_valid() to hide the arc in lockstep
// with showing the bar, instead of waiting for the next 1s warmup tick.
static void warmup_arc_set_active(bool active);

/**
 * @brief Draws sensor validity progress bar
 *
 * Displays remaining sensor lifetime using appropriate progress bar:
 * - Days mode (14 or 15): Full sensor lifetime visualization
 * - Hours mode (24): Last day countdown
 * - Minutes mode (60): Final hour countdown
 *
 * Automatically switches between modes based on remaining time.
 *
 * @note Progress bar values are incremented by 1 for display purposes
 * @note Calls lv_timer_handler() to update UI immediately
 *
 * @see switch_sensor_valid_progress_bar()
 * @see update_chart_valid_values()
 */
void draw_chart_sensor_valid()
{
    const uint8_t sensorState = librelinkup.status().sensor_state;

    // The sensor-warmup arc (ui_display.cpp: warmup_arc_set_active()) owns bar
    // visibility exclusively while SENSOR_STARTING - it hides all bars and
    // redraws every 1s tick. Showing the "expired" bar here too raced with
    // that tick and caused a ~1s flicker of the day bar under the spinner
    // on every fetch cycle.
    if (sensorState == SENSOR_STARTING)
    {
        return;
    }

    // Warmup just ended (or wasn't active) - make sure the arc/overlay is
    // hidden in this same call, so it can't still be showing for up to 1s
    // after the bar below has already switched to the real value.
    warmup_arc_set_active(false);

    int rawDays = librelinkup.sensor_lifetime().sensor_valid_days;
    int rawHours = librelinkup.sensor_lifetime().sensor_valid_hours;
    int rawMinutes = librelinkup.sensor_lifetime().sensor_valid_minutes;

    bool expired = (sensorState == SENSOR_EXPIRED) ||
                   (sensorState == SENSOR_NOT_AVAILABLE) ||
                   (librelinkup.status().timestamp_status == SENSOR_LOST);

    // Note: update_chart_valid_values() invokes switch_sensor_valid_progress_bar()
    // internally for non-expired bars, so we don't call it here too. The
    // expired branch keeps its explicit switch because update_chart_valid_values()
    // does not switch in the expired path (it greys out all bars instead).
    if (expired)
    {
        bool is14day = (librelinkup.sensor_data().sensor_runtime == 14 * 86400);
        ProgressBarUI *expiredBar = is14day ? &dayBar14 : &dayBar15;
        switch_sensor_valid_progress_bar(expiredBar);
        update_chart_valid_values(expiredBar, -1);
        return;
    }

    int days = rawDays + 1;
    int hours = rawHours + 1;
    int minutes = rawMinutes + 1;

    if (rawDays > 0)
    {
        if (librelinkup.sensor_data().sensor_runtime == 14 * 86400)
        {
            update_chart_valid_values(&dayBar14, days);
        }
        else
        {
            // 15-day runtime, or unknown runtime (fall through to 15-day bar)
            update_chart_valid_values(&dayBar15, days);
        }
        return;
    }

    if (rawHours > 0)
    {
        update_chart_valid_values(&hourBar, hours);
        return;
    }

    if (rawMinutes >= 0)
    {
        update_chart_valid_values(&minuteBar, minutes);
        return;
    }
}

/**
 * @brief Formats timestamp for chart axis labels
 *
 * Creates special markers for significant times:
 * - "00:00*" for midnight (day change marker)
 * - "12:00" for noon
 * - "HH:00" for 3-hour intervals
 * - "" (empty) for all other times
 *
 * @param[out] buffer      Output buffer for formatted string
 * @param[in]  buffer_size Size of output buffer (must be ≥6 for "HH:MM\0")
 * @param[in]  timestamp   Unix timestamp to format
 *
 * @note Uses localtime() which respects timezone settings
 * @note Midnight marker (*) helps identify day boundaries
 */
void format_time_label(char *buffer, size_t buffer_size, time_t timestamp)
{
    struct tm *tm_info = localtime(&timestamp);
    if (tm_info->tm_hour == 0 && tm_info->tm_min == 0)
    {
        snprintf(buffer, buffer_size, "00:00*");
    }
    else if (tm_info->tm_hour == 12 && tm_info->tm_min == 0)
    {
        snprintf(buffer, buffer_size, "12:00");
    }
    else if (tm_info->tm_min == 0 && (tm_info->tm_hour % 3 == 0))
    {
        snprintf(buffer, buffer_size, "%02d:00", tm_info->tm_hour);
    }
    else
    {
        snprintf(buffer, buffer_size, "");
    }
}

/**
 * @brief Adds X-axis time labels to glucose chart
 *
 * Displays three time labels on the chart:
 * - Start time (left)
 * - Middle time (center)
 * - End time (right)
 *
 * Timestamps are retrieved from sensor history data and formatted
 * using helper.format_time().
 *
 * @note Labels are static char arrays to maintain persistence
 * @note Middle timestamp is calculated from array midpoint
 */
void add_axis_labels()
{
    static char labels[3][6];
    uint8_t data_count = librelinkup.check_graphdata();
    if (data_count == 0)
    {
        lv_label_set_text(ui_Chart_x_label_start, "--:--");
        lv_label_set_text(ui_Chart_x_label_middle, "--:--");
        lv_label_set_text(ui_Chart_x_label_end, "--:--");
        return;
    }

    uint32_t first_timestamp  = librelinkup.sensor_history_data().timestamp[0];
    uint32_t middle_timestamp = librelinkup.sensor_history_data().timestamp[data_count / 2];
    uint32_t last_timestamp   = librelinkup.sensor_history_data().timestamp[data_count - 1];

    helper.format_time(labels[0], sizeof(labels[0]), first_timestamp);
    helper.format_time(labels[1], sizeof(labels[1]), middle_timestamp);
    helper.format_time(labels[2], sizeof(labels[2]), last_timestamp);

    lv_label_set_text(ui_Chart_x_label_start,  labels[0]);
    lv_label_set_text(ui_Chart_x_label_middle, labels[1]);
    lv_label_set_text(ui_Chart_x_label_end,    labels[2]);
}

/**
 * @brief Draws glucose chart with historical data
 *
 * Renders glucose chart based on selected mode:
 * - Mode 0: Target limit lines only
 * - Mode 1: Historical glucose data only
 * - Mode 3: Both limits and data (complete chart)
 *
 * @param[in] mode Drawing mode (0/1/3)
 *
 * @see highlight_last_point()
 * @see add_axis_labels()
 */
void draw_chart_glucose_data(uint8_t mode)
{
    if (mode == 0 || mode == 3)
    {
        lv_chart_set_x_start_point(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, 0);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_upperlimit,
                               librelinkup.glucose_data().glucosetargetHigh);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_lowerlimit,
                               librelinkup.glucose_data().glucosetargetLow);
    }

    if (mode == 1 || mode == 3)
    {
        uint16_t glucose_value = 0;
        uint8_t data_count = librelinkup.check_graphdata();

        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_last, LV_CHART_POINT_NONE);

        uint32_t sensor_active_time = (librelinkup.sensor_lifetime().sensor_valid_days * 24 * 60 * 60) +
                                      (librelinkup.sensor_lifetime().sensor_valid_hours * 60 * 60) +
                                      (librelinkup.sensor_lifetime().sensor_valid_minutes * 60);

        if (sensor_active_time <= librelinkup.TIMEFULLGRAPHDATA)
        {
            for (int i = 0; i < librelinkup.GRAPHDATAARRAYSIZE; i++)
            {
                uint8_t index = (librelinkup.GRAPHDATAARRAYSIZE - 1) - i;

                if (index < 0 || index >= librelinkup.GRAPHDATAARRAYSIZE)
                    continue;

                glucose_value = librelinkup.sensor_history_data().graph_data[index];

                if (glucose_value != 0)
                {
                    if (glucose_value > librelinkup.glucose_data().glucosetargetHigh ||
                        glucose_value < librelinkup.glucose_data().glucosetargetLow)
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                                 index, glucose_value);
                    }
                    else
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                                 index, glucose_value);
                    }
                }
                else
                {
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                             index, LV_CHART_POINT_NONE);
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                             index, LV_CHART_POINT_NONE);
                }
            }
        }
        else
        {
            for (int i = 0; i < data_count; i++)
            {
                uint8_t index = (data_count - 1) - i;
                glucose_value = librelinkup.sensor_history_data().graph_data[index];

                if (glucose_value != 0)
                {
                    if (glucose_value > librelinkup.glucose_data().glucosetargetHigh ||
                        glucose_value < librelinkup.glucose_data().glucosetargetLow)
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                                 (librelinkup.GRAPHDATAARRAYSIZE - 1) - i, glucose_value);
                    }
                    else
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                                 (librelinkup.GRAPHDATAARRAYSIZE - 1) - i, glucose_value);
                    }
                }
            }
        }

        uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE;

        if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_VALID)
        {
            librelinkup.sensor_history_data().graph_data[last_index] =
                librelinkup.glucose_data().glucoseMeasurement;

            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                     last_index, librelinkup.glucose_data().glucoseMeasurement);

            highlight_last_point();
        }
        else
        {
            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                     last_index, LV_CHART_POINT_NONE);
        }

        add_axis_labels();
        lv_obj_invalidate(ui_Chart_Glucose_5Min);
    }
}

/**
 * @brief Updates glucose value and trend labels on main screen
 *
 * @param[in] mode                      Display mode (0=dashes, 1=actual values)
 * @param[in] _glucose_measurement_color Color code (COLOR_WHITE/YELLOW/ORANGE/RED/BLUE)
 * @param[in] _glucose_value            Glucose value in mg/dL
 * @param[in] _trendarrow               Trend arrow string
 * @param[in] _trendmessage             Alert/warning message
 * @param[in] delta                     Change from previous reading
 */
void draw_labels(uint8_t mode, uint8_t _glucose_measurement_color,
                 uint16_t _glucose_value, String _trendarrow,
                 String _trendmessage, int16_t delta)
{
    if (mode == 0)
    {
        if (_glucose_measurement_color == 1)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 2)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 3)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 4)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 5)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        else
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_label_set_text(ui_Label_GlucoseValue, "---");
        lv_label_set_text(ui_Label_GlucoseDelta, "--- mg/dL");
        lv_label_set_text(ui_Label_GlucoseTrendArrow, "-");
    }
    else if (mode == 1)
    {
        if (_glucose_measurement_color == 1)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 2)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 3)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 4)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        else if (_glucose_measurement_color == 5)
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        else
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

        char buf_label1[4];
        snprintf(buf_label1, 4, "%d", _glucose_value);
        lv_label_set_text(ui_Label_GlucoseValue, buf_label1);
        lv_label_set_text(ui_Label_GlucoseTrendArrow, _trendarrow.c_str());

        char buf_label3[14];
        if (delta == 0)
            snprintf(buf_label3, 14, "±%d mg/dL", delta);
        else if (delta > 0)
            snprintf(buf_label3, 14, "+%d mg/dL", delta);
        else
            snprintf(buf_label3, 14, "%d mg/dL", delta);

        lv_label_set_text(ui_Label_GlucoseDelta, buf_label3);
    }

    if (strcmp(_trendmessage.c_str(), "null") != 0)
        lv_label_set_text(ui_Label_GlucoseTrendMessage, _trendmessage.c_str());
    else
        lv_label_set_text(ui_Label_GlucoseTrendMessage, "");

    ui_update_fw_hint();
}

///////////////////// FIRMWARE UPDATE HINT ////////////////////

void ui_update_fw_hint()
{
    if (ui_Label_FWUpdateHint == NULL) return;

    if (strcmp(fw_update_get_status(), "update_available") == 0) {
        const char* ver = fw_update_get_latest_version();
        if (ver != NULL && strlen(ver) > 0) {
            lv_label_set_text_fmt(ui_Label_FWUpdateHint, "FW update v%s available", ver);
        } else {
            lv_label_set_text(ui_Label_FWUpdateHint, "FW update available");
        }
        lv_obj_clear_flag(ui_Label_FWUpdateHint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_Label_FWUpdateHint, LV_OBJ_FLAG_HIDDEN);
    }
}

///////////////////// DEBUG SCREEN ////////////////////

/**
 * @brief Updates debug screen with current system and sensor information
 *
 * Displays real-time data for debugging purposes:
 * - Data refresh countdown
 * - ESP32 system time
 * - Local IP address
 * - Sensor ID and status
 * - Current glucose value and trend
 *
 * @note Only updates if debug screen is active
 */
static const char *appstate_name(AppState s)
{
    switch (s) {
        case AppState::BOOT:           return "BOOT";
        case AppState::WIFI_CONNECT:   return "WIFI_CONNECT";
        case AppState::VPN_CHECK:      return "VPN_CHECK";
        case AppState::MQTT_CONNECT:   return "MQTT_CONNECT";
        case AppState::RUN_IDLE:       return "RUN_IDLE";
        case AppState::RUN_FETCH:      return "RUN_FETCH";
        case AppState::RUN_PUBLISH:    return "RUN_PUBLISH";
        case AppState::DISPLAY_DIM:    return "DISPLAY_DIM";
        case AppState::INTERNET_CHECK: return "NET_CHECK";
        case AppState::BACKOFF:        return "BACKOFF";
        case AppState::OTA_MODE:       return "OTA_MODE";
        case AppState::FW_CHECKING:    return "FW_CHECK";
        case AppState::FW_INSTALLING:  return "FW_INSTALL";
        default:                       return "?";
    }
}

void update_debug_screen()
{
    if (lv_scr_act() != ui_Debug_screen) return;

    char buf[128];

    // ── SYSTEM ──────────────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "WiFi: %s  RSSI: %ddBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    lv_label_set_text(ui_Label_DebugIP, buf);

    if (settings.config.wg_mode)
        snprintf(buf, sizeof(buf), "WG: %s", local_ip.toString().c_str());
    else
        snprintf(buf, sizeof(buf), "WG: off");
    lv_label_set_text(ui_Label_DebugWG, buf);

    snprintf(buf, sizeof(buf), "Time: %s", helper.get_esp_time_date().c_str());
    lv_label_set_text(ui_Label_DebugTime, buf);

    snprintf(buf, sizeof(buf), "FW: %s", fw_update_get_current_version());
    lv_label_set_text(ui_Label_DebugTest, buf);

    // ── FSM ─────────────────────────────────────────────────────────────────
    const uint32_t period_ms   = g_fsm.cfg.fetch_period_ms ? g_fsm.cfg.fetch_period_ms : 60000U;
    const uint32_t elapsed_ms  = (uint32_t)(millis() - (uint32_t)g_fsm.last_fetch_ms);
    const uint32_t remaining_s = (elapsed_ms < period_ms) ? ((period_ms - elapsed_ms) / 1000U) : 0U;
    snprintf(buf, sizeof(buf), "State: %s  Fetch: %lus",
             appstate_name(g_fsm.state), (unsigned long)remaining_s);
    lv_label_set_text(ui_Label_DebugDataRefresh, buf);

    const char *reason = g_fsm.last_transition_reason ? g_fsm.last_transition_reason : "-";
    snprintf(buf, sizeof(buf), "Reason: %s  Fails: %u", reason, (unsigned)g_fsm.consecutive_failures);
    lv_label_set_text(ui_Label_DebugFsmReason, buf);

    // ── SENSOR ──────────────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "Sensor SN: %s", librelinkup.sensor_data().sensor_sn.c_str());
    lv_label_set_text(ui_Label_DebugSensor, buf);

    const int st     = librelinkup.status().sensor_state;      // calculated state (reliable)
    const int pt_raw = librelinkup.sensor_data().sensor_state; // raw API pt field
    const auto &lt = librelinkup.sensor_lifetime();
    const char *st_txt =
        (st == 1) ? "not started" :
        (st == 2) ? "starting"    :
        (st == 3) ? "ready"       :
        (st == 4) ? "expired"     :
        (st == 5) ? "shutdown"    :
        (st == 6) ? "failure"     : "unknown";
    snprintf(buf, sizeof(buf), "State: %s  Valid: %dD %dH %dM",
             st_txt, lt.sensor_valid_days, lt.sensor_valid_hours, lt.sensor_valid_minutes);
    lv_label_set_text(ui_Label_DebugSensorState, buf);

    char delta_buf[12];
    if (glucose_delta == 0)
        snprintf(delta_buf, sizeof(delta_buf), "+-0");
    else if (glucose_delta > 0)
        snprintf(delta_buf, sizeof(delta_buf), "+%d", glucose_delta);
    else
        snprintf(delta_buf, sizeof(delta_buf), "%d", glucose_delta);
    snprintf(buf, sizeof(buf), "Value: %d %s %s mg/dL",
             librelinkup.glucose_data().glucoseMeasurement,
             librelinkup.glucose_data().str_trendArrow.c_str(),
             delta_buf);
    lv_label_set_text(ui_Label_DebugSensorValue, buf);

    // ── MQTT ────────────────────────────────────────────────────────────────
    const char *conn = mqtt_client.connected() ? "connected" : "disconnected";
    const char *mode = !settings.config.mqtt_mode      ? "off"    :
                        settings.config.mqtt_master_mode ? "master" : "client";
    snprintf(buf, sizeof(buf), "MQTT: %s  Mode: %s", conn, mode);
    lv_label_set_text(ui_Label_DebugSensorTimestamp, buf);

    // ── FW UPDATE STATUS ────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "Update: %s", fw_update_get_status());
    lv_label_set_text(ui_Label_DebugFwStatus, buf);
    lv_label_set_text(ui_btn_label_fw_check,
                      fw_update_is_update_available() ? "FW Install" : "FW Chk");

    // ── HEAP (bottom) ────────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "Heap: %luKB free", (unsigned long)(ESP.getFreeHeap() / 1024));
    lv_label_set_text(ui_Label_DebugHeap, buf);
}

///////////////////// SENSOR WARMUP ARC ////////////////////

/*
 * Sensor Warmup Arc — native LVGL widget, no image asset needed.
 *
 * Progress is derived from the REAL elapsed time (sensor_non_activ_unixtime
 * -> now), not a free-running lv_anim loop, so the arc stays correct across
 * reboots/reconnects during the 60-minute warmup window. lv_anim is only
 * used for the soft visual interpolation between two 1s ticks.
 */

static lv_obj_t *ui_Arc_SensorWarmup = nullptr;
static lv_obj_t *ui_Label_SensorWarmupTitle = nullptr;
static lv_obj_t *ui_Label_SensorWarmupTime = nullptr;

// Must match LIBRELINKUP::get_remaining_warmup_time() (librelinkup.cpp).
static const uint32_t WARMUP_ARC_DURATION_SEC = 60 * 60;

static void warmup_arc_anim_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}

static void warmup_arc_set_active(bool active)
{
    if (active)
    {
        lv_obj_clear_flag(ui_Arc_SensorWarmup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Label_SensorWarmupTitle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Label_SensorWarmupTime, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(ui_Label_GlucoseValue, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Label_GlucoseDelta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Label_GlucoseTrendArrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Label_GlucoseTrendMessage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Chart_Glucose_5Min, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Chart_x_label_start, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Chart_x_label_middle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Chart_x_label_end, LV_OBJ_FLAG_HIDDEN);

        // Sensor-validity block bar (day/hour/minute) is a separate widget
        // group below the chart; NULL hides all of them at once.
        switch_sensor_valid_progress_bar(NULL);
    }
    else
    {
        lv_obj_add_flag(ui_Arc_SensorWarmup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Label_SensorWarmupTitle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Label_SensorWarmupTime, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_Label_GlucoseValue, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Label_GlucoseDelta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Label_GlucoseTrendArrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Label_GlucoseTrendMessage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Chart_Glucose_5Min, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Chart_x_label_start, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Chart_x_label_middle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_Chart_x_label_end, LV_OBJ_FLAG_HIDDEN);
        // Block bar visibility is left to the next draw_chart_sensor_valid()
        // call (runs every fetch cycle, same cycle that changes sensor_state
        // away from SENSOR_STARTING) — it already picks the correct bar.
    }
}

/**
 * @brief 1s tick for the sensor-warmup arc. Runs continuously; no-ops when
 *        the sensor isn't in the SENSOR_STARTING warmup phase.
 */
static void warmup_arc_tick_cb(lv_timer_t *)
{
    const uint8_t sensor_state = librelinkup.status().sensor_state;
    const uint32_t activation = librelinkup.sensor_data().sensor_non_activ_unixtime;

    if (sensor_state != SENSOR_STARTING || activation == 0)
    {
        warmup_arc_set_active(false);
        return;
    }

    const time_t now = time(nullptr);
    if (now < 1700000000)
    {
        // NTP not synced yet — keep last known state rather than showing
        // a bogus elapsed time computed against an unset clock.
        return;
    }

    warmup_arc_set_active(true);

    uint32_t elapsed = (now > (time_t)activation) ? (uint32_t)(now - activation) : 0;
    if (elapsed > WARMUP_ARC_DURATION_SEC) elapsed = WARMUP_ARC_DURATION_SEC;

    int32_t target = (elapsed * 100) / WARMUP_ARC_DURATION_SEC;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_Arc_SensorWarmup);
    lv_anim_set_exec_cb(&a, warmup_arc_anim_cb);
    lv_anim_set_values(&a, lv_arc_get_value(ui_Arc_SensorWarmup), target);
    lv_anim_set_time(&a, 900); // just under the 1s tick -> no visible jump
    lv_anim_start(&a);

    uint32_t remaining = WARMUP_ARC_DURATION_SEC - elapsed;
    lv_label_set_text_fmt(ui_Label_SensorWarmupTime, "%02u:%02u",
                          (unsigned)(remaining / 60), (unsigned)(remaining % 60));
}

/**
 * @brief Creates the sensor-warmup arc widget (hidden by default).
 *
 * Call once during UI setup, after ui_init(). Overlays the centre of
 * ui_Main_screen — the same area occupied by the glucose value/chart, which
 * are hidden while the arc is shown (see warmup_arc_set_active()).
 */
void ui_warmup_screen()
{
    ui_Arc_SensorWarmup = lv_arc_create(ui_Main_screen);
    lv_obj_set_size(ui_Arc_SensorWarmup, 320, 320);
    lv_obj_center(ui_Arc_SensorWarmup);
    lv_arc_set_rotation(ui_Arc_SensorWarmup, 270);
    lv_arc_set_bg_angles(ui_Arc_SensorWarmup, 0, 360);
    lv_arc_set_range(ui_Arc_SensorWarmup, 0, 100);
    lv_arc_set_value(ui_Arc_SensorWarmup, 0);
    lv_obj_remove_style(ui_Arc_SensorWarmup, NULL, LV_PART_KNOB); // pure progress ring, no handle
    lv_obj_clear_flag(ui_Arc_SensorWarmup, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(ui_Arc_SensorWarmup, lv_color_hex(0x2a2d3a), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui_Arc_SensorWarmup, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui_Arc_SensorWarmup, lv_color_hex(0xFFA500), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui_Arc_SensorWarmup, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui_Arc_SensorWarmup, true, LV_PART_INDICATOR);

    ui_Label_SensorWarmupTitle = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_font(ui_Label_SensorWarmupTitle, &JetBrainsMonoLight32, 0);
    lv_obj_set_style_text_color(ui_Label_SensorWarmupTitle, lv_color_hex(0xFFA500), 0);
    lv_obj_align(ui_Label_SensorWarmupTitle, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(ui_Label_SensorWarmupTitle, "Sensor Warmup");

    ui_Label_SensorWarmupTime = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_font(ui_Label_SensorWarmupTime, &JetBrainsMonoLight56, 0);
    lv_obj_set_style_text_color(ui_Label_SensorWarmupTime, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_align(ui_Label_SensorWarmupTime, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_text(ui_Label_SensorWarmupTime, "60:00");

    lv_obj_add_flag(ui_Arc_SensorWarmup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label_SensorWarmupTitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label_SensorWarmupTime, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(warmup_arc_tick_cb, 1000, NULL);
}
