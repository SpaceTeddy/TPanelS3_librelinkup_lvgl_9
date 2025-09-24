/*
 * Library for LIBRELINKUP function
 *
 * Christian Weithe
 * 2022-04-10
 * for ESP8266, ESP32
 */

#include "librelinkup.h"

#include "helper.h"
extern HELPER helper;

#include "settings.h"
extern SETTINGS settings;  // globale Instanz aus main.cpp

#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <mbedtls/sha256.h>

#include <FS.h>
#include <LittleFS.h>
#include <string.h>

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

// JSON-Puffergrößen
#define LIBRELINKUP_JSON_BUFFER_SIZE        12000
#define LIBRELINKUP_FILTER_JSON_BUFFER_SIZE 1024

// Lazy-Init: werden erst in begin() erzeugt
static DynamicJsonDocument* json_librelinkup = nullptr;
static DynamicJsonDocument* json_filter      = nullptr;

static WiFiClientSecure* llu_client = nullptr;
static HTTPClient https;

// --- Hilfs-Funktion: sichert, dass begin() vorher gelaufen ist
static inline bool llu_is_inited() {
    return (json_librelinkup && json_filter && llu_client);
}

/* convertToMillis
 * Parameter: hours, minutes, seconds
 * output: time in ms
 */
uint32_t LIBRELINKUP::convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    return (hours * 3600UL + minutes * 60UL + seconds) * 1000UL;
}

/* begin
 * Parameter:
 *   0 = API insecure
 *   1 = CERT aus PROGMEM
 *   2 = CERT aus LittleFS (empfohlen)
 * Return: 1 = OK, 0 = Fehler
 */
uint8_t LIBRELINKUP::begin(uint8_t use_cert) {
    // Clients/JSON lazy erzeugen
    if (!llu_client) {
        llu_client = new WiFiClientSecure();
        if (!llu_client) return 0;
        https.useHTTP10(true);
        llu_client->setTimeout(10000); // 10 s Timeout
    }
    if (!json_librelinkup) {
        json_librelinkup = new DynamicJsonDocument(LIBRELINKUP_JSON_BUFFER_SIZE);
        if (!json_librelinkup) return 0;
    }
    if (!json_filter) {
        json_filter = new DynamicJsonDocument(LIBRELINKUP_FILTER_JSON_BUFFER_SIZE);
        if (!json_filter) return 0;
    }

    // TLS-Konfig
    if (use_cert == 0) {
        llu_client->setInsecure();
    } else if (use_cert == 1) {
        llu_client->setCACert(API_ROOT_CA);
    } else if (use_cert == 2) {
        if (setCAfromfile(*llu_client, path_root_ca_googler4) == 0) { // ggf. downloaden
            DBGprint_LLU; Serial.printf("download GoogleTrustService Root R4 certificate\r\n");
            download_root_ca_to_file(url_check_GoogleTrustRootR4, path_root_ca_googler4);
        }
        setCAfromfile(*llu_client, path_root_ca_googler4);
    }

    return 1;
}

// sha256 account-id calculation as String
String LIBRELINKUP::account_id_sha256(String user_id) {
    const char *data = user_id.c_str();
    size_t len = user_id.length();

    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data), len, hash, 0);

    String hashString;
    for (int i = 0; i < 32; i++) {
        if (hash[i] < 0x10) hashString += '0';
        hashString += String(hash[i], HEX);
    }
    return hashString;
}

// check clients 0 = not connected
bool LIBRELINKUP::check_client() {
    if (!llu_is_inited()) return false;

    if (llu_client->connected() == 0) {
        llu_client->stop();
        https.end();
        return false;
    }
    return true;
}

// Get_Epoch_Time() Function that gets current epoch time
time_t LIBRELINKUP::get_epoch_time() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        logger.err("⚠️ Fehler: Konnte lokale Zeit nicht abrufen! Fallback auf `time(nullptr)`.");
        now = time(nullptr);
    } else {
        time(&now);
    }
    return now;
}

// check libre3 sensor state
uint8_t LIBRELINKUP::get_sensor_state(uint8_t state) {
    switch (state) {
        case SENSOR_NOT_STARTED: logger.debug("not yet startet"); break;
        case SENSOR_STARTING:    logger.debug("in starting phase"); break;
        case SENSOR_READY:       logger.debug("is ready"); break;
        case SENSOR_EXPIRED:     logger.debug("is expired"); break;
        case SENSOR_SHUT_DOWN:   logger.debug("is shut down"); break;
        case SENSOR_FAILURE:     logger.debug("has failure"); break;
        default:                 logger.debug("unknown"); break;
    }
    return state;
}

// check if freestyle libre3 sensor is expired
int LIBRELINKUP::check_sensor_lifetime(uint32_t unix_activation_time) {
    int result = -1;

    struct tm timeinfo;
    time_t now;

    if (!getLocalTime(&timeinfo)) {
        DBGprint_LLU; Serial.println("Failed to obtain time");
        return SENSOR_NOT_AVAILABLE;
    }

    time(&now);

    if (llu_sensor_data.sensor_id_non_active == "" &&
        llu_sensor_data.sensor_sn_non_active == "") {
        logger.debug("sensor not activ");
        result = SENSOR_NOT_AVAILABLE;
        return result;
    }
    else if (llu_sensor_data.sensor_id_non_active == "" &&
             llu_sensor_data.sensor_sn_non_active != "" &&
             unix_activation_time > 0 &&
             (unix_activation_time + 3600) > now) {
        logger.debug("sensor in startup phase!");
        int remaining_warmup_time = get_remaining_warmup_time(unix_activation_time);
        logger.debug("sensor available in: %dminutes", remaining_warmup_time);
        result = SENSOR_STARTING;
        return result;
    }
    else if ((unix_activation_time + (UNIXTIME14DAYS)) > now &&
             unix_activation_time > 0 &&
             unix_activation_time + 3600 <= now &&
             unix_activation_time + UNIXTIME14DAYS > now) {

        logger.debug("Sensor is ready!");
        result = SENSOR_READY;

        uint32_t diff_time = ((unix_activation_time + (UNIXTIME14DAYS)) - now);
        sensor_livetime.sensor_valid_days    = diff_time / 86400;
        sensor_livetime.sensor_valid_hours   = diff_time / 3600 % 24;
        sensor_livetime.sensor_valid_minutes = diff_time / 60 % 60;
        sensor_livetime.sensor_valid_seconds = diff_time % 60;

        logger.debug("Sensor expires in: Days:%02d Hours:%02d Minutes:%02d Seconds:%02d",
            sensor_livetime.sensor_valid_days,
            sensor_livetime.sensor_valid_hours,
            sensor_livetime.sensor_valid_minutes,
            sensor_livetime.sensor_valid_seconds);
        return result;
    }
    else if (llu_sensor_data.sensor_id_non_active == "" &&
             llu_sensor_data.sensor_sn_non_active != "" &&
             unix_activation_time > 0 &&
             (unix_activation_time + (UNIXTIME14DAYS)) < now) {
        DBGprint_LLU; Serial.printf("Sensor expired!\r\n");
        logger.debug("Sensor expired!");
        result = SENSOR_EXPIRED;
        return result;
    }

    return result;
}

// verbleibende Warmup-Zeit in Minuten
int LIBRELINKUP::get_remaining_warmup_time(time_t unix_activation_time) {
    time_t current_time = time(NULL);
    int remaining_time = (unix_activation_time + (60 * 60)) - current_time;
    if (remaining_time < 0) return 0;
    return remaining_time / 60;
}

// Timestamp-Check
uint8_t LIBRELINKUP::check_valid_timestamp(String librelinkup_timestamp, uint8_t print_mode) {
    uint8_t result = 0;
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo)) {
        DBGprint_LLU; Serial.println("Failed to obtain time");
        return LOCAL_TIME_ERROR;
    }

    localtime.day    = timeinfo.tm_mday;
    localtime.month  = timeinfo.tm_mon  + 1;
    localtime.year   = timeinfo.tm_year + 1900;
    localtime.hour   = timeinfo.tm_hour + timezone;
    localtime.minute = timeinfo.tm_min;
    localtime.second = timeinfo.tm_sec;

    if (localtime.hour == 24) { localtime.hour = 0; localtime.day++; }

    int hour_librelinkup_timestamp   = 0;
    int minute_librelinkup_timestamp = 0;
    int second_librelinkup_timestamp = 0;
    int day_librelinkup_timestamp    = 0;
    int month_librelinkup_timestamp  = 0;
    int year_librelinkup_timestamp   = 0;
    char meridian[3];

    if (sscanf(librelinkup_timestamp.c_str(), "%d/%d/%d %d:%d:%d %2s",
        &librelinkuptimecode.month, &librelinkuptimecode.day, &librelinkuptimecode.year,
        &librelinkuptimecode.hour, &librelinkuptimecode.minute, &librelinkuptimecode.second, meridian) != 7) {

        DBGprint_LLU; Serial.println("Error parsing date/time");
        logger.debug("Error parsing date/time");
        return SENSOR_TIMECODE_ERROR;
    }

    if (strcmp(meridian, "PM") == 0 && librelinkuptimecode.hour != 12) {
        librelinkuptimecode.hour += 12;
    } else if (strcmp(meridian, "AM") == 0 && librelinkuptimecode.hour == 12) {
        librelinkuptimecode.hour = 0;
    }

    if (print_mode == 1) {
        DBGprint_LLU;
        Serial.printf("ESP32 local Timestamp: %02d.%02d.%04d %02d:%02d:%02d\r\n",
            localtime.day, localtime.month, localtime.year,
            localtime.hour, localtime.minute, localtime.second);
        logger.notice("ESP32 local Timestamp: %02d.%02d.%04d %02d:%02d:%02d",
            localtime.day, localtime.month, localtime.year,
            localtime.hour, localtime.minute, localtime.second);

        DBGprint_LLU;
        Serial.printf("LibreLinkUp Timestamp: %02d.%02d.%04d %02d:%02d:%02d\r\n",
            librelinkuptimecode.day, librelinkuptimecode.month, librelinkuptimecode.year,
            librelinkuptimecode.hour, librelinkuptimecode.minute, librelinkuptimecode.second);
        logger.notice("LibreLinkUp Timestamp: %02d.%02d.%04d %02d:%02d:%02d",
            librelinkuptimecode.day, librelinkuptimecode.month, librelinkuptimecode.year,
            librelinkuptimecode.hour, librelinkuptimecode.minute, librelinkuptimecode.second);
    }

    if (librelinkuptimecode.day    == 0 &&
        librelinkuptimecode.month  == 0 &&
        librelinkuptimecode.year   == 0 &&
        librelinkuptimecode.hour   == 0 &&
        librelinkuptimecode.minute == 0 &&
        librelinkuptimecode.second == 0) {

        DBGprint_LLU; Serial.println("TimeCode Filter: LibreLinkUp -> 00.00.0000 00:00:00");
        logger.notice("TimeCode Filter: LibreLinkUp -> 00.00.0000 00:00:00");
        logger.notice("LLU API Timestamp: %s", librelinkup_timestamp.c_str());
        logger.notice("LLU API token: %s", llu_login_data.user_token.c_str());
        result = SENSOR_NOT_ACTIVE;
        return result;
    }

    uint32_t serverTimeMs = convertToMillis(librelinkuptimecode.hour, librelinkuptimecode.minute, librelinkuptimecode.second);
    uint32_t localTimeMs  = convertToMillis(localtime.hour, localtime.minute, localtime.second);
    int32_t timeDifferenceMs = localTimeMs - serverTimeMs;

    if ((localtime.day > librelinkuptimecode.day) || (timeDifferenceMs > LIBRELINKUPSENSORTIMEOUT)) {
        DBGprint_LLU; Serial.println("TimeCode Filter: Time Difference LocalTime - ServerTimestamp > LIBRELINKUPSENSORTIMEOUT");
        logger.debug("TimeCode Filter: Time Difference LocalTime - ServerTimestamp > LIBRELINKUPSENSORTIMEOUT");
        result = SENSOR_TIMECODE_OUT_OF_RANGE;
        return result;
    } else if ((localtime.day == librelinkuptimecode.day) && (timeDifferenceMs <= LIBRELINKUPSENSORTIMEOUT)) {
        logger.debug("TimeCode Filter: Timecode Valid");
        result = SENSOR_TIMECODE_VALID;
        return result;
    }

    return result;
}

// zählt gültige Graphdaten
uint8_t LIBRELINKUP::check_graphdata(void) {
    uint8_t count_valid_graph_data = 0;
    for (uint8_t i = 0; i < GRAPHDATAARRAYSIZE; i++) {
        if (llu_sensor_history_data.graph_data[i] != 0) {
            count_valid_graph_data++;
        }
    }
    return count_valid_graph_data;
}

// get auth data
uint16_t LIBRELINKUP::auth_user(String user_email, String user_password) {
    if (!llu_is_inited()) return 0;

    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_auth)) {
        delay(10);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.7.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");

        String httpRequestData = "{\"email\":\"" + user_email + "\",\"password\":\"" + user_password + "\"}";

        int code = https.POST(httpRequestData);
        logger.debug("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                deserializeJson((*json_librelinkup), https.getStream());

                llu_login_data.user_login_status   = (*json_librelinkup)["status"].as<uint8_t>();
                llu_login_data.user_country        = (*json_librelinkup)["data"]["user"]["country"].as<String>();
                llu_login_data.user_id             = (*json_librelinkup)["data"]["user"]["id"].as<String>();
                llu_login_data.user_token          = (*json_librelinkup)["data"]["authTicket"]["token"].as<String>();
                llu_login_data.user_token_expires  = (*json_librelinkup)["data"]["authTicket"]["expires"].as<uint32_t>();

                llu_login_data.account_id = account_id_sha256(llu_login_data.user_id);

                logger.debug("LibreLinkUp Authentification for:");
                logger.debug("user_email        : %s", settings.config.login_email.c_str());
                logger.debug("user_country      : %s", llu_login_data.user_country.c_str());
                logger.debug("user_id           : %s", llu_login_data.user_id.c_str());
                logger.debug("user_token        : %s", llu_login_data.user_token.c_str());
                logger.debug("token_exp.        : %d", llu_login_data.user_token_expires);
                logger.debug("user_login_status : %d", llu_login_data.user_login_status);
                logger.debug("account-id        : %s", llu_login_data.account_id.c_str());

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("auth json_librelinkup: Used Bytes / Total Capacity: %d / %d",
                             buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_librelinkup->clear();
            }
        } else {
            DBGprint_LLU; Serial.printf("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
        }
        https.end();

        if (llu_client->connected()) {
            DBGprint_LLU; Serial.printf("LLU client connected: %d\r\n", llu_client->connected());
            logger.debug("LLU client connected: %d\r\n", llu_client->connected());
            llu_client->flush();
            llu_client->stop();
        }
        result = 1;
    }
    return result;
}

// user Accept Terms
uint16_t LIBRELINKUP::tou_user(void) {
    if (!llu_is_inited()) return 0;

    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_tou)) {
        delay(10);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.7.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token + "");

        String httpRequestData = "";

        int code = https.POST(httpRequestData);
        DBGprint_LLU; Serial.printf("HTTP Code: [%d]\r\n", code);

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                deserializeJson((*json_librelinkup), https.getStream());

                llu_login_data.user_login_status   = (*json_librelinkup)["status"].as<uint8_t>();
                llu_login_data.user_id             = (*json_librelinkup)["data"]["user"]["id"].as<String>();
                llu_login_data.user_country        = (*json_librelinkup)["data"]["user"]["country"].as<String>();

                logger.debug("LibreLinkUp Accept Terms for: %s", settings.config.login_email.c_str());
                logger.debug("user_id           : %s", llu_login_data.user_id.c_str());
                logger.debug("user_country      : %s", llu_login_data.user_country.c_str());
                logger.debug("user_login_status : %d", llu_login_data.user_login_status);

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("tou json_librelinkup: Used Bytes / Total Capacity: %d / %d",
                             buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_librelinkup->clear();
            }
        } else {
            DBGprint_LLU; Serial.printf("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] POST... failed, error: %s\r\n", https.errorToString(code).c_str());
        }
        https.end();

        if (llu_client->connected()) {
            DBGprint_LLU; Serial.printf("LLU client connected: %d\r\n", llu_client->connected());
            llu_client->flush();
            llu_client->stop();
        }
        result = 1;
    }
    return result;
}

// get connection data
uint16_t LIBRELINKUP::get_connection_data(void) {
    if (!llu_is_inited()) return 0;

    int8_t result = 0;

    llu_glucose_data.str_measurement_timestamp = "";

    if (llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null") {
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU; Serial.println("Auth User: no user_id available!");
        auth_user(settings.config.login_email, settings.config.login_password);
        if (llu_login_data.user_login_status == 4) {
            DBGprint_LLU; Serial.println("LLU Login: Tou required");
            tou_user();
        }
    }

    if (https.begin(*llu_client, base_url + url_connection)) {
        delay(10);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.12.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Connection", "keep-alive");
        https.addHeader("Pragma", "no-cache");
        https.addHeader("Cache-Control", "no-cache");
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token);
        https.addHeader("Account-ID", llu_login_data.account_id);

        int code = https.GET();

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                (*json_filter)["data"][0]["glucoseMeasurement"]["Timestamp"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["TrendArrow"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["TrendMessage"] = true;
                (*json_filter)["data"][0]["glucoseMeasurement"]["MeasurementColor"] = true;

                deserializeJson((*json_librelinkup), https.getStream(), DeserializationOption::Filter(*json_filter));

                llu_glucose_data.glucoseMeasurement        = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"].as<int>();
                llu_glucose_data.trendArrow                = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["TrendArrow"].as<int>();
                llu_glucose_data.measurement_color         = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["MeasurementColor"].as<int>();
                llu_glucose_data.str_TrendMessage          = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["TrendMessage"].as<String>();
                llu_glucose_data.str_measurement_timestamp = (*json_librelinkup)["data"][0]["glucoseMeasurement"]["Timestamp"].as<String>();

                if      (llu_glucose_data.trendArrow == 0) llu_glucose_data.str_trendArrow = "no Data";
                else if (llu_glucose_data.trendArrow == 1) llu_glucose_data.str_trendArrow = "↓";
                else if (llu_glucose_data.trendArrow == 2) llu_glucose_data.str_trendArrow = "↘";
                else if (llu_glucose_data.trendArrow == 3) llu_glucose_data.str_trendArrow = "→";
                else if (llu_glucose_data.trendArrow == 4) llu_glucose_data.str_trendArrow = "↗";
                else if (llu_glucose_data.trendArrow == 5) llu_glucose_data.str_trendArrow = "↑";

                json_filter->clear();
                json_librelinkup->clear();

            }
            result = 1;
        } else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;

            if (code == HTTP_CODE_UNAUTHORIZED) {
                DBGprint_LLU; Serial.println("Error, wrong Token -> reauthorization...");
                auth_user(settings.config.login_email, settings.config.login_password);
                json_filter->clear();
                json_librelinkup->clear();
            }
        }
        https.end();
    } else {
        result = 0;
    }

    if (llu_client->connected()) {
        DBGprint_LLU; Serial.printf("LLU client still connected: %d\r\n", llu_client->connected());
        llu_client->flush();
        llu_client->stop();
    }

    return result;
}

// get graph glycose data
uint16_t LIBRELINKUP::get_graph_data(void) {
    if (!llu_is_inited()) return 0;

    int8_t result = 0;
    uint32_t https_api_time_measure = millis();

    check_client();

    llu_glucose_data.str_measurement_timestamp = "";
    memset(llu_sensor_history_data.graph_data, 0, GRAPHDATAARRAYSIZE);
    memset(llu_sensor_history_data.timestamp,  0, GRAPHDATAARRAYSIZE);

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

    url_graph = "/llu/connections/" + llu_login_data.user_id + "/graph";

    if (https.begin(*llu_client, base_url + url_graph)) {
        delay(10);

        https.addHeader("Content-Type", "application/json");
        https.addHeader("version", "4.12.0");
        https.addHeader("product", "llu.ios");
        https.addHeader("Authorization","Bearer " + llu_login_data.user_token);
        https.addHeader("Account-ID",   llu_login_data.account_id);

        int code = https.GET();

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {

                (*json_filter)["data"]["connection"]["targetLow"]  = true;
                (*json_filter)["data"]["connection"]["targetHigh"] = true;

                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendArrow"]     = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["TrendMessage"]   = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["MeasurementColor"] = true;
                (*json_filter)["data"]["connection"]["glucoseMeasurement"]["Timestamp"]      = true;

                (*json_filter)["data"]["connection"]["patientDevice"]["ll"] = true;
                (*json_filter)["data"]["connection"]["patientDevice"]["hl"] = true;
                (*json_filter)["data"]["connection"]["patientDevice"]["fixedLowAlarmValues"]["mgdl"] = true;

                (*json_filter)["data"]["connection"]["status"]   = true;
                (*json_filter)["data"]["connection"]["country"]  = true;
                (*json_filter)["data"]["connection"]["sensor"]["sn"] = true;
                (*json_filter)["data"]["connection"]["sensor"]["deviceId"] = true;
                (*json_filter)["data"]["connection"]["sensor"]["a"] = true;

                (*json_filter)["data"]["activeSensors"][0]["sensor"]["deviceId"] = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["sn"]       = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["a"]        = true;
                (*json_filter)["data"]["activeSensors"][0]["sensor"]["pt"]       = true;

                (*json_filter)["data"]["graphData"][0]["ValueInMgPerDl"] = true;
                (*json_filter)["data"]["graphData"][0]["Timestamp"]      = true;

                deserializeJson((*json_librelinkup), https.getStream(), DeserializationOption::Filter(*json_filter));

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
                llu_sensor_data.sensor_sn_non_active       = (*json_librelinkup)["data"]["connection"]["sensor"]["sn"].as<String>();
                llu_sensor_data.sensor_id_non_active       = (*json_librelinkup)["data"]["connection"]["sensor"]["deviceId"].as<String>();
                llu_sensor_data.sensor_non_activ_unixtime  = (*json_librelinkup)["data"]["connection"]["sensor"]["a"].as<uint32_t>();

                llu_sensor_data.sensor_id                  = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["deviceId"].as<String>();
                llu_sensor_data.sensor_sn                  = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["sn"].as<String>();
                llu_sensor_data.sensor_state               = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["pt"].as<int>();
                llu_sensor_data.sensor_activation_time     = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["a"].as<int>();

                for (uint8_t i = 0; i < GRAPHDATAARRAYSIZE; i++) {
                    llu_sensor_history_data.graph_data[i] = (*json_librelinkup)["data"]["graphData"][i]["ValueInMgPerDl"].as<uint16_t>();
                    if (llu_sensor_history_data.graph_data[i] == 0) {
                        llu_sensor_history_data.timestamp[i] = 0;
                    } else {
                        String timestampStr = (*json_librelinkup)["data"]["graphData"][i]["Timestamp"].as<String>();
                        time_t ts = parseTimestamp(timestampStr.c_str());
                        llu_sensor_history_data.timestamp[i] = ts;
                    }
                }

                // aktuellen Wert hinten anhängen (achte später beim Zeichnen auf gültigen Index!)
                llu_sensor_history_data.graph_data[((GRAPHDATAARRAYSIZE + GRAPHDATAARRAYSIZE_PLUS_ONE) - 1)] = llu_glucose_data.glucoseMeasurement;

                if      (llu_glucose_data.trendArrow == 0) llu_glucose_data.str_trendArrow = "no Data";
                else if (llu_glucose_data.trendArrow == 1) llu_glucose_data.str_trendArrow = "↓";
                else if (llu_glucose_data.trendArrow == 2) llu_glucose_data.str_trendArrow = "↘";
                else if (llu_glucose_data.trendArrow == 3) llu_glucose_data.str_trendArrow = "→";
                else if (llu_glucose_data.trendArrow == 4) llu_glucose_data.str_trendArrow = "↗";
                else if (llu_glucose_data.trendArrow == 5) llu_glucose_data.str_trendArrow = "↑";

                Json_Buffer_Info buffer_info;
                buffer_info = helper.getBufferSize(&(*json_filter));
                logger.debug("json_filter     : Used Bytes / Total Capacity: %d / %d",
                             buffer_info.usedCapacity, buffer_info.totalCapacity);

                buffer_info = helper.getBufferSize(&(*json_librelinkup));
                logger.debug("json_librelinkup: Used Bytes / Total Capacity: %d / %d",
                             buffer_info.usedCapacity, buffer_info.totalCapacity);

                json_filter->clear();
                json_librelinkup->clear();
            }
            result = 1;
            https_llu_api_fetch_time = millis() - https_api_time_measure;
        } else {
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
        https.end();
    } else {
        result = 0;
    }

    if (llu_client->connected()) {
        DBGprint_LLU; Serial.printf("LLU client still connected: %d\r\n", llu_client->connected());
        logger.debug("LLU client still connected: %d\r\n", llu_client->connected());
        llu_client->flush();
        llu_client->stop();
    }

    return result;
}

// get WiFiClientSecure client pointer
WiFiClientSecure & LIBRELINKUP::get_wifisecureclient(void) {
    // Achtung: begin() muss vorher gelaufen sein!
    return *llu_client;
}

// check connection to server
void LIBRELINKUP::check_https_connection(const char* url) {
    if (!llu_is_inited()) return;

    if (https.begin(*llu_client, url)) {
        delay(10);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");

        int code = https.GET();

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                DBGprint_LLU; Serial.printf("connection to %s successful. HTTPS Code: [%d]\r\n", url, code);
                logger.debug("connection to %s successful. HTTPS Code: [%d]", url, code);
            }
        } else {
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            logger.debug("[HTTP] GET... failed, error: %s", https.errorToString(code).c_str());
        }
        https.end();
    }
}

// set new root certificate
bool LIBRELINKUP::setCAfromfile(WiFiClientSecure &client, const char* ca_file) {
    File ca = LittleFS.open(ca_file, "r");
    if (!ca) {
        Serial.println("ERROR!");
        return 0;
    } else {
        size_t certSize = ca.size();
        if (certSize == 0) {
            DBGprint_LLU; Serial.println("CA from File is empty. please downlaod again");
            return 0;
        }
        client.loadCACert(ca, certSize);
        ca.close();
        DBGprint_LLU; Serial.println("set CA from File -> done");
        logger.notice("set CA from File -> done");
        return 1;
    }
}

void LIBRELINKUP::showCAfromfile(const char* ca_file) {
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
    Serial.printf("CA from: %s:\r\n%s\r\n", ca_file, new_certificate);
    logger.notice("CA from: %s:", ca_file);

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

uint16_t LIBRELINKUP::download_root_ca_to_file(const char* download_url, const char* file_name) {
    if (!llu_is_inited()) return 0;

    int8_t result = 0;

    File file = LittleFS.open(file_name, "w");
    if (!file) {
        DBGprint_LLU; Serial.println("- failed to open file for writing");
        logger.notice("- failed to open file for writing");
        return 0;
    }

    llu_client->setInsecure();
    DBGprint_LLU; Serial.print("download CA started...");
    logger.notice("download CA started...");

    if (https.begin(*llu_client, download_url)) {
        delay(10);

        https.addHeader("User-Agent", "Mozilla/5.0");
        https.addHeader("Content-Type", "application/json");

        int code = https.GET();

        if (code > 0) {
            if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY) {
                https.writeToStream(&file);
            }
            result = 1;
            file.close();
            Serial.println("finished");
            logger.notice("finished");
        } else {
            Serial.println("failed!");
            logger.notice("download failed!");
            DBGprint_LLU; Serial.printf("[HTTP] GET... failed, error: %s\r\n", https.errorToString(code).c_str());
            result = 0;
        }
        https.end();
    } else {
        result = 0;
    }
    return result;
}

// read2String
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

// String-Timestamp in time_t umwandeln (AM/PM)
time_t LIBRELINKUP::parseTimestamp(const char* timestampStr) {
    setlocale(LC_TIME, "C");

    struct tm tm_time;
    memset(&tm_time, 0, sizeof(struct tm));

    char timeStr[50];
    strncpy(timeStr, timestampStr, sizeof(timeStr) - 1);
    timeStr[sizeof(timeStr) - 1] = '\0';

    int is_pm = strstr(timeStr, "PM") != NULL;

    char clean_timeStr[50];
    strncpy(clean_timeStr, timeStr, sizeof(clean_timeStr) - 1);
    clean_timeStr[sizeof(clean_timeStr) - 1] = '\0';
    char* am_pm = strstr(clean_timeStr, "AM");
    if (!am_pm) am_pm = strstr(clean_timeStr, "PM");
    if (am_pm) *am_pm = '\0';

    char* ret = strptime(clean_timeStr, "%m/%d/%Y %I:%M:%S", &tm_time);
    if (!ret) {
        printf("⚠️ strptime() konnte den String nicht parsen.\n");
        return (time_t)-1;
    }

    if (is_pm && tm_time.tm_hour != 12) {
        tm_time.tm_hour += 12;
    } else if (!is_pm && tm_time.tm_hour == 12) {
        tm_time.tm_hour = 0;
    }

    tm_time.tm_isdst = -1;

    time_t timestamp = mktime(&tm_time);

    //printf("Input: %s → Parsed Time: %02d:%02d:%02d | Unix: %ld\n",timestampStr, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, (long)timestamp);

    return timestamp;
}