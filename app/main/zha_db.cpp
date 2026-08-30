#include "zha_db.h"
#include "zigbee_h2.h"

#include <LittleFS.h>
#include <uuid/log.h>

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static const char *kIndexPath = "/zha_idx.bin";
static const char *kDbPath    = "/zha_db.bin";

// Index header: magic, schema, stride, count, db length.
static const uint8_t  kHeaderSize = 16;
static const uint8_t  kEntrySize  = 16;
static const uint16_t kSchema     = 1;

static bool     g_available = false;
static uint32_t g_count     = 0;
static uint32_t g_dbLength  = 0;

// Records are streamed to the UART in chunks so a large profile never needs a
// buffer of its own; 128 B keeps the stack frame small and still fills the
// UART FIFO comfortably at 460800 baud.
static const size_t kChunk = 128;

bool zha_db_available() { return g_available; }
uint32_t zha_db_count() { return g_count; }

static uint64_t fnv1a64(const char *manufacturer, const char *model)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    auto feed = [&h](const char *s) {
        for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
            h ^= *p;
            h *= 0x100000001B3ULL;
        }
    };
    feed(manufacturer);
    h ^= 0x00;                    // the separator between the two halves
    h *= 0x100000001B3ULL;
    feed(model);
    return h;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

bool zha_db_begin()
{
    g_available = false;
    g_count = 0;

    File idx = LittleFS.open(kIndexPath, "r");
    if (!idx) {
        logger.info(F("[ZHA] no device database at %s -- the H2 will use its "
                      "built-in heuristics"), kIndexPath);
        return false;
    }

    uint8_t header[kHeaderSize];
    if (idx.read(header, kHeaderSize) != kHeaderSize) {
        logger.err(F("[ZHA] index too short"));
        idx.close();
        return false;
    }

    if (memcmp(header, "ZHAI", 4) != 0) {
        logger.err(F("[ZHA] bad index magic"));
        idx.close();
        return false;
    }

    uint16_t schema = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
    uint16_t stride = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    g_count    = rd32(header + 8);
    g_dbLength = rd32(header + 12);
    idx.close();

    if (schema != kSchema) {
        logger.err(F("[ZHA] index schema %u, expected %u -- regenerate with "
                     "tools/update_db.sh"), schema, kSchema);
        return false;
    }
    if (stride != kEntrySize) {
        logger.err(F("[ZHA] index stride %u, expected %u"), stride, kEntrySize);
        return false;
    }

    File db = LittleFS.open(kDbPath, "r");
    if (!db) {
        logger.err(F("[ZHA] index present but %s is missing"), kDbPath);
        return false;
    }
    // A truncated upload is the likeliest failure here, and it would otherwise
    // surface much later as one device returning garbage.
    if ((uint32_t)db.size() != g_dbLength) {
        logger.err(F("[ZHA] %s is %u B, index says %u -- incomplete upload?"),
                   kDbPath, (unsigned)db.size(), (unsigned)g_dbLength);
        db.close();
        return false;
    }
    db.close();

    g_available = true;
    logger.info(F("[ZHA] device database ready: %u devices, %u B"),
                (unsigned)g_count, (unsigned)g_dbLength);
    return true;
}

/// Binary search the index for `key`. ~11 seeks into an 18 KB file.
static bool findEntry(File &idx, uint64_t key, uint32_t &offset, uint32_t &length)
{
    int32_t lo = 0;
    int32_t hi = (int32_t)g_count - 1;
    uint8_t entry[kEntrySize];

    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (!idx.seek(kHeaderSize + (uint32_t)mid * kEntrySize)) return false;
        if (idx.read(entry, kEntrySize) != kEntrySize) return false;

        uint64_t h = rd64(entry);
        if (h == key) {
            offset = rd32(entry + 8);
            length = rd32(entry + 12);
            return true;
        }
        if (h < key) lo = mid + 1;
        else         hi = mid - 1;
    }
    return false;
}

/// Confirms a candidate record really is this device before we answer with it.
/// The hash is 64 bit so a collision is unlikely, but answering with the wrong
/// device's profile would misconfigure real hardware, so it is worth the read.
static bool recordMatches(File &db, uint32_t offset, uint32_t length,
                          const char *manufacturer, const char *model)
{
    // Manufacturer and model are the first two fields of every record, so the
    // check only needs its opening bytes, not the whole profile.
    char head[128];
    size_t want = length < sizeof(head) - 1 ? length : sizeof(head) - 1;
    if (!db.seek(offset)) return false;
    size_t got = db.read((uint8_t *)head, want);
    head[got] = '\0';

    char expected[128];
    snprintf(expected, sizeof(expected), "{\"m\":\"%s\",\"d\":\"%s\"",
             manufacturer, model);
    return strncmp(head, expected, strlen(expected)) == 0;
}

static bool streamRecord(File &db, uint32_t offset, uint32_t length)
{
    if (!db.seek(offset)) return false;

    uint8_t buf[kChunk];
    uint32_t remaining = length;
    while (remaining > 0) {
        size_t want = remaining < kChunk ? remaining : kChunk;
        size_t got = db.read(buf, want);
        if (got == 0) return false;
        SerialPort.write(buf, got);
        remaining -= got;
    }
    return true;
}

void zha_db_answer(uint32_t rid, const char *manufacturer, const char *model)
{
    if (manufacturer == nullptr) manufacturer = "";
    if (model == nullptr) model = "";

    if (g_available) {
        File idx = LittleFS.open(kIndexPath, "r");
        File db  = LittleFS.open(kDbPath, "r");
        if (idx && db) {
            // Exact pair first, then the wildcard forms a quirk may have
            // registered when it matches on only one half of the key.
            const char *keys[3][2] = {
                { manufacturer, model },
                { "",           model },
                { manufacturer, ""    },
            };

            for (auto &k : keys) {
                uint32_t offset = 0, length = 0;
                if (!findEntry(idx, fnv1a64(k[0], k[1]), offset, length)) continue;
                if (offset + length > g_dbLength) continue;
                if (!recordMatches(db, offset, length, k[0], k[1])) continue;

                SerialPort.printf("{\"cmd\":\"profile\",\"rid\":%lu,\"found\":true,\"p\":",
                                  (unsigned long)rid);
                bool ok = streamRecord(db, offset, length);
                SerialPort.print("}\n");
                idx.close();
                db.close();

                if (ok) {
                    logger.info(F("[ZHA] %s/%s -> profile (%u B)"),
                                manufacturer, model, (unsigned)length);
                } else {
                    // The line already went out truncated; the H2 will fail to
                    // parse it and fall back after its retries.
                    logger.err(F("[ZHA] read failed mid-record for %s/%s"),
                               manufacturer, model);
                }
                return;
            }
        }
        if (idx) idx.close();
        if (db) db.close();
    }

    SerialPort.printf("{\"cmd\":\"profile\",\"rid\":%lu,\"found\":false}\n",
                      (unsigned long)rid);
    logger.info(F("[ZHA] %s/%s not in database -- H2 falls back to heuristics"),
                manufacturer, model);
}
