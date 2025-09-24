/*
 * @Description: LVGL
 * @Author: LILYGO_L
 * @Date: 2023-09-22 11:59:37
 * @LastEditTime: 2024-11-21 11:42:10
 * @License: GPL 3.0
 */
#include <Arduino.h>
#include "main.h"
#include <string.h>
#include "lvgl.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"


// ESP helper
#include "helper.h"
HELPER helper;

/*******************************************************************************
 * Start of misc setting
 *
 ******************************************************************************/
#include "settings.h"

SETTINGS settings;

//------------------------------[ esp status settings ]--------------------------
uint8_t esp_status_counter_wifi_restart = 0;
uint8_t esp_status_counter_llu_reauth = 0;
uint8_t esp_status_counter_llu_retou = 0;


//------------------------------[ Serial Debug Macro ]--------------------------
#define DBGprint Serial.printf("[%09lu ms][%s][%s] ", (unsigned long)millis(), __FILE__, __func__)

//------------------------------[ TPanelS3 settings ]--------------------------
// #define TOUCH_MODULES_GT911
// #define TOUCH_MODULES_CST_SELF
#define TOUCH_MODULES_CST_MUTUAL
// #define TOUCH_MODULES_ZTW622
// #define TOUCH_MODULES_L58
// #define TOUCH_MODULES_FT3267
// #define TOUCH_MODULES_FT5x06

#include "TouchLib.h"
volatile bool Touch_Int_Flag = false;

TouchLib touch(Wire, TOUCH_SDA, TOUCH_SCL, CST3240_ADDRESS);

Arduino_DataBus *bus = new Arduino_XL9535SWSPI(IIC_SDA /* SDA */, IIC_SCL /* SCL */, -1 /* XL PWD */,
                                               XL95X5_CS /* XL CS */, XL95X5_SCLK /* XL SCK */, XL95X5_MOSI /* XL MOSI */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    -1 /* DE */, LCD_VSYNC /* VSYNC */, LCD_HSYNC /* HSYNC */, LCD_PCLK /* PCLK */,
    LCD_B0 /* B0 */, LCD_B1 /* B1 */, LCD_B2 /* B2 */, LCD_B3 /* B3 */, LCD_B4 /* B4 */,
    LCD_G0 /* G0 */, LCD_G1 /* G1 */, LCD_G2 /* G2 */, LCD_G3 /* G3 */, LCD_G4 /* G4 */, LCD_G5 /* G5 */,
    LCD_R0 /* R0 */, LCD_R1 /* R1 */, LCD_R2 /* R2 */, LCD_R3 /* R3 */, LCD_R4 /* R4 */,
    1 /* hsync_polarity */, 20 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 0 /* hsync_back_porch */,
    1 /* vsync_polarity */, 30 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 1 /* vsync_back_porch */,
    1 /* pclk_active_neg */, 6000000L /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high*/, 0 /* pclk_idle_high */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, -1 /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 0
	lv_draw_buf_t disp_buf;         // contains internal graphic buffer(s) called draw buffer(s)
	lv_display_t * disp_drv;        // contains callback functions
	lv_indev_t * indev_drv;
#elif LVGL_VERSION_MAJOR == 8 && LVGL_VERSION_MINOR >= 3  
  lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
	lv_disp_drv_t disp_drv;      // contains callback functions
	lv_indev_drv_t indev_drv;
#endif

/* Display flushing */
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

    #if LV_COLOR_16_SWAP
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    #else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    #endif

  lv_display_flush_ready(disp);
}

// set screen rotation // rotation = 0, 1, 2, 3  entspricht 0°, 90°, 180°, 270°
void setRotation(uint8_t r) {
  if (gfx) {
    gfx->setRotation(r);
  }
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_t *indev_driver, lv_indev_data_t *data)
{
    if (Touch_Int_Flag == true)
    {
        touch.read();
        TP_Point t = touch.getPoint(0);

        if ((touch.getPointNum() == 1) && (t.pressure > 0) && (t.state != 0))
        {
            data->state = LV_INDEV_STATE_PR;

            /*Set the coordinates*/
            data->point.x = t.x;
            data->point.y = t.y;

            Serial.printf("Touch X: %d Y: %d", t.x, t.y);
            Serial.printf("Static: %d", t.state);
            Serial.printf("Pressure: %d", t.pressure);
        }

        Touch_Int_Flag = false;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void lvgl_initialization(void)
{
    lv_init();
    lv_tick_set_cb([](){ return (uint32_t)millis(); });

    // Allocate draw buffers used by LVGL from PSRAM
    lv_color_t *buf1 = (lv_color_t*) heap_caps_malloc(
            LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
    lv_color_t *buf2 = (lv_color_t*) heap_caps_malloc(
            LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf2);

    // Initialize the display buffer
    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * LCD_HEIGHT, /*LV_DISPLAY_RENDER_MODE_FULL*/ LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    Serial.println("Register display driver to LVGL");

    // Initialize the input device driver
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

}

//------------------------------[ Backlight settings ]--------------------------
#define TRGB_STD_BACKLIGHT_BRIGHTNESS (50)

//------------------------------[ ArduinoJson settings ]--------------------------
#include <ArduinoJson.h>
#include <StreamUtils.h>

//------------------------------[ LittleFs settings ]--------------------------
#include <LittleFS.h>

//------------------------------[ MDNS settings ]--------------------------
#include <ESPmDNS.h>
String hostname_base = "librelinkup_";
String hostname = "";
void setup_mdns(void);

//------------------------------[ uuid console configuration ]--------------------
#include "commands.h"

using uuid::read_flash_string;
using uuid::flash_string_vector;
using uuid::console::Commands;
using uuid::console::Shell;
using LogFacility = ::uuid::log::Facility;

static std::shared_ptr<uuid::console::Commands> commands = std::make_shared<uuid::console::Commands>();
static uuid::telnet::TelnetService telnet{commands};

//------------------------------[ uuid log configuration ]------------------
#include "logger.h"

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};


//------------------------------[ WireGuard configuration ]----------------------
#include <WireGuard-ESP32.h>
#include "wireguardif.h"

static WireGuard wg;

//Client2_fritz_wireguard
/*IPAddress                 local_ip(192,168,0,102);                          // [Interface] Address
char private_key[]      = "wDkHdPkttFRkXn2RgVNm0EZ3AB+PEEI/SxIs64XvzWA=";   // [Interface] PrivateKey
char public_key[]       = "UcrXdP/NpxdqKP9bvViaa8jqZS4qOurD3cj4fIT+zU4=";   // [Peer] PublicKey
char preshared_key[]    = "jH721Rg83PSd0cbM4rKbYoDRGY60GM4xCs8+HPueEYg=";   // [Peer] PreSharedKey
*/
//Client3_fritz_wireguard
IPAddress                 local_ip(192,168,0,103);                          // [Interface] Address
//char private_key[]      = "aKjJMI6k8ui/x6bIYXwGDf8qAhiqjc5bRrbbyr2U3XY=";   // [Interface] PrivateKey
//char public_key[]       = "UcrXdP/NpxdqKP9bvViaa8jqZS4qOurD3cj4fIT+zU4=";   // [Peer] PublicKey
//char preshared_key[]    = "+ejma4KmXc61V2A4r55Ku40rAQS5YlamMJJavTQIV5Y=";   // [Peer] PreSharedKey

//char endpoint_address[] = "vpn0.ddnss.de";                                  // [Peer] Endpoint
//int  endpoint_port       = 50614;                                           // [Peer] Endpoint port

void setup_wg(bool enable);

//------------------------------[ HBC1 Value calculation ]--------------------------
#include "hba1c.h"
HBA1C hba1c;

//------------------------------[ LibreLinkUp settings ]--------------------------------
#include "librelinkup.h"

LIBRELINKUP librelinkup;

int16_t glucose_delta = 0;              ///< Change from last reading
uint16_t glucoseMeasurement_backup = 0; ///< Previous measurement


//------------------------------[ LVGL settings ]--------------------------
#include "ui.h"

// Update OTA update progress bar
uint8_t update_ota_progress_screen(int progress) {
    // Fortschrittsbalken aktualisieren
    //lv_bar_set_value(ui_Bar_FWUpdateProgress, progress, LV_ANIM_OFF);

    // Prozentanzeige aktualisieren
    char progress_text[10];
    snprintf(progress_text, sizeof(progress_text), "%d%%", progress);
    lv_label_set_text(ui_Label_FWUpdateProgress_percent, progress_text);

    return 1;
}

//------------------------------[ Time Zone settings]-----------------------
// Zeitzone für Mitteleuropa (CET/CEST)
const char* tz = "CET-1CEST-2,M3.5.0/2,M10.5.0/3";

//------------------------------[ ESP32 OTA settings ]--------------------------
#include <ElegantOTA.h>

AsyncWebServer server(80);

uint32_t ota_progress_millis = 0;
bool ota_in_progress = 0;

//---------------------------[ Wifi scan background task ]-----------------------
TaskHandle_t scanTaskHandle;
String availableNetworks;

// WLAN-Scan in einem separaten Task
void scanWiFiTask(void * parameter) {
    for(;;) {
        if(ota_in_progress == 0){
            int n = WiFi.scanNetworks();
            String json = "[";
            for (int i = 0; i < n; ++i) {
                if (i) json += ",";
                json += "{";
                json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
                json += "\"rssi\":" + String(WiFi.RSSI(i));
                json += "}";
            }
            json += "]";
            availableNetworks = json;
            vTaskDelay(360000 / portTICK_PERIOD_MS); // Scan alle 360 Sekunden
        }
    }
}
//---------------------------[ Webpage ]-------------------------------
#include "webpage.h"

String username;
String password;
String wifi_bssid;
String wifi_password;

void handleRoot(AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);    
}

void handleLogin(AsyncWebServerRequest *request) {
    if (request->hasParam("username", true)) {
        username = request->getParam("username", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }
    settings.config.login_email    = username;
    settings.config.login_password = password;
    settings.saveConfiguration(settings.config_filename, settings.config);    
    
    request->send(200, "text/html", "Login erfolgreich!<br><a href='/'>Zurueck</a>");
}

void handleScan(AsyncWebServerRequest *request) {
   request->send(200, "application/json", availableNetworks);
}

void handleConnect(AsyncWebServerRequest *request) {
    if (request->hasParam("networks", true)) {
        wifi_bssid = request->getParam("networks", true)->value();
        settings.config.wifi_bssid = wifi_bssid;
    }
    if (request->hasParam("wifiPassword", true)) {
        wifi_password = request->getParam("wifiPassword", true)->value();
        settings.config.wifi_password = wifi_password;
    }

    // save login data to config file
    settings.saveConfiguration(settings.config_filename, settings.config);
    // ESP32 restart
    ESP.restart();
}

void handleStatus(AsyncWebServerRequest *request) {

    settings.loadConfiguration("/config.json", settings.config);
    
    DynamicJsonDocument json_config(256);

    // Füge die Konfigurationswerte in das JSON-Dokument ein
    json_config["ota_update"] = settings.config.ota_update;
    json_config["wg_mode"]    = settings.config.wg_mode;
    json_config["mqtt_mode"]  = settings.config.mqtt_mode;
    json_config["brightness"] = settings.config.brightness;

    // Erstelle den JSON-String
    String jsonResponse;
    serializeJson(json_config, jsonResponse);

    // Sende die JSON-Antwort
    request->send(200, "application/json", jsonResponse);

    json_config.clear();
}

void handleToggleFeature(AsyncWebServerRequest *request) {
    if (request->hasParam("feature") && request->hasParam("status")) {
        String feature = request->getParam("feature")->value();
        int status = request->getParam("status")->value().toInt();

        Serial.printf("Feature: %s, Status: %d\n", feature.c_str(), status);

        // Überprüfen und anwenden der Feature-Einstellungen
        if (feature == "ota_update") {
            settings.config.ota_update = status;
            logger.notice("OTA_Update: %d", settings.config.ota_update);
            if(status == 1){
                //start OTA Web server
                server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
                request->send(200, "text/plain", "ESP32 LibreLinkup Client");
                });
                ElegantOTA.begin(&server);    // Start ElegantOTA
                server.begin();
            }else if(status == 0){
                server.end();                    // Stop Server
            }
        } else if (feature == "wg_mode") {
            settings.config.wg_mode = status;
            logger.notice("wg_mode: %d", settings.config.wg_mode);
            setup_wg(settings.config.wg_mode);
        } else if (feature == "mqtt_mode") {
            settings.config.mqtt_mode = status;
            logger.notice("mqtt_mode: %d", settings.config.mqtt_mode);
        } else {
            request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
            return;
        }

        // Speichere die aktualisierte Konfiguration
        //settings.saveConfiguration("/config.json", settings.config);

        // Sende eine erfolgreiche Antwort zurück
        request->send(200, "application/json", "{\"status\": \"updated\"}");
    } else {
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
    }
}

void handleSetBrightness(AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {  // Überprüfe den URL-Parameter
        int brightness = request->getParam("value")->value().toInt();
        Serial.printf("WebPage brightness slider feedback: %d\n", brightness);

        // Aktualisiere Helligkeit und speichere Konfiguration
        settings.config.brightness = brightness;
        set_trgb_backlight_brightness(brightness);
        //settings.saveConfiguration("/config.json", settings.config);

        // Bestätigung an den Client senden
        request->send(200, "application/json", "{\"brightness\": " + String(brightness) + "}");
    } else {
        request->send(400, "application/json", "{\"error\": \"Invalid parameters\"}");
    }
}

void handleConfigureWireGuard(AsyncWebServerRequest *request) {
    String privateKey, publicKey, presharedKey, ipAddress, endpoint, allowedIPs;
    int endpointPort = 0;

    if (request->hasParam("privateKey", true)) {
        privateKey = request->getParam("privateKey", true)->value();
    }
    if (request->hasParam("publicKey", true)) {
        publicKey = request->getParam("publicKey", true)->value();
    }
    if (request->hasParam("presharedKey", true)) {
        presharedKey = request->getParam("presharedKey", true)->value();
    }
    if (request->hasParam("ipAddress", true)) {
        ipAddress = request->getParam("ipAddress", true)->value();
    }
    if (request->hasParam("endpoint", true)) {
        endpoint = request->getParam("endpoint", true)->value();
    }
    if (request->hasParam("endpointPort", true)) {
        endpointPort = request->getParam("endpointPort", true)->value().toInt();
    }
    if (request->hasParam("allowedIPs", true)) {
        allowedIPs = request->getParam("allowedIPs", true)->value();
    }

    // Sicherstellen, dass alle erforderlichen Parameter vorhanden sind
    if (!privateKey.isEmpty() && !publicKey.isEmpty() && !presharedKey.isEmpty() && 
        !ipAddress.isEmpty() && !endpoint.isEmpty() && endpointPort > 0 && !allowedIPs.isEmpty()) {

        settings.config.wgPrivateKey = privateKey;
        settings.config.wgPublicKey = publicKey;
        settings.config.wgPresharedKey = presharedKey;
        settings.config.wgIpAddress = ipAddress;
        //settings.config.wgIpAddress.replace('.', ',');  //converts . to , for localip();
        settings.config.wgEndpoint = endpoint;
        settings.config.wgEndpointPort = endpointPort;
        settings.config.wgAllowedIPs = allowedIPs;

        logger.notice("WireGuard configuration parsed and saved");

        settings.saveConfiguration(settings.config_filename, settings.config);

        request->send(200, "application/json", "{\"status\": \"WireGuard configuration saved\"}");
    } else {
        logger.notice("Missing WireGuard parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
    }
}

void handleConfigureMQTT(AsyncWebServerRequest *request) {
    String server, username, password;
    int port = 0;

    if (request->hasParam("server", true)) {
        server = request->getParam("server", true)->value();
    }
    if (request->hasParam("port", true)) {
        port = request->getParam("port", true)->value().toInt();
    }
    if (request->hasParam("username", true)) {
        username = request->getParam("username", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }

    if (!server.isEmpty() && port > 0) {
        settings.config.mqttServer = server;
        settings.config.mqtt_port = port;
        settings.config.mqttUsername = username;
        settings.config.mqttPassword = password;

        logger.notice("MQTT configuration parsed and saved");

        settings.saveConfiguration(settings.config_filename, settings.config);

        request->send(200, "application/json", "{\"status\": \"MQTT configuration saved\"}");
    } else {
        logger.notice("Missing 'server' or 'port' parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing server or port parameters\"}");
    }
}

//-------------------------[ OTA Update ]-------------------------------
void onOTAStart() {
    // Log when OTA has started
    Serial.println("OTA update started!");
    logger.notice("OTA Update Progress has started");
    ota_in_progress = 1;
}

void onOTAProgress(size_t current, size_t final) {
    // Log every 250 milliseconds
    if (millis() - ota_progress_millis > 1000) {
        ota_progress_millis = millis();

        // Berechnung des Fortschritts in Prozent
        float progress = ((float)current / (float)final) * 100.0;

        // Ausgabe von Bytes und Prozentwert
        //Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes, Progress: %.2f%%\n", current, final, progress);
        logger.debug("OTA Progress Current: %u bytes, Final: %u bytes, Progress: %.2f%%", current, final, progress);
        
        /*
        logger.notice("===== Heap-Speicherstatus =====");
        logger.notice("Gesamter freier Heap: %d Bytes", esp_get_free_heap_size());
        logger.notice("Größter freier Block: %d Bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        logger.notice("Interner RAM (DMA-fähig): %d Bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        logger.notice("PSRAM verfügbar: %d Bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        logger.notice("==============================");
        */
       
        if(ota_in_progress == 1){
            update_ota_progress_screen(progress);
        }  
    }
}

void onOTAEnd(bool success) {
    // Log when OTA has finished
    if (success) {
        Serial.println("OTA update finished successfully!");
    } else {
        Serial.println("There was an error during OTA update!");
        logger.notice("There was an error during OTA update!");
    }
    // <Add your own code here>
    logger.notice("OTA Update Progress success: %d", success);
    if(success == 0){
        lv_disp_load_scr(ui_Main_screen);
        ota_in_progress = 0;
    }else if(success == 1){
            ota_in_progress = 0;
            lv_label_set_text(ui_Label_FWUpdateProgress_percent, "100%");
            lv_label_set_text(ui_Label_FWUpdateInfo, "FWUpdate successful!\n\nperforming Reset" );
            lv_task_handler();delay(5);
            //delay(250);
    }
}

//uint8_t elegant_ota_server_state = 0;
void setup_OTA(bool mode);

//------------------------------[ Software timer ]--------------------------
const uint64_t timer_250ms = 250;               //Timer1 250ms
uint64_t g_timer_250ms_backup = 0;              //Timer1 backup time

const uint64_t timer_1000ms = 1000;             //Timer1 250ms
uint64_t g_timer_1000ms_backup = 0;             //Timer1 backup time

const uint64_t timer_5000ms = 5000;             //Timer2 5000ms
uint64_t g_timer_5000ms_backup = 0;             //Timer2 backup time

const uint64_t timer_30000ms = 30000;           //Timer3 60000ms
uint64_t g_timer_30000ms_backup = 0;            //Timer3 backup time

const uint64_t timer_10000ms = 10000;           //Timer4 10000ms
uint64_t g_timer_10000ms_backup = 0;            //Timer4 backup time

const uint64_t timer_60000ms = 60000;           //Timer4 60000ms
uint64_t g_timer_60000ms_backup = 0;            //Timer4 backup time

const uint64_t timer_120000ms = 120000;         //Timer4 120000ms
uint64_t g_timer_120000ms_backup = 0;           //Timer4 backup time

const uint64_t timer_300100ms = 300100;         //Timer7 5,01 Minuten
uint64_t g_timer_300100ms_backup = 0;           //Timer4 backup time

const uint64_t config_sleep_timer = 3600000;     //Timer8 60 Minuten
uint64_t config_sleep_timer_backup = 0;          //Timer4 backup time

//-------------------------------------------------------------------------------

//------------------------------[ ArduinoJson settings ]-------------------------
#define ARDUINOJSON_USE_DOUBLE 1

//-------------------------------------------------------------------------------

//------------------------------[ Wifi settings ]--------------------------------
#include <WiFi.h>
#include <WiFiMulti.h>

WiFiMulti wifiMulti;
const uint32_t connectTimeoutMs = 5000;

//------------------------------[ ESP32Ping settings ]--------------------------------
#include <ESP32Ping.h>

enum InternetStatus {
    INTERNET_DISCONNECTED = 0,
    INTERNET_CONNECTED = 1,
};

const IPAddress ping_ip(1,1,1,1);
bool internet_status = INTERNET_DISCONNECTED;

//--------------------------[mqtt configuration]--------------------------------
//#include <PubSubClient.h>
#include "mqtt.h"

MQTT mqtt;
WiFiClient mqttClient;
PubSubClient mqtt_client(mqttClient);

DynamicJsonDocument json_mqtt(256);

void setup_mqtt(void);
//------------------------------------------------------------------------------

//---------------------------[helper functions]---------------------------------

//---------------------------[battery voltage]---------------------------------
#define BATTERY_ADC_PIN 4  // ADC-Pin, an den die Batteriespannung angeschlossen ist
#define ADC_MAX_VALUE 4095 // 12-Bit-Auflösung
#define ADC_REF_VOLTAGE 3.3 // Referenzspannung in Volt
#define VOLTAGE_DIVIDER_RATIO 2 // Verhältnis des Spannungsteilers

void setup_adc(void);

void setup_adc(void){
    analogReadResolution(12);       // 12-Bit-resolution (Standard)
    analogSetAttenuation(ADC_11db); // allow voltage till ~3.3V    
}

//--------------------------[esp_status functions]--------------------------------
void esp_status(){
    if (!psramFound()) {
        logger.notice("No PSRAM avalable!");
    } else {
        logger.notice("PSRAM avalable!");
    }
    
    logger.notice("===== Heap-Speicherstatus =====");
    logger.notice("Gesamter freier Heap: %d Bytes", esp_get_free_heap_size());
    logger.notice("Größter freier Block: %d Bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    logger.notice("Interner RAM (DMA-fähig): %d Bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    logger.notice("PSRAM verfügbar: %d Bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    logger.notice("==============================");
    //logger.notice("ESP32 bootcount : %d",trgb.getBootCount());
    logger.notice("Wifi Reconnects : %d",esp_status_counter_wifi_restart);
    logger.notice("LLU count reAuth: %d",esp_status_counter_llu_reauth);
    logger.notice("LLU count reTou : %d",esp_status_counter_llu_retou);
    logger.notice("OTA Server      : %d",settings.config.ota_update);
    logger.notice("MQTT Mode       : %d",settings.config.mqtt_mode);
    logger.notice("WG Mode         : %d",settings.config.wg_mode);   
    logger.notice("Local Time      : %s",helper.get_esp_time_date().c_str());
    logger.notice("TimeZone        : %d",settings.config.timezone);
    logger.notice("Brightness LCD  : %d",settings.config.brightness);

    if( wg.is_initialized() ) {
        logger.notice("WG connected! IP: %s, Gateway: %s, SubNet: %s, dnsIP: %s \r\n",local_ip.toString(),WiFi.gatewayIP().toString(),WiFi.subnetMask().toString(), WiFi.dnsIP().toString());
    }
    else{
        logger.notice("WG not connected! IP: %s, Gateway: %s, SubNet: %s, dnsIP: %s \r\n",WiFi.localIP().toString(),WiFi.gatewayIP().toString(),WiFi.subnetMask().toString(), WiFi.dnsIP().toString());
    }
}

//--------------------------[TRGB configuration]--------------------------------
// LilyGo  T-RGB  control backlight chip has 16 levels of adjustment range
// The adjustable range is 0~15, 0 is the minimum brightness, 15 is the maximum brightness
uint8_t set_trgb_backlight_brightness(uint8_t value)
{
    ledcSetup(0, 5000, 8);                              // 0-15, 5000, 8
    ledcAttachPin(LCD_BL, 0);         // EXAMPLE_PIN_NUM_BK_LIGHT, 0 - 15
    ledcWrite(0, value);        // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
    
    return value;
}

//--------------------------[mqtt configuration]--------------------------------
void mqtt_publish(){
    
    json_mqtt["glucoseMeasurement"] = librelinkup.llu_glucose_data.glucoseMeasurement;
    json_mqtt["trendArrow"]         = librelinkup.llu_glucose_data.trendArrow;
    json_mqtt["brightness"]         = settings.config.brightness;
    json_mqtt["mqtt_mode"]          = settings.config.mqtt_mode;
    json_mqtt["ota_server"]         = settings.config.ota_update;
    json_mqtt["wireguard_mode"]     = settings.config.wg_mode;
    
    serializeJson(json_mqtt, mqtt.mqtt_buffer);                 //do serialation and copy into buffer
    json_mqtt.clear();                                          //clears the data object*/
    mqtt_client.publish((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_client_data).c_str(), mqtt.mqtt_buffer);      //send to server

    json_mqtt["IP"]   = WiFi.localIP().toString();
    if(settings.config.wg_mode == 1){
        json_mqtt["IP_WG"] = local_ip.toString();
    }
    else if(settings.config.wg_mode == 0){
        json_mqtt["IP_WG"] = "not connected";
    }
    json_mqtt["SSID"] = WiFi.SSID();
    json_mqtt["RSSI"] = WiFi.RSSI();
        
    serializeJson(json_mqtt, mqtt.mqtt_buffer);                 //do serialation and copy into buffer
    
    Json_Buffer_Info buffer_info;
    buffer_info = helper.getBufferSize(&json_mqtt);
    //logger.debug("json_mqtt: Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

    json_mqtt.clear();                                          //clears the data object*/
    mqtt_client.publish((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_client_network).c_str(), mqtt.mqtt_buffer);      //send to server
}

void update_mqtt_publish(){
    //publish mqtt data to mqtt broker
    if (settings.config.mqtt_mode == 1) {
        mqtt_publish();
    }
}

// mqtt callback for message receive
// Topic: librelinkup/cmd
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  
    mqtt.mqtt_incomming_cmd = "";

    for (unsigned int i=0;i<length;i++) {
        mqtt.mqtt_incomming_cmd += (char)payload[i];
    }
    /*
    DBGprint;
    Serial.print(F("mqtt Message arrived ["));
    Serial.print(topic);
    Serial.print(F("]: "));
    Serial.println(mqtt.mqtt_incomming_cmd);
    */

    // try to Deserialize the JSON document "{"cmd":"cmd_type","parameter1":value1,"parameter2":value2}"
    DeserializationError error = deserializeJson(json_mqtt, mqtt.mqtt_incomming_cmd);

    // Test if parsing succeeds.
    if (error) {
        DBGprint;Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
    }
    else {
        // Fetch values.
        const char* cmd  = json_mqtt["cmd"];
        float parameter1 = json_mqtt["parameter1"];
        float parameter2 = json_mqtt["parameter2"];
        
        DBGprint;Serial.print(F("CMD:"));Serial.print(cmd);Serial.print(" ");Serial.print(F("parameter1:"));Serial.print(parameter1);Serial.print(" ");Serial.print(F("parameter2:"));Serial.println(parameter2);
        
        json_mqtt.clear();
        bool cmd_ok = 0;

        //-----------[commands]---------------
        if(strcmp(cmd, "reset") == 0){
            ESP.restart();
            cmd_ok = 1;
        return;
        }

        if(strcmp(cmd, "brightness") == 0){
            //set LCD BL brightness
            settings.config.brightness = set_trgb_backlight_brightness(parameter1);
            config_sleep_timer_backup = millis();
            logger.notice("TRGB sleep timer: %d , MQTT Brigness Setting: %d ",config_sleep_timer_backup, parameter1);
            cmd_ok = 1;
        }

        if(strcmp(cmd, "ota_server_mode") == 0){
            if(parameter1 == 1){
                settings.config.ota_update = 1;
                //start OTA Web server
                server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
                request->send(200, "text/plain", "ESP32 LibreLinkup Client");
                });
                ElegantOTA.begin(&server);    // Start ElegantOTA
                server.begin();
                cmd_ok = 1;
            }else if(parameter1 == 0){
                settings.config.ota_update = 0;
                server.end();                    // Stop Server
                cmd_ok = 1;
            }   
        }

        if(strcmp(cmd, "wg_mode") == 0){
            if(parameter1 == 1){
                settings.config.wg_mode = 1;
                setup_wg(1);                  //start WireGuard server
                cmd_ok = 1;
            }else if(parameter1 == 0){
                settings.config.wg_mode = 0;
                setup_wg(0);                    // Stop Server
                cmd_ok = 1;
            }   
        }

        if(strcmp(cmd, "mqtt_mode") == 0){
            if(parameter1 == 1){
                settings.config.mqtt_mode = 1;
                cmd_ok = 1;
            }else if(parameter1 == 0){
                settings.config.mqtt_mode = 0;
                cmd_ok = 1;
            }   
        }
        //-----------[commands end]---------------

        // send command back for host verification

        json_mqtt["cmd"]        = cmd;
        json_mqtt["parameter1"] = parameter1;    //for better rounding
        json_mqtt["parameter2"] = parameter2;    //for better rounding
        json_mqtt["cmd_ok"]     = cmd_ok;

        serializeJson(json_mqtt, mqtt.mqtt_buffer);                 //do serialation and copy into buffer   
        json_mqtt.clear();                                          //clears the data object

        //DBGprint; Serial.println(mqtt_buffer);
        mqtt_client.publish((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_rec_toppic).c_str(), mqtt.mqtt_buffer);      //send data out to host
        mqtt.mqtt_incomming_cmd = "";                              //reset command string for new command
    }
    
    if(mqtt.mqtt_incomming_cmd != ""){
        DBGprint;Serial.println(F("mqtt command received"));
    }
    
    // send updated date back to broker
    mqtt_publish();
}

//---------------------------[lvgl functions]---------------------------------
const int LVGL_TICK_RATE_MS = 1;  // Erhöhe alle 5 ms

void lv_tick_task(void *pvParameter) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(LVGL_TICK_RATE_MS);
    
    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        lv_tick_inc(LVGL_TICK_RATE_MS);  // Inkrementiere den LVGL-Tick um 5 ms
    }
}

//------------------------------------------------------------------------------
int16_t get_last_valid_x_position(lv_obj_t *chart, lv_chart_series_t *series) {
    uint16_t point_count = lv_chart_get_point_count(chart);
    lv_coord_t *y_array = lv_chart_get_y_array(chart, series);

    for (int i = point_count - 1; i >= 0; i--) {
        if (y_array[i] != LV_CHART_POINT_NONE) {
            return lv_chart_get_x_start_point(chart, series) + i; // Letzte X-Position berechnen
        }
    }

    return -1; // Kein gültiger Punkt gefunden
}

static void highlight_last_point() {
    const uint8_t x_pos_offset = 24;
    const uint8_t y_pos_offset = 12;

    // **Letzten gespeicherten Wert aus dem Array holen**
    int16_t last_value = librelinkup.llu_sensor_history_data.graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1];
    
    if (last_value == LV_CHART_POINT_NONE || librelinkup.llu_sensor_history_data.graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1] == 0){
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN); // hide object
        return;  // Falls kein Wert vorhanden, nichts tun
    }else{
        lv_obj_clear_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    }

    // **X-Position berechnen** (Skalierung der Punkte beachten)
    lv_coord_t chart_width = lv_obj_get_width(ui_Chart_Glucose_5Min);
    uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1;
    
    lv_coord_t x_pos = ((chart_width * last_index) / (librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE)) - x_pos_offset;

    // **Y-Position berechnen** (Y-Werte auf Chart-Höhe skalieren)
    lv_coord_t y_min = 40;  
    lv_coord_t y_max = 225;
    lv_coord_t chart_height = lv_obj_get_height(ui_Chart_Glucose_5Min);
    lv_coord_t y_pos = (chart_height - ((last_value - y_min) * chart_height) / (y_max - y_min)) - y_pos_offset;
    
    // **Set marker color
    if(last_value >= librelinkup.llu_glucose_data.glucosetargetHigh || last_value <= librelinkup.llu_glucose_data.glucoseAlarmLow){
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_RED), 0);
    }else{
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    // **Position des Markers setzen**
    lv_obj_set_pos(ui_Chart_Glucose_5Min_last_point_marker, x_pos, y_pos);  // Korrektur um den Mittelpunkt zu treffen
    
    // **Marker in den Vordergrund bringen**
    lv_obj_move_foreground(ui_Chart_Glucose_5Min_last_point_marker);
}

static void brightness_on_off_cb(lv_event_t * event)
{
    //DBGprint; Serial.println("LCD Clicked");
    logger.notice("Button Longpress for Brightness ON/OFF triggered!");

    ledcSetup(0, 5000, 8);                              // 0-15, 5000, 8
    ledcAttachPin(LCD_BL, 0);         // EXAMPLE_PIN_NUM_BK_LIGHT, 0 - 15
    
    if(settings.config.brightness == 0){
        for(uint8_t i=0;i<TRGB_STD_BACKLIGHT_BRIGHTNESS;i++){
            ledcWrite(0, i); // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
            delay(30);
        }
        settings.config.brightness = TRGB_STD_BACKLIGHT_BRIGHTNESS;

    }else{
        for(uint8_t i=settings.config.brightness;i>0;i--){
            ledcWrite(0, i); // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
            delay(30);
        }
        ledcWrite(0, 0); // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
        settings.config.brightness = 0;
    }
    
}

static void touch_gesture_cb(lv_event_t * event)
{
    logger.debug("Touch gesture:,%d",lv_indev_get_gesture_dir(lv_indev_get_act()));

    lv_obj_t * screen = (lv_obj_t *)lv_event_get_current_target(event); // Expliziter Cast notwendig
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(event)); // Korrekte Methode für LVGL 9

    switch(dir) {
        case LV_DIR_LEFT:
            logger.debug("Touch gesture: left");
            if(lv_scr_act() == ui_Login_screen){
                lv_disp_load_scr(ui_Debug_screen);
            }
            else if(lv_scr_act() == ui_Debug_screen){
                lv_disp_load_scr(ui_Main_screen);
            }      
            break;
        case LV_DIR_RIGHT:
            logger.debug("Touch gesture: right");
            if(lv_scr_act() == ui_Main_screen){
                lv_disp_load_scr(ui_Debug_screen);
            }
            else if(lv_scr_act() == ui_Debug_screen){
                lv_disp_load_scr(ui_Login_screen);
                break;
            }
            break;
        case LV_DIR_TOP:
            logger.debug("Touch gesture: Top");
            break;
        case LV_DIR_BOTTOM:
            logger.debug("Touch gesture: bottom");
            break;
    }
}

static void btn_wireguard_cb(lv_event_t * event){
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_CLICKED) {
        LV_LOG_USER("Clicked");
        logger.debug("Wireguard Mode Button clicked");
        settings.config.wg_mode ^= 1;
        settings.saveConfiguration(settings.config_filename, settings.config);
        setup_wg(settings.config.wg_mode);
    }
    else if(code == LV_EVENT_VALUE_CHANGED) {
        LV_LOG_USER("Toggled");
        
    }
}

static void ta_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ui_kb = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(ui_kb, LV_OBJ_FLAG_HIDDEN); // Tastatur verstecken
    }
}

static void btn_mqtt_cb(lv_event_t * event){
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_CLICKED) {
        LV_LOG_USER("Clicked");
        logger.debug("MQTT Mode Button clicked");
        settings.saveConfiguration(settings.config_filename, settings.config);
        if(mqtt.mqtt_enable == 0){
            mqtt.mqtt_enable = 1;
            logger.debug("MQTT client connect");
        }else if(mqtt.mqtt_enable == 1){
            mqtt.mqtt_enable = 0;
            logger.debug("MQTT client disconnected");
            mqtt_client.unsubscribe((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
            mqtt_client.disconnect();
        }
    }
    else if(code == LV_EVENT_VALUE_CHANGED) {
        LV_LOG_USER("Toggled");
        
    }
}

static void btn_ota_cb(lv_event_t * event){
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_CLICKED) {
        LV_LOG_USER("Clicked");
        logger.debug("OTA Mode Button clicked");
        settings.saveConfiguration(settings.config_filename, settings.config);
        if(settings.config.ota_update == 1){
            settings.config.ota_update = 0;
            setup_OTA(settings.config.ota_update);
        }else if(settings.config.ota_update == 0){
            settings.config.ota_update = 1;
            setup_OTA(settings.config.ota_update);
        }
    }
    else if(code == LV_EVENT_VALUE_CHANGED) {
        LV_LOG_USER("Toggled");
    }
}

static void btn_login_event_cb(lv_event_t *event) {
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        const char* email = lv_textarea_get_text(ui_ta_email);
        const char* password = lv_textarea_get_text(ui_ta_password);
        settings.config.login_email = email;
        settings.config.login_password = password;
        Serial.printf("Entered Email: %s\n", settings.config.login_email);
        Serial.printf("Entered Password: %s\n", settings.config.login_password);
        // done
        settings.saveConfiguration(settings.config_filename, settings.config);        
        lv_disp_load_scr(ui_Main_screen);
    }
}

static int last_x = -1;  // Letzte X-Position speichern

static void touch_event_cb(lv_event_t * e) {
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_indev_t * indev = lv_event_get_indev(e);
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // **X-Koordinate relativ zum Chart berechnen**
    lv_coord_t chart_x = lv_obj_get_x(ui_Chart_Glucose_5Min);
    lv_coord_t relative_x = point.x - chart_x;

    // **X-Koordinate in Datenpunkt-Index umwandeln**
    int num_points = lv_chart_get_point_count(ui_Chart_Glucose_5Min);
    int index = (relative_x * num_points) / lv_obj_get_width(ui_Chart_Glucose_5Min);
    if (index >= num_points) index = num_points - 1;
    if (index < 0) index = 0;

    // **Y-Wert aus der Chart-Serie holen**
    int16_t value = librelinkup.llu_sensor_history_data.graph_data[index];

    // Falls kein gültiger Wert → Marker verstecken
    if (value == LV_CHART_POINT_NONE || value == 0) {
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
        return;
    } else {
        lv_obj_clear_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    }

    // **Chart-Dimensionen ohne Padding verwenden**
    lv_coord_t chart_height = lv_obj_get_height(ui_Chart_Glucose_5Min);
    lv_coord_t y_min = 40;  
    lv_coord_t y_max = 225;

    // **Y-Wert auf Chart-Höhe skalieren (Padding entfernt)**
    lv_coord_t y_pos = chart_height - ((value - y_min) * chart_height) / (y_max - y_min);

    // **Offset-Korrektur für exakte Position**
    const uint8_t x_pos_offset = 24;
    const uint8_t y_pos_offset = 6;

    // **Marker-Farbe setzen**
    if (value >= librelinkup.llu_glucose_data.glucosetargetHigh || value <= librelinkup.llu_glucose_data.glucoseAlarmLow) {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_RED), 0);
    } else {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    // **Marker-Position setzen**
    lv_obj_set_pos(ui_Chart_Glucose_5Min_last_point_marker, relative_x - x_pos_offset, y_pos - y_pos_offset);

    // **Marker in den Vordergrund bringen**
    lv_obj_move_foreground(ui_Chart_Glucose_5Min_last_point_marker);

    // **Y-Wert als Label anzeigen**
    uint8_t mode = 1;
    uint8_t color = (value >= librelinkup.llu_glucose_data.glucosetargetHigh || value <= librelinkup.llu_glucose_data.glucoseAlarmLow) ? LV_PALETTE_RED : LV_PALETTE_GREEN;
    uint16_t glucose_value = value;
    
    draw_labels(mode, color, glucose_value, librelinkup.llu_glucose_data.str_trendArrow, librelinkup.llu_glucose_data.str_TrendMessage, 0);
    
    // **Debugging-Ausgabe**
    logger.notice("Touch X (rel): %d, Index: %d, Value: %d, Y: %d", relative_x, index, value, y_pos);

    // Letzte X-Position speichern
    last_x = relative_x;
}

//------------------------------------------------------------------------------

// lcd status indication
void lcd_status_indication(bool on_off, uint8_t color){
    if(on_off == 0){
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, " " );
    }else if(on_off == 1){
        switch (color)
        {
        case 0:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT); 
            break;
        case 1:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT); 
            break;
        case 2:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT); 
            break;

        default:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        }
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, "*" );
    }
    lv_timer_handler();delay(5);
}

//---------------------------[librelinkup functions]---------------------------------
void draw_chart_sensor_valid(){

    // draw valid days chart
    if(librelinkup.sensor_livetime.sensor_valid_days > 0 && librelinkup.sensor_livetime.sensor_valid_hours >= 0){
        switch_sensor_valid_progress_bar(&dayBar, 0);
        update_chart_valid_values(&dayBar,librelinkup.sensor_livetime.sensor_valid_days +1);
    }
    else if(librelinkup.sensor_livetime.sensor_valid_days == 0 && (librelinkup.sensor_livetime.sensor_valid_hours > 0 && librelinkup.sensor_livetime.sensor_valid_hours < 24)){
        switch_sensor_valid_progress_bar(&hourBar, 1);
        update_chart_valid_values(&hourBar, librelinkup.sensor_livetime.sensor_valid_hours +1);   
    }
    else if(librelinkup.sensor_livetime.sensor_valid_days == 0 && librelinkup.sensor_livetime.sensor_valid_hours == 0 && librelinkup.sensor_livetime.sensor_valid_minutes < 60){
        switch_sensor_valid_progress_bar(&minuteBar, 2);
        update_chart_valid_values(&minuteBar, librelinkup.sensor_livetime.sensor_valid_minutes +1);
    }
    
    lv_timer_handler();delay(5);
}

// 🟢 Zeitformatierung
void format_time_label(char *buffer, size_t buffer_size, time_t timestamp) {
    struct tm *tm_info = localtime(&timestamp);
    if (tm_info->tm_hour == 0 && tm_info->tm_min == 0) {
        snprintf(buffer, buffer_size, "00:00*"); // Tageswechsel
    } else if (tm_info->tm_hour == 12 && tm_info->tm_min == 0) {
        snprintf(buffer, buffer_size, "12:00");  // Mittag
    } else if (tm_info->tm_min == 0 && (tm_info->tm_hour % 3 == 0)) {
        snprintf(buffer, buffer_size, "%02d:00", tm_info->tm_hour); // alle 3 Stunden
    } else {
        snprintf(buffer, buffer_size, ""); // kein Label
    }
}

// 🟢 Labels manuell hinzufügen
void add_axis_labels() {
     
    // ✅ X-Achse: Nur drei Labels (Anfang, Mitte, Ende)
    static char labels[3][6]; // "HH:MM" + Null-Terminator
    uint8_t data_count = librelinkup.check_graphdata();
    
    // Ersten, mittleren und letzten Zeitstempel ermitteln
    uint32_t first_timestamp = librelinkup.llu_sensor_history_data.timestamp[0];
    uint32_t middle_timestamp = librelinkup.llu_sensor_history_data.timestamp[data_count / 2];
    uint32_t last_timestamp = librelinkup.llu_sensor_history_data.timestamp[data_count - 1];

    // Zeitstempel formatieren
    helper.format_time(labels[0], sizeof(labels[0]), first_timestamp);  // Erster Zeitstempel
    helper.format_time(labels[1], sizeof(labels[1]), middle_timestamp); // Mittlerer Zeitstempel
    helper.format_time(labels[2], sizeof(labels[2]), last_timestamp);   // Letzter Zeitstempel

    // Label für den ersten Zeitstempel (ganz links)
    lv_label_set_text(ui_Chart_x_label_start, labels[0]);

    // Label für den mittleren Zeitstempel (Mitte)
    lv_label_set_text(ui_Chart_x_label_middle, labels[1]);

    // Label für den letzten Zeitstempel (ganz rechts)
    lv_label_set_text(ui_Chart_x_label_end, labels[2]);

}
// draw glucose chart mode=0/1/3 (limit_lines/historical_data/limit&data)
void draw_chart_glucose_data(uint8_t mode, bool fiveminuteupdate) {
    if (mode == 0 || mode == 3) {
        // Draw limit lines
        lv_chart_set_x_start_point(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, 0);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_upperlimit, librelinkup.llu_glucose_data.glucosetargetHigh);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_lowerlimit, librelinkup.llu_glucose_data.glucosetargetLow);
    }

    if (mode == 1 || mode == 3) {
        uint16_t glucose_value = 0;
        static uint8_t data_count_backup = 0;
        uint8_t data_count = librelinkup.check_graphdata();
        
        // Alle bisherigen Punkte löschen
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_last, LV_CHART_POINT_NONE);

        // Durch alle historischen Daten iterieren
        uint32_t sensor_active_time = (librelinkup.sensor_livetime.sensor_valid_days * 24 * 60 * 60) + 
                                      (librelinkup.sensor_livetime.sensor_valid_hours * 60 * 60) + 
                                      (librelinkup.sensor_livetime.sensor_valid_minutes * 60);
        
        if (sensor_active_time <= librelinkup.TIMEFULLGRAPHDATA) {
            for (int i = 0; i < librelinkup.GRAPHDATAARRAYSIZE; i++) {
                uint8_t index = (librelinkup.GRAPHDATAARRAYSIZE-1)-i;  // Rechtsbündig füllen

                if (index < 0 || index >= librelinkup.GRAPHDATAARRAYSIZE) continue;  // Sicherheitsprüfung

                glucose_value = librelinkup.llu_sensor_history_data.graph_data[index];

                if (glucose_value != 0) {
                    if (glucose_value > librelinkup.llu_glucose_data.glucosetargetHigh || glucose_value < librelinkup.llu_glucose_data.glucosetargetLow) {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, index, glucose_value);
                    } else {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, index, glucose_value);
                    }
                } else {
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,  index, LV_CHART_POINT_NONE);
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, index, LV_CHART_POINT_NONE); 
                }
            }
        }
        else { // Falls der Sensor länger aktiv ist nach den ersten 12h
            logger.debug("Draw graph startup time");
            
            for (int i = 0; i < data_count; i++) {
                uint8_t index = (data_count-1)-i;
                glucose_value = librelinkup.llu_sensor_history_data.graph_data[index];
                
                if(glucose_value != 0){
                    if(glucose_value > librelinkup.llu_glucose_data.glucosetargetHigh || glucose_value < librelinkup.llu_glucose_data.glucosetargetLow){
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, 
                                            (librelinkup.GRAPHDATAARRAYSIZE-1)-i, glucose_value);
                    }else {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, 
                                            (librelinkup.GRAPHDATAARRAYSIZE-1)-i, glucose_value);
                    }
                }
            }
        }

        // Aktuellen Messwert setzen bei Index 141
        uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE;  // Index 141 ist jetzt der aktuelle Messwert

        if (librelinkup.llu_status.timestamp_status == SENSOR_TIMECODE_VALID) {
            librelinkup.llu_sensor_history_data.graph_data[last_index] = librelinkup.llu_glucose_data.glucoseMeasurement;

            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, last_index, librelinkup.llu_glucose_data.glucoseMeasurement);

            // Highlight den letzten Punkt
            highlight_last_point();
        } else {
            // Keine Daten → Lücke
            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, last_index, LV_CHART_POINT_NONE);
        }

        // X-Achsenbeschriftungen hinzufügen
        add_axis_labels();

        // Chart aktualisieren
        lv_obj_invalidate(ui_Chart_Glucose_5Min);
        //lv_task_handler();delay(5);
    }
}

void draw_labels(uint8_t mode, uint8_t _glucose_measurement_color, uint16_t _glucose_value, String _trendarrow, String _trendmessage, int16_t delta){
    
    if(mode == 0){
        if(_glucose_measurement_color == COLOR_WHITE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        }else if(_glucose_measurement_color == COLOR_YELLOW){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        }else if(_glucose_measurement_color == COLOR_ORANGE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }else if(_glucose_measurement_color == COLOR_RED){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }else if(_glucose_measurement_color == COLOR_BLUE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }else{
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lv_label_set_text(ui_Label_GlucoseValue, "---" );
        lv_label_set_text(ui_Label_GlucoseDelta, "--- mg/dL");
        lv_label_set_text(ui_Label_GlucoseTrendArrow, "-");
    }else if(mode == 1){
        if(_glucose_measurement_color == COLOR_WHITE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        }else if(_glucose_measurement_color == COLOR_YELLOW){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        }else if(_glucose_measurement_color == COLOR_ORANGE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }else if(_glucose_measurement_color == COLOR_RED){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }else if(_glucose_measurement_color == COLOR_BLUE){
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF), LV_PART_MAIN | LV_STATE_DEFAULT); 
        }
 
        char buf_label1[4];
        snprintf(buf_label1, 4, "%d", _glucose_value);
        lv_label_set_text(ui_Label_GlucoseValue, buf_label1);
        lv_label_set_text(ui_Label_GlucoseTrendArrow, _trendarrow.c_str());
        
        char buf_label3[14];
        if(delta == 0){
            snprintf(buf_label3, 14, "±%d mg/dL", delta);           //get the "+" for positive values
        }else if(delta > 0){
            snprintf(buf_label3, 14, "+%d mg/dL", delta);
        }else if(delta < 0){
            snprintf(buf_label3, 14, "%d mg/dL", delta);
        }
        lv_label_set_text(ui_Label_GlucoseDelta, buf_label3);
    }
    // Show Glucose TrendMessage, if available
    if(strcmp(_trendmessage.c_str(), "null") != 0){
        lv_label_set_text(ui_Label_GlucoseTrendMessage, _trendmessage.c_str());
    }else{
        lv_label_set_text(ui_Label_GlucoseTrendMessage, "" );
    }
}

void handle_internet_disconnection() {
    static uint8_t counter_internet_offline = 0;
    if (++counter_internet_offline == 5) {
        counter_internet_offline = 0;
        logger.notice("Client offline -> reconnect to WiFi");
        esp_status_counter_wifi_restart++;
        WiFi.disconnect();
        WiFi.reconnect();
    }
    draw_labels(false, librelinkup.llu_glucose_data.measurement_color, librelinkup.llu_glucose_data.glucoseMeasurement,
                librelinkup.llu_glucose_data.str_trendArrow, librelinkup.llu_glucose_data.str_TrendMessage, 0);
}

void handle_llu_api_error() {
    internet_status = 2;
    lcd_status_indication(0, 1);
    librelinkup.sensor_reconnect = 1;
    logger.notice("API Error: get graph data");
}

void handle_sensor_reconnect() {
    logger.notice("Sensor reconnect!");
    librelinkup.sensor_reconnect = 0;
    glucose_delta = 0;
    draw_chart_glucose_data(3, false);
}

void handle_invalid_timestamp() {
    
    draw_labels(false, librelinkup.llu_glucose_data.measurement_color, librelinkup.llu_glucose_data.glucoseMeasurement,
                    librelinkup.llu_glucose_data.str_trendArrow, librelinkup.llu_glucose_data.str_TrendMessage, 0);

    if (librelinkup.llu_status.timestamp_status == SENSOR_TIMECODE_OUT_OF_RANGE) {
        logger.notice("glucoseMeasurement: no valid sensor data");
        librelinkup.sensor_reconnect = 1;
    }

    static uint8_t invalid_timestamp_counter = 0;
    if (librelinkup.llu_status.timestamp_status == SENSOR_TIMECODE_ERROR &&
        librelinkup.llu_status.sensor_state == SENSOR_NOT_AVAILABLE) {
        
        if (++invalid_timestamp_counter == 5) {
            esp_status_counter_llu_reauth++;
            logger.notice("LLU Re-auth...");
            librelinkup.auth_user(settings.config.login_email, settings.config.login_password);
            if (librelinkup.llu_login_data.user_login_status == 4) {
                esp_status_counter_llu_retou++;
                librelinkup.tou_user();
            }
        }

        if (invalid_timestamp_counter == 10) {
            invalid_timestamp_counter = 0;
            logger.notice("invalid_timestamp_counter out of range! -> call restart");
            // esp_restart();
        }
    }
}

void synchronize_time_offset() {
    helper.timedifference = helper.synchronizeWithServer(librelinkup.librelinkuptimecode.hour,
                                                         librelinkup.librelinkuptimecode.minute,
                                                         librelinkup.librelinkuptimecode.second,
                                                         librelinkup.localtime.hour,
                                                         librelinkup.localtime.minute,
                                                         librelinkup.localtime.second);
    g_timer_60000ms_backup = (helper.timedifference < helper.TIME_DIFF_THRESHOLD) ? millis()
                                                              : millis() + (helper.timedifference + librelinkup.https_llu_api_fetch_time);
}

void update_trend_message() {
    char buffer[30];  // Puffer für den String
    int remaining_time = 0;

    switch (librelinkup.llu_status.sensor_state) {
        case SENSOR_EXPIRED:
            librelinkup.llu_glucose_data.str_TrendMessage = "sensor expired!";
            logger.notice("sensor expired!");
            break;
        case SENSOR_NOT_AVAILABLE:
            librelinkup.llu_glucose_data.str_TrendMessage = "no active sensor";
            logger.notice("no active sensor");
            break;
        case SENSOR_STARTING:
            remaining_time = librelinkup.get_remaining_warmup_time(librelinkup.llu_sensor_data.sensor_non_activ_unixtime);
            sprintf(buffer, "sensor ready in %d min", remaining_time);
            librelinkup.llu_glucose_data.str_TrendMessage = buffer;
            logger.notice("Sensor in starting phase!");
            break;
        case SENSOR_READY:
            librelinkup.llu_glucose_data.str_TrendMessage = "";
            break;
    }
}

void update_five_minute_counter() {
    
    static uint8_t five_minute_chart_update_counter = 5;            // counter that redraws the glucose chat ever 5 minutes
    
    // Zähler dekrementieren
    five_minute_chart_update_counter--;

    // Debug-Log zur Überprüfung
    logger.debug("five_minute_chart_update_counter: %d", five_minute_chart_update_counter);

    // Wenn der Zähler auf 0 fällt, Chart-Update durchführen
    if (five_minute_chart_update_counter <= 0) {  
        five_minute_chart_update_counter = 5;  // Zurücksetzen auf 5 Minuten
        logger.debug("Triggering 5-minute chart update...");
        draw_chart_glucose_data(1, true);  // Führe das 5-Minuten-Update aus
        
        if(librelinkup.llu_status.sensor_state == SENSOR_READY){
            update_glucose_json_logging();
            glucose_statistics(); // print glucose statistics
        }
    }
}

void update_glucose_data() {
    
    if (WiFi.status() != WL_CONNECTED) {
        handle_internet_disconnection();
        return;
    }

    glucose_delta = 0;
    lcd_status_indication(1, 1);

    if (librelinkup.get_graph_data() == 0) {
        handle_llu_api_error();
        return;
    }

    lcd_status_indication(0, 1);

    logger.debug("LLU API fetch time: %dms", librelinkup.https_llu_api_fetch_time);

    // Sensorstatus und Zeitstempel auslesen
    librelinkup.llu_status.sensor_state = librelinkup.check_sensor_lifetime(librelinkup.llu_sensor_data.sensor_non_activ_unixtime);
    librelinkup.llu_status.timestamp_status = librelinkup.check_valid_timestamp(librelinkup.llu_glucose_data.str_measurement_timestamp, 1);
    librelinkup.llu_status.last_timestamp_unixtime = helper.convertStrToUnixTime(librelinkup.llu_glucose_data.str_measurement_timestamp);

    // Set TrendMessage based on sensor status
    update_trend_message();
    
    // check if LLU Timestamp is valid and process data
    if (librelinkup.llu_status.timestamp_status == SENSOR_TIMECODE_VALID) {
        
        synchronize_time_offset();

        if (librelinkup.sensor_reconnect == 1) {
            handle_sensor_reconnect();
        } else {
            glucose_delta = librelinkup.llu_glucose_data.glucoseMeasurement - glucoseMeasurement_backup;
        }

        logger.notice("glucoseMeasurement: %d %s ∆: %d", librelinkup.llu_glucose_data.glucoseMeasurement,
                      librelinkup.llu_glucose_data.str_trendArrow.c_str(), glucose_delta);

        draw_chart_sensor_valid();
        draw_labels(true, librelinkup.llu_glucose_data.measurement_color, librelinkup.llu_glucose_data.glucoseMeasurement,
                    librelinkup.llu_glucose_data.str_trendArrow, librelinkup.llu_glucose_data.str_TrendMessage, glucose_delta);
        draw_chart_glucose_data(3, false);

        glucoseMeasurement_backup = librelinkup.llu_glucose_data.glucoseMeasurement;
    } else {
        handle_invalid_timestamp();
        draw_labels(false, librelinkup.llu_glucose_data.measurement_color, librelinkup.llu_glucose_data.glucoseMeasurement,
                librelinkup.llu_glucose_data.str_trendArrow, librelinkup.llu_glucose_data.str_TrendMessage, 0);
    }
}

void update_glucose_json_logging(){
    uint32_t unixtime_now = librelinkup.get_epoch_time();
    hba1c.addGlucoseValue(unixtime_now, librelinkup.llu_glucose_data.glucoseMeasurement);
    logger.debug("addGlucoseValue to LittleFS: %d / %d", unixtime_now, librelinkup.llu_glucose_data.glucoseMeasurement );
}

void glucose_statistics(){
    uint8_t data_count = librelinkup.check_graphdata();
    float mean_glucose_value_from_history = hba1c.calculateGlucoseMeanFromHistory(librelinkup.llu_sensor_history_data.graph_data, data_count);
    float mean_glucose_value_from_json = hba1c.calculateGlucoseMeanFromJson(today_json_filename);
    float mean_glucose_weekly_value_from_json = hba1c.calculateGlucoseMeanForLast7Days();
    float std_dev = hba1c.calculate_standard_deviation(librelinkup.llu_sensor_history_data.graph_data, data_count, mean_glucose_value_from_history);
    logger.notice("========== Glucose Statistics =============", mean_glucose_value_from_history);
    logger.notice("current glucose value        : %d mg/dl", librelinkup.llu_glucose_data.glucoseMeasurement);
    logger.notice("mean of histroy glucose value: %.0f mg/dl", mean_glucose_value_from_history);
    logger.notice("mean of weekly glucose value : %.0f mg/dl", mean_glucose_weekly_value_from_json);
    logger.notice("HbA1c-Value of histroy data  : %.2f %%", hba1c.calculate_hba1c(mean_glucose_value_from_history));
    logger.notice("TIR-Value of histroy data    : %.2f %%", hba1c.calculate_time_in_range(librelinkup.llu_sensor_history_data.graph_data, data_count, 70, 180));
    logger.notice("Std-Dev of histroy data      : %.2f σ", std_dev);
    logger.notice("Glukosevariabilität          : %.2f cv", hba1c.calculate_coefficient_of_variation(std_dev, mean_glucose_value_from_history));
    logger.notice("===========================================");
}   

//-------------------------[ Loop Task ]-------------------------------
void LoopTask(void *pvParameters) {
    Serial.println("Loop Task gestartet...");
    while (1) {
        
        // ElegantOTA
        ElegantOTA.loop();              // OTA-Loop in einer separaten Task ausführen
        
        //mqtt client
        mqtt_client.loop();            //loop and keep mqtt client to server 

        //telnet loop
        uuid::loop();
        telnet.loop();
        Shell::loop_all();yield();

        vTaskDelay(pdMS_TO_TICKS(1));  // Task nicht blockieren
    }
}

//---------------------------[Setup Hardware]--------------------------------
// setup Serial
void setup_serial(){
    Serial.begin(115200); 
    Serial.setTxTimeoutMs(1);  // workaround for blocking output if no host is connected to native USB CDC
    Serial.println(); delay(500);
    DBGprint; Serial.println(F("Libre Link Up Api client with lvgl"));
}

// setup LittleFS
void setup_littlefs(){
    if(!LittleFS.begin()){
        DBGprint;Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }
}

void setup_tpanels3(){

    // LV_Tick Task
    xTaskCreatePinnedToCore(
        lv_tick_task,   // Task-Funktion
        "lv_tick_task", // Name des Tasks
        2048,           // Stack-Größe (in Bytes)
        NULL,           // Parameter (optional)
        1,              // Priorität (1 ist niedrig)
        NULL,           // Task-Handle (optional)
        1               // Core (0 oder 1)
    );

    // init Backlight
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, 0x20); // set backlight to 50%

    set_trgb_backlight_brightness(15); // set backlight to 15%

    // init Touch Pin Interrupt
    attachInterrupt(
        TOUCH_INT,
        []
        {
            Touch_Int_Flag = true;
            // Serial.println("get_int");
        },
        FALLING); 

    Wire.begin(IIC_SDA, IIC_SCL);

    gfx->begin();
    gfx->setRotation(2); // rotation = 0, 1, 2, 3  entspricht 0°, 90°, 180°, 270°
    gfx->fillScreen(BLACK);

    gfx->XL_digitalWrite(XL95X5_TOUCH_RST, LOW);
    delay(200);
    gfx->XL_digitalWrite(XL95X5_TOUCH_RST, HIGH);
    delay(200);

    touch.init();
    
    lvgl_initialization();

    // inits LV widgets
    ui_init();
    lv_label_set_text(ui_Label_WelcomeInfo, "LibreLinkUp\nClient" );
    lv_timer_handler(); /* let the GUI do its work */
    delay(1000);
}

// setup config
void setup_load_system_config(){
    settings.loadConfiguration(settings.config_filename, settings.config);
    librelinkup.timezone = settings.config.timezone;
}

// setup WiFi connection
void setup_wifi() {
  
    lv_label_set_text(ui_Label_WelcomeWifiInfo, "connecting to Wifi..." );
    lv_timer_handler();
    
    // Set in station mode
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm); //+10dBm
     
    // Register multi WiFi networks
    wifiMulti.addAP(settings.config.wifi_bssid.c_str(), settings.config.wifi_password.c_str());
    
    DBGprint; Serial.println(F("connecting to Wifi..."));

    if (wifiMulti.run(connectTimeoutMs) == WL_CONNECTED) {
        DBGprint; Serial.print(F("SSID:"));Serial.print(WiFi.SSID()); Serial.print(F(" IP:")); Serial.print(WiFi.localIP());Serial.print(F(" RSSI: "));Serial.println(WiFi.RSSI()); 
        
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "connected!" );
        lv_timer_handler();
        delay(1000);
        
        lv_label_set_text(ui_Label_WelcomeWifiInfo, WiFi.localIP().toString().c_str());
        lv_timer_handler();

        //get local time from ntp Server
        DBGprint;Serial.println("Adjusting system time.from ntp server..");
        //configTime(2 * 60 * 60, 0, "pool.ntp.org", "ntp.nict.jp", "time.google.com");        
        configTime(0,0,"pool.ntp.org", "ntp.nict.jp", "time.google.com");
        setenv("TZ",tz,1);
        tzset();
        helper.printLocalTime(0);
        
        //The ESP32 tries to reconnect automatically when the connection is lost
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);

        // Disable AP mode once connected
        if (WiFi.softAPgetStationNum() == 0) { // Only if no clients are connected
            WiFi.softAPdisconnect(true);
            DBGprint;Serial.println("AP disabled, connected to WiFi");
            logger.notice("AP disabled, connected to WiFi");
        }

    } else {
        // no wifi connection, start AP for configuration
        settings.config.wifi_bssid = "";
        settings.config.wifi_password = "";
        settings.saveConfiguration(settings.config_filename, settings.config);
        WiFi.softAP(settings.apSSID, settings.apPassword);
        DBGprint;Serial.println("Access Point started, AP:");
        DBGprint;Serial.println(settings.apSSID);
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "Wifi AP 192.168.4.1" );
        lv_timer_handler();
        delay(1000);
    }   
}

// setup Wireguard Client
void setup_wg(bool enable){

    if (enable == 1){
        IPAddress local_ip = helper.parseIPAddress(settings.config.wgIpAddress);
        logger.notice("local_ip: %d %s", local_ip, local_ip);
        if(!wg.is_initialized()){
            DBGprint; Serial.println("Initializing WG interface...");
            if( !wg.begin(
                    local_ip,
                    settings.config.wgPrivateKey.c_str(),
                    settings.config.wgEndpoint.c_str(),
                    settings.config.wgPublicKey.c_str(),
                    (uint16_t)settings.config.wgEndpointPort,
                    settings.config.wgPresharedKey.c_str()) ) {
                DBGprint; Serial.println("Failed to initialize WG interface.");
                wg.end();
            }
        }
        else {
            DBGprint; Serial.println("Shutting down WG interface...");
            wg.end();
        }
        
        delay(500);
        
        //check internet connection
        if(Ping.ping(ping_ip)){
            DBGprint; Serial.print("WG connected!"); Serial.print(F(" IP:")); Serial.println(local_ip);
            logger.notice("WG connected! IP: %s, Gateway: %s, SubNet: %s, dnsIP: %s ",local_ip.toString(),WiFi.gatewayIP().toString(),WiFi.subnetMask().toString(), WiFi.dnsIP().toString());
            //logger.notice("Ping %s: OK", ping_ip.toString());
            settings.config.wg_mode = 1;
    
        }else {
            DBGprint; Serial.println("FAILED to ping! WG not connected!");
            logger.notice("Ping %s: NOK", ping_ip.toString());
            settings.config.wg_mode = 0;
        }
    }else if(enable == 0){
        wg.end();
    }
}

// setup MDNS
void setup_mdns(){
    //use mdns for host name resolution
    hostname = hostname_base + helper.get_flashmemory_id();
    if (!MDNS.begin(hostname.c_str())) { 
        DBGprint;Serial.println("Error setting up MDNS responder!");
        logger.notice("Error setting up MDNS responder!");
    }
    DBGprint;Serial.println("mDNS responder started");
    logger.notice("mDNS responder started ... Hostname: %s", hostname.c_str());
}

// setup librelinkup
void setup_librelinkup(){
  if(settings.config.login_email == "" || settings.config.login_password == ""){
    lv_disp_load_scr(ui_Login_screen);
  }
  librelinkup.begin(2);
}

// setup mqtt connection
void setup_mqtt() {
    
    mqtt_client.setServer(mqtt.mqtt_server, mqtt.mqtt_port);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_client.setBufferSize(512);
    
    mqtt.mqtt_client_name = "/" + helper.get_flashmemory_id();
    
    if (!mqtt_client.connected()){
        mqtt_client.connect((mqtt.mqtt_base + mqtt.mqtt_client_name).c_str(), mqtt.mqtt_user, mqtt.mqtt_password);
        mqtt_client.subscribe((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
    }
}

//Setup OTA
void setup_OTA(bool mode){
    
    if(mode == true){
        server.on("/", HTTP_GET, handleRoot);
        server.on("/scan", HTTP_GET, handleScan);
        server.on("/login", HTTP_POST, handleLogin);
        server.on("/connect", HTTP_POST, handleConnect);
        server.on("/status", HTTP_GET, handleStatus);
        server.on("/toggle", HTTP_POST, handleToggleFeature);
        server.on("/setBrightness", HTTP_POST, handleSetBrightness);
        server.on("/configureWireGuard", HTTP_POST, handleConfigureWireGuard);
        server.on("/configureMQTT", HTTP_POST, handleConfigureMQTT);

        xTaskCreatePinnedToCore(        // Separaten Task für den WiFi-Scan starten
            scanWiFiTask,               // Funktion
            "WiFi Scan Task",           // Name des Tasks
            4096,                       // Stack-Größe
            NULL,                       // Parameter
            1,                          // Task-Priorität
            NULL,                       // Task-Handle
            1                           // Core 1 verwenden
        );

        ElegantOTA.begin(&server);      // Start ElegantOTA
        ElegantOTA.onStart(onOTAStart);
        ElegantOTA.onProgress(onOTAProgress);
        ElegantOTA.onEnd(onOTAEnd);

        server.begin();
        DBGprint;Serial.println("HTTP server started");

    }
    else{
        server.end();                // Stop Server
        DBGprint;Serial.println("HTTP server stopped");
    }
}

//Setup Console
void setup_uuid_console(){
    registerCommands(commands);
    telnet.start();
}

//Setup Task for loop funktions
void setup_task(){
    // **Loop Task in separatem Task starten**
    xTaskCreatePinnedToCore(
        LoopTask,         // Funktionsname
        "LoopTask",       // Task-Name
        4096,             // Stack-Größe
        NULL,             // Parameter
        1,                // Priorität
        NULL,             // Task-Handle
        1                 // Core 1
    );
}

//-----------------------------------------------------------------------------


void setup()
{
    //Setup ADC for battery voltage
    setup_adc();
    
    //Setup UART
    setup_serial();

    //Setup LittleFS
    setup_littlefs();

    //Loads Config from file
    setup_load_system_config();

    //setup TPanelS3
    setup_tpanels3();

    //Setup WiFi
    setup_wifi();

    //Setup Telnet
    setup_uuid_console();

    //Setup WireGuard
    setup_wg(settings.config.wg_mode);

    //Setup MDNS
    setup_mdns();

    //Setup mqtt
    setup_mqtt();

    //Setup librelinkup
    setup_librelinkup();

    //Setup HBA1C
    hba1c.begin();

    //Setup OTA
    setup_OTA(settings.config.ota_update);

    //Setup Loop Task
    setup_task();
    //---------------------------------------------------------------------------------------------
    
    //------------------------------[LVGL Callbacks registation]-------------------------------
    // Main screen Touch Callback function
    //lv_obj_add_event_cb(ui_Main_screen, brightness_on_off_cb, LV_EVENT_LONG_PRESSED, NULL);
    //lv_obj_add_event_cb(ui_Chart_Glucose_5Min, brightness_on_off_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_add_event_cb(ui_Chart_Glucose_5Min, touch_event_cb, LV_EVENT_PRESSING, NULL);

    //touch: get touch gesture events
    //lv_obj_add_event_cb(ui_Main_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_Debug_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_btn_wireguard, btn_wireguard_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_mqtt, btn_mqtt_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_ota_update, btn_ota_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_ta_email, ta_event_cb, LV_EVENT_ALL, ui_kb);
    lv_obj_add_event_cb(btn_login, btn_login_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(ui_ta_email, [](lv_event_t *event) {
                    lv_keyboard_set_textarea(ui_kb, ui_ta_email);
                    lv_obj_clear_flag(ui_kb, LV_OBJ_FLAG_HIDDEN);
                    }, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ui_ta_password, [](lv_event_t *event) {
                    lv_keyboard_set_textarea(ui_kb, ui_ta_password);
                    lv_obj_clear_flag(ui_kb, LV_OBJ_FLAG_HIDDEN);
                    }, LV_EVENT_FOCUSED, NULL);
    //---------------------------------------------------------------------------------------------
    
    //get first glycose data
    update_glucose_data();

    // decrease update counter -1 and update if five_minute_chart_update_counter == 0
    update_five_minute_counter();
        
    //publish mqtt data to mqtt broker
    update_mqtt_publish();
    
    //writes first logging data
    update_glucose_json_logging();

    //--------------------------------------------------------------------------------------
    // Change to main screen
    lv_disp_load_scr(ui_Main_screen);

    //---------------------------------------------------------------------------------------------

}

void loop()
{
    //---------- all other loop function in LoopTask() function ---------------------
    /*must be in loop and not in seperate task*/
    /*
    uuid::loop();
    telnet.loop();
    Shell::loop_all();yield();
    */

    //LVGL update
    lv_timer_handler();delay(1);                /* let the GUI do its work */
    
    //-------------------[Software Timer]-----------------------  
    if(millis() - g_timer_250ms_backup > timer_250ms){
        g_timer_250ms_backup = millis(); 

        if(ota_in_progress == 1 && lv_screen_active() != ui_FWUpdate_screen){
            lv_disp_load_scr(ui_FWUpdate_screen);
            lv_label_set_text(ui_Label_FWUpdateInfo, "Firmware Update in progress..." );
        }
    }

    if(millis() - g_timer_1000ms_backup > timer_1000ms){
        g_timer_1000ms_backup = millis();

        //check battery voltage
        int raw_adc = analogRead(BATTERY_ADC_PIN);
        float voltage = (raw_adc * ADC_REF_VOLTAGE / ADC_MAX_VALUE) * VOLTAGE_DIVIDER_RATIO;
        //debugI("Battery Voltage: %.2fV", voltage);

        //check and update debug screen
        if (lv_scr_act() == ui_Debug_screen){
            uint64_t time_delta = (59 - (((millis() - g_timer_60000ms_backup)))/1000); //-59 seconds
            String internet_data_refresh_in = "Data Refresh in: " + String(time_delta) + "sec.";
            lv_label_set_text(ui_Label_DebugDataRefresh, internet_data_refresh_in.c_str() );

            String esp32_time_date = "ESP32 Time: " + helper.get_esp_time_date();
            lv_label_set_text(ui_Label_DebugTime, esp32_time_date.c_str() );
            String ip_address = "IP: " + WiFi.localIP().toString();
            lv_label_set_text(ui_Label_DebugIP, ip_address.c_str());
            String sensor_sn = "Sensor SN: " + librelinkup.llu_sensor_data.sensor_sn;
            String sensor_id = "Sensor: " + librelinkup.llu_sensor_data.sensor_id;
            lv_label_set_text(ui_Label_DebugSensor, sensor_id.c_str() );
            
            String sensor_valid_time = "Valid: ";
            char buf_label1[35];
            snprintf(buf_label1, 35, "%dDays %dHours %dMinutes",librelinkup.sensor_livetime.sensor_valid_days,librelinkup.sensor_livetime.sensor_valid_hours, librelinkup.sensor_livetime.sensor_valid_minutes);
            sensor_valid_time = sensor_valid_time + buf_label1;
            lv_label_set_text(ui_Label_DebugSensorTimestamp, sensor_valid_time.c_str() );

            String str_sensor_state = "Sensor State: ";
            char buf_label2[35];
            if(librelinkup.llu_sensor_data.sensor_state == 0){
                snprintf(buf_label2, 35, "%d => unknown",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 1){
                snprintf(buf_label2, 35, "%d => not startet yet",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 2){
                snprintf(buf_label2, 35, "%d => starting phase",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 3){
                snprintf(buf_label2, 35, "%d => ready",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 4){
                snprintf(buf_label2, 35, "%d => expired",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 5){
                snprintf(buf_label2, 35, "%d => shut down",librelinkup.llu_sensor_data.sensor_state);
            }else if(librelinkup.llu_sensor_data.sensor_state == 6){
                snprintf(buf_label2, 35, "%d => has failure",librelinkup.llu_sensor_data.sensor_state);
            }
            str_sensor_state = str_sensor_state + buf_label2;
            lv_label_set_text(ui_Label_DebugSensorState, str_sensor_state.c_str() );

            String str_sensor_value = "Sensor Value: ";
            char buf_label_value[35];
            char buf_label_delta[14];

            if(glucose_delta == 0){
                snprintf(buf_label_delta, 14, "±%d mg/dL", glucose_delta);           //get the "+" for positive values
            }else if(glucose_delta > 0){
                snprintf(buf_label_delta, 14, "+%d mg/dL", glucose_delta);
            }else if(glucose_delta < 0){
                snprintf(buf_label_delta, 14, "%d mg/dL", glucose_delta);
            }
            str_sensor_value = str_sensor_value + String(librelinkup.llu_glucose_data.glucoseMeasurement) + librelinkup.llu_glucose_data.str_trendArrow + " " + buf_label_delta;
            lv_label_set_text(ui_Label_DebugSensorValue, str_sensor_value.c_str()); 
        }
    }

    if(millis() - g_timer_5000ms_backup > timer_5000ms){
        g_timer_5000ms_backup = millis();

        if (!mqtt_client.connected() && mqtt.mqtt_enable == 1){
            
            mqtt_client.connect((mqtt.mqtt_base + mqtt.mqtt_client_name).c_str(), mqtt.mqtt_user, mqtt.mqtt_password);
            mqtt_client.subscribe((mqtt.mqtt_base + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
            
            if (!mqtt_client.connected()){
                DBGprint;Serial.printf("mqtt_client reconnect...failed!\n");
                logger.notice("mqtt_client reconnect...failed!\r\n");
            }else if (mqtt_client.connected()){
                DBGprint;Serial.printf("mqtt_client reconnect...success!\n");
                logger.notice("mqtt_client reconnect...success!\r\n");
            }
        }
    } 

    if(millis() - g_timer_30000ms_backup > timer_30000ms){
      g_timer_30000ms_backup = millis();
    }

    if(millis() - g_timer_10000ms_backup > timer_10000ms){
      g_timer_10000ms_backup = millis(); 
    }

    if(millis() - g_timer_60000ms_backup > timer_60000ms){
        g_timer_60000ms_backup = millis();
    
        //fetch data from LibreLinkUp server if system has internet access
        if(ota_in_progress == 0){
            update_glucose_data();
        
            // decrease update counter -1 and update if five_minute_chart_update_counter == 0
            update_five_minute_counter();
        
            //publish mqtt data to mqtt broker
            update_mqtt_publish();
        }
    }

    if(millis() - g_timer_120000ms_backup > timer_120000ms){
      g_timer_120000ms_backup = millis();
    }

    if(millis() - config_sleep_timer_backup > config_sleep_timer){
        config_sleep_timer_backup = millis();

        // save settings
        //settings.saveConfiguration("/config.json", settings.config);

        // check internet status
        internet_status = helper.check_internet_status();
        if(internet_status != 1){
            DBGprint;Serial.println("Client offline -> reconnect to WiFi");
            logger.notice("Client offline -> reconnect to WiFi");
            esp_status_counter_wifi_restart++;
            WiFi.disconnect();
            delay(2000);
            WiFi.reconnect();
        }

        for(uint8_t i=settings.config.brightness;i>0;i--){
            ledcWrite(0, i); // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
            delay(30);
        }
        //ledcWrite(0, 0); // 0-15, 0-255 (with 8 bit resolution); 0=totally dark;255=totally shiny
        settings.config.brightness = 0;
        logger.notice("TRGB sleep timer: %d , Brigness Setting: %d ",config_sleep_timer_backup, settings.config.brightness);
    }
}