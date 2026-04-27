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
extern PubSubClient      mqtt_client;
extern SETTINGS          settings;
extern HELPER            helper;
extern LIBRELINKUP       librelinkup;
extern bool              ota_in_progress;
extern IPAddress         local_ip;
extern AppFsm            g_fsm;
extern bool              flag_mqtt_master_rx;
extern uint64_t          config_sleep_timer_backup;
extern TPanelS3          tpanels3;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static JsonDocument json_mqtt;
static bool         g_allow_retained_once  = true;
static bool         g_allow_raw_first      = true;
static int64_t      g_last_raw_meas_epoch  = -1;

// ── HA discovery: entity table types ────────────────────────────────────────

struct HaSensor {
    const char *obj_id;
    const char *name;
    bool        net_topic;    // true → network topic, false → data topic
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
    { "glucose",          "Glucose",          false, "{{ value_json.glucoseMeasurement }}", "mg/dL", "",               "measurement" },
    { "trend",            "Trendarrow",       false, "{{ value_json.trendStr | default(value_json.trendArrow) }}", "", "", "" },
    { "trend_num",        "Trendarrow Value", false, "{{ value_json.trendArrow }}",                                 "", "", "measurement" },
    { "rssi",             "WiFi RSSI",        true,  "{{ value_json.RSSI }}",               "dBm",   "signal_strength", "measurement" },
    { "lcd_brightness",   "LCD Brightness",   false, "{{ value_json.brightness }}",         "",      "",               "measurement" },
    { "ota_state",        "OTA Server State", false, "{{ value_json.ota_server }}",         "",      "",               "" },
    { "wg_state",         "WireGuard State",  false, "{{ value_json.wireguard_mode }}",     "",      "",               "" },
    { "mqtt_state",       "MQTT State",       false, "{{ value_json.mqtt_mode }}",          "",      "",               "" },
    { "mqtt_master_state","MQTT Master State",false, "{{ value_json.mqtt_master_mode }}",   "",      "",               "" },
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
    d["configuration_url"] = String("http://") + WiFi.localIP().toString();
}

static void ha_publish_doc(JsonDocument &doc, const char *ha_type,
                           const char *dev_id, const char *obj_id)
{
    char topic[128];
    snprintf(topic, sizeof(topic),
             "homeassistant/%s/%s_%s/config", ha_type, dev_id, obj_id);
    String payload;
    serializeJson(doc, payload);
    mqtt_client.publish(topic, (const uint8_t *)payload.c_str(), payload.length(), true);
    logger.notice("HA discovery: %s", topic);
}

// ── HA discovery: main publish function ─────────────────────────────────────

void mqtt_publish_ha_discovery()
{
    if (!settings.config.mqtt_mode)    return;
    if (!settings.config.ha_discovery) return;
    if (!mqtt_client.connected())      return;

    const char *dev_id = mqtt.mqtt_client_name.c_str();

    char data_topic[96], net_topic[96], cmd_topic[96], dev_name[64];
    snprintf(data_topic, sizeof(data_topic), "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_client_data.c_str());
    snprintf(net_topic,  sizeof(net_topic),  "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_client_network.c_str());
    snprintf(cmd_topic,  sizeof(cmd_topic),  "%s/%s%s",
             mqtt.mqtt_base.c_str(), dev_id, mqtt.mqtt_subscibe_toppic.c_str());
    snprintf(dev_name,   sizeof(dev_name),   "LibreLinkUp %s", dev_id);

    // ── Sensors ──────────────────────────────────────────────────────────────
    for (size_t i = 0; i < sizeof(k_ha_sensors) / sizeof(k_ha_sensors[0]); i++) {
        const HaSensor *s = &k_ha_sensors[i];

        JsonDocument doc;
        doc["name"]           = s->name;
        doc["unique_id"]      = String(dev_id) + "_" + s->obj_id;
        doc["state_topic"]    = s->net_topic ? net_topic : data_topic;
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
    mqtt_client.publish(
        (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_data).c_str(),
        mqtt.mqtt_buffer, false);

    if (settings.config.mqtt_master_mode)
    {
        const String &payload = librelinkup.get_last_graph_json();
        const String  topic   = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;
        mqtt_client.publish(topic.c_str(), (const uint8_t *)payload.c_str(), payload.length(), true);
    }

    json_mqtt["IP"]   = WiFi.localIP().toString();
    json_mqtt["IP_WG"] = (settings.config.wg_mode == 1) ? local_ip.toString() : String("not connected");
    json_mqtt["SSID"] = WiFi.SSID();
    json_mqtt["RSSI"] = WiFi.RSSI();

    serializeJson(json_mqtt, mqtt.mqtt_buffer);
    json_mqtt.clear();
    mqtt_client.publish(
        (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_network).c_str(),
        mqtt.mqtt_buffer, false);
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

        bool cmd_ok = false;

        if (strcmp(cmd, "reset") == 0)
        {
            cmd_ok = true;
            ESP.restart();
        }
        else if (strcmp(cmd, "brightness") == 0)
        {
            settings.config.brightness = tpanels3.set_backlight_brightness(parameter1);
            config_sleep_timer_backup  = millis();
            app_fsm_notify_user_activity(g_fsm);
            cmd_ok = true;
        }
        else if (strcmp(cmd, "ota_server_mode") == 0)
        {
            settings.config.ota_update = (parameter1 == 1);
            cmd_ok = true;
        }
        else if (strcmp(cmd, "wg_mode") == 0)
        {
            settings.config.wg_mode = (parameter1 == 1);
            setup_wg(settings.config.wg_mode);
            cmd_ok = true;
        }
        else if (strcmp(cmd, "mqtt_mode") == 0)
        {
            settings.config.mqtt_mode = (parameter1 == 1);
            cmd_ok = true;
        }
        else if (strcmp(cmd, "mqtt_master_mode") == 0)
        {
            settings.config.mqtt_master_mode = (parameter1 == 1);
            settings.saveConfiguration(settings.config_filename, settings.config);
            mqtt_publish_ha_discovery();
            cmd_ok = true;
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

        json_mqtt.clear();
        json_mqtt["cmd"]        = cmd;
        json_mqtt["parameter1"] = parameter1;
        json_mqtt["parameter2"] = parameter2;
        json_mqtt["cmd_ok"]     = cmd_ok;
        serializeJson(json_mqtt, mqtt.mqtt_buffer);
        json_mqtt.clear();
        mqtt_client.publish(
            (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_rec_toppic).c_str(),
            mqtt.mqtt_buffer);

        mqtt.mqtt_incomming_cmd = "";
        mqtt_publish();
        return;
    }

    logger.debug("MQTT topic ignored");
}

bool setup_mqtt()
{
    mqtt_client.setServer(mqtt.mqtt_server, mqtt.mqtt_port);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_client.setBufferSize(16384);
    mqtt_client.setSocketTimeout(3);
    mqtt_client.setKeepAlive(30);

    mqtt.mqtt_client_name = helper.get_flashmemory_id();
    const String clientId = mqtt.mqtt_client_name;

    if (mqtt_client.connected()) return true;

    logger.notice("MQTT: connecting... clientId=%s target=%s:%u",
                  clientId.c_str(), mqtt.mqtt_server, (unsigned)mqtt.mqtt_port);

    const bool ok = mqtt_client.connect(clientId.c_str(), mqtt.mqtt_user, mqtt.mqtt_password);
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
