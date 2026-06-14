#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

// Shared UART instance used by main loop, commands, and H2 OTA.
extern HardwareSerial SerialPort;

// Last known H2 snapshot for UI/debug pages.
extern String g_h2_fw_version;
extern String g_h2_fw_build;
extern String g_h2_chip_model;
extern String g_h2_chip_rev;
extern String g_h2_chip_mac;
extern String g_h2_chip_cores;
extern String g_h2_chip_cpu_mhz;
extern String g_h2_chip_xtal_mhz;
extern String g_h2_chip_features;
extern String g_h2_last_type;
extern String g_h2_last_json;
extern uint32_t g_h2_last_seen_ms;

void h2_reset_chip();
void setup_UART_IPC();
void h2_send(const char *cmd);
void zigbee_h2_poll_uart();
