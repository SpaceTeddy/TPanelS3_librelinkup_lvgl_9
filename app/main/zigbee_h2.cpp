#include "zigbee_h2.h"

#include <ArduinoJson.h>
#include <uuid/log.h>

#include "app_fsm.h"
#include "h2_ota.h"
#include "main.h"
#include "pin_config.h"

HardwareSerial SerialPort(2);

String g_h2_fw_version = "";
String g_h2_fw_build   = "";
String g_h2_chip_model = "";
String g_h2_chip_rev   = "";
String g_h2_chip_mac   = "";
String g_h2_chip_cores = "";
String g_h2_chip_cpu_mhz = "";
String g_h2_chip_xtal_mhz = "";
String g_h2_chip_features = "";
String g_h2_last_type  = "";
String g_h2_last_json  = "";
uint32_t g_h2_last_seen_ms = 0;

extern AppFsm g_fsm;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

void h2_reset_chip()
{
    pinMode(ESP32H2_EN, OUTPUT);
    digitalWrite(ESP32H2_EN, LOW);
    delay(50);
    digitalWrite(ESP32H2_EN, HIGH);
    Serial.println(F("[H2] EN toggled - chip reset"));
}

void setup_UART_IPC()
{
    h2_reset_chip();
    delay(500);

    SerialPort.begin(460800, SERIAL_8N1, ESP32H2_RX, ESP32H2_TX);
    Serial.println();
    Serial.printf("[%09lu ms][%s][%s] ", (unsigned long)millis(), __FILE__, __func__);
    Serial.println(F("Init SerialPort for IPC"));

    h2_send("{\"cmd\":\"version\"}");
    h2_send("{\"cmd\":\"chipinfo\"}");
}

void h2_send(const char *cmd)
{
    SerialPort.print(cmd);
    SerialPort.print('\n');
    logger.notice("[H2] TX: %s", cmd);
}

static void h2_handle_message(const String &line)
{
    logger.notice("[H2] raw: %s", line.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok)
    {
        logger.warning("[H2] JSON parse error: %s", line.c_str());
        return;
    }

    const char *type = doc["type"] | "";
    g_h2_last_seen_ms = millis();
    g_h2_last_type = String(type);
    g_h2_last_json = line;

    const char *v1 = doc["version"] | "";
    const char *v2 = doc["fw"] | "";
    const char *v3 = doc["app_version"] | "";
    const char *b1 = doc["build"] | "";
    const char *b2 = doc["git"] | "";
    const char *m1 = doc["model"] | "";
    const char *m2 = doc["chip"] | "";
    const char *m3 = doc["chip_model"] | "";
    const char *r1 = doc["revision"] | "";
    const char *r2 = doc["rev"] | "";
    const char *r3 = doc["chip_revision"] | "";
    const int ri1 = doc["revision"] | -1;
    const int ri2 = doc["rev"] | -1;
    const char *a1 = doc["mac"] | "";
    const char *a2 = doc["eui64"] | "";
    const char *a3 = doc["chip_mac"] | "";
    const char *f1 = doc["features"] | "";
    const int c1 = doc["cores"] | -1;
    const int hz1 = doc["cpuMHz"] | -1;
    const int xt1 = doc["xtalMHz"] | -1;

    if (strlen(v1) > 0 || strlen(v2) > 0 || strlen(v3) > 0)
        g_h2_fw_version = (strlen(v1) > 0) ? String(v1) : ((strlen(v2) > 0) ? String(v2) : String(v3));
    if (strlen(b1) > 0 || strlen(b2) > 0)
        g_h2_fw_build = (strlen(b1) > 0) ? String(b1) : String(b2);
    if (strlen(m1) > 0 || strlen(m2) > 0 || strlen(m3) > 0)
        g_h2_chip_model = (strlen(m1) > 0) ? String(m1) : ((strlen(m2) > 0) ? String(m2) : String(m3));
    if (strlen(r1) > 0 || strlen(r2) > 0 || strlen(r3) > 0)
        g_h2_chip_rev = (strlen(r1) > 0) ? String(r1) : ((strlen(r2) > 0) ? String(r2) : String(r3));
    else if (ri1 >= 0)
        g_h2_chip_rev = String(ri1);
    else if (ri2 >= 0)
        g_h2_chip_rev = String(ri2);
    if (strlen(a1) > 0 || strlen(a2) > 0 || strlen(a3) > 0)
        g_h2_chip_mac = (strlen(a1) > 0) ? String(a1) : ((strlen(a2) > 0) ? String(a2) : String(a3));
    if (strlen(f1) > 0)
        g_h2_chip_features = String(f1);
    if (c1 >= 0)
        g_h2_chip_cores = String(c1);
    if (hz1 >= 0)
        g_h2_chip_cpu_mhz = String(hz1);
    if (xt1 >= 0)
        g_h2_chip_xtal_mhz = String(xt1);

    if (strcmp(type, "list") == 0 || strcmp(type, "join") == 0)
    {
        JsonArray devices = doc["devices"].as<JsonArray>();
        bool streaming = (line.indexOf("\"idx\"") >= 0);
        int  idx       = streaming ? doc["idx"].as<int>() : 0;
        int  total     = streaming ? doc["total"].as<int>() : (int)devices.size();
        if (!streaming) {
            logger.notice("[H2] devices (%s): %d entries", type, (int)devices.size());
        }
        for (JsonObject d : devices) {
            if (streaming) {
                logger.notice("[H2] device (%s) %d/%d: 0x%04x %s  mfr=%s model=%s ep=%u online=%d occ=%d temp=%.1f bat=%d",
                    type, idx, total,
                    d["addr"].as<uint16_t>(), d["ieee"] | "?", d["mfr"]  | "?",
                    d["model"] | "?", d["ep"].as<uint8_t>(), (int)(d["online"] | false),
                    d["occ"].isNull() ? -1 : (int)d["occ"].as<bool>(),
                    d["temp"].isNull() ? 0.0f : (float)d["temp"].as<float>(),
                    d["bat"].isNull() ? -1 : (int)d["bat"].as<int>());
            } else {
                logger.notice("[H2]  0x%04x %s  mfr=%s model=%s ep=%u online=%d occ=%d temp=%.1f bat=%d",
                    d["addr"].as<uint16_t>(), d["ieee"] | "?", d["mfr"]  | "?",
                    d["model"] | "?", d["ep"].as<uint8_t>(), (int)(d["online"] | false),
                    d["occ"].isNull() ? -1 : (int)d["occ"].as<bool>(),
                    d["temp"].isNull() ? 0.0f : (float)d["temp"].as<float>(),
                    d["bat"].isNull() ? -1 : (int)d["bat"].as<int>());
            }
        }
    }
    else if (strcmp(type, "ack") == 0)
    {
        logger.notice("[H2] ack [%s] ok=%d %s", doc["cmd"] | "?", (bool)(doc["ok"] | false), doc["msg"] | "");
    }
    else if (strcmp(type, "status") == 0)
    {
        logger.notice("[H2] status  ch=%d  pan=0x%04x  epid=%s  coord=%s  role=%s  joined=%d  factory_new=%d  tx=%ddBm  nwk_upd=%d  devices=%d  heap=%u  uptime=%us",
            doc["ch"]          | -1,
            doc["pan"]         | 0,
            doc["epid"]        | "?",
            doc["coord"]       | "?",
            doc["role"]        | "?",
            (int)(doc["joined"]      | false),
            (int)(doc["factory_new"] | false),
            doc["tx_dbm"]      | 0,
            doc["nwk_upd_id"]  | 0,
            doc["devices"]     | -1,
            (unsigned)(doc["heap"]     | 0),
            (unsigned)(doc["uptime_s"] | 0));
    }
    else if (strcmp(type, "scan") == 0)
    {
        size_t count = 0;
        if (!doc["nets"].isNull()) count = doc["nets"].size();
        else if (!doc["networks"].isNull()) count = doc["networks"].size();
        logger.notice("[H2] scan: %d networks found", (int)count);
    }
    else if (strcmp(type, "wakeup") == 0)
    {
        logger.notice("[H2] wakeup reason: %s", doc["reason"] | "?");
    }
    else if (strcmp(type, "ota_progress") == 0)
    {
        logger.notice("[H2] OTA: %d / %d bytes", doc["written"].as<int>(), doc["total"].as<int>());
    }
    else if (strcmp(type, "version") == 0)
    {
        logger.notice("[H2] version fw=%s build=%s",
                      g_h2_fw_version.length() ? g_h2_fw_version.c_str() : "-",
                      g_h2_fw_build.length() ? g_h2_fw_build.c_str() : "-");
    }
    else if (strcmp(type, "chipinfo") == 0)
    {
        logger.notice("[H2] chipinfo model=%s rev=%s mac=%s",
                      g_h2_chip_model.length() ? g_h2_chip_model.c_str() : "-",
                      g_h2_chip_rev.length() ? g_h2_chip_rev.c_str() : "-",
                      g_h2_chip_mac.length() ? g_h2_chip_mac.c_str() : "-");
    }
    else if (strcmp(type, "chip") == 0)
    {
        logger.notice("[H2] chip model=%s rev=%s cores=%s cpuMHz=%s mac=%s",
                      g_h2_chip_model.length() ? g_h2_chip_model.c_str() : "-",
                      g_h2_chip_rev.length() ? g_h2_chip_rev.c_str() : "-",
                      g_h2_chip_cores.length() ? g_h2_chip_cores.c_str() : "-",
                      g_h2_chip_cpu_mhz.length() ? g_h2_chip_cpu_mhz.c_str() : "-",
                      g_h2_chip_mac.length() ? g_h2_chip_mac.c_str() : "-");
    }
    else if (strcmp(type, "motion") == 0)
    {
        const bool occ = doc["occ"] | false;
        logger.notice("[H2] motion occ=%d lux=%d temp=%.1f bat=%d",
                      (int)occ, doc["lux"] | -1, doc["temp"] | 0.0, doc["bat"] | -1);
        if (occ)
            app_fsm_notify_user_activity(g_fsm);
    }
    else if (strcmp(type, "sensor") == 0)
    {
        logger.notice("[H2] sensor lux=%d temp=%.1f bat=%d",
                      doc["lux"] | -1, doc["temp"] | 0.0, doc["bat"] | -1);
        app_fsm_notify_user_activity(g_fsm);
    }
    else
    {
        logger.notice("[H2] msg [%s]: %s", type, line.c_str());
    }
}

void zigbee_h2_poll_uart()
{
    static String uart_ipc_buf;
    while (!h2_ota_in_progress() && SerialPort.available() > 0)
    {
        char c = (char)SerialPort.read();
        if (c == '\n')
        {
            if (!uart_ipc_buf.isEmpty())
            {
                if (uart_ipc_buf.startsWith("{"))
                    h2_handle_message(uart_ipc_buf);
                else
                    logger.notice("[H2] %s", uart_ipc_buf.c_str());
                uart_ipc_buf.clear();
            }
        }
        else if (c != '\r')
        {
            uart_ipc_buf += c;
            if (uart_ipc_buf.length() > 2048)
                uart_ipc_buf.clear();
        }
    }
}
