#include "h2_ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "mbedtls/base64.h"
#include <esp_heap_caps.h>
#include <uuid/log.h>

extern HardwareSerial SerialPort;

extern const uint8_t x509_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

// 384 raw bytes → exactly 512 base64 chars
static constexpr int CHUNK_RAW           = 384;
static constexpr int CHUNK_B64           = 512;
static constexpr unsigned long ACK_TIMEOUT_MS = 15000;

static volatile bool   g_ota_active  = false;
static volatile size_t g_ota_written = 0;
static volatile size_t g_ota_total   = 0;

// Source for the active transfer (only one is set at a time)
static char     g_ota_url[512];
static uint8_t* g_ota_buf  = nullptr;
static size_t   g_ota_size = 0;

bool   h2_ota_in_progress() { return g_ota_active; }
void   h2_ota_force_clear() { g_ota_active = false; }
size_t h2_ota_written()     { return g_ota_written; }
size_t h2_ota_total()       { return g_ota_total; }

// ──────────────────────────────────────────────────────────────
// UART helpers (called from the OTA task — main loop is paused)
// ──────────────────────────────────────────────────────────────

static void h2_tx(const char* s)
{
    SerialPort.print(s);
    SerialPort.print('\n');
}

static bool read_line(String& out, unsigned long timeout_ms)
{
    out.clear();
    unsigned long t0 = millis();
    while (millis() - t0 < timeout_ms) {
        while (SerialPort.available()) {
            char c = (char)SerialPort.read();
            if (c == '\n') return !out.isEmpty();
            if (c != '\r') out += c;
        }
        delay(5);
    }
    return false;
}

static bool wait_ack(const char* cmd_name, unsigned long timeout_ms = ACK_TIMEOUT_MS)
{
    String line;
    unsigned long deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        unsigned long left = deadline - millis();
        if (!read_line(line, left)) break;
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        if (strcmp(doc["type"] | "", "ack") == 0 &&
            strcmp(doc["cmd"]  | "", cmd_name) == 0) {
            bool ok = doc["ok"] | false;
            if (!ok) logger.warning("[H2-OTA] nok: %s", doc["msg"] | "");
            return ok;
        }
    }
    logger.warning("[H2-OTA] timeout for ack '%s'", cmd_name);
    return false;
}

static bool wait_progress(unsigned long timeout_ms = ACK_TIMEOUT_MS)
{
    String line;
    unsigned long deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        unsigned long left = deadline - millis();
        if (!read_line(line, left)) break;
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        const char* type = doc["type"] | "";
        if (strcmp(type, "ota_progress") == 0) return true;
        if (strcmp(type, "ack") == 0 && !(doc["ok"] | true)) {
            logger.warning("[H2-OTA] H2 error: %s", doc["msg"] | "?");
            return false;
        }
    }
    logger.warning("[H2-OTA] timeout for ota_progress");
    return false;
}

// ──────────────────────────────────────────────────────────────
// Core flash routine — reads from a callback that yields raw bytes
// ──────────────────────────────────────────────────────────────

struct ChunkSource {
    virtual int read(uint8_t* buf, int maxlen) = 0; // returns bytes read, 0=done, <0=error
    virtual ~ChunkSource() = default;
};

static bool flash_to_h2(ChunkSource& src, size_t total)
{
    g_ota_written = 0;
    g_ota_total   = total;
    uint32_t last_progress_log_ms = 0;
    int last_progress_percent = -1;

    // ota_start
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"ota_start\",\"size\":%u}", (unsigned)total);
    h2_tx(cmd);
    if (!wait_ack("ota_start")) return false;

    uint8_t raw[CHUNK_RAW];
    uint8_t b64[CHUNK_B64 + 4];
    char    json_buf[CHUNK_B64 + 32];
    size_t  b64_len;

    size_t  sent      = 0;
    int     chunk_num = 0;

    while (sent < total) {
        int got = src.read(raw, CHUNK_RAW);
        if (got < 0) { logger.warning("[H2-OTA] read error at chunk %d", chunk_num); return false; }
        if (got == 0) break;

        if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, raw, got) != 0) {
            logger.warning("[H2-OTA] base64 fail at chunk %d", chunk_num);
            return false;
        }
        b64[b64_len] = '\0';

        snprintf(json_buf, sizeof(json_buf), "{\"cmd\":\"ota_data\",\"data\":\"%s\"}", (char*)b64);
        h2_tx(json_buf);

        if (!wait_progress()) return false;

        sent += got;
        g_ota_written = sent;
        chunk_num++;

        if (total > 0) {
            float progress = (100.0f * (float)sent) / (float)total;
            if (progress > 100.0f) progress = 100.0f;
            int progress_i = (int)progress;
            uint32_t now = millis();
            if (progress_i != last_progress_percent &&
                (last_progress_log_ms == 0 || (now - last_progress_log_ms) >= 1000)) {
                last_progress_percent = progress_i;
                last_progress_log_ms = now;
                logger.notice("[H2-OTA] Progress: %.2f%% (%u / %u Bytes)",
                              progress, (unsigned)sent, (unsigned)total);
            }
        }
    }

    // ota_end
    h2_tx("{\"cmd\":\"ota_end\"}");
    if (!wait_ack("ota_end", 20000)) return false;

    logger.notice("[H2-OTA] done — H2 is rebooting");
    return true;
}

// ──────────────────────────────────────────────────────────────
// Buffer source
// ──────────────────────────────────────────────────────────────

struct BufSource : ChunkSource {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    BufSource(const uint8_t* d, size_t s) : data(d), size(s) {}

    int read(uint8_t* buf, int maxlen) override {
        if (pos >= size) return 0;
        int n = min((size_t)maxlen, size - pos);
        memcpy(buf, data + pos, n);
        pos += n;
        return n;
    }
};

static void buf_ota_task(void* /*arg*/)
{
    logger.notice("[H2-OTA] buffer OTA: %u bytes", (unsigned)g_ota_size);
    BufSource src(g_ota_buf, g_ota_size);
    flash_to_h2(src, g_ota_size);

    heap_caps_free(g_ota_buf);
    g_ota_buf  = nullptr;
    g_ota_size = 0;
    g_ota_active = false;
    vTaskDelete(nullptr);
}

bool h2_ota_start_from_buffer(uint8_t* data, size_t size)
{
    if (g_ota_active) { logger.warning("[H2-OTA] busy"); return false; }
    if (!data || size == 0) { logger.warning("[H2-OTA] empty buffer"); return false; }

    g_ota_buf    = data;
    g_ota_size   = size;
    g_ota_active = true;

    if (xTaskCreate(buf_ota_task, "h2_ota", 12288, nullptr, 3, nullptr) != pdPASS) {
        logger.warning("[H2-OTA] task create failed");
        heap_caps_free(g_ota_buf); g_ota_buf = nullptr;
        g_ota_active = false;
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────
// URL source (HTTP/HTTPS streaming)
// ──────────────────────────────────────────────────────────────

struct HttpSource : ChunkSource {
    WiFiClientSecure tls;
    HTTPClient       http;
    WiFiClient*      stream = nullptr;
    bool             ok     = false;
    int              total  = 0;

    bool begin(const char* url) {
        tls.setCACertBundle(x509_crt_bundle_start);
        bool is_https = strncmp(url, "https://", 8) == 0;
        if (is_https) http.begin(tls, url);
        else          http.begin(url);
        http.setTimeout(30000);

        int code = http.GET();
        if (code != 200) { logger.warning("[H2-OTA] HTTP %d", code); return false; }
        total = http.getSize();
        if (total <= 0) { logger.warning("[H2-OTA] unknown size"); return false; }
        stream = http.getStreamPtr();
        ok = true;
        return true;
    }

    int read(uint8_t* buf, int maxlen) override {
        if (!stream) return -1;
        int got = 0;
        unsigned long t0 = millis();
        while (got < maxlen && millis() - t0 < 10000) {
            int avail = stream->available();
            if (avail > 0) {
                int r = stream->read(buf + got, maxlen - got);
                if (r > 0) got += r;
            } else if (!stream->connected()) {
                break;
            } else {
                delay(1);
            }
        }
        return got;
    }

    ~HttpSource() { http.end(); }
};

static void url_ota_task(void* /*arg*/)
{
    logger.notice("[H2-OTA] URL OTA: %s", g_ota_url);
    HttpSource src;
    if (src.begin(g_ota_url))
        flash_to_h2(src, src.total);

    g_ota_active = false;
    vTaskDelete(nullptr);
}

bool h2_ota_start(const char* url)
{
    if (g_ota_active) { logger.warning("[H2-OTA] busy"); return false; }
    if (!url || strlen(url) == 0 || strlen(url) >= sizeof(g_ota_url)) {
        logger.warning("[H2-OTA] invalid URL"); return false;
    }

    strncpy(g_ota_url, url, sizeof(g_ota_url) - 1);
    g_ota_url[sizeof(g_ota_url) - 1] = '\0';
    g_ota_active = true;

    if (xTaskCreate(url_ota_task, "h2_ota", 12288, nullptr, 3, nullptr) != pdPASS) {
        logger.warning("[H2-OTA] task create failed");
        g_ota_active = false;
        return false;
    }
    return true;
}
