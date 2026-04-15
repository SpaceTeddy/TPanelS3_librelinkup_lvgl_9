// web_glucose_api.cpp (DROP-IN v8: prefer local timestamp[] over factory_timestamp[])
// ------------------------------------------------------------
// Latest:  /api/glucose  -> mgdl, delta, trend, targets, ts_ok, life_*
// History: /api/glucose/history -> values: [null | {v,ts}]  (ts in epoch seconds)
// IMPORTANT: uses llu_sensor_history_data.timestamp[i] if available, otherwise factory_timestamp[i]
// ------------------------------------------------------------

#include <Arduino.h>
#include <time.h>
#include <librelinkup.h>

extern LIBRELINKUP librelinkup;
extern int16_t glucose_delta;

String web_get_glucose_latest_json() {
    const uint16_t mgdl = (uint16_t)librelinkup.glucose_data().glucoseMeasurement;
    const uint16_t low  = (uint16_t)librelinkup.glucose_data().glucosetargetLow;
    const uint16_t high = (uint16_t)librelinkup.glucose_data().glucosetargetHigh;
    const bool ts_ok    = (librelinkup.status().timestamp_status == SENSOR_TIMECODE_VALID);

    
    // Warm-up (Libre): sensor starting -> remaining warmup minutes (based on sensor_non_activ_unixtime)
    const int sensor_state = (int)librelinkup.status().sensor_state;
    int warmup_min = 0;
    int warmup_sec = 0;
    bool warmup_active = false;
    if (sensor_state == SENSOR_STARTING) {
        // Your project already uses this helper; keep behavior consistent with device UI
        // App-like rounding: compute remaining seconds and use ceil(minutes)
        time_t now = time(NULL);
        time_t end = (time_t)librelinkup.sensor_data().sensor_non_activ_unixtime + (60 * 60);
        long rem_sec = (long)(end - now);
        if (rem_sec < 0) rem_sec = 0;
        warmup_sec = (int)rem_sec;
        warmup_min = (int)((rem_sec + 59) / 60); // ceil to match app
if (warmup_min < 0) warmup_min = 0;
        // Best-effort seconds (minute resolution)        warmup_active = (warmup_min > 0);
    }
const int delta = (int)glucose_delta;
    const char* trend = librelinkup.glucose_data().str_trendArrow.c_str();

    // Remaining lifetime from activation_time + sensor_runtime (14/15 days)
    const uint32_t activation = (uint32_t)librelinkup.sensor_data().sensor_activation_time;
    uint32_t lifetime_s = (uint32_t)librelinkup.sensor_data().sensor_runtime;
    if (lifetime_s == 0) lifetime_s = 15UL * 24UL * 3600UL; // safe fallback
    const uint32_t now = (uint32_t)time(nullptr);

    uint32_t remaining = 0;
    if (sensor_state == SENSOR_EXPIRED) {
        remaining = 0;
    } else if (activation > 0 && now > activation) {

        const uint32_t end = activation + lifetime_s;
        if (end > now) remaining = end - now;
    }

    const int life_days    = (int)(remaining / 86400UL);
    const int life_hours   = (int)((remaining % 86400UL) / 3600UL);
    const int life_minutes = (int)((remaining % 3600UL) / 60UL);
    const int life_seconds = (int)(remaining % 60UL);

    String out;
    out.reserve(256);
    out += "{";
    out += "\"mgdl\":"; out += mgdl;
    out += ",\"delta\":"; out += delta;
    out += ",\"trend\":\""; out += (trend ? trend : ""); out += "\"";
    out += ",\"low\":"; out += low;
    out += ",\"high\":"; out += high;
    out += ",\"ts_ok\":"; out += (ts_ok ? "true" : "false");
    out += ",\"life_days\":"; out += life_days;
    out += ",\"life_hours\":"; out += life_hours;
    out += ",\"life_minutes\":"; out += life_minutes;
    out += ",\"life_seconds\":"; out += life_seconds;
    out += ",\"life_runtime_days\":"; out += (int)(lifetime_s / 86400UL);
    
    out += ",\"sensor_state\":"; out += sensor_state;
    out += ",\"warmup_active\":"; out += (warmup_active ? "true" : "false");
    out += ",\"warmup_min\":"; out += warmup_min;
    out += ",\"warmup_sec\":"; out += warmup_sec;
out += "}";
    return out;
}

String web_get_glucose_history_json() {
    const uint16_t N = (uint16_t)librelinkup.GRAPHDATAARRAYSIZE;
    const uint16_t low  = (uint16_t)librelinkup.glucose_data().glucosetargetLow;
    const uint16_t high = (uint16_t)librelinkup.glucose_data().glucosetargetHigh;

    String out;
    out.reserve(120 + N * 26);

    out += "{";
    out += "\"low\":"; out += low;
    out += ",\"high\":"; out += high;
    out += ",\"values\":[";

    for (uint16_t i = 0; i < N; i++) {
        const uint16_t v = (uint16_t)librelinkup.sensor_history_data().graph_data[i];

        // Prefer local timestamp[i] if present
        uint32_t ts = 0;
        // Some builds may name it timestamp (as user said). Fallback to factory_timestamp.
        ts = (uint32_t)librelinkup.sensor_history_data().timestamp[i];
        if (ts == 0) ts = (uint32_t)librelinkup.sensor_history_data().factory_timestamp[i];

        if (v == 0 || ts == 0) {
            out += "null";
        } else {
            out += "{\"v\":";
            out += v;
            out += ",\"ts\":";
            out += ts;
            out += "}";
        }
        if (i + 1 < N) out += ",";
    }

    out += "]}";
    return out;
}
