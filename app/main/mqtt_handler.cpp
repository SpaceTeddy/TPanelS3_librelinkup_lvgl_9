#include "mqtt_handler.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <librelinkup.h>
#include <uuid/log.h>

#include "mqtt.h"
#include "settings.h"
#include "helper.h"
#include "app_fsm.h"
#include "tpanels3.h"
#include "main.h"

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
extern AsyncWebServer    server;
extern TPanelS3          tpanels3;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static JsonDocument json_mqtt;
static bool         g_allow_retained_once  = true;
static bool         g_allow_raw_first      = true;
static int64_t      g_last_raw_meas_epoch  = -1;

void mqtt_publish()
{
    json_mqtt["glucoseMeasurement"] = librelinkup.glucose_data().glucoseMeasurement;
    json_mqtt["trendArrow"]         = librelinkup.glucose_data().trendArrow;
    json_mqtt["brightness"]         = settings.config.brightness;
    json_mqtt["mqtt_mode"]          = settings.config.mqtt_mode;
    json_mqtt["ota_server"]         = settings.config.ota_update;
    json_mqtt["wireguard_mode"]     = settings.config.wg_mode;

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
            if (parameter1 == 1)
            {
                settings.config.ota_update = 1;
                server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
                    request->send(200, "text/plain", "ESP32 LibreLinkup Client");
                });
                ElegantOTA.begin(&server);
                server.begin();
            }
            else
            {
                settings.config.ota_update = 0;
                server.end();
            }
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
