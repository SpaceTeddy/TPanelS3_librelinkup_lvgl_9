// webpage.h  (1:1 DROP-IN)
// ------------------------------------------------------------
// Drop-in replacement for your existing webpage.h
// Change: SSID override UI:
// - Keep dropdown (name="networks")
// - Add text field "wifiSsidManual" (without name)
// - On submit: if the text field is filled, set "networks" to that value
//   (without backend changes).
// ------------------------------------------------------------

#ifndef webpage_H
#define webpage_H

#pragma once
#include <ESPAsyncWebServer.h>

/**
 * Registers all HTTP routes/handlers on the server.
 * Call this ONCE after creating the AsyncWebServer (e.g. in setup_OTA(true)).
 */
void register_webpage_routes(AsyncWebServer& server);

#endif // webpage_H
