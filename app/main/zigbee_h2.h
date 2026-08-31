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

/// Registry of paired Zigbee devices, kept in sync from the H2's "list",
/// "join", "motion" and "sensor" messages.
#define H2_MAX_DEVICES 32

struct H2Device {
    uint16_t addr;
    char     ieee[26];
    char     mfr[24];
    char     model[24];
    uint8_t  ep;
    bool     online;
    int8_t   occ;            ///< -1 unknown, else 0/1
    int8_t   on;             ///< -1 not switchable / unknown, else 0/1
    int16_t  level;          ///< -1 not dimmable / unknown, else 0-100 percent
    float    temp;           ///< NAN when unknown
    int16_t  bat;            ///< -1 unknown, else percent
    uint32_t last_seen_ms;
    /// How the coordinator resolved this device against the ZHA database:
    /// "matched" (record applied), "known" (in the database but it carries
    /// nothing actionable), "unknown", "waiting", "idle". Empty until the H2
    /// has reported it.
    char     zha[8];
    bool     used;
};

/// Serialises the registry as a JSON array. Takes the registry lock.
void h2_devices_json(String &out);

/// Number of occupied registry slots.
size_t h2_devices_count();

/// Drops one device from the local registry. Does not talk to the H2 --
/// callers that want it gone from the Zigbee network send "remove"/"forget"
/// as well. Returns false if the address was not in the table.
bool h2_dev_forget(uint16_t addr);

/// Serialises the last coordinator status snapshot. Takes the registry lock.
void h2_status_json(String &out);

/// Serialises the networks found by the last scan. Takes the registry lock.
void h2_scan_json(String &out);

/// Queues a command for the loop task to send. Safe to call from other tasks
/// -- unlike h2_send(), which writes the UART directly and must only be used
/// from the task that owns it.
bool h2_enqueue(const char *cmd);
