// web_glucose_api.cpp  (robust, drop-in)
// ------------------------------------------------------------
// Liefert Glukose-Daten für das Web-Dashboard (/)
// Voraussetzungen laut User:
//   - Header: librelinkup.h
//   - Globales Objekt: librelinkup
// ------------------------------------------------------------

#include <Arduino.h>
#include "librelinkup.h"

extern LIBRELINKUP librelinkup;
extern int glucose_delta;

// Konfiguration:
// Wenn keine History vorhanden ist: Fallback-Verhalten.
// - true = wiederhole den letzten bekannten Messwert N-mal (zeichnet eine gerade Linie)
// - false = sende N mal null (zeichnet Lücken)
static const bool FALLBACK_REPEAT_LAST = true;

String web_get_glucose_latest_json() {
    const uint16_t mgdl = (uint16_t)librelinkup.llu_glucose_data.glucoseMeasurement;
    const uint16_t low  = (uint16_t)librelinkup.llu_glucose_data.glucosetargetLow;
    const uint16_t high = (uint16_t)librelinkup.llu_glucose_data.glucosetargetHigh;
    const bool ts_ok    = (librelinkup.llu_status.timestamp_status == SENSOR_TIMECODE_VALID);
const int delta = (int)glucose_delta;  // du sagst: global im System gespeichert
    const char* trend = librelinkup.llu_glucose_data.str_trendArrow.c_str(); // z.B. "↑" o.ä.

    String out;
    out.reserve(220);
    out += "{";
    out += "\"src\":\"web_glucose_api\",";
    out += "\"mgdl\":";
    out += String(mgdl);
    out += ",\"delta\":";
    out += String(delta);
    out += ",\"trend\":";
    out += "\"";
    // Achtung: trend ist ein String. Falls er jemals Anführungszeichen enthalten könnte, müssten wir escapen.
    out += (trend ? trend : "");
    out += "\"";
    out += ",\"low\":";
    out += String(low);
    out += ",\"high\":";
    out += String(high);
    out += ",\"ts_ok\":";
    out += (ts_ok ? "true" : "false");
    out += "}";
    return out;
}

String web_get_glucose_history_json() {
    // GRAPHDATAARRAYSIZE kommt aus deinem librelinkup-Objekt
    const uint16_t N = (uint16_t)librelinkup.GRAPHDATAARRAYSIZE;

    // Dein LVGL-Code schreibt einen "last point" auf index GRAPHDATAARRAYSIZE.
    // Das ist nur sicher, wenn graph_data tatsächlich N+1 Elemente hat.
    // Wir versuchen zuerst, valide Punkte via check_graphdata() zu lesen.
    uint16_t data_count = 0;
    bool have_check = true;
    // Manche Builds bringen check_graphdata als Methode, prüfen ob verfügbar:
    // Wir gehen davon aus, dass check_graphdata() existiert (wie in deinem LVGL-Beispiel).
    data_count = librelinkup.check_graphdata();

    // Prepare JSON as String (RAM-schonend)
    String out;
    out.reserve(64 + (N + 1) * 6);

    out += "{";
    out += "\"src\":\"web_glucose_api\",";
    out += "\"low\":";
    out += String((uint16_t)librelinkup.llu_glucose_data.glucosetargetLow);
    out += ",\"high\":";
    out += String((uint16_t)librelinkup.llu_glucose_data.glucosetargetHigh);
    out += ",\"n\":";
    out += String((unsigned)N);
    out += ",\"values\":[";

    // If data_count == 0 -> no valid historic points found
    if (data_count == 0) {
        // Fallback: either repeat last measurement or produce nulls
        uint16_t last = (uint16_t)librelinkup.llu_glucose_data.glucoseMeasurement;
        for (uint16_t i = 0; i < N; i++) {
            if (FALLBACK_REPEAT_LAST) {
                // if last==0 (also not available) output null
                if (last == 0) out += "null";
                else out += String(last);
            } else {
                out += "null";
            }
            out += (i + 1 < N ? "," : "");
        }
    } else {
        // We will transmit the last N points in the graph_data array as you store them.
        // Behavior mirrors your LVGL drawing: indices 0..N-1 correspond to older->newer.
        // Ensure we don't read out-of-bounds: assume graph_data has at least N elements.
        for (uint16_t i = 0; i < N; i++) {
            uint16_t v = (uint16_t)librelinkup.llu_sensor_history_data.graph_data[i];
            if (v == 0) out += "null";
            else out += String(v);
            out += (i + 1 < N ? "," : "");
        }
    }

    out += "]}";
    return out;
}