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
#include "ui.h"
#include <LittleFS.h>
#include <lvgl.h>
#include "tpanels3.h"

#include <cstdarg> // for va_list, va_start, va_end
#include <cstdio>  // for vsnprintf

extern SETTINGS settings;
extern LIBRELINKUP librelinkup;
extern TPanelS3 tpanels3;
extern WiFiClient mqttClient;
extern PubSubClient mqtt_client;
extern MQTT mqtt;
extern HBA1C hba1c;
extern HELPER helper;
extern uint16_t telnet_port;

extern int app_check_internet_status(IPAddress ip, uint16_t port);

//------------------------[uuid logger]-----------------------------------
/** @brief Module logger instance. */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

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
    if (index < arguments.size()) {
        return std::stoi(arguments[index]);
    }
    return defaultValue;
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
    if (!arguments.empty()) {
        String LoginName = arguments[0].c_str();
        shell.printfln("LoginName: %s", LoginName.c_str());
        settings.config.login_email = LoginName;

        String LoginPassword = arguments[1].c_str();
        shell.printfln("LoginName: %s", LoginPassword.c_str());
        settings.config.login_password = LoginPassword;
        librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);

        settings.saveConfiguration(settings.config_filename, settings.config);
    }
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
        shell.printfln("Usage: wifi <ssid_or_bssid> [password]");
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
            shell.printfln("invalid argument: %s", screen_argument);
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
            shell.printfln("invalid argument: %s", config_argument);
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
            shell.printfln("invalid argument: %s", ota_argument);
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
    hba1c.listJsonFilesTelnet();
}

/**
 * @brief Handler for command: `print_json_file <filename>`
 * @param shell Shell output.
 * @param arguments arguments[0] = filename
 *
 * Prints the decoded JSON glucose file contents (timestamp + glucose).
 */
void printJsonFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.printJsonFileTelnet(arguments[0].c_str());
    }
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
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.deleteJsonFile(arguments[0].c_str());
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
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.debugRawFileContents(arguments[0].c_str());
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
            shell.printfln("Last_LCD_Position: %03d Value: %03d",
                (librelinkup.GRAPHDATAARRAYSIZE),
                librelinkup.sensor_history_data().graph_data[librelinkup.GRAPHDATAARRAYSIZE]
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
            glucose_statistics();
        }
        else {
            shell.printfln("invalid argument: %s", llu_argument);
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

    shell.printfln(F("Sensortype: %s"), sensor_type);
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
            shell.printfln("invalid argument: %s", mqtt_argument);
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
            shell.printfln("invalid argument: %s", mqtt_argument);
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
            shell.printfln("invalid argument: %s", wireguard_argument);
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
            shell.printfln("invalid argument: %s", downloadRootCaToFile_argument);
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
    if (!arguments.empty()) {
        String https_url     = arguments[0].c_str();
        String littlefs_path = arguments[1].c_str();

        if (librelinkup.download_root_ca_to_file(https_url.c_str(), littlefs_path.c_str()) == 1) {
            shell.println(F("Root certificate downloaded successfully."));
        } else {
            shell.println(F("Error downloading Root certificate."));
        }
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
            shell.printfln("invalid argument: %s", setRootCaFromFile_argument);
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
            shell.printfln("invalid argument: %s", showCaCommand_argument);
        }
    }
}

/**
 * @brief Register all console commands.
 * @param commands Command registry to populate.
 *
 * This function wires command strings to handler callbacks.
 */
void registerCommands(std::shared_ptr<uuid::console::Commands> commands) {
    commands->add_command(uuid::flash_string_vector{F("help")}, helpCommand);
    commands->add_command(uuid::flash_string_vector{F("exit")}, exitCommand);
    commands->add_command(uuid::flash_string_vector{F("reboot")}, espResetCommand);
    commands->add_command(uuid::flash_string_vector{F("esp_status")}, espStatusCommand);
    commands->add_command(uuid::flash_string_vector{F("screens")}, uuid::flash_string_vector{F("<next|prev>")}, switch_screensCommand);
    commands->add_command(uuid::flash_string_vector{F("log_level")}, uuid::flash_string_vector{F("<OFF|INFO|NOTICE|DEBUG|ALL>")}, LoglevelCommand);
    commands->add_command(uuid::flash_string_vector{F("config")}, uuid::flash_string_vector{F("<load|save>")}, configSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("wifi_settings")}, uuid::flash_string_vector{F("<bssid>"), F("<password>")}, WiFiSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("timezone")}, uuid::flash_string_vector{F("<+/-hours>")}, timezoneCommand);
    commands->add_command(uuid::flash_string_vector{F("ota")}, uuid::flash_string_vector{F("<enable|disable>")}, otaSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("trgb_brightness")}, uuid::flash_string_vector{F("<0-256>")}, trgbBrightnessCommand);
    commands->add_command(uuid::flash_string_vector{F("create_json_week_files")}, create_json_week_files_Command);
    commands->add_command(uuid::flash_string_vector{F("add_glucosevalue_to_json")}, addGlucoseValueToJsonCommand);
    commands->add_command(uuid::flash_string_vector{F("list_json_files")}, printJsonFileListCommand);
    commands->add_command(uuid::flash_string_vector{F("print_json_file")}, uuid::flash_string_vector{F("<filename>")}, printJsonFileCommand);
    commands->add_command(uuid::flash_string_vector{F("delete_json_file")}, uuid::flash_string_vector{F("<filename>")}, deleteJsonFileCommand);
    commands->add_command(uuid::flash_string_vector{F("print_raw_json_file")}, uuid::flash_string_vector{F("<filename>")}, debugRawFileContentsCommand);
    commands->add_command(uuid::flash_string_vector{F("llu_login_data")}, uuid::flash_string_vector{F("<email@domain.com>"), F("<password>")}, LLULoginDataCommand);
    commands->add_command(uuid::flash_string_vector{F("llu_sensor_type")}, uuid::flash_string_vector{F("<Libre3|Libre3Plus>")}, lluSensorTypeCommand);
    commands->add_command(uuid::flash_string_vector{F("llu")}, uuid::flash_string_vector{F("\t<value>\n\r\t<user_id>\n\r\t<user_token>\n\r\t<auth>\n\r\t<tou>\n\r\t<sensor_id>\n\r\t<sensor_sn>\n\r\t<sensor_type>\n\r\t<sensor_expiry>\n\r\t<timestamp>\n\r\t<history>\n\r\t<graphdata>\n\r\t<graph_redraw>\n\r\t<get_graphdata>\n\r\t<statistics>")}, lluCommand);
    commands->add_command(uuid::flash_string_vector{F("ping")}, PingCommand);
    commands->add_command(uuid::flash_string_vector{F("mqtt_client")}, uuid::flash_string_vector{F("<enable|disable>")}, mqttClientSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("mqtt_master_mode")}, uuid::flash_string_vector{F("<enable|disable>")}, mqttMasterModeCommand);
    commands->add_command(uuid::flash_string_vector{F("wireguard")}, uuid::flash_string_vector{F("<enable|disable>")}, wgSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("download_ca_to_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, downloadRootCaToFileCommand);
    commands->add_command(uuid::flash_string_vector{F("download_ca_from_url")}, uuid::flash_string_vector{F("<https_url>"), F("<littlefs_path>")}, downloadRootCaFromURLToFileCommand);
    commands->add_command(uuid::flash_string_vector{F("set_ca_from_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, setCaFromFileCommand);
    commands->add_command(uuid::flash_string_vector{F("show_ca_from_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, showCaFromFileCommand);
}
