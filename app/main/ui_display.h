/**
 * @file ui_display.h
 * @brief LVGL display rendering — chart, labels, and debug screen.
 *
 * Declares functions for drawing the glucose chart, updating value and
 * trend labels, rendering the sensor validity progress bar, formatting
 * axis labels, driving the LCD status indicator, and refreshing the
 * debug information screen.
 *
 * @author Chris
 * @license GPL 3.0
 */

#pragma once

#include <Arduino.h>
#include "lvgl.h"

void    draw_chart_sensor_valid();
void    format_time_label(char *buffer, size_t buffer_size, time_t timestamp);
void    add_axis_labels();
void    draw_chart_glucose_data(uint8_t mode);
void    request_chart_redraw(uint8_t mode);
void    process_chart_redraw();
void    request_lcd_status_indication(bool visible);
void    process_lcd_status_indication();
/// Attach @p cb as an LV_EVENT_DRAW_MAIN_BEGIN tracer to the widgets this file
/// owns privately (warmup arc/labels, sensor-validity bars), so a renderer hang
/// can name them. See setup_lvgl_draw_trace() in main.cpp.
void    ui_display_register_draw_trace(lv_event_cb_t cb);
void    draw_labels(uint8_t mode, uint8_t _glucose_measurement_color,
                    uint16_t _glucose_value, String _trendarrow,
                    String _trendmessage, int16_t delta);
void    update_debug_screen();
void    ui_update_fw_hint();
void    ui_warmup_screen();
