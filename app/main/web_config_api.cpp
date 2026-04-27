// web_config_api.cpp (DROP-IN)
// ------------------------------------------------------------
// Provides JSON for /api/config to prefill the configuration page.
// Uses: extern SETTINGS settings;
// NOTE: This endpoint should be protected (e.g. same BasicAuth as /configuration).
// ------------------------------------------------------------

#include <Arduino.h>
#include "settings.h"   // <-- adjust include if your SETTINGS type is declared elsewhere

extern SETTINGS settings;

static String json_escape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\"':  out += "\\\\\""; break;
      case '\n':  out += "\\\\n"; break;
      case '\r':  out += "\\\\r"; break;
      case '\t':  out += "\\\\t"; break;
      default:
        // Control chars
        if ((unsigned char)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

String web_get_config_json() {
  // Build JSON manually to avoid bringing in heavy JSON libs
  String out;
  out.reserve(1800);
  out += "{";

  // LLU login (used for /login form)
  out += "\"login_email\":\"" + json_escape(settings.config.login_email) + "\",";
  out += "\"login_password\":\"" + json_escape(settings.config.login_password) + "\",";

  // WiFi / system
  out += "\"wifi_bssid\":\"" + json_escape(settings.config.wifi_bssid) + "\",";
  out += "\"wifi_password\":\"" + json_escape(settings.config.wifi_password) + "\",";

  // Multi-WiFi networks array
  out += "\"wifi_networks\":[";
  for (size_t i = 0; i < settings.config.wifi_networks.size(); i++) {
    if (i > 0) out += ",";
    out += "{\"ssid\":\"" + json_escape(settings.config.wifi_networks[i].ssid) + "\",";
    out += "\"password\":\"" + json_escape(settings.config.wifi_networks[i].password) + "\"}";
  }
  out += "],";
  out += "\"timezone\":" + String(settings.config.timezone) + ",";
  out += "\"ota_update\":" + String((int)settings.config.ota_update) + ",";
  out += "\"wg_mode\":" + String((int)settings.config.wg_mode) + ",";
  out += "\"mqtt_mode\":" + String((int)settings.config.mqtt_mode) + ",";
  out += "\"mqtt_master_mode\":" + String((int)settings.config.mqtt_master_mode) + ",";
  out += "\"brightness\":" + String((int)settings.config.brightness) + ",";
  out += "\"telnet_port\":" + String((int)settings.config.telnet_port) + ",";

  // MQTT
  out += "\"mqttServer\":\"" + json_escape(settings.config.mqttServer) + "\",";
  out += "\"mqttPort\":" + String((int)settings.config.mqtt_port) + ",";
  out += "\"mqttUsername\":\"" + json_escape(settings.config.mqttUsername) + "\",";
  out += "\"mqttPassword\":\"" + json_escape(settings.config.mqttPassword) + "\",";

  // WireGuard
  out += "\"wgPrivateKey\":\"" + json_escape(settings.config.wgPrivateKey) + "\",";
  out += "\"wgPublicKey\":\"" + json_escape(settings.config.wgPublicKey) + "\",";
  out += "\"wgPresharedKey\":\"" + json_escape(settings.config.wgPresharedKey) + "\",";
  out += "\"wgIpAddress\":\"" + json_escape(settings.config.wgIpAddress) + "\",";
  out += "\"wgEndpoint\":\"" + json_escape(settings.config.wgEndpoint) + "\",";
  out += "\"wgEndpointPort\":" + String((unsigned long)settings.config.wgEndpointPort) + ",";
  out += "\"wgAllowedIPs\":\"" + json_escape(settings.config.wgAllowedIPs) + "\",";

  out += "\"sleep_timer\":" + String((unsigned long long)settings.config.sleep_timer) + ",";
  out += "\"ha_discovery\":" + String((int)settings.config.ha_discovery) + ",";
  out += "\"ota_staging\":" + String((int)settings.config.ota_staging) + ",";
  out += "\"ota_force\":" + String((int)settings.config.ota_force);

  out += "}";
  return out;
}
