#include "zigbee_h2.h"
#include "zha_db.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <math.h>
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

// ── Device registry ─────────────────────────────────────────────────────────
// Written by the loop task (UART poll), read by AsyncWebServer callbacks on
// the AsyncTCP task -- hence the lock. Entries are upserted by address and
// never wiped: a "list" reply can arrive streamed (idx/total), and clearing
// the table first would blank the UI mid-transfer. Stale entries age out via
// last_seen_ms, which the UI shows.
static H2Device        g_h2_devices[H2_MAX_DEVICES];
static SemaphoreHandle_t g_h2_dev_lock = nullptr;

// Commands from other tasks. h2_send() writes the UART directly and would
// interleave with the loop task's own writes, so foreign callers queue instead
// and the loop task drains this in zigbee_h2_poll_uart().
struct H2Cmd { char buf[160]; };
static QueueHandle_t   g_h2_cmd_queue = nullptr;

// Last coordinator snapshot and scan result, both guarded by g_h2_dev_lock.
struct H2Status {
    int      ch = -1;
    unsigned pan = 0;
    char     epid[26] = "";
    char     role[16] = "";
    bool     joined = false;
    int      tx_dbm = 0;
    int      devices = -1;
    unsigned heap = 0;
    unsigned uptime_s = 0;
    uint32_t received_ms = 0;
};
static H2Status g_h2_status;
static String   g_h2_scan_nets = "[]";
static uint32_t g_h2_scan_ms   = 0;

/// Copies only when there is something to copy: partial updates (a motion or
/// sensor message carries no model or vendor) must not erase what a list reply
/// already established.
static void h2_copy_field(char *dst, size_t cap, const char *src)
{
    if (src == nullptr || src[0] == '\0') return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/// Insert or update one device. Caller must hold g_h2_dev_lock.
static void h2_dev_upsert_locked(uint16_t addr, const char *ieee, const char *mfr,
                                 const char *model, int ep, int online,
                                 int occ, float temp, int bat, int on)
{
    int slot = -1;
    for (int i = 0; i < H2_MAX_DEVICES; i++) {
        if (g_h2_devices[i].used && g_h2_devices[i].addr == addr) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < H2_MAX_DEVICES; i++) {
            if (!g_h2_devices[i].used) { slot = i; break; }
        }
    }
    // Table full: drop the entry we have not heard from in the longest time.
    if (slot < 0) {
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < H2_MAX_DEVICES; i++) {
            if (g_h2_devices[i].last_seen_ms < oldest) { oldest = g_h2_devices[i].last_seen_ms; slot = i; }
        }
        memset(&g_h2_devices[slot], 0, sizeof(H2Device));
    }

    H2Device &d = g_h2_devices[slot];
    if (!d.used) {
        memset(&d, 0, sizeof(H2Device));
        d.occ  = -1;
        d.on   = -1;
        d.bat  = -1;
        d.temp = NAN;
        d.used = true;
    }
    d.addr = addr;
    h2_copy_field(d.ieee,  sizeof(d.ieee),  ieee);
    h2_copy_field(d.mfr,   sizeof(d.mfr),   mfr);
    h2_copy_field(d.model, sizeof(d.model), model);
    if (ep     >= 0) d.ep     = (uint8_t)ep;
    if (online >= 0) d.online = (online != 0);
    if (occ    >= 0) d.occ    = (int8_t)occ;
    if (on     >= 0) d.on     = (int8_t)on;
    if (bat    >= 0) d.bat    = (int16_t)bat;
    if (!isnan(temp)) d.temp  = temp;
    d.last_seen_ms = millis();
}

static void h2_dev_upsert(uint16_t addr, const char *ieee, const char *mfr,
                          const char *model, int ep, int online,
                          int occ, float temp, int bat, int on)
{
    if (g_h2_dev_lock == nullptr) return;
    if (xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    h2_dev_upsert_locked(addr, ieee, mfr, model, ep, online, occ, temp, bat, on);
    xSemaphoreGive(g_h2_dev_lock);
}

void h2_devices_json(String &out)
{
    out = "[";
    if (g_h2_dev_lock == nullptr) { out += "]"; return; }
    if (xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(200)) != pdTRUE) { out += "]"; return; }

    const uint32_t now = millis();
    bool first = true;
    char entry[320];
    for (int i = 0; i < H2_MAX_DEVICES; i++) {
        const H2Device &d = g_h2_devices[i];
        if (!d.used) continue;
        char temp_buf[16];
        if (isnan(d.temp)) strcpy(temp_buf, "null");
        else               snprintf(temp_buf, sizeof(temp_buf), "%.1f", d.temp);
        snprintf(entry, sizeof(entry),
                 "%s{\"addr\":%u,\"hex\":\"0x%04X\",\"ieee\":\"%s\",\"mfr\":\"%s\","
                 "\"model\":\"%s\",\"ep\":%u,\"online\":%s,\"occ\":%d,"
                 "\"temp\":%s,\"bat\":%d,\"on\":%d,\"age_s\":%u}",
                 first ? "" : ",", (unsigned)d.addr, (unsigned)d.addr,
                 d.ieee, d.mfr, d.model, (unsigned)d.ep,
                 d.online ? "true" : "false", (int)d.occ,
                 temp_buf, (int)d.bat, (int)d.on,
                 (unsigned)((now - d.last_seen_ms) / 1000UL));
        out += entry;
        first = false;
    }
    xSemaphoreGive(g_h2_dev_lock);
    out += "]";
}

size_t h2_devices_count()
{
    size_t n = 0;
    if (g_h2_dev_lock == nullptr) return 0;
    if (xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    for (int i = 0; i < H2_MAX_DEVICES; i++) if (g_h2_devices[i].used) n++;
    xSemaphoreGive(g_h2_dev_lock);
    return n;
}

void h2_status_json(String &out)
{
    if (g_h2_dev_lock == nullptr ||
        xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        out = "{\"valid\":false}";
        return;
    }
    char buf[420];
    const bool valid = (g_h2_status.received_ms != 0);
    snprintf(buf, sizeof(buf),
             "{\"valid\":%s,\"ch\":%d,\"pan\":\"0x%04X\",\"epid\":\"%s\",\"role\":\"%s\","
             "\"joined\":%s,\"tx_dbm\":%d,\"devices\":%d,\"heap\":%u,\"uptime_s\":%u,"
             "\"age_s\":%u}",
             valid ? "true" : "false", g_h2_status.ch, g_h2_status.pan,
             g_h2_status.epid, g_h2_status.role,
             g_h2_status.joined ? "true" : "false", g_h2_status.tx_dbm,
             g_h2_status.devices, g_h2_status.heap, g_h2_status.uptime_s,
             valid ? (unsigned)((millis() - g_h2_status.received_ms) / 1000UL) : 0u);
    xSemaphoreGive(g_h2_dev_lock);
    out = buf;
}

void h2_scan_json(String &out)
{
    if (g_h2_dev_lock == nullptr ||
        xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        out = "{\"age_s\":-1,\"nets\":[]}";
        return;
    }
    out  = "{\"age_s\":";
    out += (g_h2_scan_ms == 0) ? String(-1)
                               : String((unsigned)((millis() - g_h2_scan_ms) / 1000UL));
    out += ",\"nets\":";
    out += g_h2_scan_nets;
    out += "}";
    xSemaphoreGive(g_h2_dev_lock);
}

bool h2_dev_forget(uint16_t addr)
{
    if (g_h2_dev_lock == nullptr) return false;
    if (xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool hit = false;
    for (int i = 0; i < H2_MAX_DEVICES; i++) {
        if (g_h2_devices[i].used && g_h2_devices[i].addr == addr) {
            memset(&g_h2_devices[i], 0, sizeof(H2Device));
            hit = true;
            break;
        }
    }
    xSemaphoreGive(g_h2_dev_lock);
    return hit;
}

bool h2_enqueue(const char *cmd)
{
    if (g_h2_cmd_queue == nullptr || cmd == nullptr) return false;
    H2Cmd item;
    h2_copy_field(item.buf, sizeof(item.buf), cmd);
    return xQueueSend(g_h2_cmd_queue, &item, 0) == pdTRUE;
}

void setup_UART_IPC()
{
    h2_reset_chip();
    delay(500);

    SerialPort.begin(460800, SERIAL_8N1, ESP32H2_RX, ESP32H2_TX);
    Serial.println();
    Serial.printf("[%09lu ms][%s][%s] ", (unsigned long)millis(), __FILE__, __func__);
    Serial.println(F("Init SerialPort for IPC"));

    if (g_h2_dev_lock == nullptr)  g_h2_dev_lock  = xSemaphoreCreateMutex();
    if (g_h2_cmd_queue == nullptr) g_h2_cmd_queue = xQueueCreate(8, sizeof(H2Cmd));

    h2_send("{\"cmd\":\"version\"}");
    h2_send("{\"cmd\":\"chipinfo\"}");
    h2_send("{\"cmd\":\"list\"}");
    h2_send("{\"cmd\":\"status\"}");
}

void h2_send(const char *cmd)
{
    SerialPort.print(cmd);
    SerialPort.print('\n');
    logger.debug("[H2] TX: %s", cmd);
}

static void h2_handle_message(const String &line)
{
    logger.debug("[H2] raw: %s", line.c_str());

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
            logger.debug("[H2] devices (%s): %d entries", type, (int)devices.size());
        }
        for (JsonObject d : devices) {
            if (streaming) {
                logger.debug("[H2] device (%s) %d/%d: 0x%04x %s  mfr=%s model=%s ep=%u online=%d occ=%d temp=%.1f bat=%d",
                    type, idx, total,
                    d["addr"].as<uint16_t>(), d["ieee"] | "?", d["mfr"]  | "?",
                    d["model"] | "?", d["ep"].as<uint8_t>(), (int)(d["online"] | false),
                    d["occ"].isNull() ? -1 : (int)d["occ"].as<bool>(),
                    d["temp"].isNull() ? 0.0f : (float)d["temp"].as<float>(),
                    d["bat"].isNull() ? -1 : (int)d["bat"].as<int>());
            } else {
                logger.debug("[H2]  0x%04x %s  mfr=%s model=%s ep=%u online=%d occ=%d temp=%.1f bat=%d",
                    d["addr"].as<uint16_t>(), d["ieee"] | "?", d["mfr"]  | "?",
                    d["model"] | "?", d["ep"].as<uint8_t>(), (int)(d["online"] | false),
                    d["occ"].isNull() ? -1 : (int)d["occ"].as<bool>(),
                    d["temp"].isNull() ? 0.0f : (float)d["temp"].as<float>(),
                    d["bat"].isNull() ? -1 : (int)d["bat"].as<int>());
            }
            h2_dev_upsert(d["addr"].as<uint16_t>(),
                          d["ieee"]  | "", d["mfr"] | "", d["model"] | "",
                          d["ep"].isNull()     ? -1 : (int)d["ep"].as<uint8_t>(),
                          d["online"].isNull() ? -1 : (int)(d["online"].as<bool>() ? 1 : 0),
                          d["occ"].isNull()    ? -1 : (int)(d["occ"].as<bool>() ? 1 : 0),
                          d["temp"].isNull()   ? NAN : d["temp"].as<float>(),
                          d["bat"].isNull()    ? -1 : d["bat"].as<int>(),
                          d["on"].isNull()     ? -1 : (int)(d["on"].as<bool>() ? 1 : 0));
        }
    }
    else if (strcmp(type, "ack") == 0)
    {
        logger.debug("[H2] ack [%s] ok=%d %s", doc["cmd"] | "?", (bool)(doc["ok"] | false), doc["msg"] | "");
    }
    else if (strcmp(type, "status") == 0)
    {
        logger.debug("[H2] status  ch=%d  pan=0x%04x  epid=%s  coord=%s  role=%s  joined=%d  factory_new=%d  tx=%ddBm  nwk_upd=%d  devices=%d  heap=%u  uptime=%us",
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

        if (g_h2_dev_lock != nullptr &&
            xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_h2_status.ch       = doc["ch"]   | -1;
            g_h2_status.pan      = (unsigned)(doc["pan"] | 0);
            h2_copy_field(g_h2_status.epid, sizeof(g_h2_status.epid), doc["epid"] | "");
            h2_copy_field(g_h2_status.role, sizeof(g_h2_status.role), doc["role"] | "");
            g_h2_status.joined   = doc["joined"] | false;
            g_h2_status.tx_dbm   = doc["tx_dbm"] | 0;
            g_h2_status.devices  = doc["devices"] | -1;
            g_h2_status.heap     = (unsigned)(doc["heap"]     | 0);
            g_h2_status.uptime_s = (unsigned)(doc["uptime_s"] | 0);
            g_h2_status.received_ms = millis();
            xSemaphoreGive(g_h2_dev_lock);
        }
    }
    else if (strcmp(type, "scan") == 0)
    {
        size_t count = 0;
        if (!doc["nets"].isNull()) count = doc["nets"].size();
        else if (!doc["networks"].isNull()) count = doc["networks"].size();
        logger.debug("[H2] scan: %d networks found", (int)count);

        JsonVariant nets = doc["nets"].isNull() ? doc["networks"] : doc["nets"];
        String serialised = "[]";
        if (!nets.isNull()) serializeJson(nets, serialised);
        if (g_h2_dev_lock != nullptr &&
            xSemaphoreTake(g_h2_dev_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_h2_scan_nets = serialised;
            g_h2_scan_ms   = millis();
            xSemaphoreGive(g_h2_dev_lock);
        }
    }
    else if (strcmp(type, "channel") == 0)
    {
        logger.debug("[H2] channel  current=%d  configured=%d",
                      doc["ch"] | -1, doc["configured"] | -1);
    }
    else if (strcmp(type, "wakeup") == 0)
    {
        logger.debug("[H2] wakeup reason: %s", doc["reason"] | "?");
    }
    else if (strcmp(type, "ota_progress") == 0)
    {
        logger.debug("[H2] OTA: %d / %d bytes", doc["written"].as<int>(), doc["total"].as<int>());
    }
    else if (strcmp(type, "version") == 0)
    {
        logger.debug("[H2] version fw=%s build=%s",
                      g_h2_fw_version.length() ? g_h2_fw_version.c_str() : "-",
                      g_h2_fw_build.length() ? g_h2_fw_build.c_str() : "-");
    }
    else if (strcmp(type, "chipinfo") == 0)
    {
        logger.debug("[H2] chipinfo model=%s rev=%s mac=%s",
                      g_h2_chip_model.length() ? g_h2_chip_model.c_str() : "-",
                      g_h2_chip_rev.length() ? g_h2_chip_rev.c_str() : "-",
                      g_h2_chip_mac.length() ? g_h2_chip_mac.c_str() : "-");
    }
    else if (strcmp(type, "chip") == 0)
    {
        logger.debug("[H2] chip model=%s rev=%s cores=%s cpuMHz=%s mac=%s",
                      g_h2_chip_model.length() ? g_h2_chip_model.c_str() : "-",
                      g_h2_chip_rev.length() ? g_h2_chip_rev.c_str() : "-",
                      g_h2_chip_cores.length() ? g_h2_chip_cores.c_str() : "-",
                      g_h2_chip_cpu_mhz.length() ? g_h2_chip_cpu_mhz.c_str() : "-",
                      g_h2_chip_mac.length() ? g_h2_chip_mac.c_str() : "-");
    }
    else if (strcmp(type, "motion") == 0)
    {
        const bool occ = doc["occ"] | false;
        logger.debug("[H2] motion occ=%d lux=%d temp=%.1f bat=%d",
                      (int)occ, doc["lux"] | -1, doc["temp"] | 0.0, doc["bat"] | -1);
        if (!doc["addr"].isNull())
            h2_dev_upsert(doc["addr"].as<uint16_t>(), doc["ieee"] | "", "", "",
                          -1, 1, occ ? 1 : 0,
                          doc["temp"].isNull() ? NAN : doc["temp"].as<float>(),
                          doc["bat"].isNull()  ? -1  : doc["bat"].as<int>(), -1);
        if (occ)
            app_fsm_notify_user_activity(g_fsm);
    }
    else if (strcmp(type, "profile_req") == 0)
    {
        // Unsolicited: the H2 asks us, not the other way round. It has just
        // learned a joining device's identity and needs that device's profile
        // from the ZHA database before it can bind and configure reporting.
        zha_db_answer(doc["rid"] | 0UL,
                      doc["manufacturerName"] | "",
                      doc["modelId"] | "");
    }
    else if (strcmp(type, "sensor") == 0)
    {
        logger.debug("[H2] sensor lux=%d temp=%.1f bat=%d",
                      doc["lux"] | -1, doc["temp"] | 0.0, doc["bat"] | -1);
        if (!doc["addr"].isNull())
            h2_dev_upsert(doc["addr"].as<uint16_t>(), doc["ieee"] | "", "", "",
                          -1, 1, -1,
                          doc["temp"].isNull() ? NAN : doc["temp"].as<float>(),
                          doc["bat"].isNull()  ? -1  : doc["bat"].as<int>(), -1);
        app_fsm_notify_user_activity(g_fsm);
    }
    else
    {
        logger.debug("[H2] msg [%s]: %s", type, line.c_str());
    }
}

void zigbee_h2_poll_uart()
{
    static String uart_ipc_buf;

    // Drain queued commands from other tasks first -- this is the only place
    // that writes the UART besides the OTA transfer.
    if (g_h2_cmd_queue != nullptr && !h2_ota_in_progress()) {
        H2Cmd item;
        while (xQueueReceive(g_h2_cmd_queue, &item, 0) == pdTRUE)
            h2_send(item.buf);
    }

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
                    logger.debug("[H2] %s", uart_ipc_buf.c_str());
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
