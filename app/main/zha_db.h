#pragma once

#include <Arduino.h>

// ZHA device database lookup.
//
// The H2 coordinator knows how to talk ZCL but carries no per-device
// knowledge -- it has no room for it beside the Zigbee stack. When a device
// joins and reports its manufacturer and model, the H2 asks us for that
// device's profile, and we answer from a database generated out of ZHA's
// quirks and stored in LittleFS.
//
// The database is built in the coordinator repo (tools/update_db.sh there) and
// shipped as two files in data/:
//
//   /zha_idx.bin   sorted hash -> (offset, length), binary searched in place
//   /zha_db.bin    each device's profile, stored as the exact JSON object the
//                  H2 expects to receive
//
// Because the records are pre-rendered, answering a request needs no JSON
// parsing and no large buffer: seek, then stream the bytes to the UART between
// a fixed prefix and suffix.

/// Opens the index and validates its header. Safe to call when the files are
/// absent -- lookups then simply report nothing found, and the H2 falls back
/// to its own heuristics. Call after LittleFS is mounted.
bool zha_db_begin();

/// True when a usable database is present.
bool zha_db_available();

/// Number of devices in the database, 0 if unavailable.
uint32_t zha_db_count();

/// Answers one profile_req from the H2 by writing a complete JSON line to the
/// coordinator UART. Always answers, including when nothing matched, so the H2
/// never has to wait out its timeout.
///
/// Must be called from the task that owns the UART -- the same rule h2_send()
/// follows.
void zha_db_answer(uint32_t rid, const char *manufacturer, const char *model);
