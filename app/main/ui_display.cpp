#include "ui_display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <librelinkup.h>
#include <uuid/log.h>

#include "lvgl.h"
#include "ui.h"
#include "app_fsm.h"
#include "helper.h"
#include "main.h"

extern LIBRELINKUP librelinkup;
extern HELPER      helper;
extern AppFsm      g_fsm;

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
 * @note Calls lv_timer_handler() to update display immediately
 * @note 5ms delay ensures display update completes
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
    lv_timer_handler();
    delay(5);
}

///////////////////// LIBRELINKUP CHART FUNCTIONS ////////////////////

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
    int rawDays = librelinkup.sensor_lifetime().sensor_valid_days;
    int rawHours = librelinkup.sensor_lifetime().sensor_valid_hours;
    int rawMinutes = librelinkup.sensor_lifetime().sensor_valid_minutes;
    const uint8_t sensorState = librelinkup.status().sensor_state;

    bool expired = (sensorState == SENSOR_EXPIRED) ||
                   (rawDays < 0) || (rawHours < 0) || (rawMinutes < 0);

    if (expired)
    {
        switch_sensor_valid_progress_bar(&dayBar15);
        update_chart_valid_values(&dayBar15, -1);
        return;
    }

    int days = rawDays + 1;
    int hours = rawHours + 1;
    int minutes = rawMinutes + 1;

    if (rawDays > 0)
    {
        if (librelinkup.sensor_data().sensor_runtime == 14 * 86400)
        {
            switch_sensor_valid_progress_bar(&dayBar14);
            update_chart_valid_values(&dayBar14, days);
        }
        else if (librelinkup.sensor_data().sensor_runtime == 15 * 86400)
        {
            switch_sensor_valid_progress_bar(&dayBar15);
            update_chart_valid_values(&dayBar15, days);
        }
        else
        {
            switch_sensor_valid_progress_bar(&dayBar15);
            update_chart_valid_values(&dayBar15, days);
        }
        return;
    }

    if (rawHours > 0)
    {
        switch_sensor_valid_progress_bar(&hourBar);
        update_chart_valid_values(&hourBar, hours);
        return;
    }

    if (rawMinutes >= 0)
    {
        switch_sensor_valid_progress_bar(&minuteBar);
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
void update_debug_screen()
{
    if (lv_scr_act() == ui_Debug_screen)
    {
        const uint32_t period_ms  = g_fsm.cfg.fetch_period_ms ? g_fsm.cfg.fetch_period_ms : 60000U;
        const uint32_t elapsed_ms = (uint32_t)(millis() - (uint32_t)g_fsm.last_fetch_ms);
        uint32_t remaining_s      = (elapsed_ms < period_ms) ? ((period_ms - elapsed_ms) / 1000U) : 0;

        char buf[96];

        snprintf(buf, sizeof(buf), "Data Refresh in: %lu sec.", (unsigned long)remaining_s);
        lv_label_set_text(ui_Label_DebugDataRefresh, buf);

        snprintf(buf, sizeof(buf), "ESP32 Time: %s", helper.get_esp_time_date().c_str());
        lv_label_set_text(ui_Label_DebugTime, buf);

        IPAddress ip = WiFi.localIP();
        snprintf(buf, sizeof(buf), "IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        lv_label_set_text(ui_Label_DebugIP, buf);

        snprintf(buf, sizeof(buf), "Sensor: %s", librelinkup.sensor_data().sensor_id.c_str());
        lv_label_set_text(ui_Label_DebugSensor, buf);

        snprintf(buf, sizeof(buf), "Valid: %dDays %dHours %dMinutes",
                 librelinkup.sensor_lifetime().sensor_valid_days,
                 librelinkup.sensor_lifetime().sensor_valid_hours,
                 librelinkup.sensor_lifetime().sensor_valid_minutes);
        lv_label_set_text(ui_Label_DebugSensorTimestamp, buf);

        const int st = librelinkup.sensor_data().sensor_state;
        const char *st_txt =
            (st == 0) ? "unknown"          :
            (st == 1) ? "not started yet"  :
            (st == 2) ? "starting phase"   :
            (st == 3) ? "ready"            :
            (st == 4) ? "expired"          :
            (st == 5) ? "shut down"        :
            (st == 6) ? "has failure"      : "other";

        snprintf(buf, sizeof(buf), "Sensor State: %d => %s", st, st_txt);
        lv_label_set_text(ui_Label_DebugSensorState, buf);

        char delta_buf[16];
        if (glucose_delta == 0)
            snprintf(delta_buf, sizeof(delta_buf), "±%d", glucose_delta);
        else if (glucose_delta > 0)
            snprintf(delta_buf, sizeof(delta_buf), "+%d", glucose_delta);
        else
            snprintf(delta_buf, sizeof(delta_buf), "%d", glucose_delta);

        snprintf(buf, sizeof(buf), "Sensor Value: %d%s %s mg/dL",
                 librelinkup.glucose_data().glucoseMeasurement,
                 librelinkup.glucose_data().str_trendArrow.c_str(),
                 delta_buf);
        lv_label_set_text(ui_Label_DebugSensorValue, buf);
    }
}
