#include "main.h"
#include "commands.h"
#include "settings.h"
#include "librelinkup.h"
#include "mqtt.h"
#include "hba1c.h"
#include <ESP32Ping.h>
#include <LittleFS.h>

#include <cstdarg> // for va_list, va_start, va_end
#include <cstdio>  // for vsnprintf

extern SETTINGS settings;
extern LIBRELINKUP librelinkup;
extern WiFiClient mqttClient;
extern PubSubClient mqtt_client;
extern MQTT mqtt;
extern HBA1C hba1c;
extern HELPER helper;
extern uint16_t telnet_port;

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

// Funktion zur Anzeige des Willkommensbanners
void displayWelcomeBanner(uuid::console::Shell &shell) {
    shell.println(F("==========================================="));
    shell.println(F(" Willkommen zur ESP32 Telnet Konsole       "));
    shell.println(F(" UUID-Konsole v1.0                         "));
    shell.println(F(" Geben Sie 'help' ein, um Befehle zu sehen "));
    shell.println(F("==========================================="));
}

int parseArgument(const std::vector<std::string> &arguments, size_t index, int defaultValue = 0) {
    if (index < arguments.size()) {
        return std::stoi(arguments[index]);
    }
    return defaultValue;
}

void helpCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Available commands:"));
    shell.print_all_available_commands(); 
}

void exitCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("exit shell"));
    shell.stop();
}

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

void LLULoginDataCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {    
    if (!arguments.empty()) {
        String LoginName = arguments[0].c_str();
        shell.printfln("LoginName: %s",LoginName.c_str());
        settings.config.login_email = LoginName;

        String LoginPassword = arguments[1].c_str();
        shell.printfln("LoginName: %s",LoginPassword.c_str());
        settings.config.login_password = LoginPassword;
        
        settings.saveConfiguration(settings.config_filename, settings.config);
    }
}

void WiFiSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {    
    if (!arguments.empty()) {
        String wifi_bssid = arguments[0].c_str();
        shell.printfln("WiFi BSSID: %s",wifi_bssid.c_str());
        settings.config.wifi_bssid = wifi_bssid;

        String wifi_password = arguments[1].c_str();
        shell.printfln("WiFi Password: %s",wifi_password.c_str());
        settings.config.wifi_password = wifi_password;

        settings.saveConfiguration(settings.config_filename, settings.config);
        setup_wifi();
    }
}

void espResetCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("ESP Reset!"));
    ESP.restart();
}

void espStatusCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    esp_status();
    shell.printfln(F("ESP status: WiFi connected, free heap: %d"), ESP.getFreeHeap());
}

void configSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {

        String config_argument = arguments[0].c_str(); // load or save

        if(config_argument == "load"){
            settings.loadConfiguration(settings.config_filename, settings.config);
            shell.println(F("Configuration loaded."));
            shell.printfln("load:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
            settings.config.login_email.c_str(),
            settings.config.login_password.c_str(),
            settings.config.wifi_bssid.c_str(),
            settings.config.wifi_password.c_str(),
            settings.config.timezone,
            settings.config.ota_update,
            settings.config.wg_mode,
            settings.config.mqtt_mode,
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
            settings.config.sleep_timer);
        }
        else if((config_argument == "save")){
            settings.saveConfiguration(settings.config_filename, settings.config);
            shell.println(F("Configuration saved."));
            shell.printfln("save:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
            settings.config.login_email.c_str(),
            settings.config.login_password.c_str(),
            settings.config.wifi_bssid.c_str(),
            settings.config.wifi_password.c_str(),
            settings.config.timezone,
            settings.config.ota_update,
            settings.config.wg_mode,
            settings.config.mqtt_mode,
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
            settings.config.sleep_timer);
        }
        else {
            shell.printfln("invalid argument: %s",config_argument);
        }
    }
}

void timezoneCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    int timezone = parseArgument(arguments, 0, settings.config.timezone);
    settings.config.timezone = timezone;
    librelinkup.timezone = timezone;
    settings.saveConfiguration(settings.config_filename, settings.config);
    shell.printfln(F("Timezone set to %d and saved to config"), timezone);
}

void otaSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {
        String ota_argument = arguments[0].c_str();
        if(ota_argument == "enable"){
            settings.config.ota_update = 1;
            setup_OTA(settings.config.ota_update);
            shell.println(F("OTA enabled."));
        }
        else if((ota_argument == "disable")){
            settings.config.ota_update = 0;
            setup_OTA(settings.config.ota_update);
            shell.println(F("OTA disabled."));
        }
        else {
            shell.printfln("invalid argument: %s",ota_argument);
        }
    }
}

void trgbBrightnessCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    int brightness = parseArgument(arguments, 0, settings.config.brightness);
    settings.config.brightness = brightness;
    set_trgb_backlight_brightness(brightness);
    shell.printfln(F("Brightness set to %d"), brightness);
}

void printJsonFileListCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Print Json Filelist..."));
    hba1c.listJsonFilesTelnet();

}

void printJsonFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.printJsonFileTelnet(arguments[0].c_str());
    }
}

void create_json_week_files_Command(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.printfln(F("Create Json data test files"));
    hba1c.createTestJsonFiles();
}

void addGlucoseValueToJsonCommand(uuid::console::Shell &shell, const std::vector<std::string> &){
    shell.printfln(F("Add glucose value to json file..."));
    uint32_t unixtime_now = librelinkup.get_epoch_time();
    hba1c.addGlucoseValue(unixtime_now, librelinkup.llu_glucose_data.glucoseMeasurement);
}

void deleteJsonFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.deleteJsonFile(arguments[0].c_str());
    }
}

void debugRawFileContentsCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments){
    if (!arguments.empty()) {
        String filename_argument = arguments[0].c_str();
        shell.printfln(F("Filename: %s"), filename_argument.c_str());
        hba1c.debugRawFileContents(arguments[0].c_str());
    }
}

void lluCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {

    if (!arguments.empty()) {
        String llu_argument = arguments[0].c_str();
        if(llu_argument == "value"){
            shell.printfln("glucoseMeasurement: %d %s ∆: %d",librelinkup.llu_glucose_data.glucoseMeasurement, librelinkup.llu_glucose_data.str_trendArrow.c_str(), glucose_delta);
        }
        else if((llu_argument == "user_id")){
            shell.printfln(F("LLU User_ID: %s"), librelinkup.llu_login_data.user_id.c_str());
        }
        else if((llu_argument == "user_token")){
            //shell.printfln(F("LLU User_Token: %s"), librelinkup.user_token.c_str());

            String token_part1;
            String token_part2;

            token_part1 = librelinkup.llu_login_data.user_token.substring(0, 220);                // Die ersten 220 Zeichen
            token_part2 = librelinkup.llu_login_data.user_token.substring(220, librelinkup.llu_login_data.user_token.length());  // Der Rest
            
            shell.printf("LLU User_Token: %s", token_part1.c_str());
            shell.printfln("%s", token_part2.c_str());
        }
        else if((llu_argument == "auth")){
            shell.println(F("LLU Auth..."));
            librelinkup.auth_user(settings.config.login_email,settings.config.login_password);
            shell.printfln("LLU User_ID: %s", librelinkup.llu_login_data.user_id.c_str());
            
            String token_part1;
            String token_part2;

            token_part1 = librelinkup.llu_login_data.user_token.substring(0, 220);                // Die ersten 220 Zeichen
            token_part2 = librelinkup.llu_login_data.user_token.substring(220, librelinkup.llu_login_data.user_token.length());  // Der Rest
            
            shell.printf("LLU User_Token: %s", token_part1.c_str());
            shell.printfln("%s", token_part2.c_str());
        }
        else if((llu_argument == "tou")){
            shell.println(F("LLU Tou..."));
            librelinkup.tou_user();
        }
        else if((llu_argument == "sensor_id")){
            shell.printfln(F("LLU Sensor_ID: %s"), librelinkup.llu_sensor_data.sensor_id.c_str());
        }
        else if((llu_argument == "sensor_sn")){
            shell.printfln(F("LLU Sensor_SN: %s"), librelinkup.llu_sensor_data.sensor_sn.c_str());
        }
        else if((llu_argument == "timestamp")){
            shell.printfln(F("LLU Timestamp: %s"), librelinkup.llu_glucose_data.str_measurement_timestamp.c_str());
        }
        else if((llu_argument == "history")){
            static char time_in_hours[librelinkup.GRAPHDATAARRAYSIZE][6]; // "HH:MM" + Null-Terminator
            shell.printfln(F("LLU History...:"));
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln("Historical data: [%d/%d]",data_count, librelinkup.GRAPHDATAARRAYSIZE);
            for(uint8_t i=0;i<librelinkup.GRAPHDATAARRAYSIZE;i++){
                helper.format_time(time_in_hours[i], sizeof(time_in_hours[i]), librelinkup.llu_sensor_history_data.timestamp[i]);
                shell.printfln("ArrayPos.: %03d Value: %03d TimeStamp: %d (%s)", i, librelinkup.llu_sensor_history_data.graph_data[i], librelinkup.llu_sensor_history_data.timestamp[i], time_in_hours[i]);
            }
        }
        else if((llu_argument == "graphdata")){
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln(F("Historical data: [%d/%d]"), data_count, librelinkup.GRAPHDATAARRAYSIZE);
            shell.printfln("Last_LCD_Position: %03d Value: %03d", (librelinkup.GRAPHDATAARRAYSIZE), librelinkup.llu_sensor_history_data.graph_data[librelinkup.GRAPHDATAARRAYSIZE]);
            
            // Startposition berechnen
            int index = librelinkup.GRAPHDATAARRAYSIZE - 1;  // Index 140            if (start_index < 0) start_index = 0;  // Falls weniger als 0 → auf 0 setzen
            
            for (int i = data_count - 1; i >= 0; i--) {
                shell.printfln("Position: %03d Value: %03d TimeStamp: %d", index, librelinkup.llu_sensor_history_data.graph_data[i], librelinkup.llu_sensor_history_data.timestamp[i]);
                index--;  // Nach links rücken
            }
        }
        else if((llu_argument == "graph_redraw")){
            shell.println(F("LLU Graph redraw..."));
            draw_chart_glucose_data(3, false);
        }
        else if((llu_argument == "get_graphdata")){
            shell.println(F("LLU Get GraphData..."));
            librelinkup.get_graph_data();
            shell.printfln("SensorSN_non_activated: %s", librelinkup.llu_sensor_data.sensor_sn_non_active.c_str());
            uint8_t data_count = librelinkup.check_graphdata();
            shell.printfln("glucoseMeasurement: %d %s",librelinkup.llu_glucose_data.glucoseMeasurement, librelinkup.llu_glucose_data.str_trendArrow.c_str());
            shell.printfln("Historical data: [%d/%d]",data_count, librelinkup.GRAPHDATAARRAYSIZE);
        }
        else if((llu_argument == "statistics")){
            shell.println(F("LLU print glucose statistics..."));
            glucose_statistics();
        }
        else {
            shell.printfln("invalid argument: %s",llu_argument);
        }
    }else{
        shell.println(F("command: llu <> <>"));
    }

}

void PingCommand(uuid::console::Shell &shell, const std::vector<std::string> &) {
    shell.print(F("Ping IP..."));
    const IPAddress ping_ip(1,1,1,1);
    if(Ping.ping(ping_ip)){
            shell.println(F("OK"));
        }else{
            shell.println(F("NOK"));
        }
}

void mqttClientSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {
        String mqtt_argument = arguments[0].c_str();
        if(mqtt_argument == "enable"){
            mqtt.mqtt_enable = 1;
            shell.println(F("MQTT client enabled."));
        }
        else if((mqtt_argument == "disable")){
            mqtt.mqtt_enable = 0;
            mqtt_client.unsubscribe((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
            mqtt_client.disconnect();
            shell.println(F("MQTT client disabled."));
        }
        else {
            shell.printfln("invalid argument: %s",mqtt_argument);
        }
    }
}

void wgSettingCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    if (!arguments.empty()) {
        String wireguard_argument = arguments[0].c_str();
        if(wireguard_argument == "enable"){
            settings.config.wg_mode = 1;
            setup_wg(1);
            shell.println(F("WireGuard enabled."));
        }
        else if((wireguard_argument == "disable")){
            settings.config.wg_mode = 0;
            setup_wg(0);
            shell.println(F("WireGuard disabled."));
        }
        else {
            shell.printfln("invalid argument: %s",wireguard_argument);
        }
    } 
}

void downloadRootCaToFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {
        String downloadRootCaToFile_argument = arguments[0].c_str();
        if(downloadRootCaToFile_argument == "DigiCert"){
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_DigiCertGlobalRootG2, librelinkup.path_root_ca_dcgrg2) == 1) {
                shell.println(F("DigiCert Global Root G2 certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading DigiCert Global Root G2 certificate."));
            }
        }
        else if((downloadRootCaToFile_argument == "Baltimore")){
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_BaltimoreCyberTrustRoot, librelinkup.path_root_ca_baltimore) == 1) {
                shell.println(F("Baltimore CyberTrust Root certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading Baltimore CyberTrust Root certificate."));
            }
        }
        else if((downloadRootCaToFile_argument == "GoogleTrust")){
            if (librelinkup.download_root_ca_to_file(librelinkup.url_dl_GoogleTrustRootR4, librelinkup.path_root_ca_googler4) == 1) {
                shell.println(F("Google Trust Root R4 certificate downloaded successfully."));
            } else {
                shell.println(F("Error downloading Google Trust Root R4 certificate."));
            }
        }
        else {
            shell.printfln("invalid argument: %s",downloadRootCaToFile_argument);
        }
    }else{
        shell.println(F("command: download_ca_to_file <DigiCert|Baltimore|GoogleTrust>"));
    }
}

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

void setCaFromFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {
        String setRootCaFromFile_argument = arguments[0].c_str();
        if(setRootCaFromFile_argument == "DigiCert"){
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_dcgrg2);
            shell.println(F("DigiCert Global Root G2 certificate loaded from LittleFS."));
        }
        else if((setRootCaFromFile_argument == "Baltimore")){
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_baltimore);
            shell.println(F("Baltimore CyberTrust Root certificate loaded from LittleFS."));
        }
        else if((setRootCaFromFile_argument == "GoogleTrust")){
            librelinkup.setCAfromfile(librelinkup.get_wifisecureclient(), librelinkup.path_root_ca_googler4);
            shell.println(F("Google Trust Root R4 certificate loaded from LittleFS."));
        }
        else {
            shell.printfln("invalid argument: %s",setRootCaFromFile_argument);
        }
    }
}

void showCaFromFileCommand(uuid::console::Shell &shell, const std::vector<std::string> &arguments) {
    
    if (!arguments.empty()) {
        String showCaCommand_argument = arguments[0].c_str();
        if(showCaCommand_argument == "DigiCert"){
            librelinkup.showCAfromfile(librelinkup.path_root_ca_dcgrg2);
            shell.println(F("Displayed DigiCert Global Root G2 certificate."));
        }
        else if((showCaCommand_argument == "Baltimore")){
            librelinkup.showCAfromfile(librelinkup.path_root_ca_baltimore);
            shell.println(F("Displayed Baltimore CyberTrust Root certificate."));
        }
        else if((showCaCommand_argument == "GoogleTrust")){
            librelinkup.showCAfromfile(librelinkup.path_root_ca_googler4);
            shell.println(F("Google Trust Root R4 certificate."));
        }
        else {
            shell.printfln("invalid argument: %s",showCaCommand_argument);
        }
    }
}

void registerCommands(std::shared_ptr<uuid::console::Commands> commands) {
    commands->add_command(uuid::flash_string_vector{F("help")}, helpCommand);
    commands->add_command(uuid::flash_string_vector{F("exit")}, exitCommand);
    commands->add_command(uuid::flash_string_vector{F("esp_reset")}, espResetCommand);
    commands->add_command(uuid::flash_string_vector{F("esp_status")}, espStatusCommand);
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
    commands->add_command(uuid::flash_string_vector{F("llu")}, uuid::flash_string_vector{F("\t<value>\n\r\t<user_id>\n\r\t<user_token>\n\r\t<auth>\n\r\t<tou>\n\r\t<timestamp>\n\r\t<history>\n\r\t<graphdata>\n\r\t<graph_redraw>\n\r\t<get_graphdata>\n\r\t<statistics>")}, lluCommand);
    commands->add_command(uuid::flash_string_vector{F("ping")}, PingCommand);
    commands->add_command(uuid::flash_string_vector{F("mqtt_client")}, uuid::flash_string_vector{F("<enable|disable>")}, mqttClientSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("wireguard")}, uuid::flash_string_vector{F("<enable|disable>")}, wgSettingCommand);
    commands->add_command(uuid::flash_string_vector{F("download_ca_to_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, downloadRootCaToFileCommand);
    commands->add_command(uuid::flash_string_vector{F("download_ca_from_url")}, uuid::flash_string_vector{F("<https_url>"), F("<littlefs_path>")}, downloadRootCaFromURLToFileCommand);
    commands->add_command(uuid::flash_string_vector{F("set_ca_from_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, setCaFromFileCommand);
    commands->add_command(uuid::flash_string_vector{F("show_ca_from_file")}, uuid::flash_string_vector{F("<DigiCert|Baltimore|GoogleTrust>")}, showCaFromFileCommand);
}