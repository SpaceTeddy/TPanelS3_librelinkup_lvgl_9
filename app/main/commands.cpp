/**
 * @file commands.cpp
 * @brief Implementation of console/telnet commands and command handlers.
 *
 * This module registers interactive commands for the UUID console shell and provides
 * the corresponding handler functions. Commands may:
 * - print status information
 * - change runtime settings (and persist them)
 * - trigger network actions (WiFi/MQTT/WireGuard/LibreLinkUp)
 * - access LittleFS JSON files (glucose data)
 *
 * @ingroup console_commands
 */

#include "main.h"
#include "commands.h"
#include "settings.h"
#include <librelinkup.h>
#include "mqtt.h"
#include "hba1c.h"
#include "http_update.h"
#include "h2_ota.h"
#include "ui.h"
#include "ui_display.h"
#include <LittleFS.h>
#include <lvgl.h>
#include "tpanels3.h"

#include <cstdarg> // for va_list, va_start, va_end
#include <cstdio>  // for vsnprintf
#include <cstdint> // for uintptr_t
#include <cstdlib> // for strtol

extern SETTINGS settings;
extern LIBRELINKUP librelinkup;
extern TPanelS3 tpanels3;
extern WiFiClient mqttClient;
extern PubSubClient mqtt_client;
extern MQTT mqtt;
extern HBA1C hba1c;
extern HELPER helper;
extern uint16_t telnet_port;
extern HardwareSerial SerialPort;

extern int  app_check_internet_status(IPAddress ip, uint16_t port);
extern void start_ap_mode();
extern void setup_wifi();
extern bool g_force_ap_mode;

//------------------------[uuid logger]-----------------------------------
/** @brief Module logger instance. */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

// One-shot helper task for `h2 pair <seconds>`.
static volatile bool g_h2_pair_task_active = false;

static void h2_pair_task(void* arg) {
    uint32_t seconds = (uint32_t)(uintptr_t)arg;
    if (seconds == 0) seconds = 120;

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permit\",\"seconds\":%u}", (unsigned)seconds);
    SerialPort.print(cmd);
    SerialPort.print('\n');
    logger.notice("[H2] pair helper: sent permit for %u s", (unsigned)seconds);

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000UL));

    SerialPort.print("{\"cmd\":\"list\"}\n");
    logger.notice("[H2] pair helper: permit window ended -> requested list");

    g_h2_pair_task_active = false;
    vTaskDelete(nullptr);
}

/**
 * @brief Print a welcome banner on shell connect.
 * @param shell Active shell instance.
 *
 * Intended to be called when a user opens a telnet/console session.
 */
void displayWelcomeBanner(uuid::console::Shell &shell) {
    shell.println(F("==========================================="));
    shell.println(F(" Willkommen zur ESP32 Telnet Konsole       "));
    shell.println(F(" UUID-Konsole v1.0                         "));
    shell.println(F(" Geben Sie 'help' ein, um Befehle zu sehen "));
    shell.println(F("==========================================="));
}

/**
 * @brief Parse an integer from command arguments.
 * @param arguments Argument list (argv-like).
 * @param index Argument index to parse.
 * @param defaultValue Default returned if missing/invalid.
 * @return Parsed integer or defaultValue.
 *
 * @note Uses std::stoi; invalid numeric strings may throw. If you want it fully
 *       exception-safe on embedded, wrap stoi in try/catch.
 */
int parseArgument(const std::vector<std::string> &arguments, size_t index, int defaultValue /*= 0*/) {
    if (index >= arguments.size()) {
        return defaultValue;
    }

    const char *raw = arguments[index].c_str();
    char *endptr = nullptr;
    long value = strtol(raw, &endptr, 10);

    if (endptr == raw || *endptr != '\0') {
        return defaultValue;
    }

    return static_cast<int>(value);
}

/**
 * @brief Handler for command: `help`
 * @param shell Shell to write output to.
 */
void helpCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Available commands:"));
    shell.print_all_available_commands();
}

/**
 * @brief Handler for command: `exit`
 * @param shell Shell to stop.
 *
 * Stops the interactive shell session.
 */
void exitCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("exit shell"));
    shell.stop();
}

/**
 * @brief Handler for command: `log_level <OFF|INFO|NOTICE|DEBUG|ALL>`
 * @param shell Shell instance (log level is stored per shell).
 * @param arguments First argument is the desired log level (lowercase expected by parser).
 */
void LoglevelCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        uuid::log::Level level;
        if (uuid::log::parse_level_lowercase(arguments[0], level)) {
            shell.log_level(level);
        } else {
            shell.printfln(F("invalid_log_level"));
            return;
        }
    }
    shell.printfln("Set Loglevel to: %s", uuid::log::format_level_uppercase(shell.log_level()));
}

/**
 * @brief Handler for command: `llu_login_data <email> <password>`
 * @param shell Shell output.
 * @param arguments arguments[0]=email, arguments[1]=password
 *
 * Persists LibreLinkUp login credentials to the configuration file.
 */
void LLULoginDataCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.size() < 2) {
        shell.printfln(F("Usage: llu_login_data <email> <password>"));
        return;
    }

    String login_name = arguments[0].c_str();
    settings.config.login_email = login_name;

    String login_password = arguments[1].c_str();
    settings.config.login_password = login_password;
    librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);

    settings.saveConfiguration(settings.config_filename, settings.config);
    shell.printfln("LoginName: %s", login_name.c_str());
    shell.println(F("LoginPassword: <hidden>"));
}

/**
 * @brief Handler for command: `wifi_settings <bssid> <password>`
 * @param shell Shell output.
 * @param arguments arguments[0]=bssid, arguments[1]=password
 *
 * Saves WiFi settings and triggers WiFi setup.
 */
void WiFiSettingCommand(uuid::console::Shell &shell,
                        const std::vector<std::string> &arguments)
{
    // Mindestens SSID/BSSID muss da sein
    if (arguments.size() < 1) {
        shell.printfln("Usage: wifi_settings <ssid_or_bssid> [password]");
        return;
    }

    String wifi_bssid = arguments[0].c_str();
    shell.printfln("WiFi BSSID/SSID: %s", wifi_bssid.c_str());
    settings.config.wifi_bssid = wifi_bssid;

    // Passwort ist optional
    if (arguments.size() >= 2) {
        String wifi_password = arguments[1].c_str();
        shell.printfln("WiFi Password: %s", wifi_password.c_str());
        settings.config.wifi_password = wifi_password;
    } else if(arguments.size() == 1){
        // Offenes WLAN: Passwort explizit leer setzen
        settings.config.wifi_password = "";
        shell.printfln("WiFi Password: <empty> (open network)");
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    setup_wifi();
}

/**
 * @brief Handler for command: `wifi <ap|connect>`
 * @param shell Shell output.
 * @param arguments arguments[0]=ap|connect
 *
 * `wifi ap`      — immediately start AP mode (192.168.4.1).
 * `wifi connect` — clear g_force_ap_mode and reconnect to saved networks.
 */
void wifiModeCommand(uuid::console::Shell &shell,
                     const std::vector<std::string> &arguments)
{
    if (arguments.empty()) {
        shell.printfln(F("Usage: wifi <ap|connect>"));
        return;
    }
    String sub = arguments[0].c_str();
    if (sub == "ap") {
        shell.printfln(F("Switching to AP mode (192.168.4.1)..."));
        start_ap_mode();
    } else if (sub == "connect") {
        shell.printfln(F("Clearing AP mode flag, reconnecting..."));
        g_force_ap_mode = false;
        setup_wifi();
    } else {
        shell.printfln(F("Usage: wifi <ap|connect>"));
    }
}

/**
 * @brief Handler for command: `reboot`
 * @param shell Shell output.
 *
 * Restarts the ESP32.
 */
void espResetCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("ESP Reboot!"));
    ESP.restart();
}

/**
 * @brief Handler for command: `esp_status`
 * @param shell Shell output.
 *
 * Prints basic device status, including free heap.
 */
void espStatusCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    esp_status();
    shell.printfln(F("ESP status: WiFi connected, free heap: %d"), ESP.getFreeHeap());
}

/**
 * @brief Handler for command: `screens <next|prev>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "next" or "prev"
 *
 * Switches LVGL screens in a small cyclic order:
 * Main -> Debug -> Login -> Main (next) and reverse for prev.
 */
void switch_screensCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String screen_argument = arguments[0].c_str(); // next or previous

        if (screen_argument == "next") {
            if (lv_scr_act() == ui_Main_screen) {
                lv_disp_load_scr(ui_Debug_screen);
            } else if (lv_scr_act() == ui_Debug_screen) {
                lv_disp_load_scr(ui_Login_screen);
            } else if (lv_scr_act() == ui_Login_screen) {
                lv_disp_load_scr(ui_Main_screen);
            }
        } else if ((screen_argument == "prev")) {
            if (lv_scr_act() == ui_Main_screen) {
                lv_disp_load_scr(ui_Login_screen);
            } else if (lv_scr_act() == ui_Login_screen) {
                lv_disp_load_scr(ui_Debug_screen);
            } else if (lv_scr_act() == ui_Debug_screen) {
                lv_disp_load_scr(ui_Main_screen);
            }
        } else {
            shell.printfln("invalid argument: %s", screen_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `config <load|save>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "load" or "save"
 *
 * Loads or saves the current configuration from/to LittleFS and prints the config.
 */
void configSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String config_argument = arguments[0].c_str(); // load or save

        if (config_argument == "load") {
            settings.loadConfiguration(settings.config_filename, settings.config);
            shell.println(F("Configuration loaded."));
            shell.printfln(
                "load:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, mqtt_master_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
                settings.config.login_email.c_str(),
                settings.config.login_password.c_str(),
                settings.config.wifi_bssid.c_str(),
                settings.config.wifi_password.c_str(),
                settings.config.timezone,
                settings.config.ota_update,
                settings.config.wg_mode,
                settings.config.mqtt_mode,
                settings.config.mqtt_master_mode,
                settings.config.brightness,
                settings.config.telnet_port,
                settings.config.mqttServer.c_str(),
                settings.config.mqtt_port,
                settings.config.mqttUsername.c_str(),
                settings.config.mqttPassword.c_str(),
                settings.config.wgPrivateKey.c_str(),
                settings.config.wgPublicKey.c_str(),
                settings.config.wgPresharedKey.c_str(),
                settings.config.wgIpAddress.c_str(),
                settings.config.wgEndpoint.c_str(),
                settings.config.wgEndpointPort,
                settings.config.wgAllowedIPs.c_str(),
                settings.config.sleep_timer
            );
        } else if ((config_argument == "save")) {
            settings.saveConfiguration(settings.config_filename, settings.config);
            shell.println(F("Configuration saved."));
            shell.printfln(
                "save:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, mqtt_master_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
                settings.config.login_email.c_str(),
                settings.config.login_password.c_str(),
                settings.config.wifi_bssid.c_str(),
                settings.config.wifi_password.c_str(),
                settings.config.timezone,
                settings.config.ota_update,
                settings.config.wg_mode,
                settings.config.mqtt_mode,
                settings.config.mqtt_master_mode,
                settings.config.brightness,
                settings.config.telnet_port,
                settings.config.mqttServer.c_str(),
                settings.config.mqtt_port,
                settings.config.mqttUsername.c_str(),
                settings.config.mqttPassword.c_str(),
                settings.config.wgPrivateKey.c_str(),
                settings.config.wgPublicKey.c_str(),
                settings.config.wgPresharedKey.c_str(),
                settings.config.wgIpAddress.c_str(),
                settings.config.wgEndpoint.c_str(),
                settings.config.wgEndpointPort,
                settings.config.wgAllowedIPs.c_str(),
                settings.config.sleep_timer
            );
        } else {
            shell.printfln("invalid argument: %s", config_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `timezone <+/-hours>`
 * @param shell Shell output.
 * @param arguments arguments[0] = timezone offset (hours)
 *
 * Updates timezone in settings and LibreLinkUp, then persists configuration.
 */
void timezoneCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    int timezone = parseArgument(arguments, 0, settings.config.timezone);
    settings.config.timezone = timezone;
    librelinkup.timezone_offset() = timezone;
    settings.saveConfiguration(settings.config_filename, settings.config);
    shell.printfln(F("Timezone set to %d and saved to config"), timezone);
}

/**
 * @brief Handler for command: `ota <enable|disable>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "enable" or "disable"
 *
 * Enables/disables OTA at runtime (and persists setting).
 */
void otaSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String ota_argument = arguments[0].c_str();
        if (ota_argument == "enable") {
            settings.config.ota_update = 1;
            setup_OTA(settings.config.ota_update);
            shell.println(F("OTA enabled."));
        } else if ((ota_argument == "disable")) {
            settings.config.ota_update = 0;
            setup_OTA(settings.config.ota_update);
            shell.println(F("OTA disabled."));
        } else {
            shell.printfln("invalid argument: %s", ota_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `trgb_brightness <0-256>`
 * @param shell Shell output.
 * @param arguments arguments[0] = brightness value
 *
 * Updates the LCD backlight brightness via TPanelS3.
 */
void trgbBrightnessCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    int brightness = parseArgument(arguments, 0, settings.config.brightness);
    settings.config.brightness = brightness;
    tpanels3.set_backlight_brightness(brightness);
    shell.printfln(F("Brightness set to %d"), brightness);
}

/**
 * @brief Handler for command: `list_json_files`
 * @param shell Shell output.
 *
 * Lists all JSON files in LittleFS via HBA1C helper.
 */
void printJsonFileListCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Print Json Filelist..."));

    File root = LittleFS.open("/");
    if (!root) {
        shell.printfln(F("Error: could not open LittleFS root!"));
        return;
    }

    shell.printfln(F("=== All stored JSON files ==="));

    int total_files = 0;
    int json_files = 0;
    File file = root.openNextFile();
    while (file) {
        total_files++;
        String filename = file.name();

        if (filename.endsWith(".json")) {
            json_files++;
            shell.printfln("File: %s | Size: %d bytes", filename.c_str(), file.size());
        }

        if ((total_files % 20) == 0) {
            yield();
        }

        file = root.openNextFile();
    }

    shell.printfln(F("=== Done: %d JSON files (%d total files) ==="), json_files, total_files);
}

/**
 * @brief Handler for command: `print_json_file <filename>`
 * @param shell Shell output.
 * @param arguments arguments[0] = filename
 *
 * Prints the decoded JSON glucose file contents (timestamp + glucose).
 */
void printJsonFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.printfln(F("Usage: print_json_file <filename>"));
        return;
    }

    String filename_argument = arguments[0].c_str();
    filename_argument.trim();
    if (filename_argument.length() == 0) {
        shell.printfln(F("Usage: print_json_file <filename>"));
        return;
    }

    // Allow quoted input: print_json_file "2026-03-10.json"
    if (filename_argument.startsWith("\"") && filename_argument.endsWith("\"") && filename_argument.length() >= 2) {
        filename_argument.remove(0, 1);
        filename_argument.remove(filename_argument.length() - 1, 1);
    }

    String path = filename_argument;
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    shell.printfln(F("Filename: %s"), filename_argument.c_str());

    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        // Fallback: try the raw argument variant as well.
        file = LittleFS.open(filename_argument.c_str(), "r");
    }
    if (!file) {
        shell.printfln(F("Error: file %s not found!"), filename_argument.c_str());
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        shell.printfln(F("Error reading %s: %s"), filename_argument.c_str(), error.c_str());
        return;
    }

    if (!doc.is<JsonArray>()) {
        shell.printfln(F("Error: JSON file %s is not an array!"), filename_argument.c_str());
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    shell.printfln(F("=== Glucose values from %s (entries: %d) ==="), filename_argument.c_str(), arr.size());

    int idx = 0;
    for (JsonObject obj : arr) {
        time_t timestamp = obj["timestamp"].as<time_t>();
        uint16_t glucose = obj["glucose"].as<uint16_t>();

        struct tm *timeinfo = localtime(&timestamp);
        char timeString[20];
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeinfo);

        shell.printfln("Time: %s | Glucose: %d mg/dL", timeString, glucose);
        idx++;

        // Keep the system responsive while printing long files over telnet.
        if ((idx % 10) == 0) {
            yield();
        }
    }

    shell.printfln(F("Debug: JSON output completed (%d entries)."), idx);
}

/**
 * @brief Handler for command: `create_json_week_files`
 * @param shell Shell output.
 *
 * Creates 7 test files with random glucose values (for development/testing).
 */
void create_json_week_files_Command(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Create Json data test files"));
    hba1c.createTestJsonFiles();
}

/**
 * @brief Handler for command: `add_glucosevalue_to_json`
 * @param shell Shell output.
 *
 * Adds the current LibreLinkUp glucose measurement to today's JSON file.
 */
void addGlucoseValueToJsonCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Add glucose value to json file..."));
    uint32_t unixtime_now = librelinkup.get_epoch_time();
    hba1c.addGlucoseValue(unixtime_now, librelinkup.glucose_data().glucoseMeasurement);
}

/**
 * @brief Handler for command: `delete_json_file <filename>`
 * @param shell Shell output.
 * @param arguments arguments[0] = filename
 *
 * Deletes the given JSON file from LittleFS.
 */
void deleteJsonFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.printfln(F("Usage: delete_json_file <filename>"));
        return;
    }

    String filename_argument = arguments[0].c_str();
    filename_argument.trim();
    if (filename_argument.startsWith("\"") && filename_argument.endsWith("\"") && filename_argument.length() >= 2) {
        filename_argument.remove(0, 1);
        filename_argument.remove(filename_argument.length() - 1, 1);
    }

    String path = filename_argument;
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    shell.printfln(F("Filename: %s"), filename_argument.c_str());
    if (!hba1c.deleteJsonFile(path.c_str())) {
        // Fallback for unexpected FS naming behavior
        hba1c.deleteJsonFile(filename_argument.c_str());
    }
}

/**
 * @brief Handler for command: `print_raw_json_file <filename>`
 * @param shell Shell output.
 * @param arguments arguments[0] = filename
 *
 * Prints the raw file contents (no JSON parsing).
 */
void debugRawFileContentsCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.printfln(F("Usage: print_raw_json_file <filename>"));
        return;
    }

    String filename_argument = arguments[0].c_str();
    filename_argument.trim();
    if (filename_argument.startsWith("\"") && filename_argument.endsWith("\"") && filename_argument.length() >= 2) {
        filename_argument.remove(0, 1);
        filename_argument.remove(filename_argument.length() - 1, 1);
    }

    String path = filename_argument;
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    shell.printfln(F("Filename: %s"), filename_argument.c_str());
    if (LittleFS.exists(path.c_str())) {
        hba1c.debugRawFileContents(path.c_str());
    } else {
        hba1c.debugRawFileContents(filename_argument.c_str());
    }
}

/**
 * @brief Handler for command: `llu <subcommand>`
 * @param shell Shell output.
 * @param arguments First argument selects a LibreLinkUp action (value/user_id/auth/...).
 *
 * Subcommands currently supported:
 * - value, user_id, user_token, auth, tou
 * - sensor_id, sensor_sn, sensor_type, sensor_expiry
 * - timestamp, history, graphdata, graph_redraw, get_graphdata, statistics
 */
void lluCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String llu_argument = arguments[0].c_str();

        if (llu_argument == "value") {
            shell.printfln("glucoseMeasurement: %d %s ∆: %d",
                librelinkup.glucose_data().glucoseMeasurement,
                librelinkup.glucose_data().str_trendArrow.c_str(),
                glucose_delta
            );
        }
        else if ((llu_argument == "user_id")) {
            shell.printfln(F("LLU User_ID: %s"), librelinkup.login_data().user_id.c_str());
        }
        else if ((llu_argument == "user_token")) {
            String token_part1;
            String token_part2;

            token_part1 = librelinkup.login_data().user_token.substring(0, 220);
            token_part2 = librelinkup.login_data().user_token.substring(
                220, librelinkup.login_data().user_token.length()
            );

            shell.printf("LLU User_Token: %s", token_part1.c_str());
            shell.printfln("%s", token_part2.c_str());
        }
        else if ((llu_argument == "auth")) {
            shell.println(F("LLU Auth..."));
            librelinkup.auth_user(settings.config.login_email, settings.config.login_password);
            shell.printfln("LLU User_ID: %s", librelinkup.login_data().user_id.c_str());

            String token_part1;
            String token_part2;

            token_part1 = librelinkup.login_data().user_token.substring(0, 220);
            token_part2 = librelinkup.login_data().user_token.substring(
                220, librelinkup.login_data().user_token.length()
            );

            shell.printf("LLU User_Token: %s", token_part1.c_str());
            shell.printfln("%s", token_part2.c_str());
        }
        else if ((llu_argument == "tou")) {
            shell.println(F("LLU Tou..."));
            librelinkup.tou_user();
        }
        else if ((llu_argument == "sensor_id")) {
            shell.printfln(F("LLU Sensor_ID: %s"), librelinkup.sensor_data().sensor_id.c_str());
        }
        else if ((llu_argument == "sensor_sn")) {
            shell.printfln(F("LLU Sensor_SN: %s"), librelinkup.sensor_data().sensor_sn.c_str());
        }
        else if ((llu_argument == "sensor_type")) {
            String sensor_type_str = "";
            if (librelinkup.sensor_data().sensor_runtime == 14 * 86400) {
                sensor_type_str = "FreeStyle Libre 3";
            } else if (librelinkup.sensor_data().sensor_runtime == 15 * 86400) {
                sensor_type_str = "FreeStyle Libre 3 Plus";
            } else {
                sensor_type_str = "Unknown";
            }
            shell.printfln(F("LLU Sensor Type: %s"), sensor_type_str.c_str());
        }
        else if ((llu_argument == "sensor_expiry")) {
            shell.printfln("LLU Sensor Expiry: %dDays %dHours %dMinutes",
                librelinkup.sensor_lifetime().sensor_valid_days,
                librelinkup.sensor_lifetime().sensor_valid_hours,
                librelinkup.sensor_lifetime().sensor_valid_minutes
            );
        }
        else if ((llu_argument == "timestamp")) {
            shell.printfln(F("LLU Timestamp: %s"), librelinkup.glucose_data().str_measurement_timestamp.c_str());
        }
        else if ((llu_argument == "history")) {
            static char time_in_hours[librelinkup.GRAPHDATAARRAYSIZE][6]; // "HH:MM" + null terminator
            static char factory_time_in_hours[librelinkup.GRAPHDATAARRAYSIZE][6]; // "HH:MM" + null terminator
            shell.printfln(F("LLU History...:"));
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln("Historical data: [%d/%d]", data_count, librelinkup.GRAPHDATAARRAYSIZE);
            shell.printfln("%-9s | %-7s | %-13s | %-13s | %-17s | %-19s",
                            "ArrayPos",
                            "Value",
                            "TimeStamp",
                            "Time(HH:MM)",
                            "FactoryTimeStamp",
                            "FactoryTime(HH:MM)");
            shell.printfln("%-9s | %-7s | %-13s | %-13s | %-17s | %-19s",
                            "--------",
                            "-----",
                            "---------",
                            "-----------",
                            "----------------",
                            "------------------");
            for (uint8_t i = 0; i < librelinkup.GRAPHDATAARRAYSIZE; i++) {
                helper.format_time(time_in_hours[i], sizeof(time_in_hours[i]), librelinkup.sensor_history_data().timestamp[i]);
                helper.format_time(factory_time_in_hours[i], sizeof(factory_time_in_hours[i]), librelinkup.sensor_history_data().factory_timestamp[i]);
                shell.printfln("%-9u | %-7u | %-13lu | %-13s | %-17lu | %-19s",
                                i,
                                librelinkup.sensor_history_data().graph_data[i],
                                librelinkup.sensor_history_data().timestamp[i],
                                time_in_hours[i],
                                librelinkup.sensor_history_data().factory_timestamp[i],
                                factory_time_in_hours[i]);
            }
        }
        else if ((llu_argument == "graphdata")) {
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln(F("Historical data: [%d/%d]"), data_count, librelinkup.GRAPHDATAARRAYSIZE);
            const int last_index = librelinkup.GRAPHDATAARRAYSIZE - 1;
            shell.printfln("Last_LCD_Position: %03d Value: %03d",
                last_index,
                librelinkup.sensor_history_data().graph_data[last_index]
            );

            int index = librelinkup.GRAPHDATAARRAYSIZE - 1;

            for (int i = data_count - 1; i >= 0; i--) {
                shell.printfln("Position: %03d Value: %03d TimeStamp: %d",
                    index,
                    librelinkup.sensor_history_data().graph_data[i],
                    librelinkup.sensor_history_data().timestamp[i]
                );
                index--;
            }
        }
        else if ((llu_argument == "graph_redraw")) {
            shell.println(F("LLU Graph redraw..."));
            uint8_t data_count = librelinkup.check_graphdata();
            if (data_count == 0) {
                shell.println(F("No graph data available yet. Run `llu get_graphdata` first."));
                return;
            }
            draw_chart_glucose_data(3);
        }
        else if ((llu_argument == "get_graphdata")) {
            shell.println(F("LLU Get GraphData..."));
            librelinkup.get_graph_data();
            shell.printfln("SensorSN_non_activated: %s", librelinkup.sensor_data().sensor_sn_non_active.c_str());
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln("glucoseMeasurement: %d %s",
                librelinkup.glucose_data().glucoseMeasurement,
                librelinkup.glucose_data().str_trendArrow.c_str()
            );
            shell.printfln("Historical data: [%d/%d]", data_count, librelinkup.GRAPHDATAARRAYSIZE);
        }
        else if ((llu_argument == "statistics")) {
            shell.println(F("LLU print glucose statistics..."));
            //glucose_statistics();
            uint8_t data_count = librelinkup.check_graphdata();

            float mean_glucose_value_from_history = hba1c.calculateGlucoseMeanFromHistory(
                librelinkup.sensor_history_data().graph_data, data_count);
            float mean_glucose_value_from_json = hba1c.calculateGlucoseMeanFromJson(
                today_json_filename);
            float mean_glucose_weekly_value_from_json = hba1c.calculateGlucoseMeanForLast7Days();
            float std_dev = hba1c.calculate_standard_deviation(
                librelinkup.sensor_history_data().graph_data, data_count,
                mean_glucose_value_from_history);

            shell.println("========== Glucose Statistics =============");
            shell.printfln("Current glucose value        : %d mg/dl",
                        librelinkup.glucose_data().glucoseMeasurement);
            shell.printfln("Mean of history glucose value: %.0f mg/dl",
                        mean_glucose_value_from_history);
            shell.printfln("Mean of daily JSON value     : %.0f mg/dl",
                        mean_glucose_value_from_json);
            shell.printfln("Mean of weekly glucose value : %.0f mg/dl",
                        mean_glucose_weekly_value_from_json);
            shell.printfln("HbA1c-Value of history data  : %.2f %%",
                        hba1c.calculate_hba1c(mean_glucose_value_from_history));
            shell.printfln("TIR-Value of history data    : %.2f %%",
                        hba1c.calculate_time_in_range(
                            librelinkup.sensor_history_data().graph_data,
                            data_count, 70, 180));
            shell.printfln("Std-Dev of history data      : %.2f σ", std_dev);
            shell.printfln("Glucose variability (CV)     : %.2f %%",
                        hba1c.calculate_coefficient_of_variation(std_dev,
                                                                mean_glucose_value_from_history));
            shell.println("===========================================");
        }
        else {
            shell.printfln("invalid argument: %s", llu_argument.c_str());
        }
    } else {
        shell.println(F("command: llu <> <>"));
    }
}

/**
 * @brief Handler for command: `llu_sensor_type <Libre3|Libre3Plus>`
 * @param shell Shell output.
 * @param arguments arguments[0] = sensor type string.
 *
 * Updates sensor runtime and UI progress bar accordingly.
 */
void lluSensorTypeCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.printfln(F("Usage: llu_sensor_type <Libre3|Libre3Plus>"));
        return;
    }

    String sensor_type = arguments[0].c_str();

    if (sensor_type == "Libre3") {
        librelinkup.sensor_data().sensor_runtime = 14 * 86400; // 14 days
        switch_sensor_valid_progress_bar(&dayBar14);
    } else if ((sensor_type == "Libre3Plus")) {
        librelinkup.sensor_data().sensor_runtime = 15 * 86400; // 15 days
        switch_sensor_valid_progress_bar(&dayBar15);
    } else {
        shell.printfln("invalid sensor type: %s", sensor_type.c_str());
        return;
    }

    shell.printfln(F("Sensortype: %s"), sensor_type.c_str());
}

/**
 * @brief Handler for command: `ping`
 * @param shell Shell output.
 *
 * Performs an ICMP ping to 1.1.1.1 using ESP32Ping.
 */
void PingCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.print(F("TCP connection check..."));
    
    if (app_check_internet_status(IPAddress(192, 168, 0, 202), 1883) == true) {
        shell.println(F("OK"));
    } else {
        shell.println(F("NOK"));
    }
}

/**
 * @brief Handler for command: `mqtt_client <enable|disable>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "enable" or "disable"
 *
 * Enables/disables MQTT client mode. On disable it unsubscribes and disconnects.
 */
void mqttClientSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String mqtt_argument = arguments[0].c_str();
        if (mqtt_argument == "enable") {
            mqtt.mqtt_enable = 1;
            shell.println(F("MQTT client enabled."));
        } else if ((mqtt_argument == "disable")) {
            mqtt.mqtt_enable = 0;
            mqtt_client.unsubscribe((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
            mqtt_client.disconnect();
            shell.println(F("MQTT client disabled."));
        } else {
            shell.printfln("invalid argument: %s", mqtt_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `mqtt_master_mode <enable|disable>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "enable" or "disable"
 *
 * Switches between master/client mode behavior by subscribing/unsubscribing to master topics.
 */
void mqttMasterModeCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String mqtt_argument = arguments[0].c_str();
        if (mqtt_argument == "enable") {
            settings.config.mqtt_master_mode = 1;
            mqtt_client.unsubscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data).c_str());
            shell.println(F("MQTT Master Mode enabled."));
        } else if ((mqtt_argument == "disable")) {
            settings.config.mqtt_master_mode = 0;
            shell.println(F("MQTT Master Mode disabled, activating Client mode..."));
            mqtt_client.unsubscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data).c_str());
            mqtt_client.subscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data).c_str());
        } else {
            shell.printfln("invalid argument: %s", mqtt_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `wireguard <enable|disable>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "enable" or "disable"
 *
 * Enables/disables WireGuard and triggers setup.
 */
void wgSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String wireguard_argument = arguments[0].c_str();
        if (wireguard_argument == "enable") {
            settings.config.wg_mode = 1;
            setup_wg(1);
            shell.println(F("WireGuard enabled."));
        } else if ((wireguard_argument == "disable")) {
            settings.config.wg_mode = 0;
            setup_wg(0);
            shell.println(F("WireGuard disabled."));
        } else {
            shell.printfln("invalid argument: %s", wireguard_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `download_ca_to_file <DigiCert|Baltimore|GoogleTrust>`
 * @param shell Shell output.
 * @param arguments arguments[0] = certificate preset name.
 *
 * Downloads a known root CA certificate and stores it in LittleFS using LibreLinkUp helper.
 */
void downloadRootCaToFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String downloadRootCaToFile_argument = arguments[0].c_str();
        if (downloadRootCaToFile_argument == "DigiCert") {
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_DigiCertGlobalRootG2, librelinkup.path_root_ca_dcgrg2) == 1) {
                shell.println(F("DigiCert Global Root G2 certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading DigiCert Global Root G2 certificate."));
            }
        } else if ((downloadRootCaToFile_argument == "Baltimore")) {
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_BaltimoreCyberTrustRoot, librelinkup.path_root_ca_baltimore) == 1) {
                shell.println(F("Baltimore CyberTrust Root certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading Baltimore CyberTrust Root certificate."));
            }
        } else if ((downloadRootCaToFile_argument == "GoogleTrust")) {
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_GoogleTrustRootR4, librelinkup.path_root_ca_googler4) == 1) {
                shell.println(F("Google Trust Root R4 certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading Google Trust Root R4 certificate."));
            }
        } else {
            shell.printfln("invalid argument: %s", downloadRootCaToFile_argument.c_str());
        }
    } else {
        shell.println(F("command: download_ca_to_file <DigiCert|Baltimore|GoogleTrust>"));
    }
}

/**
 * @brief Handler for command: `download_ca_from_url <https_url> <littlefs_path>`
 * @param shell Shell output.
 * @param arguments arguments[0]=https URL, arguments[1]=LittleFS destination path
 *
 * Downloads a root certificate from an arbitrary URL and stores it on LittleFS.
 */
void downloadRootCaFromURLToFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.size() < 2) {
        shell.println(F("Usage: download_ca_from_url <https_url> <littlefs_path>"));
        return;
    }

    String https_url     = arguments[0].c_str();
    String littlefs_path = arguments[1].c_str();

    if (librelinkup.download_root_ca_to_file(https_url.c_str(), littlefs_path.c_str()) == 1) {
        shell.println(F("Root certificate downloaded successfully."));
    } else {
        shell.println(F("Error downloading Root certificate."));
    }
}

/**
 * @brief Handler for command: `set_ca_from_file <DigiCert|Baltimore|GoogleTrust>`
 * @param shell Shell output.
 * @param arguments arguments[0]=certificate preset name
 *
 * Loads a previously downloaded root CA from LittleFS and applies it to the WiFiSecureClient.
 */
void setCaFromFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String setRootCaFromFile_argument = arguments[0].c_str();
        if (setRootCaFromFile_argument == "DigiCert") {
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_dcgrg2);
            shell.println(F("DigiCert Global Root G2 certificate loaded from LittleFS."));
        } else if ((setRootCaFromFile_argument == "Baltimore")) {
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_baltimore);
            shell.println(F("Baltimore CyberTrust Root certificate loaded from LittleFS."));
        } else if ((setRootCaFromFile_argument == "GoogleTrust")) {
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_googler4);
            shell.println(F("Google Trust Root R4 certificate loaded from LittleFS."));
        } else {
            shell.printfln("invalid argument: %s", setRootCaFromFile_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `show_ca_from_file <DigiCert|Baltimore|GoogleTrust>`
 * @param shell Shell output.
 * @param arguments arguments[0]=certificate preset name
 *
 * Prints a stored CA certificate file (for debugging/verification).
 */
void showCaFromFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String showCaCommand_argument = arguments[0].c_str();
        if (showCaCommand_argument == "DigiCert") {
            librelinkup.showCAfromfile(librelinkup.path_root_ca_dcgrg2);
            shell.println(F("Displayed DigiCert Global Root G2 certificate."));
        } else if ((showCaCommand_argument == "Baltimore")) {
            librelinkup.showCAfromfile(librelinkup.path_root_ca_baltimore);
            shell.println(F("Displayed Baltimore CyberTrust Root certificate."));
        } else if ((showCaCommand_argument == "GoogleTrust")) {
            librelinkup.showCAfromfile(librelinkup.path_root_ca_googler4);
            shell.println(F("Google Trust Root R4 certificate."));
        } else {
            shell.printfln("invalid argument: %s", showCaCommand_argument.c_str());
        }
    }
}

/**
 * @brief Handler for command: `fw_update <check|install|status>`
 * @param shell Shell output.
 * @param arguments arguments[0] = "check", "install", or "status"
 *
 * Subcommands:
 * - check   : Request an immediate manifest check for a newer firmware version.
 * - install : Schedule installation of the available update (fails if none available).
 * - status  : Print the current firmware update status as JSON.
 */
void fwUpdateCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.println(F("Usage: fw_update <check|install|status|channel|force>"));
        return;
    }

    const String arg = arguments[0].c_str();

    if (arg == "check") {
        fw_update_request_check_now();
        shell.printfln("FW check requested. Current status: %s", fw_update_get_status());

    } else if (arg == "install") {
        String message;
        if (fw_update_request_install(message)) {
            shell.printfln("FW install scheduled: %s", message.c_str());
        } else {
            shell.printfln("FW install rejected: %s", message.c_str());
        }

    } else if (arg == "status") {
        shell.println(fw_update_get_status_json().c_str());

    } else if (arg == "channel") {
        if (arguments.size() < 2) {
            shell.printfln("OTA channel: %s", settings.config.ota_staging ? "staging" : "release");
            shell.println(F("Usage: fw_update channel <release|staging>"));
            return;
        }
        const String ch = arguments[1].c_str();
        if (ch == "release") {
            settings.config.ota_staging = 0;
            settings.saveConfiguration(settings.config_filename, settings.config);
            fw_update_request_check_now();
            shell.println(F("OTA channel set to: release"));
        } else if (ch == "staging") {
            settings.config.ota_staging = 1;
            settings.saveConfiguration(settings.config_filename, settings.config);
            fw_update_request_check_now();
            shell.println(F("OTA channel set to: staging"));
        } else {
            shell.printfln("Unknown channel: %s. Use release or staging.", ch.c_str());
        }

    } else if (arg == "force") {
        if (arguments.size() < 2) {
            shell.printfln("OTA force: %s", settings.config.ota_force ? "on" : "off");
            shell.println(F("Usage: fw_update force <on|off>"));
            return;
        }
        const String val = arguments[1].c_str();
        if (val == "on") {
            settings.config.ota_force = 1;
            settings.saveConfiguration(settings.config_filename, settings.config);
            fw_update_request_check_now();
            shell.println(F("OTA force mode: on (any manifest version will be installed)"));
        } else if (val == "off") {
            settings.config.ota_force = 0;
            settings.saveConfiguration(settings.config_filename, settings.config);
            fw_update_request_check_now();
            shell.println(F("OTA force mode: off (only newer versions will be installed)"));
        } else {
            shell.printfln("Unknown value: %s. Use on or off.", val.c_str());
        }

    } else {
        shell.printfln("Unknown subcommand: %s", arg.c_str());
        shell.println(F("Usage: fw_update <check|install|status|channel|force>"));
    }
}

/**
 * @brief Register all console commands.
 * @param commands Command registry to populate.
 *
 * This function wires command strings to handler callbacks.
 */
/**
 * @brief Handler for command: `h2_send <string>`
 * @param shell Shell output.
 * @param arguments arguments[0..] joined with spaces and sent to the ESP32-H2 via UART.
 */
void h2SendCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        shell.println(F("Usage: h2_send <string>  e.g.: h2_send {\"cmd\":\"scan\"}"));
        return;
    }

    // Rejoin all arguments (allows spaces inside the JSON without quoting)
    String msg;
    for (size_t i = 0; i < arguments.size(); i++) {
        if (i > 0) msg += ' ';
        msg += arguments[i].c_str();
    }

    if (h2_ota_in_progress()) {
        shell.println(F("H2 OTA in progress — command blocked"));
        return;
    }
    SerialPort.println(msg);   // sends msg + '\n' to H2
    shell.printfln("H2 >> %s", msg.c_str());
}

// ── h2 <subcommand> ───────────────────────────────────────────
static void h2_tx(const char* json) {
    if (h2_ota_in_progress()) return;
    SerialPort.print(json);
    SerialPort.print('\n');
}

void h2Command(uuid::console::Shell &shell, const std::vector<std::string> &args) {
    if (args.empty()) {
        shell.println(F("Usage: h2 <list|version|discover|poll|scan|permit|pair|on|off|toggle|forget|remove|reboot|sleep|deepsleep|wakeup|reset|raw>"));
        shell.println(F("  h2 poll [addr]"));
        shell.println(F("  h2 discover [addr]"));
        shell.println(F("  h2 scan [dur]"));
        shell.println(F("  h2 permit [seconds]"));
        shell.println(F("  h2 pair [seconds]"));
        shell.println(F("  h2 on|off|toggle <addr> [ep]"));
        shell.println(F("  h2 forget <addr> | h2 forget all"));
        shell.println(F("  h2 remove <addr>"));
        shell.println(F("  h2 sleep|deepsleep [seconds]"));
        shell.println(F("  h2 raw '{\"cmd\":\"list\"}'"));
        return;
    }
    if (h2_ota_in_progress()) {
        shell.println(F("H2 OTA in progress"));
        return;
    }

    const std::string& sub = args[0];
    char buf[128] = {0};

    if (sub == "list") {
        strcpy(buf, "{\"cmd\":\"list\"}");
        h2_tx(buf);

    } else if (sub == "version") {
        strcpy(buf, "{\"cmd\":\"version\"}");
        h2_tx(buf);

    } else if (sub == "discover") {
        if (args.size() >= 2)
            snprintf(buf, sizeof(buf), "{\"cmd\":\"discover\",\"addr\":%s}", args[1].c_str());
        else
            strcpy(buf, "{\"cmd\":\"discover\"}");
        h2_tx(buf);

    } else if (sub == "poll") {
        if (args.size() >= 2)
            snprintf(buf, sizeof(buf), "{\"cmd\":\"poll\",\"addr\":%s}", args[1].c_str());
        else
            strcpy(buf, "{\"cmd\":\"poll\"}");
        h2_tx(buf);

    } else if (sub == "scan") {
        uint8_t dur = (args.size() >= 2) ? atoi(args[1].c_str()) : 3;
        snprintf(buf, sizeof(buf), "{\"cmd\":\"scan\",\"dur\":%u}", dur);
        h2_tx(buf);

    } else if (sub == "permit") {
        uint8_t sec = (args.size() >= 2) ? atoi(args[1].c_str()) : 60;
        snprintf(buf, sizeof(buf), "{\"cmd\":\"permit\",\"seconds\":%u}", sec);
        h2_tx(buf);

    } else if (sub == "pair") {
        uint32_t sec = (args.size() >= 2) ? atoi(args[1].c_str()) : 120;
        if (sec == 0) sec = 120;

        if (g_h2_pair_task_active) {
            shell.println(F("H2 pair helper already running"));
            return;
        }

        g_h2_pair_task_active = true;
        if (xTaskCreate(h2_pair_task, "h2_pair", 4096, (void*)(uintptr_t)sec, 1, nullptr) != pdPASS) {
            g_h2_pair_task_active = false;
            shell.println(F("Failed to start H2 pair helper task"));
            return;
        }
        shell.printfln("H2 pair helper started: permit %us, auto-list at end", (unsigned)sec);
        snprintf(buf, sizeof(buf), "{\"cmd\":\"permit\",\"seconds\":%u}", (unsigned)sec);

    } else if (sub == "on" || sub == "off" || sub == "toggle") {
        if (args.size() < 2) { shell.printfln("Usage: h2 %s <addr> [ep]", sub.c_str()); return; }
        uint8_t ep = (args.size() >= 3) ? atoi(args[2].c_str()) : 1;
        snprintf(buf, sizeof(buf), "{\"cmd\":\"%s\",\"addr\":%s,\"ep\":%u}", sub.c_str(), args[1].c_str(), ep);
        h2_tx(buf);

    } else if (sub == "forget") {
        if (args.size() < 2) {
            shell.println(F("Usage: h2 forget <addr>|all"));
            return;
        }
        if (args[1] == "all") {
            strcpy(buf, "{\"cmd\":\"forget\",\"all\":true}");
        } else {
            snprintf(buf, sizeof(buf), "{\"cmd\":\"forget\",\"addr\":%s}", args[1].c_str());
        }
        h2_tx(buf);

    } else if (sub == "remove") {
        if (args.size() < 2) {
            shell.println(F("Usage: h2 remove <addr>"));
            return;
        }
        snprintf(buf, sizeof(buf), "{\"cmd\":\"remove\",\"addr\":%s}", args[1].c_str());
        h2_tx(buf);

    } else if (sub == "reboot") {
        strcpy(buf, "{\"cmd\":\"reboot\"}");
        h2_tx(buf);

    } else if (sub == "sleep") {
        uint32_t sec = (args.size() >= 2) ? atoi(args[1].c_str()) : 60;
        snprintf(buf, sizeof(buf), "{\"cmd\":\"sleep\",\"seconds\":%u}", sec);
        h2_tx(buf);

    } else if (sub == "deepsleep") {
        uint32_t sec = (args.size() >= 2) ? atoi(args[1].c_str()) : 60;
        snprintf(buf, sizeof(buf), "{\"cmd\":\"deepsleep\",\"seconds\":%u}", sec);
        h2_tx(buf);

    } else if (sub == "wakeup") {
        strcpy(buf, "{\"cmd\":\"wakeup\"}");
        h2_tx(buf);

    } else if (sub == "reset") {
        strcpy(buf, "{\"cmd\":\"reset\"}");
        h2_tx(buf);

    } else if (sub == "raw") {
        if (args.size() < 2) {
            shell.println(F("Usage: h2 raw <json>"));
            return;
        }
        String msg;
        for (size_t i = 1; i < args.size(); i++) {
            if (i > 1) msg += ' ';
            msg += args[i].c_str();
        }
        h2_tx(msg.c_str());
        shell.printfln("H2 >> %s", msg.c_str());
        return;

    } else {
        shell.printfln("Unknown H2 command: %s", sub.c_str());
        return;
    }
    shell.printfln("H2 >> %s", buf[0] ? buf : sub.c_str());
}

void h2OtaAbortCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    if (!h2_ota_in_progress()) {
        shell.println(F("H2 OTA not active"));
        return;
    }
    // Force-clear the flag so the main loop resumes reading UART.
    // The background task will still finish on its own.
    h2_ota_force_clear();
    shell.println(F("H2 OTA flag cleared — UART reading resumed"));
}

// Returns all .json filenames from LittleFS (used for tab-completion of file commands).
static std::vector<std::string> list_json_files_completion(
        uuid::console::Shell &, const std::vector<std::string> &, const std::string &) {
    std::vector<std::string> files;
    File root = LittleFS.open("/");
    if (root) {
        File f = root.openNextFile();
        while (f) {
            String name = f.name();
            if (name.endsWith(".json")) {
                files.push_back(name.c_str());
            }
            f = root.openNextFile();
        }
    }
    return files;
}

void registerCommands(std::shared_ptr<uuid::console::Commands> commands) {
    using SV = std::vector<std::string>;
    using Shell = uuid::console::Shell;

    commands->add_command(uuid::flash_string_vector{F("help")}, helpCommand);
    commands->add_command(uuid::flash_string_vector{F("exit")}, exitCommand);
    commands->add_command(uuid::flash_string_vector{F("reboot")}, espResetCommand);
    commands->add_command(uuid::flash_string_vector{F("esp_status")}, espStatusCommand);
    commands->add_command(uuid::flash_string_vector{F("ping")}, PingCommand);
    commands->add_command(uuid::flash_string_vector{F("create_json_week_files")}, create_json_week_files_Command);
    commands->add_command(uuid::flash_string_vector{F("add_glucosevalue_to_json")}, addGlucoseValueToJsonCommand);
    commands->add_command(uuid::flash_string_vector{F("list_json_files")}, printJsonFileListCommand);

    commands->add_command(uuid::flash_string_vector{F("screens")},
        uuid::flash_string_vector{F("<next|prev>")},
        switch_screensCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"next", "prev"}; });

    commands->add_command(uuid::flash_string_vector{F("log_level")},
        uuid::flash_string_vector{F("<off|info|notice|debug|all>")},
        LoglevelCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"off", "info", "notice", "debug", "all"}; });

    commands->add_command(uuid::flash_string_vector{F("config")},
        uuid::flash_string_vector{F("<load|save>")},
        configSettingCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"load", "save"}; });

    commands->add_command(uuid::flash_string_vector{F("wifi_settings")},
        uuid::flash_string_vector{F("<bssid>"), F("<password>")},
        WiFiSettingCommand);

    commands->add_command(uuid::flash_string_vector{F("wifi")},
        uuid::flash_string_vector{F("<ap|connect>")},
        wifiModeCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"ap", "connect"}; });

    commands->add_command(uuid::flash_string_vector{F("timezone")},
        uuid::flash_string_vector{F("<+/-hours>")},
        timezoneCommand);

    commands->add_command(uuid::flash_string_vector{F("ota")},
        uuid::flash_string_vector{F("<enable|disable>")},
        otaSettingCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"enable", "disable"}; });

    commands->add_command(uuid::flash_string_vector{F("trgb_brightness")},
        uuid::flash_string_vector{F("<0-256>")},
        trgbBrightnessCommand);

    commands->add_command(uuid::flash_string_vector{F("print_json_file")},
        uuid::flash_string_vector{F("<filename>")},
        printJsonFileCommand,
        list_json_files_completion);

    commands->add_command(uuid::flash_string_vector{F("delete_json_file")},
        uuid::flash_string_vector{F("<filename>")},
        deleteJsonFileCommand,
        list_json_files_completion);

    commands->add_command(uuid::flash_string_vector{F("print_raw_json_file")},
        uuid::flash_string_vector{F("<filename>")},
        debugRawFileContentsCommand,
        list_json_files_completion);

    commands->add_command(uuid::flash_string_vector{F("llu_login_data")},
        uuid::flash_string_vector{F("<email@domain.com>"), F("<password>")},
        LLULoginDataCommand);

    commands->add_command(uuid::flash_string_vector{F("llu_sensor_type")},
        uuid::flash_string_vector{F("<Libre3|Libre3Plus>")},
        lluSensorTypeCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"Libre3", "Libre3Plus"}; });

    commands->add_command(uuid::flash_string_vector{F("llu")},
        uuid::flash_string_vector{F("<subcommand>")},
        lluCommand,
        [](Shell &, const SV &, const std::string &) -> SV {
            return {"value", "user_id", "user_token", "auth", "tou",
                    "sensor_id", "sensor_sn", "sensor_type", "sensor_expiry",
                    "timestamp", "history", "graphdata", "graph_redraw",
                    "get_graphdata", "statistics"};
        });

    commands->add_command(uuid::flash_string_vector{F("mqtt_client")},
        uuid::flash_string_vector{F("<enable|disable>")},
        mqttClientSettingCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"enable", "disable"}; });

    commands->add_command(uuid::flash_string_vector{F("mqtt_master_mode")},
        uuid::flash_string_vector{F("<enable|disable>")},
        mqttMasterModeCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"enable", "disable"}; });

    commands->add_command(uuid::flash_string_vector{F("wireguard")},
        uuid::flash_string_vector{F("<enable|disable>")},
        wgSettingCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"enable", "disable"}; });

    commands->add_command(uuid::flash_string_vector{F("download_ca_to_file")},
        uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")},
        downloadRootCaToFileCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"DigiCert", "Baltimore", "GoogleTrust"}; });

    commands->add_command(uuid::flash_string_vector{F("download_ca_from_url")},
        uuid::flash_string_vector{F("<https_url>"), F("<littlefs_path>")},
        downloadRootCaFromURLToFileCommand);

    commands->add_command(uuid::flash_string_vector{F("set_ca_from_file")},
        uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")},
        setCaFromFileCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"DigiCert", "Baltimore", "GoogleTrust"}; });

    commands->add_command(uuid::flash_string_vector{F("show_ca_from_file")},
        uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")},
        showCaFromFileCommand,
        [](Shell &, const SV &, const std::string &) -> SV { return {"DigiCert", "Baltimore", "GoogleTrust"}; });

    commands->add_command(uuid::flash_string_vector{F("h2_send")},
        uuid::flash_string_vector{F("<string>")},
        h2SendCommand);

    commands->add_command(uuid::flash_string_vector{F("h2")},
        uuid::flash_string_vector{F("<subcommand>"), F("[args...]")},
        h2Command,
        [](Shell &, const SV &args, const std::string &) -> SV {
            if (args.empty())
                return {"list","poll","scan","permit","pair","on","off","toggle",
                        "reboot","sleep","deepsleep","wakeup","reset"};
            return {};
        });

    commands->add_command(uuid::flash_string_vector{F("h2_ota_abort")},
        uuid::flash_string_vector{},
        h2OtaAbortCommand);

    commands->add_command(uuid::flash_string_vector{F("fw_update")},
        uuid::flash_string_vector{F("<check|install|status|channel|force>")},
        fwUpdateCommand,
        [](Shell &, const SV &args, const std::string &) -> SV {
            if (args.empty())        return {"check", "install", "status", "channel", "force"};
            if (args[0] == "channel") return {"release", "staging"};
            if (args[0] == "force")   return {"on", "off"};
            return {};
        });
}
