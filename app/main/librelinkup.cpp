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
#define LIBRELINKUP_JSON_BUFFER_SIZE        12000 //6144
#define LIBRELINKUP_FILTER_JSON_BUFFER_SIZE 1024


// DynamicJsonDocument mit dem PSRAM-Speicher initialisieren
DynamicJsonDocument* json_librelinkup = new DynamicJsonDocument(LIBRELINKUP_JSON_BUFFER_SIZE);
DynamicJsonDocument* json_filter = new DynamicJsonDocument(LIBRELINKUP_FILTER_JSON_BUFFER_SIZE);

/*
// Aufräumen falls nötig
  json_librelinkup->~DynamicJsonDocument(); // Destruktor manuell aufrufen
  heap_caps_free(psramPtr_librelinkup);
*/

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

/* begin 
 * 
 * Parameter:   0= API communication Insecure
 *              1= CERT from PROGMEM
 *              2= CERT from LittleFS (default)
 * 
 * output: 0=
 *         1=
 */
uint8_t LIBRELINKUP::begin(uint8_t use_cert){
    
    // setup http client
    https.useHTTP10(true);
    llu_client->setTimeout(10000); //10 sec timeout

    if(use_cert == 0){
        llu_client->setInsecure();
    }else if(use_cert == 1){
        llu_client->setCACert(API_ROOT_CA);
    }else if(use_cert == 2){  
        if(setCAfromfile(*llu_client, path_root_ca_googler4) == 0){    //if cert is not available, or path wrong... DL again
            DBGprint_LLU;Serial.printf("download GoogleTrustService Root R4 certificate\r\n");
            download_root_ca_to_file(url_check_GoogleTrustRootR4, path_root_ca_googler4);
        }
        setCAfromfile(*llu_client, path_root_ca_googler4); // try to set cert again
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
int LIBRELINKUP::check_sensor_lifetime(uint32_t unix_activation_time){
    
    int result = -1;
    
    struct tm timeinfo;
    time_t now;

    // get local time as int ------------------
    if(!getLocalTime(&timeinfo)){
        DBGprint_LLU; Serial.println("Failed to obtain time");
        return SENSOR_NOT_AVAILABLE;
    }

    time(&now); // get epoche time
    
    /*
    logger.debug("sensor_id_non_active:       %s", sensor_id_non_active.c_str());
    logger.debug("sensor_sn_non_active:       %s", sensor_sn_non_active.c_str());
    logger.debug("sensor_activation_unixtime: %d", sensor_non_activ_unixtime);
    */
   
    //check if sensor is available
    if(llu_sensor_data.sensor_id_non_active == "" && llu_sensor_data.sensor_sn_non_active == ""){
        //DBGprint_LLU;Serial.printf("no active sensor\r\n");
        logger.debug("sensor not activ");
        result = SENSOR_NOT_AVAILABLE;
        return result;
    }
    //check if sensor is in startup phase
    else if(llu_sensor_data.sensor_id_non_active == "" && llu_sensor_data.sensor_sn_non_active != ""  &&
            unix_activation_time > 0 &&
            (unix_activation_time + 3600) > now){
            logger.debug("sensor in startup phase!");
            int remaining_warmup_time = get_remaining_warmup_time(unix_activation_time);
            logger.debug("sensor available in: %dminutes", remaining_warmup_time);
            result = SENSOR_STARTING;
            return result;
    }
    //check of valid remaining time if sensor is ready
    else if((unix_activation_time + (UNIXTIME14DAYS)) > now &&
             unix_activation_time > 0 &&
             unix_activation_time + 3600 <= now && 
             unix_activation_time + UNIXTIME14DAYS > now){          
            
            //DBGprint_LLU;Serial.printf("Sensor is ready\r\n");
            logger.debug("Sensor is ready!");
            result = SENSOR_READY;

            //calculate remaining time in days / hours / minute / seconds 
            uint32_t diff_time = ((unix_activation_time + (UNIXTIME14DAYS)) - now);

            sensor_livetime.sensor_valid_days    = diff_time / 86400;
            sensor_livetime.sensor_valid_hours   = diff_time / 3600 % 24;
            sensor_livetime.sensor_valid_minutes = diff_time / 60 % 60;
            sensor_livetime.sensor_valid_seconds = diff_time % 60;
            
            //DBGprint_LLU;Serial.printf("Sensor expires in: Days:%02d Hours:%02d Minutes:%02d Seconds:%02d\r\n",sensor_livetime.sensor_valid_days, sensor_livetime.sensor_valid_hours, sensor_livetime.sensor_valid_minutes, sensor_livetime.sensor_valid_seconds);
            logger.debug("Sensor expires in: Days:%02d Hours:%02d Minutes:%02d Seconds:%02d",sensor_livetime.sensor_valid_days, sensor_livetime.sensor_valid_hours, sensor_livetime.sensor_valid_minutes, sensor_livetime.sensor_valid_seconds);
            return result;

    }else if(llu_sensor_data.sensor_id_non_active == "" && 
             llu_sensor_data.sensor_sn_non_active != "" && 
             unix_activation_time > 0   &&
             (unix_activation_time + (UNIXTIME14DAYS)) < now){
        DBGprint_LLU;Serial.printf("Sensor expired!\r\n");
        logger.debug("Sensor expired!");
        result = SENSOR_EXPIRED;
        return result;
    }

    return result;
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
uint8_t LIBRELINKUP::check_valid_timestamp(String librelinkup_timestamp, uint8_t print_mode){

    uint8_t result = 0;
    struct tm timeinfo;
    
    // get local time as int ------------------
    if(!getLocalTime(&timeinfo)){
        DBGprint_LLU; Serial.println("Failed to obtain time");
        return LOCAL_TIME_ERROR;
    }

    localtime.day    = timeinfo.tm_mday;            // day
    localtime.month  = timeinfo.tm_mon  + 1;        // month (0-based, therefore +1)
    localtime.year   = timeinfo.tm_year + 1900;     // year since 1900
    localtime.hour   = timeinfo.tm_hour + timezone; // hour + timezone
    localtime.minute = timeinfo.tm_min;             // minute
    localtime.second = timeinfo.tm_sec;             // second

    if (localtime.hour == 24) {
        localtime.hour = 0;
        localtime.day++;
    }

    // Optional: day saving time check
    if (timeinfo.tm_isdst == 0) {
        // no daysaving
    } else if (timeinfo.tm_isdst == 1) {
        // active
    } else {
        // error
    }
    //--------------------------------------------------
    
    // get LLU json timestamp time as int ------------------
    int hour_librelinkup_timestamp   = 0;
    int minute_librelinkup_timestamp = 0;
    int second_librelinkup_timestamp = 0;
    int day_librelinkup_timestamp    = 0;
    int month_librelinkup_timestamp  = 0;
    int year_librelinkup_timestamp   = 0;
    char meridian[3];
    
    if (sscanf(librelinkup_timestamp.c_str(), "%d/%d/%d %d:%d:%d %2s", &librelinkuptimecode.month, &librelinkuptimecode.day, &librelinkuptimecode.year, &librelinkuptimecode.hour, &librelinkuptimecode.minute, &librelinkuptimecode.second, meridian) != 7) {
        DBGprint_LLU;Serial.println("Error parsing date/time");
        logger.debug("Error parsing date/time");

        return SENSOR_TIMECODE_ERROR;
    }
    // Adjust for PM
    if (strcmp(meridian, "PM") == 0 && librelinkuptimecode.hour != 12) {
        librelinkuptimecode.hour += 12;
    } else if (strcmp(meridian, "AM") == 0 && librelinkuptimecode.hour == 12) {
        librelinkuptimecode.hour = 0;
    }
    //---------------------------------------------------------

    if(print_mode == 1){      

        DBGprint_LLU;
        Serial.printf("ESP32 local Timestamp: %02d.%02d.%04d ",localtime.day,localtime.month,localtime.year);
        Serial.printf("%02d:%02d:%02d\r\n",localtime.hour,localtime.minute,localtime.second);
        logger.notice("ESP32 local Timestamp: %02d.%02d.%04d %02d:%02d:%02d",localtime.day,localtime.month,localtime.year,localtime.hour,localtime.minute,localtime.second);
        
        DBGprint_LLU; 
        Serial.printf("LibreLinkUp Timestamp: %02d.%02d.%04d ",librelinkuptimecode.day,librelinkuptimecode.month,librelinkuptimecode.year);
        Serial.printf("%02d:%02d:%02d\r\n",librelinkuptimecode.hour,librelinkuptimecode.minute,librelinkuptimecode.second);
        logger.notice("LibreLinkUp Timestamp: %02d.%02d.%04d %02d:%02d:%02d",librelinkuptimecode.day,librelinkuptimecode.month,librelinkuptimecode.year,librelinkuptimecode.hour,librelinkuptimecode.minute,librelinkuptimecode.second);

    }
    // Timecode Filter ---------------------------------------------------------------
    // check LibreLinkUp timecode for "00:00:00 00.00.0000"
    if( librelinkuptimecode.day    == 0 && \
        librelinkuptimecode.month  == 0 && \
        librelinkuptimecode.year   == 0 && \
        librelinkuptimecode.hour   == 0 && \
        librelinkuptimecode.minute == 0 && \
        librelinkuptimecode.second == 0){
        
        DBGprint_LLU;Serial.println("TimeCode Filter: LibreLinkUp -> 00.00.0000 00:00:00");
        logger.notice("TimeCode Filter: LibreLinkUp -> 00.00.0000 00:00:00");
        logger.notice("LLU API Timestamp: %s",librelinkup_timestamp.c_str());
        logger.notice("LLU API token: %s",llu_login_data.user_token.c_str());
        result = SENSOR_NOT_ACTIVE;
        
        return result;
    }
    
    // Filter: check if timecode is valid
    uint32_t serverTimeMs = convertToMillis(librelinkuptimecode.hour, librelinkuptimecode.minute, librelinkuptimecode.second);
    uint32_t localTimeMs  = convertToMillis(localtime.hour, localtime.minute, localtime.second);
    
    // calculate timedifference
    int32_t timeDifferenceMs = localTimeMs - serverTimeMs;
    
    if((localtime.day > librelinkuptimecode.day) || (timeDifferenceMs > LIBRELINKUPSENSORTIMEOUT)){
        
        DBGprint_LLU;Serial.println("TimeCode Filter: Time Difference LocalTime - ServerTimestamp > LIBRELINKUPSENSORTIMEOUT");
        logger.debug("TimeCode Filter: Time Difference LocalTime - ServerTimestamp > LIBRELINKUPSENSORTIMEOUT");
        
        result = SENSOR_TIMECODE_OUT_OF_RANGE;
        return result;
    }
    
    else if((localtime.day == librelinkuptimecode.day) && (timeDifferenceMs <= LIBRELINKUPSENSORTIMEOUT)){
        logger.debug("TimeCode Filter: Timecode Valid");
        result = SENSOR_TIMECODE_VALID;
        return result;
    }

    return result;
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

// get auth data from api.libreview.io
uint16_t LIBRELINKUP::auth_user(String user_email, String user_password){
    
    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_auth)) {
        delay(10);        
        //Serial.println("Connected to: " + url);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.7.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");

        // JSON data to send with HTTP POST
        String httpRequestData = "{\"email\":\"" + user_email + "\",\"password\":\"" + user_password + "\"}";           
        
        // Send HTTP POST request
        int code = https.POST(httpRequestData);
        //DBGprint_LLU;Serial.printf("HTTP Code: [%d]\r\n", code);
        logger.debug("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                
                //Parse response
                deserializeJson((*json_librelinkup), https.getStream());
                    
                //Read values
                //serializeJsonPretty(*json_librelinkup, Serial);Serial.println();

                llu_login_data.user_login_status   = (*json_librelinkup)["status"].as<uint8_t>();
                llu_login_data.user_country        = (*json_librelinkup)["data"]["user"]["country"].as<String>();
                llu_login_data.user_id             = (*json_librelinkup)["data"]["user"]["id"].as<String>();
                llu_login_data.user_token          = (*json_librelinkup)["data"]["authTicket"]["token"].as<String>();
                llu_login_data.user_token_expires  = (*json_librelinkup)["data"]["authTicket"]["expires"].as<uint32_t>();
                
                // calculate SHA256 Hash for Account-ID header
                llu_login_data.account_id = account_id_sha256(llu_login_data.user_id);

                Serial.println();
                DBGprint_LLU;Serial.println("LibreLinkUp Authentification for:");
                DBGprint_LLU;Serial.print("user_email        : ");Serial.println(settings.config.login_email);
                DBGprint_LLU;Serial.print("user_country      : ");Serial.println(llu_login_data.user_country);
                DBGprint_LLU;Serial.print("user_id           : ");Serial.println(llu_login_data.user_id);
                DBGprint_LLU;Serial.print("user_token        : ");Serial.println(llu_login_data.user_token);
                DBGprint_LLU;Serial.print("token_exp.        : ");Serial.println(llu_login_data.user_token_expires);
                DBGprint_LLU;Serial.print("user_login_status : ");Serial.println(llu_login_data.user_login_status);
                DBGprint_LLU;Serial.print("account-id        : ");Serial.println(llu_login_data.account_id);

                logger.debug("LibreLinkUp Authentification for:");
                logger.debug("user_email        : %s",settings.config.login_email.c_str());
                logger.debug("user_country      : %s",llu_login_data.user_country.c_str());
                logger.debug("user_id           : %s",llu_login_data.user_id.c_str());
                logger.debug("user_token        : %s",llu_login_data.user_token.c_str());
                logger.debug("token_exp.        : %d",llu_login_data.user_token_expires);
                logger.debug("user_login_status : %d",llu_login_data.user_login_status);
                logger.debug("account-id        : %s",llu_login_data.account_id.c_str());

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("auth json_librelinkup: Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

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
            logger.debug("LLU client connected: %d\r\n",llu_client->connected());
            llu_client->flush();
            llu_client->stop();
        }
        result = 1;
    }
    
    return result;
}

// user Accept Terms api.libreview.io
uint16_t LIBRELINKUP::tou_user(void){
    
    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_tou)) {
        delay(10);        
        //Serial.println("Connected to: " + url);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.7.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");
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
        delay(10);        

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.12.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        //https.addHeader("Accept-Encoding", "gzip, deflate, br");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");
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

    // resets previuos timestamp
    llu_glucose_data.str_measurement_timestamp = "";
    // delete all historical glucose data
    memset(llu_sensor_history_data.graph_data,0,GRAPHDATAARRAYSIZE);
    memset(llu_sensor_history_data.timestamp,0,GRAPHDATAARRAYSIZE);

    // get user ID and Token, if AuthToken not already pulled 
    if(llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null"){
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU;Serial.println("Auth User: no user_id available!");
        auth_user(settings.config.login_email,settings.config.login_password);
        if(llu_login_data.user_login_status == 4){
            DBGprint_LLU;Serial.println("LLU Login: Tou required");
            logger.debug("LLU Login: Tou required");
            tou_user();
        }
    }

    // create API url 
    url_graph = "/llu/connections/" + llu_login_data.user_id + "/graph";

    // get API graph data from LibreView server 
    if(https.begin(*llu_client, base_url + url_graph)) {
        delay(10);        

        //https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.12.0");
        https.addHeader("product", "llu.ios");
        //https.addHeader("Connection", "keep-alive");
        //https.addHeader("Accept-Encoding", "gzip, deflate, br");
        //https.addHeader("Pragma", "no-cache");
        //https.addHeader("Cache-Control", "no-cache");
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token);
        https.addHeader("Account-ID", llu_login_data.account_id);

        int code = https.GET();
        //DBGprint_LLU;Serial.printf("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                // The filter: it contains "true" for each value we want to keep
                (*json_filter)["data"]["connection"]["targetLow"] = true;
                (*json_filter)["data"]["connection"]["targetHigh"] = true;
                
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendArrow"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendMessage"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["MeasurementColor"] = true;
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

                /*
                (*json_filter)["ticket"]["token"] = true; // token does not change. 
                (*json_filter)["ticket"]["expires"] = true;
                */

                (*json_filter)["data"]["graphData"][0]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["graphData"][0]["Timestamp"] = true;
                /*
                (*json_filter)["data"]["graphData"][0]["MeasurementColor"] = true;
                (*json_filter)["data"]["graphData"][0]["isHigh"] = true;
                (*json_filter)["data"]["graphData"][0]["isLow"] = true;
                */

                // Deserialize the document with json_filter setting. keep buffer size in mind.
                deserializeJson((*json_librelinkup), https.getStream(), DeserializationOption::Filter(*json_filter));
                
                // Print the result
                //serializeJsonPretty((*json_librelinkup), Serial); Serial.println();

                llu_glucose_data.glucoseMeasurement          = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["ValueInMgPerDl"].as<int>();
                llu_glucose_data.trendArrow                  = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["TrendArrow"].as<int>();
                llu_glucose_data.measurement_color           = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["MeasurementColor"].as<int>();
                llu_glucose_data.str_TrendMessage            = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["TrendMessage"].as<String>();
                llu_glucose_data.str_measurement_timestamp   = (*json_librelinkup)["data"]["connection"]["glucoseMeasurement"]["Timestamp"].as<String>();

                llu_glucose_data.glucosetargetLow            = (*json_librelinkup)["data"]["connection"]["targetLow"].as<int>();
                llu_glucose_data.glucosetargetHigh           = (*json_librelinkup)["data"]["connection"]["targetHigh"].as<int>();
                llu_glucose_data.glucoseAlarmLow             = (*json_librelinkup)["data"]["connection"]["patientDevice"]["ll"].as<int>();
                llu_glucose_data.glucoseAlarmHigh            = (*json_librelinkup)["data"]["connection"]["patientDevice"]["hl"].as<int>();
                llu_glucose_data.glucosefixedLowAlarmValues  = (*json_librelinkup)["data"]["connection"]["patientDevice"]["fixedLowAlarmValues"]["mgdl"].as<int>();

                llu_login_data.connection_country          = (*json_librelinkup)["data"]["connection"]["country"].as<String>();
                llu_login_data.connection_status           = (*json_librelinkup)["data"]["connection"]["status"].as<int>();
                llu_sensor_data.sensor_sn_non_active        = (*json_librelinkup)["data"]["connection"]["sensor"]["sn"].as<String>();
                llu_sensor_data.sensor_id_non_active        = (*json_librelinkup)["data"]["connection"]["sensor"]["deviceId"].as<String>();
                llu_sensor_data.sensor_non_activ_unixtime   = (*json_librelinkup)["data"]["connection"]["sensor"]["a"].as<uint32_t>();
                
                llu_sensor_data.sensor_id                   = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["deviceId"].as<String>();
                llu_sensor_data.sensor_sn                   = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["sn"].as<String>();
                llu_sensor_data.sensor_state                = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["pt"].as<int>();
                llu_sensor_data.sensor_activation_time      = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["a"].as<int>();
                /*
                user_token                  = (*json_librelinkup)["ticket"]["token"].as<String>();     // token does not change.
                user_token_expires          = (*json_librelinkup)["ticket"]["expires"].as<uint32_t>();
                */
                
                // get historical glucose data (timestamp and value)
                for(uint8_t i=0;i<GRAPHDATAARRAYSIZE;i++){
                    llu_sensor_history_data.graph_data[i] = (*json_librelinkup)["data"]["graphData"][i]["ValueInMgPerDl"].as<uint16_t>();
                    if(llu_sensor_history_data.graph_data[i] == 0){
                        llu_sensor_history_data.timestamp[i] = 0;
                    }else{
                        String timestampStr = (*json_librelinkup)["data"]["graphData"][i]["Timestamp"].as<String>();
                        time_t ts = parseTimestamp(timestampStr.c_str());
                        llu_sensor_history_data.timestamp[i]  = ts;
                    }
                    //DBGprint_LLU;Serial.print("librelinkup_graph_data:");Serial.println(graph_data[i]);
                }
                
                // add current glucosemeasurement to last position (142)
                llu_sensor_history_data.graph_data[((GRAPHDATAARRAYSIZE+GRAPHDATAARRAYSIZE_PLUS_ONE)-1)] = llu_glucose_data.glucoseMeasurement;

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
                
                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_filter));
                logger.debug("json_filter     : Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("json_librelinkup: Used Bytes / Total Capacity: %d / %d", buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_filter->clear();
                json_librelinkup->clear();                                          //clears the data object
            }
            result = 1;
            https_llu_api_fetch_time = millis() - https_api_time_measure;
        }
        else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;
                        
            if (code == HTTP_CODE_UNAUTHORIZED){    //Token Auth Error handling
                DBGprint_LLU; Serial.println("Error, wrong Token -> reauthorization...");
                logger.debug("Error, wrong Token -> reauthorization...");
                json_filter->clear();
                json_librelinkup->clear();
                auth_user(settings.config.login_email,settings.config.login_password);
                result = get_graph_data();
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
        logger.debug("LLU client still connected: %d\r\n",llu_client->connected());
        llu_client->flush();
        llu_client->stop();
    }

    return result;
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
        delay(10);        

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
        delay(10);        

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
/*
time_t LIBRELINKUP::parseTimestamp(const char* timestampStr) {
    struct tm tm_time;
    memset(&tm_time, 0, sizeof(struct tm));
    strptime(timestampStr, "%m/%d/%Y %I:%M:%S %p", &tm_time);
    return mktime(&tm_time);
}
*/

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