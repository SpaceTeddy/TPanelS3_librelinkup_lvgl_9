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

int16_t get_last_valid_x_position(lv_obj_t *chart, lv_chart_series_t *series);
void    lcd_status_indication(bool on_off, uint8_t color);
void    draw_chart_sensor_valid();
void    format_time_label(char *buffer, size_t buffer_size, time_t timestamp);
void    add_axis_labels();
void    draw_chart_glucose_data(uint8_t mode);
void    draw_labels(uint8_t mode, uint8_t _glucose_measurement_color,
                    uint16_t _glucose_value, String _trendarrow,
                    String _trendmessage, int16_t delta);
void    update_debug_screen();
void    ui_update_fw_hint();
void    ui_warmup_screen();
