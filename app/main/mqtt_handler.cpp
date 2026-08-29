/**
 * @file mqtt_handler.cpp
 * @brief MQTT client setup, publishing, and command handling.
 *
 * Implements connection management, topic subscription, JSON publishing
 * of device state and glucose data, and processing of incoming MQTT
 * commands (brightness, OTA server mode, WireGuard, MQTT mode,
 * MQTT master mode, reset).
 *
 * In master mode the device publishes the full graph JSON to a shared
 * retained topic; in client mode it subscribes to that topic and ingests
 * the data directly without calling the LibreLinkUp API.
 *
 * @author Chris
 * @license GPL 3.0
 */

#include "mqtt_handler.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <WiFi.h>
#include <librelinkup.h>
#include <uuid/log.h>

#include "mqtt.h"
#include "settings.h"
#include "helper.h"
#include "app_fsm.h"
#include "tpanels3.h"
#include "main.h"
#include "http_update.h"

extern MQTT              mqtt;
extern volatile uint32_t g_lv_mem_total;    ///< see main.cpp, sampled once a second
extern volatile uint32_t g_lv_mem_used_max; ///< LVGL's own high-water mark
extern volatile uint8_t  g_lv_mem_used_pct;
extern volatile uint8_t  g_lv_mem_frag_pct;
extern PubSubClient      mqtt_client;
extern WiFiClient        mqttClient;
extern volatile const char* g_loop_breadcrumb;
extern SETTINGS          settings;
extern HELPER            helper;
extern LIBRELINKUP       librelinkup;
extern bool              ota_in_progress;
extern IPAddress         local_ip;
extern AppFsm            g_fsm;
extern void              ui_blank_screen_for_reset();
extern bool              flag_mqtt_master_rx;
extern TPanelS3          tpanels3;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static JsonDocument json_mqtt;
static bool         g_allow_retained_once  = true;
static bool         g_allow_raw_first      = true;
static int64_t      g_last_raw_meas_epoch  = -1;

// ── HA discovery: entity table types ────────────────────────────────────────

/// Which of the three published topics a discovery entry points at.
enum HaTopic : uint8_t { HA_DATA, HA_NET, HA_HEALTH };

struct HaSensor {
    const char *obj_id;
    const char *name;
    HaTopic     topic;
    const char *value_tmpl;
    const char *unit;
    const char *dev_class;
    const char *state_class;
};

struct HaSwitch {
    const char *obj_id;
    const char *name;
    const char *payload_on;
    const char *payload_off;
    const char *value_tmpl;   // rendered against data_topic
};

// ── HA discovery: entity tables ─────────────────────────────────────────────

static const HaSensor k_ha_sensors[] = {
    { "glucose",             "Glucose",          HA_DATA, "{{ value_json.glucoseMeasurement }}", "mg/dL", "",               "measurement" },
    { "glucose_target_high", "Glucose Target High", HA_DATA, "{{ value_json.glucoseTargetHigh }}", "mg/dL", "", "measurement" },
    { "glucose_target_low",  "Glucose Target Low",  HA_DATA, "{{ value_json.glucoseTargetLow }}",  "mg/dL", "", "measurement" },
    { "trend",               "Trendarrow",       HA_DATA, "{{ value_json.trendStr | default(value_json.trendArrow) }}", "", "", "" },
    { "trend_num",           "Trendarrow Value", HA_DATA, "{{ value_json.trendArrow }}",                                 "", "", "measurement" },
    { "rssi",                "WiFi RSSI",        HA_NET,  "{{ value_json.RSSI }}",               "dBm",   "signal_strength", "measurement" },
    { "lcd_brightness",      "LCD Brightness",   HA_DATA, "{{ value_json.brightness }}",         "",      "",               "measurement" },
    { "ota_state",           "OTA Server State", HA_DATA, "{{ value_json.ota_server }}",         "",      "",               "" },
    { "wg_state",            "WireGuard State",  HA_DATA, "{{ value_json.wireguard_mode }}",     "",      "",               "" },
    { "mqtt_state",          "MQTT State",       HA_DATA, "{{ value_json.mqtt_mode }}",          "",      "",               "" },
    { "mqtt_master_state",   "MQTT Master State",HA_DATA, "{{ value_json.mqtt_master_mode }}",   "",      "",               "" },

    // Memory telemetry, published once per cycle on the health topic. Picked
    // for what an actual investigation needed: `alloc`/`blocks` together show
    // a slow leak (both climb) as opposed to a deep but stable allocation
    // peak, `largest` is what decides whether a TLS session still fits (it
    // needs 16 KB in one piece, not just 16 KB free), and `min` is the hard
    // low-water mark -- it only ever falls, so a step in it marks the moment
    // a new worst case happened.
    { "heap_int_free",       "Heap Internal Free",     HA_HEALTH, "{{ value_json.heap_int_free }}",    "B", "data_size", "measurement" },
    { "heap_int_largest",    "Heap Internal Largest",  HA_HEALTH, "{{ value_json.heap_int_largest }}", "B", "data_size", "measurement" },
    { "heap_int_min",        "Heap Internal Low Water",HA_HEALTH, "{{ value_json.heap_int_min }}",     "B", "data_size", "measurement" },
    { "heap_int_alloc",      "Heap Internal Allocated",HA_HEALTH, "{{ value_json.heap_int_alloc }}",   "B", "data_size", "measurement" },
    { "heap_int_blocks",     "Heap Internal Blocks",   HA_HEALTH, "{{ value_json.heap_int_blocks }}",  "",  "",          "measurement" },
    { "psram_free",          "PSRAM Free",             HA_HEALTH, "{{ value_json.psram_free }}",       "B", "data_size", "measurement" },
    { "lvgl_max_used",       "LVGL Peak Used",         HA_HEALTH, "{{ value_json.lvgl_max_used }}",    "B", "data_size", "measurement" },
    { "lvgl_frag",           "LVGL Fragmentation",     HA_HEALTH, "{{ value_json.lvgl_frag }}",        "%", "",          "measurement" },
    { "loop_stack_free",     "Loop Task Stack Free",   HA_HEALTH, "{{ value_json.loop_stack_free }}",  "B", "data_size", "measurement" },
    { "uptime",              "Uptime",                 HA_HEALTH, "{{ value_json.uptime_s }}",         "s", "duration",  "total_increasing" },
};

static const HaSwitch k_ha_switches[] = {
    { "brightness_sw", "Brightness",
      "{\"cmd\":\"brightness\",\"parameter1\":255}",
      "{\"cmd\":\"brightness\",\"parameter1\":0}",
      "{% if value_json.brightness | int > 0 %}1{% else %}0{% endif %}" },
    { "ota_server", "OTA Server",
      "{\"cmd\":\"ota_server_mode\",\"parameter1\":1}",
      "{\"cmd\":\"ota_server_mode\",\"parameter1\":0}",
      "{{ value_json.ota_server }}" },
    { "wg_mode", "WireGuard VPN",
      "{\"cmd\":\"wg_mode\",\"parameter1\":1}",
      "{\"cmd\":\"wg_mode\",\"parameter1\":0}",
      "{{ value_json.wireguard_mode }}" },
    { "mqtt_mode", "MQTT",
      "{\"cmd\":\"mqtt_mode\",\"parameter1\":1}",
      "{\"cmd\":\"mqtt_mode\",\"parameter1\":0}",
      "{{ value_json.mqtt_mode }}" },
    { "mqtt_master_mode", "MQTT Master Mode",
      "{\"cmd\":\"mqtt_master_mode\",\"parameter1\":1}",
      "{\"cmd\":\"mqtt_master_mode\",\"parameter1\":0}",
      "{{ value_json.mqtt_master_mode }}" },
};

// ── HA discovery: helper functions ──────────────────────────────────────────

static void ha_add_device(JsonDocument &doc, const char *dev_id, const char *dev_name)
{
    JsonObject d           = doc["device"].to<JsonObject>();
    d["identifiers"][0]    = dev_id;
    d["name"]              = dev_name;
    d["manufacturer"]      = "ESP32";
    d["model"]             = "LibreLinkUp Client";
    d["sw_version"]        = fw_update_get_current_version();
    d["configuration_url"] = String("http://") +
        (settings.config.wg_mode == 1 ? local_ip.toString() : WiFi.localIP().toString());
}

static void ha_publish_doc(JsonDocument &doc, const char *ha_type,
                           const char *dev_id, const char *obj_id)
{
    char topic[128];
    snprintf(topic, sizeof(topic),
             "homeassistant/%s/%s_%s/config", ha_type, dev_id, obj_id);
    String payload;
    serializeJson(doc, payload);
    g_loop_breadcrumb = "mqtt.pub.ha_discovery";
    if (!mqtt_client.publish(topic, (const uint8_t *)payload.c_str(), payload.length(), true))
    {
        logger.warning("MQTT publish HA discovery failed (topic=%s, state=%d)", topic, mqtt_client.state());
    }
    g_loop_breadcrumb = "idle";
    logger.notice("HA discovery: %s", topic);
}

// ── HA discovery: main publish function ─────────────────────────────────────

void mqtt_publish_ha_discovery()
{
    if (!settings.config.mqtt_mode)    return;
    if (!settings.config.ha_discovery) return;
    if (!mqtt_client.connected())      return;

    const char *dev_id = mqtt.mqtt_client_name.c_str();

    char data_topic[96], net_topic[96], health_topic[96], cmd_topic[96], dev_name[64];
    snprintf(data_topic, sizeof(data_topic), "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_client_data.c_str());
    snprintf(net_topic,  sizeof(net_topic),  "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_client_network.c_str());
    snprintf(health_topic, sizeof(health_topic), "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_client_health.c_str());
    snprintf(cmd_topic,  sizeof(cmd_topic),  "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_subscibe_toppic.c_str());
    snprintf(dev_name,   sizeof(dev_name),   "LibreLinkUp %s", dev_id);

    // ── Sensors ──────────────────────────────────────────────────────────────
    for (size_t i = 0; i < sizeof(k_ha_sensors) / sizeof(k_ha_sensors[0]); i++) {
        const HaSensor *s = &k_ha_sensors[i];

        JsonDocument doc;
        doc["name"]           = s->name;
        doc["unique_id"]      = String(dev_id) + "_" + s->obj_id;
        doc["state_topic"]    = (s->topic == HA_NET)    ? net_topic
                              : (s->topic == HA_HEALTH) ? health_topic
                                                        : data_topic;
        doc["value_template"] = s->value_tmpl;
        if (s->unit[0])       doc["unit_of_measurement"] = s->unit;
        if (s->dev_class[0])  doc["device_class"]        = s->dev_class;
        if (s->state_class[0])doc["state_class"]         = s->state_class;
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "sensor", dev_id, s->obj_id);
    }

    // ── Switches ─────────────────────────────────────────────────────────────
    for (size_t i = 0; i < sizeof(k_ha_switches) / sizeof(k_ha_switches[0]); i++) {
        const HaSwitch *sw = &k_ha_switches[i];

        JsonDocument doc;
        doc["name"]           = sw->name;
        doc["unique_id"]      = String(dev_id) + "_" + sw->obj_id;
        doc["command_topic"]  = cmd_topic;
        doc["payload_on"]     = sw->payload_on;
        doc["payload_off"]    = sw->payload_off;
        doc["state_topic"]    = data_topic;
        doc["value_template"] = sw->value_tmpl;
        doc["state_on"]       = "1";
        doc["state_off"]      = "0";
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "switch", dev_id, sw->obj_id);
    }

    // ── Number (brightness slider) ────────────────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]             = "Brightness slider";
        doc["unique_id"]        = String(dev_id) + "_brightness";
        doc["command_topic"]    = cmd_topic;
        doc["command_template"] = "{\"cmd\":\"brightness\",\"parameter1\":{{ value | int }}}";
        doc["min"]              = 0;
        doc["max"]              = 255;
        doc["step"]             = 1;
        doc["state_topic"]      = data_topic;
        doc["value_template"]   = "{{ value_json.brightness }}";
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "number", dev_id, "brightness");
    }

    // ── Button (reset) ────────────────────────────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]          = "Reset";
        doc["unique_id"]     = String(dev_id) + "_reset";
        doc["command_topic"] = cmd_topic;
        doc["payload_press"] = "{\"cmd\":\"reset\",\"parameter1\":0}";
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "button", dev_id, "reset");
    }

    // ── Update (firmware) ─────────────────────────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]                    = "Firmware";
        doc["unique_id"]               = String(dev_id) + "_firmware";
        doc["state_topic"]             = data_topic;
        doc["value_template"]          = "{{ value_json.fw_installed }}";
        doc["latest_version_topic"]    = data_topic;
        doc["latest_version_template"] = "{{ value_json.fw_latest }}";
        doc["command_topic"]           = cmd_topic;
        doc["payload_install"]         = "{\"cmd\":\"fw_install\",\"parameter1\":0}";
        doc["device_class"]            = "firmware";
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "update", dev_id, "firmware");
    }

    // ── Button (check for firmware update) ───────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]          = "Check for Update";
        doc["unique_id"]     = String(dev_id) + "_fw_check";
        doc["command_topic"] = cmd_topic;
        doc["payload_press"] = "{\"cmd\":\"fw_check\",\"parameter1\":0}";
        ha_add_device(doc, dev_id, dev_name);
        ha_publish_doc(doc, "button", dev_id, "fw_check");
    }
}

static const char* trend_to_str(int t)
{
    switch (t) {
        case 1: return "\xe2\x86\x93";        // ↓
        case 2: return "\xe2\x86\x98";        // ↘
        case 3: return "\xe2\x86\x92";        // →
        case 4: return "\xe2\x86\x97";        // ↗
        case 5: return "\xe2\x86\x91";        // ↑
        default: return "?";
    }
}

void mqtt_publish()
{
    json_mqtt["glucoseMeasurement"] = librelinkup.glucose_data().glucoseMeasurement;
    json_mqtt["glucoseTargetHigh"]  = librelinkup.glucose_data().glucosetargetHigh;
    json_mqtt["glucoseTargetLow"]   = librelinkup.glucose_data().glucosetargetLow;
    json_mqtt["trendArrow"]         = librelinkup.glucose_data().trendArrow;
    json_mqtt["trendStr"]           = trend_to_str(librelinkup.glucose_data().trendArrow);
    json_mqtt["brightness"]         = settings.config.brightness;
    json_mqtt["mqtt_mode"]          = settings.config.mqtt_mode;
    json_mqtt["mqtt_master_mode"]   = settings.config.mqtt_master_mode;
    json_mqtt["ota_server"]         = settings.config.ota_update;
    json_mqtt["wireguard_mode"]     = settings.config.wg_mode;
    json_mqtt["fw_installed"]       = fw_update_get_current_version();
    json_mqtt["fw_latest"]          = fw_update_get_latest_version();

    serializeJson(json_mqtt, mqtt.mqtt_buffer);
    json_mqtt.clear();
    g_loop_breadcrumb = "mqtt.pub.status";
    logger.debug("MQTT publish status: %u bytes", (unsigned)strlen(mqtt.mqtt_buffer));
    if (!mqtt_client.publish(
        (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_data).c_str(),
        mqtt.mqtt_buffer, false))
    {
        logger.warning("MQTT publish status failed (state=%d)", mqtt_client.state());
    }

    if (settings.config.mqtt_master_mode)
    {
        const String &payload = librelinkup.get_last_graph_json();
        const String  topic   = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;
        g_loop_breadcrumb = "mqtt.pub.master_graph";
        logger.debug("MQTT publish master graph: %u bytes", (unsigned)payload.length());
        // Stream straight out of payload's own buffer via beginPublish()/
        // write()/endPublish() instead of publish(), which first copies the
        // whole payload byte-by-byte into PubSubClient's internal buffer
        // (see PubSubClient::publish()) before sending it. For this ~15 KB
        // PSRAM-backed payload that byte-copy was a second full PSRAM pass
        // stacked right on top of building the string, timed exactly at
        // fetch completion -- confirmed as the tearing trigger by testing
        // with mqtt_master_mode disabled (no tearing) vs enabled (tearing).
        // Streaming skips that copy entirely.
        bool ok = mqtt_client.beginPublish(topic.c_str(), payload.length(), true);
        if (ok)
        {
            size_t written = mqtt_client.write((const uint8_t *)payload.c_str(), payload.length());
            ok = (written == payload.length());
            mqtt_client.endPublish();
        }
        if (!ok)
        {
            logger.warning("MQTT publish master graph failed (state=%d)", mqtt_client.state());
        }
    }

    json_mqtt["IP"]   = WiFi.localIP().toString();
    json_mqtt["IP_WG"] = (settings.config.wg_mode == 1) ? local_ip.toString() : String("not connected");
    json_mqtt["SSID"] = WiFi.SSID();
    json_mqtt["RSSI"] = WiFi.RSSI();

    serializeJson(json_mqtt, mqtt.mqtt_buffer);
    json_mqtt.clear();
    g_loop_breadcrumb = "mqtt.pub.network";
    logger.debug("MQTT publish network: %u bytes", (unsigned)strlen(mqtt.mqtt_buffer));
    if (!mqtt_client.publish(
        (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_network).c_str(),
        mqtt.mqtt_buffer, false))
    {
        logger.warning("MQTT publish network failed (state=%d)", mqtt_client.state());
    }

    // ── Health: memory figures for long-term trending ────────────────────────
    // The same numbers `esp_status` prints, but recorded per cycle so a slow
    // drift becomes visible -- a single reading cannot distinguish a deep but
    // stable allocation peak from something that creeps up over days.
    //
    // MALLOC_CAP_INTERNAL alone would also count the 32-bit-only IRAM heap,
    // which is permanently full and makes the aggregate meaningless; the
    // byte-addressable internal memory is what WiFi, mbedTLS and the drivers
    // draw from. `largest` matters more than `free`: a TLS session needs
    // 16 KB in *one piece*, so a fragmented heap can show plenty free and
    // still fail the handshake.
    multi_heap_info_t internal_info;
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    json_mqtt["uptime_s"]         = (uint32_t)(millis() / 1000UL);
    json_mqtt["heap_int_free"]    = (uint32_t)internal_info.total_free_bytes;
    json_mqtt["heap_int_largest"] = (uint32_t)heap_caps_get_largest_free_block(
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Lifetime low-water mark: only ever falls, so a step marks the moment a
    // new worst case occurred. Not a gauge -- read it as an event marker.
    json_mqtt["heap_int_min"]     = (uint32_t)heap_caps_get_minimum_free_size(
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    json_mqtt["heap_int_alloc"]   = (uint32_t)internal_info.total_allocated_bytes;
    json_mqtt["heap_int_blocks"]  = (uint32_t)internal_info.allocated_blocks;
    json_mqtt["psram_free"]       = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // LVGL draws from its own fixed pool, not the ESP heap -- the two say
    // nothing about each other. Values come from the loop task's last sample.
    json_mqtt["lvgl_max_used"]    = (uint32_t)g_lv_mem_used_max;
    json_mqtt["lvgl_total"]       = (uint32_t)g_lv_mem_total;
    json_mqtt["lvgl_frag"]        = (uint8_t)g_lv_mem_frag_pct;
    // mqtt_publish() runs on the loop task, so this is that task's headroom.
    // A watermark shrinking over days is a slow stack overflow in the making.
    json_mqtt["loop_stack_free"]  = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    // Serialised into a String, not mqtt.mqtt_buffer: that buffer is 255 bytes
    // and serializeJson() would truncate silently, publishing invalid JSON.
    String health_payload;
    serializeJson(json_mqtt, health_payload);
    json_mqtt.clear();
    g_loop_breadcrumb = "mqtt.pub.health";
    logger.debug("MQTT publish health: %u bytes", (unsigned)health_payload.length());
    if (!mqtt_client.publish(
        (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_health).c_str(),
        health_payload.c_str(), false))
    {
        logger.warning("MQTT publish health failed (state=%d)", mqtt_client.state());
    }

    g_loop_breadcrumb = "idle";
}

void update_mqtt_publish()
{
    if (settings.config.mqtt_mode == 1)
        mqtt_publish();
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
    if (ota_in_progress) return;

    String t(topic);
    const String topic_raw = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;
    const String topic_cmd = mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic;

    logger.notice("MQTT RX topic=%s len=%u", t.c_str(), (unsigned)length);

    // ---- RAW data from MASTER (client mode) ----
    if (t == topic_raw && !settings.config.mqtt_master_mode)
    {
        logger.notice("MQTT raw data received");

        if (!librelinkup.ingest_graph_json(payload, length))
        {
            logger.notice("MQTT raw ingest failed");
            return;
        }

        const String &ts         = librelinkup.glucose_data().str_measurement_timestamp;
        const int64_t meas_epoch = (int64_t)helper.convertStrToUnixTime(ts);

        if (meas_epoch <= 0)
        {
            logger.notice("MQTT raw ignored: invalid meas timestamp '%s'", ts.c_str());
            return;
        }

        if (g_allow_raw_first)
        {
            g_allow_raw_first    = false;
            g_last_raw_meas_epoch = meas_epoch;
            logger.notice("MQTT raw accepted (first/retained) meas_epoch=%lld", (long long)meas_epoch);
            flag_mqtt_master_rx = true;
            app_fsm_notify_mqtt_master_rx(g_fsm);
            return;
        }

        g_last_raw_meas_epoch = meas_epoch;
        logger.notice("MQTT raw accepted (new) meas_epoch=%lld", (long long)meas_epoch);
        flag_mqtt_master_rx = true;
        app_fsm_notify_mqtt_master_rx(g_fsm);
        return;
    }

    // ---- Commands ----
    if (t == topic_cmd)
    {
        mqtt.mqtt_incomming_cmd = "";
        for (unsigned int i = 0; i < length; i++)
            mqtt.mqtt_incomming_cmd += (char)payload[i];

        DeserializationError error = deserializeJson(json_mqtt, mqtt.mqtt_incomming_cmd);
        if (error)
        {
            logger.notice("CMD deserialize failed: %s", error.f_str());
            mqtt.mqtt_incomming_cmd = "";
            return;
        }

        const char *cmd        = json_mqtt["cmd"];
        float       parameter1 = json_mqtt["parameter1"];
        float       parameter2 = json_mqtt["parameter2"];
        logger.notice("CMD=%s p1=%.2f p2=%.2f", cmd, parameter1, parameter2);

        bool cmd_ok     = false;
        bool needs_save = false;

        if (strcmp(cmd, "reset") == 0)
        {
            cmd_ok = true;
            ui_blank_screen_for_reset();
            ESP.restart();
        }
        else if (strcmp(cmd, "brightness") == 0)
        {
            settings.config.brightness = tpanels3.set_backlight_brightness(parameter1);
            app_fsm_notify_user_activity(g_fsm);
            cmd_ok     = true;
            needs_save = true;
        }
        else if (strcmp(cmd, "ota_server_mode") == 0)
        {
            settings.config.ota_update = (parameter1 == 1);
            cmd_ok     = true;
            needs_save = true;
        }
        else if (strcmp(cmd, "wg_mode") == 0)
        {
            settings.config.wg_mode = (parameter1 == 1);
            setup_wg(settings.config.wg_mode);
            cmd_ok     = true;
            needs_save = true;
        }
        else if (strcmp(cmd, "mqtt_mode") == 0)
        {
            settings.config.mqtt_mode = (parameter1 == 1);
            cmd_ok     = true;
            needs_save = true;
        }
        else if (strcmp(cmd, "mqtt_master_mode") == 0)
        {
            settings.config.mqtt_master_mode = (parameter1 == 1);
            mqtt_publish_ha_discovery();
            cmd_ok     = true;
            needs_save = true;
        }
        else if (strcmp(cmd, "fw_check") == 0)
        {
            fw_update_request_check_now();
            cmd_ok = true;
        }
        else if (strcmp(cmd, "fw_install") == 0)
        {
            String msg;
            cmd_ok = fw_update_request_install(msg);
        }

        if (needs_save)
            settings.saveConfiguration(settings.config_filename, settings.config);

        json_mqtt.clear();
        json_mqtt["cmd"]        = cmd;
        json_mqtt["parameter1"] = parameter1;
        json_mqtt["parameter2"] = parameter2;
        json_mqtt["cmd_ok"]     = cmd_ok;
        serializeJson(json_mqtt, mqtt.mqtt_buffer);
        json_mqtt.clear();
        g_loop_breadcrumb = "mqtt.pub.cmd_ack";
        if (!mqtt_client.publish(
            (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_rec_toppic).c_str(),
            mqtt.mqtt_buffer))
        {
            logger.warning("MQTT publish cmd_ack failed (state=%d)", mqtt_client.state());
        }
        g_loop_breadcrumb = "idle";

        mqtt.mqtt_incomming_cmd = "";
        mqtt_publish();
        return;
    }

    logger.debug("MQTT topic ignored");
}

bool setup_mqtt()
{
    mqtt_client.setServer(mqtt.mqtt_server.c_str(), mqtt.mqtt_port);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_client.setBufferSize(16384);
    mqtt_client.setSocketTimeout(3);
    mqtt_client.setKeepAlive(30);
    // PubSubClient::setSocketTimeout() only bounds its own internal read-polling
    // loop; publish()/write() call the underlying WiFiClient directly with no
    // timeout of its own. Set it here so SO_SNDTIMEO/SO_RCVTIMEO are actually
    // applied to the socket (belt-and-braces: ESP32's lwIP has had bugs where
    // SO_SNDTIMEO isn't honored on TCP sends, so this is a mitigation, not a
    // guarantee -- the task watchdog in main.cpp is the real safety net).
    mqttClient.setTimeout(3);

    mqtt.mqtt_client_name = helper.get_flashmemory_id();
    const String clientId = mqtt.mqtt_client_name;

    if (mqtt_client.connected()) return true;

    logger.notice("MQTT: connecting... clientId=%s target=%s:%u",
                  clientId.c_str(), mqtt.mqtt_server.c_str(), (unsigned)mqtt.mqtt_port);

    const bool ok = mqtt_client.connect(clientId.c_str(), mqtt.mqtt_user.c_str(), mqtt.mqtt_password.c_str());
    logger.debug("MQTT connect ok=%d state=%d", (int)ok, mqtt_client.state());

    if (!ok || !mqtt_client.connected())
    {
        logger.debug("MQTT: connect failed, state=%d", mqtt_client.state());
        return false;
    }

    logger.notice("MQTT: connected");
    g_allow_retained_once = true;
    g_allow_raw_first     = true;
    mqtt_publish_ha_discovery();

    const String subCmd = mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic;
    const String subRaw = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id   + mqtt.mqtt_client_data;

    mqtt_client.unsubscribe(subCmd.c_str());
    mqtt_client.unsubscribe(subRaw.c_str());

    const bool s1 = mqtt_client.subscribe(subCmd.c_str());
    bool       s2 = true;

    if (!settings.config.mqtt_master_mode)
    {
        s2 = mqtt_client.subscribe(subRaw.c_str());
        logger.notice("MQTT subscribe raw: %s ok=%d", subRaw.c_str(), (int)s2);
    }

    logger.notice("MQTT subscribe cmd: %s ok=%d", subCmd.c_str(), (int)s1);
    return (s1 && s2);
}
