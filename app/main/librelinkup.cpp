/*
 * Library for LIBRELINKUP function
 * 
 * Christian Weithe
 * 2022-04-10
 * for ESP8266, ESP32
 * 
*/

#include "librelinkup.h"

#include "helper.h"
extern HELPER helper;

#include "settings.h"
extern SETTINGS settings;                   // Deklariert die globale Instanz aus main.cpp

#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <mbedtls/sha256.h>

#include <FS.h>
#include <LittleFS.h>
#include <string.h>

// Globale JSON-Pointer
#define LIBRELINKUP_JSON_BUFFER_SIZE        16384 //6144
#define LIBRELINKUP_FILTER_JSON_BUFFER_SIZE 2048  //1024


// DynamicJsonDocument mit dem PSRAM-Speicher initialisieren
DynamicJsonDocument* json_librelinkup = new DynamicJsonDocument(LIBRELINKUP_JSON_BUFFER_SIZE);
DynamicJsonDocument* json_filter = new DynamicJsonDocument(LIBRELINKUP_FILTER_JSON_BUFFER_SIZE);

// HTTP Client und Secure Client für LibreLinkUp API
WiFiClientSecure *llu_client = new WiFiClientSecure;
HTTPClient https;

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

/* convertToMillis 
 * 
 * Parameter:   uint8_t hours, 
 *              uint8_t minutes, 
 *              uint8_t seconds
 * 
 * output:      time in ms
 *         
 */
// Funktion zur Umrechnung von Stunden, Minuten und Sekunden in Millisekunden
uint32_t LIBRELINKUP::convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    return (hours * 3600UL + minutes * 60UL + seconds) * 1000UL;
}

/**
 * @brief Extracts the host name from a URL or host string.
 *
 * This function takes a string that may contain a full URL
 * (including scheme, port, and path) or just a host name and
 * returns only the host part.
 *
 * The following components are removed if present:
 * - URL scheme (e.g. "http://", "https://")
 * - Path (everything after the first '/')
 * - Port number (everything after the first ':')
 *
 * Leading and trailing whitespace is ignored.
 *
 * @param urlOrHost
 *        Input string containing a full URL or a host name.
 *
 * @return
 *        The extracted host name without scheme, port, or path.
 *
 * @note
 *        This function does not perform URL validation and assumes
 *        a well-formed input. IPv6 addresses are not supported.
 *
 * @example
 * @code
 * extractHost("https://example.com:8080/path"); // returns "example.com"
 * extractHost("example.com");                   // returns "example.com"
 * extractHost("example.com:443");               // returns "example.com"
 * extractHost(" http://example.com ");          // returns "example.com"
 * @endcode
 */
String LIBRELINKUP::extractHost(const String& urlOrHost) {
    String s = urlOrHost;
    s.trim();

    // remove scheme
    int p = s.indexOf("://");
    if (p >= 0) s = s.substring(p + 3);

    // cut path
    p = s.indexOf('/');
    if (p >= 0) s = s.substring(0, p);

    // cut port
    p = s.indexOf(':');
    if (p >= 0) s = s.substring(0, p);

    return s;
}

/* begin 
 * 
 * Parameter:   0= API communication Insecure
 *              1= CERT from PROGMEM
 *              2= CERT from LittleFS (default)
 * 
 * output: 0=
 *         1=
 */
uint8_t LIBRELINKUP::begin(uint8_t use_cert) {

    IPAddress api_ip;
    const String host = extractHost(String(base_url));  // <-- base_url kann URL oder Host sein

    if (!WiFi.hostByName(host.c_str(), api_ip)) {
        logger.debug("DNS failed for host: %s (base_url: %s)", host.c_str(), String(base_url).c_str());
        return 0;  // <-- wichtig: Fehler wirklich als Fehler behandeln
    }

    logger.info("API Server IP: %s", api_ip.toString().c_str());

    // setup http client
    https.useHTTP10(false);
    https.setTimeout(10000);
    https.setReuse(false);
    llu_client->setTimeout(10000);
    //llu_client->setNoDelay(false);

    if(use_cert == 0){
        llu_client->setInsecure();
    }else if(use_cert == 1){
        llu_client->setCACert(API_ROOT_CA);
    }else if(use_cert == 2){
        if(setCAfromfile(*llu_client, path_root_ca_googler4) == 0){
            DBGprint_LLU; Serial.printf("download GoogleTrustService Root R4 certificate\r\n");
            download_root_ca_to_file(url_check_GoogleTrustRootR4, path_root_ca_googler4);
        }
        setCAfromfile(*llu_client, path_root_ca_googler4);
    }

    return 1;
}

//sha256 account-id calculation as String
String LIBRELINKUP::account_id_sha256(String user_id){
    // change input to byte array
    const char *data = user_id.c_str();
    size_t len = user_id.length();

    // Buffer 32 Byte for SHA-256 Hash
    unsigned char hash[32];

    // SHA-256 calculate
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data), len, hash, 0);

    // create Hex-String
    String hashString;
    for (int i = 0; i < 32; i++) {
        if (hash[i] < 0x10) hashString += '0'; // add leading zeros
        hashString += String(hash[i], HEX);    // transform Byte to Hex
    }
    
    return hashString;
}

// check clients 0= not connected
bool LIBRELINKUP::check_client(){

    if(llu_client->connected() == 0){
        llu_client->stop();
        https.end();
        return 0;
    }
    return 1;
}

// Get_Epoch_Time() Function that gets current epoch time
time_t LIBRELINKUP::get_epoch_time() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        logger.err("⚠️ Fehler: Konnte lokale Zeit nicht abrufen! Fallback auf `time(nullptr)`.");
        now = time(nullptr);  // Falls getLocalTime() fehlschlägt, nutze time()
    } else {
        time(&now);
    }
    return now;
}

// compare two sensor serials
// Rückgabewert: -1 wenn s1 < s2, 0 wenn gleich, 1 wenn s1 > s2
int LIBRELINKUP::check_sensor_type(const char *s1, const char *s2) {
    int result = strcmp(s1, s2);
    
    if (result < 0) {
        return -1;  // s1 ist älter
    } else if (result > 0) {
        return 1;   // s1 ist neuer
    }
    
    return 0;  // Beide sind identisch
}

// check libre3 sensor state
uint8_t LIBRELINKUP::get_sensor_state(uint8_t state){

    //DBGprint_LLU;Serial.print("Sensor state: ");

    switch (state)
    {
    case SENSOR_NOT_STARTED:
        logger.debug("not yet startet");
        break;
    case SENSOR_STARTING:
        //Serial.println("in starting phase");
        logger.debug("in starting phase");
        break;
    case SENSOR_READY:
        logger.debug("is ready");
        break;
    case SENSOR_EXPIRED:
        logger.debug("is expired");
        break;
    case SENSOR_SHUT_DOWN:
        logger.debug("is shut down");
        break;
    case SENSOR_FAILURE:
        logger.debug("has failure");
        break;
    
    default:
        logger.debug("unknown");
        break;
    }

    return state;
}

// check if freestyle libre3 sensor is expired
// sensor_days: 14 -> 14 Tage, 15 -> 15 Tage (Libre 3 Plus)
int LIBRELINKUP::check_sensor_lifetime(uint32_t unix_activation_time, uint32_t sensor_runtime){
    
    int result = -1;
    
    struct tm timeinfo;
    time_t now;

    // lokale Zeit holen ------------------
    if(!getLocalTime(&timeinfo)){
        DBGprint_LLU; Serial.println("Failed to obtain time");
        return SENSOR_NOT_AVAILABLE;
    }

    time(&now); // epoch time

    // Sensor nicht verfügbar
    if(llu_sensor_data.sensor_id_non_active == "" && llu_sensor_data.sensor_sn_non_active == ""){
        logger.debug("sensor not activ");
        return SENSOR_NOT_AVAILABLE;
    }

    // Warmup-Phase (60 Minuten)
    if(llu_sensor_data.sensor_id_non_active == "" && 
       llu_sensor_data.sensor_sn_non_active != "" &&
       unix_activation_time > 0 &&
       (unix_activation_time + 3600) > now){
        logger.debug("sensor in startup phase!");
        int remaining_warmup_time = get_remaining_warmup_time(unix_activation_time);
        logger.debug("sensor available in: %dminutes", remaining_warmup_time);
        llu_sensor_data.sensor_sn = ""; // reset active sensor sn druring warmup

        return SENSOR_STARTING;
    }

    // Sensor aktiv & innerhalb Laufzeit
    if( unix_activation_time > 0 &&
        (unix_activation_time + 3600) <= now &&
        (unix_activation_time + sensor_runtime) > now ){
        
        logger.debug("Sensor is ready!");
        result = SENSOR_READY;

        // Restlaufzeit berechnen
        uint32_t diff_time = (unix_activation_time + sensor_runtime) - now;

        sensor_livetime.sensor_valid_days    = diff_time / 86400;
        sensor_livetime.sensor_valid_hours   = (diff_time / 3600) % 24;
        sensor_livetime.sensor_valid_minutes = (diff_time / 60) % 60;
        sensor_livetime.sensor_valid_seconds = diff_time % 60;

        logger.debug("Sensor expires in: Days:%02d Hours:%02d Minutes:%02d Seconds:%02d",
                     sensor_livetime.sensor_valid_days,
                     sensor_livetime.sensor_valid_hours,
                     sensor_livetime.sensor_valid_minutes,
                     sensor_livetime.sensor_valid_seconds);
        return result;
    }

    // Sensor abgelaufen
    if(llu_sensor_data.sensor_id_non_active == "" && 
       llu_sensor_data.sensor_sn_non_active != "" &&
       unix_activation_time > 0 &&
       (unix_activation_time + sensor_runtime) < now){
        DBGprint_LLU; Serial.printf("Sensor expired!\r\n");
        logger.debug("Sensor expired!");
        return SENSOR_EXPIRED;
    }

    return result;
}

// check sensor type and set remaining sensor time
int LIBRELINKUP::check_sensor_type() {
    static bool already_checked = false;  ///< Flag to avoid multiple checks

    // Check if a serial number exists and has not yet been checked
    if (llu_sensor_data.sensor_sn.length() > 0 && !already_checked) {

        // Compare serial number with reference
        int cmp = check_sensor_type(
            llu_sensor_data.sensor_sn.c_str(),
            llu_sensor_data.LIBRE3PLUS_SERIAL_START.c_str()
        );

        if (cmp == 1) {
            // Sensor is Libre 3 Plus
            logger.debug("Sensor Type: Libre 3 Plus");
            llu_sensor_data.sensor_runtime = UNIXTIME15DAYS; // 15 days runtime
            return 1;

        } else if (cmp == -1) {
            // Sensor is Libre 3
            logger.debug("Sensor Type: Libre 3");
            llu_sensor_data.sensor_runtime = UNIXTIME14DAYS; // 14 days runtime
            return -1;

        } else {
            // Unknown sensor type
            logger.debug("Sensor Type: unknown sensor type");
            return 0;
        }

        // Set flag so the type is not checked multiple times
        already_checked = true;
    }
    return 0;
}

// Funktion zur Berechnung der verbleibenden Zeit in Minuten
int LIBRELINKUP::get_remaining_warmup_time(time_t unix_activation_time) {
    time_t current_time = time(NULL);  // Aktuelle Zeit holen (Unix-Zeit)
    int remaining_time = (unix_activation_time + (60 * 60)) - current_time;  // 60 Minuten Warmup

    // Falls die Zeit bereits abgelaufen ist, auf 0 setzen
    if (remaining_time < 0) return 0;

    return remaining_time / 60;  // Sekunden in Minuten umrechnen
}

// check glucose api.libreview.io valid timestamp with ESP32 local time 
// (0= error or not valid; 1=valid; 2=timecode "00:00:00 00.00.0000" 3= no activated sensor)        

uint8_t LIBRELINKUP::check_valid_timestamp_factory(
    const String& factory_ts,
    const String& cloud_ts,     // <-- normaler Timestamp-String (optional, fürs Logging)
    uint8_t print_mode)
{

    time_t now = time(nullptr);
    if (now < 1700000000) {
        logger.notice("Failed to obtain valid time (NTP?)");
        return LOCAL_TIME_ERROR;
    }

    time_t tCloud = parseTimestamp(cloud_ts.c_str());
    time_t tFactory = parseTimestamp(factory_ts.c_str());
    if (tFactory == 0) {
        logger.notice("Error parsing FactoryTimestamp: %s", factory_ts.c_str());
        return SENSOR_TIMECODE_ERROR;
    }

    // Local = Factory + offset
    time_t tLocalMeas = tFactory;
    tLocalMeas = tFactory + (time_t)tz_offset_s_locked;
    
    int32_t diff_ms = (int32_t)difftime(now, tLocalMeas) * 1000;

    if (print_mode == 1) {
        logger.debug("tz_locked=%d offset_s=%ld", (int)tz_locked, (long)tz_offset_s_locked);
        logger.debug("ESP32 now epoch               : %ld", (long)now);
        logger.debug("Factory epoch                 : %ld", (long)tFactory);
        logger.debug("Cloud TS epoch                : %ld", (long)tCloud);
        logger.debug("Local(Factory - offset) epoch : %ld", (long)tLocalMeas);
        logger.debug("diff_ms                       : %ld (timeout=%ld)",
                      (long)diff_ms, (long)LIBRELINKUPSENSORTIMEOUT);
    }

    if (diff_ms < 0) return SENSOR_TIMECODE_OUT_OF_RANGE;
    if (diff_ms > LIBRELINKUPSENSORTIMEOUT) return SENSOR_TIMECODE_OUT_OF_RANGE;
    return SENSOR_TIMECODE_VALID;
}


// check glucose api.libreview.io graphdata. returns count of non Zero value
uint8_t LIBRELINKUP::check_graphdata(void){
    
    uint8_t count_valid_graph_data = 0;

    for(uint8_t i=0;i<GRAPHDATAARRAYSIZE;i++){
        if(llu_sensor_history_data.graph_data[i] != 0){
            count_valid_graph_data++;
        }
    }
    
    return count_valid_graph_data;
}


// redirect case
String LIBRELINKUP::regionToBaseUrl(const String& region) {
    if (region == "de" || region == "eu") return "https://api-de.libreview.io";
    return "https://api.libreview.io";
}


// user Accept Terms api.libreview.io
uint16_t LIBRELINKUP::tou_user(void){
    
    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_tou)) {
        //delay(10);        
        vTaskDelay(pdMS_TO_TICKS(10));
        //Serial.println("Connected to: " + url);

        addDefaultLLUHeaders(https);        
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token + "");

        // JSON data to send with HTTP POST
        String httpRequestData = "";           
        
        // Send HTTP POST request
        int code = https.POST(httpRequestData);
        DBGprint_LLU;Serial.printf("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                
                //Parse response
                deserializeJson((*json_librelinkup), https.getStream());
                    
                //Read values
                //serializeJsonPretty(*json_librelinkup, Serial);Serial.println();

                llu_login_data.user_login_status   = (*json_librelinkup)["status"].as<uint8_t>();
                llu_login_data.user_id             = (*json_librelinkup)["data"]["user"]["id"].as<String>();
                llu_login_data.user_country        = (*json_librelinkup)["data"]["user"]["country"].as<String>();
                
                Serial.println();
                DBGprint_LLU;Serial.print("LibreLinkUp Accept Terms for: ");Serial.println(settings.config.login_email);
                DBGprint_LLU;Serial.print("user_id           : ");Serial.println(llu_login_data.user_id);
                DBGprint_LLU;Serial.print("user_country      : ");Serial.println(llu_login_data.user_country);
                DBGprint_LLU;Serial.print("user_login_status : ");Serial.println(llu_login_data.user_login_status);
                Serial.println();

                logger.debug("LibreLinkUp Accept Terms for: %s",settings.config.login_email.c_str());
                logger.debug("user_id           : %s",llu_login_data.user_id.c_str());
                logger.debug("user_country      : %s",llu_login_data.user_country.c_str());
                logger.debug("user_login_status : %d",llu_login_data.user_login_status);

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("tou json_librelinkup: Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_librelinkup->clear();                                          //clears the data object
            }
        }
        else {
            DBGprint_LLU; Serial.printf("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
        }
        // Free resources
        https.end();
        //check if client is still connected
        if(llu_client->connected()){
            DBGprint_LLU;Serial.printf("LLU client connected: %d\r\n",llu_client->connected());
            llu_client->flush();
            llu_client->stop();
        }
        result = 1;
    }
    
    return result;
}

// get auth data from api.libreview.io
uint16_t LIBRELINKUP::auth_user(String user_email, String user_password){

    uint8_t result = 0;

    // important: pro call reset
    llu_client->stop();
    https.end();

    if (https.begin(*llu_client, base_url + url_user_auth)) {
        vTaskDelay(pdMS_TO_TICKS(10));

        addDefaultLLUHeaders(https);

        String httpRequestData = "{\"email\":\"" + user_email + "\",\"password\":\"" + user_password + "\"}";
        int code = https.POST(httpRequestData);

        logger.debug("HTTP Code: [%d]\r\n", code);

        if (code > 0 && (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY)) {

            deserializeJson((*json_librelinkup), https.getStream());
            //serializeJsonPretty(*json_librelinkup, Serial); Serial.println();

            bool redirect = (*json_librelinkup)["data"]["redirect"] | false;
            String region = (*json_librelinkup)["data"]["region"] | "";
            String baseUrlStr = String(base_url);
            
            // Check for redirect and if the region has changed (from default) add new api region
            if (redirect) {
                DBGprint_LLU;Serial.printf("Login redirect requested, region=%s\n\r", region.c_str());
                logger.notice("Login redirect requested, region=%s", region.c_str());
                https.end();

                static char base_url_buf[64];

                snprintf(base_url_buf, sizeof(base_url_buf),
                        "https://api-%s.libreview.io", region.c_str());
                base_url = base_url_buf;

                json_librelinkup->clear();
                llu_client->stop();

                // retry once
                return auth_user(user_email, user_password);
            }

            // standard Login-Path (authTicket already existing)
            llu_login_data.user_login_status   = (*json_librelinkup)["status"].as<uint8_t>();
            llu_login_data.user_country        = (*json_librelinkup)["data"]["user"]["country"].as<String>();
            llu_login_data.user_id             = (*json_librelinkup)["data"]["user"]["id"].as<String>();
            llu_login_data.user_token          = (*json_librelinkup)["data"]["authTicket"]["token"].as<String>();
            llu_login_data.user_token_expires  = (*json_librelinkup)["data"]["authTicket"]["expires"].as<uint32_t>();

            llu_login_data.account_id = account_id_sha256(llu_login_data.user_id);

            json_librelinkup->clear();
            result = 1;
        }

        https.end();
        llu_client->stop();
    }

    return result;
}


// get graph glycose data from api.libreview.io
uint16_t LIBRELINKUP::get_connection_data(void){
    
    int8_t result = 0;
    
    // resets previuos timestamp
    llu_glucose_data.str_measurement_timestamp = "";
    
    // get user ID and Token, if AuthToken not already pulled 
    if(llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null" /*strcmp(user_token.c_str(), "null") == 0*/){
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU;Serial.println("Auth User: no user_id available!");
        auth_user(settings.config.login_email,settings.config.login_password);
        if(llu_login_data.user_login_status == 4){
            DBGprint_LLU;Serial.println("LLU Login: Tou required");
            tou_user();
        }
    }

    // get API graph data from LibreView server 
    if(https.begin(*llu_client, base_url + url_connection)) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // Add LLU default headers
        addDefaultLLUHeaders(https);
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token);
        https.addHeader("Account-ID", llu_login_data.account_id);

        int code = https.GET();
        //DBGprint_LLU;Serial.printf("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                // The filter: it contains "true" for each value we want to keep
                (*json_filter)["data"][0]["glucoseMeasurement"]["Timestamp"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["TrendArrow"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["TrendMessage"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["MeasurementColor"] = true;
                
                /*
                (*json_filter)["data"][0]["targetLow"] = true;
                (*json_filter)["data"][0]["targetHigh"] = true;

                (*json_filter)["data"][0]["sensor"]["deviceId"] = true;
                (*json_filter)["data"][0]["sensor"]["sn"] = true;
                (*json_filter)["data"][0]["sensor"]["a"] = true;
                (*json_filter)["data"][0]["sensor"]["pt"] = true;

                (*json_filter)["data"][0]["patientDevice"]["ll"] = true;
                (*json_filter)["data"][0]["patientDevice"]["hl"] = true;
                (*json_filter)["data"][0]["patientDevice"]["fixedLowAlarmValues"]["mgdl"] = true;
                
                (*json_filter)["ticket"]["token"] = true;
                (*json_filter)["ticket"]["expires"] = true;
                */

                // Deserialize the document with json_filter setting. keep buffer size in mind.
                deserializeJson((*json_librelinkup), https.getStream(), DeserializationOption::Filter(*json_filter));
                
                // Print the result
                //serializeJsonPretty(((*json_librelinkup)), Serial); Serial.println();

                llu_glucose_data.glucoseMeasurement          = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"].as<int>();
                llu_glucose_data.trendArrow                  = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["TrendArrow"].as<int>();
                llu_glucose_data.measurement_color           = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["MeasurementColor"].as<int>();
                llu_glucose_data.str_TrendMessage            = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["TrendMessage"].as<String>();
                llu_glucose_data.str_measurement_timestamp   = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["Timestamp"].as<String>();

                /*
                glucosetargetLow            = (*json_librelinkup)["data"][0]["targetLow"].as<int>();
                glucosetargetHigh           = (*json_librelinkup)["data"][0]["targetHigh"].as<int>();
                glucoseAlarmLow             = (*json_librelinkup)["data"][0]["patientDevice"]["ll"].as<int>();
                glucoseAlarmHigh            = (*json_librelinkup)["data"][0]["patientDevice"]["hl"].as<int>();
                glucosefixedLowAlarmValues  = (*json_librelinkup)["data"][0]["patientDevice"]["fixedLowAlarmValues"]["mgdl"].as<int>();

                sensor_id                   = (*json_librelinkup)["data"][0]["sensor"]["deviceId"].as<String>();
                sensor_sn                   = (*json_librelinkup)["data"][0]["sensor"]["sn"].as<String>();
                sensor_state                = (*json_librelinkup)["data"][0]["sensor"]["pt"].as<int>();
                sensor_activation_time      = (*json_librelinkup)["data"][0]["sensor"]["a"].as<int>();
                
                user_token                  = (*json_librelinkup)["ticket"]["token"].as<String>();
                user_token_expires          = (*json_librelinkup)["ticket"]["expires"].as<uint32_t>();
                */

                //DBGprint_LLU;Serial.print("glucoseMeasurement: ");Serial.print(glucoseMeasurement);
                
                if(llu_glucose_data.trendArrow == 0){
                    llu_glucose_data.str_trendArrow = "no Data";
                }else if(llu_glucose_data.trendArrow == 1){
                    llu_glucose_data.str_trendArrow = "↓";
                }else if(llu_glucose_data.trendArrow == 2){
                    llu_glucose_data.str_trendArrow = "↘";
                }else if(llu_glucose_data.trendArrow == 3){
                    llu_glucose_data.str_trendArrow = "→";
                }else if(llu_glucose_data.trendArrow == 4){
                    llu_glucose_data.str_trendArrow = "↗";
                }else if(llu_glucose_data.trendArrow == 5){
                    llu_glucose_data.str_trendArrow = "↑";
                }
                
                json_filter->clear();
                json_librelinkup->clear();                                          //clears the data object

            }
            result = 1;
        }
        else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;
                        
            if (code == HTTP_CODE_UNAUTHORIZED){    //Token Auth Error handling
                DBGprint_LLU; Serial.println("Error, wrong Token -> reauthorization...");
                auth_user(settings.config.login_email,settings.config.login_password);             
                json_filter->clear();
                json_librelinkup->clear();
            }
        }
        // Free https resources
        https.end();

    }else{
        result = 0;
    }

    //check if client is still connected
    if(llu_client->connected()){
        DBGprint_LLU;Serial.printf("LLU client still connected: %d\r\n",llu_client->connected());
        llu_client->flush();
        llu_client->stop();
    }

    return result;
}

// get graph glycose data from api.libreview.io
uint16_t LIBRELINKUP::get_graph_data(void){

    int8_t result = 0;
    uint32_t https_api_time_measure = millis();

    check_client();

    // get user ID and Token, if AuthToken not already pulled 
    if (llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null") {
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU; Serial.println("Auth User: no user_id available!");
        auth_user(settings.config.login_email, settings.config.login_password);
        if (llu_login_data.user_login_status == 4) {
            DBGprint_LLU; Serial.println("LLU Login: Tou required");
            logger.debug("LLU Login: Tou required");
            tou_user();
        }
    }

    // create API url 
    url_graph = "/llu/connections/" + llu_login_data.user_id + "/graph";

    // get API graph data from LibreView server 
    if (https.begin(*llu_client, base_url + url_graph)) {
        vTaskDelay(pdMS_TO_TICKS(10));        

        // Add LLU default headers
        addDefaultLLUHeaders(https);
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token);
        https.addHeader("Account-ID", llu_login_data.account_id);

        int code = https.GET();
        logger.debug("HTTP code=%d size=%d", code, https.getSize());
        
        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                // JSON filter
                (*json_filter)["data"]["connection"]["targetLow"] = true;
                (*json_filter)["data"]["connection"]["targetHigh"] = true;

                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendArrow"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendMessage"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["MeasurementColor"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["FactoryTimestamp"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["Timestamp"] = true;

                (*json_filter)["data"]["connection"]["patientDevice"]["ll"] = true;
                (*json_filter)["data"]["connection"]["patientDevice"]["hl"] = true;
                (*json_filter)["data"]["connection"]["patientDevice"]["fixedLowAlarmValues"]["mgdl"] = true;

                (*json_filter)["data"]["connection"]["status"] = true;
                (*json_filter)["data"]["connection"]["country"] = true;
                (*json_filter)["data"]["connection"]["sensor"]["sn"] = true;
                (*json_filter)["data"]["connection"]["sensor"]["deviceId"] = true;
                (*json_filter)["data"]["connection"]["sensor"]["a"] = true;

                (*json_filter)["data"]["activeSensors"][0]["sensor"]["deviceId"] = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["sn"] = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["a"] = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["pt"] = true;

                (*json_filter)["data"]["graphData"][0]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["graphData"][0]["FactoryTimestamp"] = true;
                (*json_filter)["data"]["graphData"][0]["Timestamp"] = true;

                // Deserialize with filter
                String body = https.getString();  // reads full response
                DeserializationError err = deserializeJson((*json_librelinkup), body,
                                          DeserializationOption::Filter(*json_filter));

                if (err) {
                    logger.debug("HTTPS deserialize failed: %s", err.c_str());
                    json_filter->clear();
                    json_librelinkup->clear();
                    https.end();
                    return 0;
                }

                // keep raw JSON as string (your getter)
                last_graph_json = "";
                serializeJson((*json_librelinkup), last_graph_json);

                // ONE parser for both sources
                bool ok = parse_graph_json_doc();

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("json_librelinkup: Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

                buffer_info = helper.getBufferSize(&(*json_filter));
                logger.debug("json_filter     : Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_filter->clear();
                json_librelinkup->clear();

                result = ok ? 1 : 0;
            }

            https_llu_api_fetch_time = millis() - https_api_time_measure;
        }
        else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;

            if (code == HTTP_CODE_UNAUTHORIZED) {
                DBGprint_LLU; Serial.println("Error, wrong Token -> reauthorization...");
                logger.debug("Error, wrong Token -> reauthorization...");
                json_filter->clear();
                json_librelinkup->clear();
                auth_user(settings.config.login_email, settings.config.login_password);
                result = get_graph_data();
            }
        }

        // Free https resources
        https.end();

    } else {
        result = 0;
    }

    // check if client is still connected
    if (llu_client->connected()) {
        DBGprint_LLU; Serial.printf("LLU client still connected: %d\r\n", llu_client->connected());
        logger.debug("LLU client still connected: %d\r\n", llu_client->connected());
        llu_client->flush();
        llu_client->stop();
    }

    return result;
}

// ingest_graph_json from external source
bool LIBRELINKUP::ingest_graph_json(const uint8_t* data, size_t len) {

    if (!data || len == 0) return false;

    DeserializationError err = deserializeJson(*json_librelinkup, data, len);
    if (err) {
        logger.debug("ingest_graph_json: deserialize failed: %s", err.c_str());
        json_librelinkup->clear();
        return false;
    }

    // keep raw JSON as string (optional but helpful)
    last_graph_json = "";
    serializeJson(*json_librelinkup, last_graph_json);

    bool ok = parse_graph_json_doc();

    json_librelinkup->clear();
    return ok;
}

// parse_graph_json_doc from internal json_librelinkup
bool LIBRELINKUP::parse_graph_json_doc() {

    // resets previous timestamp
    llu_glucose_data.str_measurement_timestamp = "";

    // delete all historical glucose data
    memset(llu_sensor_history_data.graph_data, 0, GRAPHDATAARRAYSIZE);
    memset(llu_sensor_history_data.timestamp,  0, GRAPHDATAARRAYSIZE);

    // --- Parse current measurement ---
    llu_glucose_data.glucoseMeasurement               = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["ValueInMgPerDl"].as<int>();
    llu_glucose_data.trendArrow                       = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["TrendArrow"].as<int>();
    llu_glucose_data.measurement_color                = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["MeasurementColor"].as<int>();
    llu_glucose_data.str_TrendMessage                 = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["TrendMessage"].as<String>();
    llu_glucose_data.str_measurement_timestamp        = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["Timestamp"].as<String>();
    llu_glucose_data.str_measurement_factorytimestamp = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["FactoryTimestamp"].as<String>();

    // --- Parse targets/alarms ---
    llu_glucose_data.glucosetargetLow                 = (*json_librelinkup)["data"]["connection"]["targetLow"].as<int>();
    llu_glucose_data.glucosetargetHigh                = (*json_librelinkup)["data"]["connection"]["targetHigh"].as<int>();
    llu_glucose_data.glucoseAlarmLow                  = (*json_librelinkup)["data"]["connection"]["patientDevice"]["ll"].as<int>();
    llu_glucose_data.glucoseAlarmHigh                 = (*json_librelinkup)["data"]["connection"]["patientDevice"]["hl"].as<int>();
    llu_glucose_data.glucosefixedLowAlarmValues       = (*json_librelinkup)["data"]["connection"]["patientDevice"]["fixedLowAlarmValues"]["mgdl"].as<int>();

    // --- Parse connection/sensor info ---
    llu_login_data.connection_country         = (*json_librelinkup)["data"]["connection"]["country"].as<String>();
    llu_login_data.connection_status          = (*json_librelinkup)["data"]["connection"]["status"].as<int>();

    llu_sensor_data.sensor_sn_non_active      = (*json_librelinkup)["data"]["connection"]["sensor"]["sn"].as<String>();
    llu_sensor_data.sensor_id_non_active      = (*json_librelinkup)["data"]["connection"]["sensor"]["deviceId"].as<String>();
    llu_sensor_data.sensor_non_activ_unixtime = (*json_librelinkup)["data"]["connection"]["sensor"]["a"].as<uint32_t>();

    llu_sensor_data.sensor_id                 = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["deviceId"].as<String>();
    llu_sensor_data.sensor_sn                 = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["sn"].as<String>();
    llu_sensor_data.sensor_state              = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["pt"].as<int>();
    llu_sensor_data.sensor_activation_time    = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["a"].as<int>();

    // check timezone offset
    /*
    update_timezone_offset(
        llu_glucose_data.str_measurement_timestamp,
        llu_glucose_data.str_measurement_factorytimestamp
    );*/

    update_tz_offset_once(
        (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["Timestamp"].as<String>(),
        (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["FactoryTimestamp"].as<String>()
    );
    
    // --- Parse historical glucose data ---
    for (uint8_t i = 0; i < GRAPHDATAARRAYSIZE; i++) {
        llu_sensor_history_data.graph_data[i] =
            (*json_librelinkup)["data"]["graphData"][i]["ValueInMgPerDl"].as<uint16_t>();

        if (llu_sensor_history_data.graph_data[i] == 0) {
            llu_sensor_history_data.timestamp[i] = 0;
            llu_sensor_history_data.factory_timestamp[i] = 0;
        } else {
            // parse timestamps and factory timestamps and convert to time_t
            String timestampStr =
                (*json_librelinkup)["data"]["graphData"][i]["Timestamp"].as<String>();
            time_t ts = parseTimestamp(timestampStr.c_str());
            llu_sensor_history_data.timestamp[i] = ts;

            String factory_timestampStr =
                (*json_librelinkup)["data"]["graphData"][i]["FactoryTimestamp"].as<String>();
            ts = parseTimestamp(factory_timestampStr.c_str());
            llu_sensor_history_data.factory_timestamp[i] = ts;
        }
    }

    // add current glucosemeasurement to last position (142)
    llu_sensor_history_data.graph_data[(GRAPHDATAARRAYSIZE + GRAPHDATAARRAYSIZE_PLUS_ONE - 1)] =
        llu_glucose_data.glucoseMeasurement;

    // --- Trend arrow mapping ---
    if (llu_glucose_data.trendArrow == 0) {
        llu_glucose_data.str_trendArrow = "no Data";
    } else if (llu_glucose_data.trendArrow == 1) {
        llu_glucose_data.str_trendArrow = "↓";
    } else if (llu_glucose_data.trendArrow == 2) {
        llu_glucose_data.str_trendArrow = "↘";
    } else if (llu_glucose_data.trendArrow == 3) {
        llu_glucose_data.str_trendArrow = "→";
    } else if (llu_glucose_data.trendArrow == 4) {
        llu_glucose_data.str_trendArrow = "↗";
    } else if (llu_glucose_data.trendArrow == 5) {
        llu_glucose_data.str_trendArrow = "↑";
    }

    return true;
}

// get last graph json data as String
const String& LIBRELINKUP::get_last_graph_json() const {
    return last_graph_json;
}

// get WiFiClientSecure client pointer
WiFiClientSecure & LIBRELINKUP::get_wifisecureclient(void){
    return *llu_client;
}

//check connection to server
void LIBRELINKUP::check_https_connection(const char* url){
        
    // Test server connection
    // get API graph data from LibreView server 
    if(https.begin(*llu_client, url)) {
        vTaskDelay(pdMS_TO_TICKS(10));    

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        
        int code = https.GET();

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                DBGprint_LLU;Serial.printf("connection to %s successful. HTTPS Code: [%d]\r\n", url, code);
                logger.debug("connection to %s successful. HTTPS Code: [%d]", url, code);
            }
        }
        else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] GET... failed, error: %s", https.errorToString(code).c_str());
        }
        // Free https resources
        https.end();
    }

}

// set new root certificate
bool LIBRELINKUP::setCAfromfile(WiFiClientSecure &client, const char* ca_file){
    
    File ca = LittleFS.open(ca_file, "r");
        
    if(!ca) {
        Serial.println("ERROR!");
        return 0;
    } else {
        size_t certSize = ca.size();
        if(certSize == 0){ // dummy value to check if file content is valid
            DBGprint_LLU;Serial.println("CA from File is empty. please downlaod again");
            return 0;
        }
        client.loadCACert(ca,certSize);
        ca.close();
        DBGprint_LLU;Serial.println("set CA from File -> done");
        logger.notice("set CA from File -> done");
        return 1;
    }
}

// get root certificate from file
void LIBRELINKUP::showCAfromfile(const char* ca_file){
    
    //get file size
    logger.notice("opening ca file to read file size...");
    File file = LittleFS.open(ca_file, "r");
    if (!file) {
        logger.notice("Failed to open file!");
        return;
    }
    size_t certSize = file.size();
    logger.notice("Cert file size: %d bytes", certSize);
    file.close();
    
    char* new_certificate;
    new_certificate = (char*)malloc(certSize);
    read2String(LittleFS, ca_file, new_certificate, certSize);
    Serial.printf("CA from: %s:\r\n%s\r\n",ca_file, new_certificate);
    logger.notice("CA from: %s:",ca_file);
    
    //logger output of CA file
    const char* current = new_certificate;
    while (*current) {
        const char* next = strchr(current, '\n');
        if (next) {
            logger.notice(String(current).substring(0, next - current).c_str());
            current = next + 1;
        } else {
            logger.notice(current);
            break;
        }
    }

    free(new_certificate);
}

// get certificate file
uint16_t LIBRELINKUP::download_root_ca_to_file(const char* download_url, const char* file_name){
    
    int8_t result = 0;

    File file = LittleFS.open(file_name, "w");

    if (!file) {
        DBGprint_LLU;Serial.println("- failed to open file for writing");
        logger.notice("- failed to open file for writing");
        return 0;
    }

    llu_client->setInsecure();
    DBGprint_LLU;Serial.print("download CA started...");
    logger.notice("download CA started...");

    // get API graph data from LibreView server 
    if(https.begin(*llu_client, download_url)) {
        //delay(10);
        vTaskDelay(pdMS_TO_TICKS(10));       

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        
        int code = https.GET();
        //DBGprint_LLU;Serial.printf("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                https.writeToStream(&file);
            }
            result = 1;
            file.close();
            Serial.println("finished");
            logger.notice("finished");
        }
        else {
            Serial.println("failed!");
            logger.notice("download failed!");
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;
        }
        // Free https resources
        https.end();

    }else{
        result = 0;
    }

    return result;
}

//---------------------------------------------------------------------------
//get certificate from LittleFS
//read2String(SPIFFS, REMOTE_CERT_FILE, myCertificate, file lenght);

bool LIBRELINKUP::read2String(fs::FS &fs, const char *path, char *myString, size_t maxLength) {
    File file = fs.open(path);
    if (!file || file.isDirectory()) {
        return false;
    }
    size_t iChar = 0;
    while (file.available() && iChar < maxLength - 1) {
        myString[iChar] = file.read();
        iChar++;
    }
    myString[iChar] = '\0';
    file.close();
    return true;
}

// String-Timestamp in time_t umwandeln
time_t LIBRELINKUP::parseTimestamp(const char* timestampStr) {
    setlocale(LC_TIME, "C"); // Erzwingt die C-Standard-Locale für AM/PM-Interpretation

    struct tm tm_time;
    memset(&tm_time, 0, sizeof(struct tm));

    // Parst Datum + Zeit OHNE AM/PM-Interpretation
    char timeStr[50];
    strncpy(timeStr, timestampStr, sizeof(timeStr) - 1);
    timeStr[sizeof(timeStr) - 1] = '\0';

    // Prüfe auf AM oder PM
    int is_pm = strstr(timeStr, "PM") != NULL;

    // Entferne AM/PM aus dem String für strptime
    char clean_timeStr[50];
    strncpy(clean_timeStr, timeStr, sizeof(clean_timeStr) - 1);
    clean_timeStr[sizeof(clean_timeStr) - 1] = '\0';
    char* am_pm = strstr(clean_timeStr, "AM");
    if (!am_pm) am_pm = strstr(clean_timeStr, "PM");
    if (am_pm) *am_pm = '\0'; // AM/PM entfernen

    // Parse nur Datum + Uhrzeit
    char* ret = strptime(clean_timeStr, "%m/%d/%Y %I:%M:%S", &tm_time);
    if (!ret) {
        printf("⚠️ strptime() konnte den String nicht parsen.\n");
        return -1;
    }

    // Manuelle AM/PM Anpassung
    if (is_pm && tm_time.tm_hour != 12) {
        tm_time.tm_hour += 12; // PM → +12 Stunden
    } else if (!is_pm && tm_time.tm_hour == 12) {
        tm_time.tm_hour = 0; // 12 AM → 00:00 Uhr
    }

    tm_time.tm_isdst = -1; // Sommerzeit automatisch erkennen

    time_t timestamp = mktime(&tm_time);

    // Debug-Ausgabe
    //printf("Input: %s → Parsed Time: %02d:%02d:%02d | Unix: %ld\n", timestampStr, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, timestamp);

    return timestamp;
}


bool LIBRELINKUP::update_tz_offset_once(const String& ts_local, const String& ts_factory)
{
    time_t tLocal   = parseTimestamp(ts_local.c_str());
    time_t tFactory = parseTimestamp(ts_factory.c_str());

    if (tLocal == 0 || tFactory == 0) {
        logger.debug("tz: parse failed (local=%ld factory=%ld)", (long)tLocal, (long)tFactory);
        return false;
    }

    int32_t off_s = (int32_t)difftime(tLocal, tFactory);

    // Sanity: typischerweise ganze Stunden
    if (abs(off_s) > 15 * 3600) {
        logger.notice("tz: offset implausible: %ld s", (long)off_s);
        return false;
    }

    // korrekt runden (auch für negative Offsets!)
    int16_t off_h = (int16_t)((off_s >= 0) ? ((off_s + 1800) / 3600)
                                           : ((off_s - 1800) / 3600));

    tz_offset_s_locked = off_s;      // volle Sekundengenauigkeit behalten
    tz_offset_h_locked = off_h;
    tz_locked = true;

    logger.debug("tz lock (1-sample): off_s=%ld off_h=%d",
                 (long)tz_offset_s_locked, (int)tz_offset_h_locked);

    return true;
}