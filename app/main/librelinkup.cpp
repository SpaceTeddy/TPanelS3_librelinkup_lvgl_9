/*
 * Library for LIBRELINKUP function
 * 
 * Christian Weithe
 * 2022-04-10
 * for ESP8266, ESP32
 * 
*/

#include "librelinkup.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <mbedtls/sha256.h>

#include <FS.h>
#include <LittleFS.h>
#include <string.h>
#include <uuid/log.h>

// Google Trust Services R4 Root CA for api.libreview.io
static const char API_ROOT_CA[] PROGMEM = R"CERT(...)CERT";

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

/**
 * @brief Converts time components to milliseconds.
 * @param hours Hour component (0-23)
 * @param minutes Minute component (0-59)
 * @param seconds Second component (0-59)
 * @return Calculated milliseconds
 */
uint32_t LIBRELINKUP::convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    return (hours * 3600UL + minutes * 60UL + seconds) * 1000UL;
}

/**
 * @brief Adds default headers to the HTTP client.
 *
 * This function adds the necessary default headers for LibreLinkUp API requests.
 *
 * @param https The HTTP client instance.
 */
void LIBRELINKUP::addDefaultLLUHeaders(HTTPClient& https) {
    https.addHeader("User-Agent", "Mozilla/5.0");
    https.addHeader("Content-Type", "application/json");
    https.addHeader("version", "4.16.0");
    https.addHeader("product", "llu.ios");
    https.addHeader("Connection", "close");
    https.addHeader("Pragma", "no-cache");
    https.addHeader("Cache-Control", "no-cache");
}

/**
 * @brief Adds authentication headers to the HTTP client.
 *
 * This function adds the necessary authentication headers for LibreLinkUp API requests.
 *
 * @param https       The HTTP client instance.
 * @param bearer      The bearer token for authentication.
 * @param accountId   The account ID for the request.
 */
void LIBRELINKUP::addAuthHeaders(HTTPClient& https, const String& bearer, const String& accountId) {
    https.addHeader("Authorization", "Bearer " + bearer);
    if (accountId.length()) {
        https.addHeader("Account-ID", accountId);
    }
}

/**
 * @brief Re-authenticates the user with the LibreLinkUp API.
 *
 * This function attempts to re-authenticate the user using stored credentials.
 *
 * @return 1 if re-authentication is successful, 0 otherwise.
 */
uint16_t LIBRELINKUP::reauth_user() {
    if (llu_login_data.email.isEmpty() || llu_login_data.password.isEmpty()) {
        logger.warning("Reauth requested but no stored credentials available");
        return 0;
    }
    return auth_user(llu_login_data.email, llu_login_data.password);
}

/**
 * @brief Sets the user credentials for LibreLinkUp API access.
 *
 * This function stores the provided email and password for later use in API authentication.
 *
 * @param user_email    The user's email address.
 * @param user_password The user's password.
 */
void LIBRELINKUP::set_credentials(const String& user_email, const String& user_password) {
    llu_login_data.email = user_email;
    llu_login_data.password = user_password;
}

/**
 * @brief Checks if user credentials are available.
 *
 * This function checks if both email and password are stored.
 *
 * @return true if credentials are available, false otherwise.
 */
bool LIBRELINKUP::has_credentials() const {
    return !(llu_login_data.email.isEmpty() || llu_login_data.password.isEmpty());
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

/**
 * @brief Initializes the LibreLinkUp client.
 *
 * This function sets up the HTTP client and configures the necessary certificates for secure communication.
 *
 * @param use_cert The certificate usage mode (0=Insecure, 1=PROGMEM, 2=LittleFS).
 * @return 1 if initialization is successful, 0 otherwise.
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

/**
 * @brief Calculates the SHA-256 hash of the user ID.
 * @param user_id The user ID string.
 * @return The SHA-256 hash as a hexadecimal string.
 */
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

/**
 * @brief Checks the connection status of the HTTP client.
 * @return 1 if the client is connected, 0 otherwise.
 */
bool LIBRELINKUP::check_client(){

    if(llu_client->connected() == 0){
        llu_client->stop();
        https.end();
        return 0;
    }
    return 1;
}

/**
 * @brief Gets the current epoch time.
 * @return The current epoch time.
 */
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

/**
 * @brief Compares two sensor serial numbers.
 * @param s1 First sensor serial number.
 * @param s2 Second sensor serial number.
 * @return -1 if s1 < s2, 0 if equal, 1 if s1 > s2.
 */
int LIBRELINKUP::check_sensor_type(const char *s1, const char *s2) {
    int result = strcmp(s1, s2);
    
    if (result < 0) {
        return -1;  // s1 ist älter
    } else if (result > 0) {
        return 1;   // s1 ist neuer
    }
    
    return 0;  // Beide sind identisch
}

/**
 * @brief Gets the current state of the sensor.
 * @param state The sensor state to check.
 * @return The sensor state.
 */
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

/**
 * @brief Checks the lifetime of the sensor.
 * @param unix_activation_time The Unix timestamp of sensor activation.
 * @param sensor_runtime The runtime of the sensor in seconds.
 * @return The sensor state based on its lifetime.
 */
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

/**
 * @brief Checks the sensor type based on its serial number.
 *
 * This function compares the sensor's serial number with a known reference to determine its type.
 * It also sets the sensor runtime based on the identified type.
 *
 * @return 1 if the sensor is identified as Libre 3 Plus, -1 if it's Libre 3, and 0 if unknown or not checked.
 */
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

/**
 * @brief Gets the remaining warm-up time for the sensor.
 * @param unix_activation_time The Unix timestamp of sensor activation.
 * @return The remaining warm-up time in minutes.
 */
int LIBRELINKUP::get_remaining_warmup_time(time_t unix_activation_time) {
    time_t current_time = time(NULL);  // Aktuelle Zeit holen (Unix-Zeit)
    int remaining_time = (unix_activation_time + (60 * 60)) - current_time;  // 60 Minuten Warmup

    // Falls die Zeit bereits abgelaufen ist, auf 0 setzen
    if (remaining_time < 0) return 0;

    return remaining_time / 60;  // Sekunden in Minuten umrechnen
}

/**
 * @brief Checks the validity of the factory timestamp against the cloud timestamp.
 * @param factory_ts The factory timestamp.
 * @param cloud_ts The cloud timestamp.
 * @param print_mode The print mode for debugging.
 * @return The validity state of the timestamp.
 */
uint8_t LIBRELINKUP::check_valid_timestamp_factory(
    const String& factory_ts,
    const String& cloud_ts,
    uint8_t print_mode)
{
    time_t now = time(nullptr);
    if (now < 1700000000) {
        logger.debug("Failed to obtain valid time (NTP?)");
        return LOCAL_TIME_ERROR;
    }

    time_t tCloud = parseTimestamp(cloud_ts.c_str());
    time_t tFactory = parseTimestamp(factory_ts.c_str());
    if (tFactory == 0) {
        logger.debug("Error parsing FactoryTimestamp: %s", factory_ts.c_str());
        return SENSOR_TIMECODE_ERROR;
    }

    // Determine local UTC offset robustly from current system TZ/DST state
    // NOTE: Ensure TZ is set to Europe/Berlin rules once at boot:
    // setenv("TZ","CET-1CEST,M3.5.0/2,M10.5.0/3",1); tzset();
    struct tm lt {};
    localtime_r(&now, &lt);

    int32_t offset_s = (lt.tm_isdst > 0) ? 7200 : 3600;

    // Optional: keep your "locked" offset variables consistent
    tz_locked = 1;
    tz_offset_s_locked = offset_s;

    // Local time of measurement would be Factory + offset
    time_t tLocalMeas = tFactory + (time_t)offset_s;

    // diff between "now" (UTC epoch) and "meas expressed in UTC epoch"
    // We want to know how long ago the measurement happened in local terms:
    // now - (factory + offset)
    int32_t diff_ms = (int32_t)(now - (tFactory + (time_t)offset_s)) * 1000;

    if (print_mode == 1) {
        logger.debug("tz_locked=%d offset_s=%ld", (int)tz_locked, (long)tz_offset_s_locked);
        logger.debug("ESP32 now epoch                   : %ld", (long)now);
        logger.debug("Factory epoch                     : %ld", (long)tFactory);
        logger.debug("Cloud TS epoch                    : %ld", (long)tCloud);
        logger.debug("Local meas epoch (factory+off)    : %ld", (long)tLocalMeas);
        logger.debug("diff_ms                           : %ld (timeout=%ld)",
                      (long)diff_ms, (long)LIBRELINKUPSENSORTIMEOUT);
    }
    if (diff_ms < 0 || (diff_ms > LIBRELINKUPSENSORTIMEOUT && diff_ms < LIBRELINKUPSENSORLOSTTIMEOUT)){
        logger.debug("Sensor Factory TimeStamp out of range");
        return SENSOR_TIMECODE_OUT_OF_RANGE;
    }
    if(diff_ms > LIBRELINKUPSENSORLOSTTIMEOUT){
        logger.debug("Sensor active but probably lost by user");
        return SENSOR_LOST;
    }
    return SENSOR_TIMECODE_VALID;
}


/**
 * @brief Checks the validity of the glucose graph data.
 * @return The count of valid glucose data points.
 */
uint8_t LIBRELINKUP::check_graphdata(void){
    
    uint8_t count_valid_graph_data = 0;

    for(uint8_t i=0;i<GRAPHDATAARRAYSIZE;i++){
        if(llu_sensor_history_data.graph_data[i] != 0){
            count_valid_graph_data++;
        }
    }
    
    return count_valid_graph_data;
}


/**
 * @brief Converts a region code to the corresponding base URL for the LibreLinkUp API.
 * @param region The region code (e.g., "de", "eu").
 * @return The base URL for the specified region.
 */
String LIBRELINKUP::regionToBaseUrl(const String& region) {
    if (region == "de" || region == "eu") return "https://api-de.libreview.io";
    return "https://api.libreview.io";
}


/**
 * @brief Accepts the terms of use for the LibreLinkUp user.
 * @return 1 if successful, 0 otherwise.
 */
uint16_t LIBRELINKUP::tou_user(void){
    
    uint8_t result = 0;

    if (https.begin(*llu_client, base_url + url_user_tou)) {
        //delay(10);        
        vTaskDelay(pdMS_TO_TICKS(10));
        //Serial.println("Connected to: " + url);

        addDefaultLLUHeaders(https);
        addAuthHeaders(https, llu_login_data.user_token, "");

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
                DBGprint_LLU;Serial.print("LibreLinkUp Accept Terms for: ");Serial.println(llu_login_data.email);
                DBGprint_LLU;Serial.print("user_id           : ");Serial.println(llu_login_data.user_id);
                DBGprint_LLU;Serial.print("user_country      : ");Serial.println(llu_login_data.user_country);
                DBGprint_LLU;Serial.print("user_login_status : ");Serial.println(llu_login_data.user_login_status);
                Serial.println();

                logger.debug("LibreLinkUp Accept Terms for: %s",llu_login_data.email.c_str());
                logger.debug("user_id           : %s",llu_login_data.user_id.c_str());
                logger.debug("user_country      : %s",llu_login_data.user_country.c_str());
                logger.debug("user_login_status : %d",llu_login_data.user_login_status);

                logger.debug("tou json_librelinkup: Used Bytes / Total Capacity: %u / %u",
                             (unsigned)json_librelinkup->memoryUsage(),
                             (unsigned)json_librelinkup->capacity());

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

/**
 * @brief Authenticates a user with the LibreLinkUp API.
 * @param user_email The user's email address.
 * @param user_password The user's password.
 * @return 1 if authentication is successful, 0 otherwise.
 */
uint16_t LIBRELINKUP::auth_user(String user_email, String user_password){

    uint8_t result = 0;

    // Keep credentials for automatic re-authentication inside the library.
    set_credentials(user_email, user_password);

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


/**
 * @brief Gets the connection data from the LibreLinkUp API.
 * @return 1 if successful, 0 otherwise.
 */
uint16_t LIBRELINKUP::get_connection_data(void){
    
    int8_t result = 0;
    
    // resets previuos timestamp
    llu_glucose_data.str_measurement_timestamp = "";
    
    // get user ID and Token, if AuthToken not already pulled 
    if(llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null" /*strcmp(user_token.c_str(), "null") == 0*/){
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU;Serial.println("Auth User: no user_id available!");
        if (reauth_user() == 0) {
            logger.warning("Auth User failed: missing credentials or reauth failed");
            return 0;
        }
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
        addAuthHeaders(https, llu_login_data.user_token, llu_login_data.account_id);

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
                reauth_user();
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

/**
 * @brief Gets the graph data from the LibreLinkUp API.
 * @return 1 if successful, 0 otherwise.
 */
uint16_t LIBRELINKUP::get_graph_data(void){

    int8_t result = 0;
    uint32_t https_api_time_measure = millis();

    check_client();

    // get user ID and Token, if AuthToken not already pulled 
    if (llu_login_data.user_id == "" || llu_login_data.user_token == "" || llu_login_data.user_token == "null") {
        logger.debug("Auth User: no user_id available!");
        DBGprint_LLU; Serial.println("Auth User: no user_id available!");
        if (reauth_user() == 0) {
            logger.warning("Auth User failed: missing credentials or reauth failed");
            return 0;
        }
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
        addAuthHeaders(https, llu_login_data.user_token, llu_login_data.account_id);

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

                logger.debug("json_librelinkup: Used Bytes / Total Capacity: %u / %u",
                             (unsigned)json_librelinkup->memoryUsage(),
                             (unsigned)json_librelinkup->capacity());
                logger.debug("json_filter     : Used Bytes / Total Capacity: %u / %u",
                             (unsigned)json_filter->memoryUsage(),
                             (unsigned)json_filter->capacity());

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
                reauth_user();
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

/**
 * @brief Ingests graph JSON data from an external source.
 * @param data The JSON data to ingest.
 * @param len The length of the JSON data.
 * @return 1 if successful, 0 otherwise.
 */
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

    // --- Update timezone offset once ---
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

/**
 * @brief Gets the last graph JSON data as a String.
 * @return A reference to the last graph JSON data.
 */
const String& LIBRELINKUP::get_last_graph_json() const {
    return last_graph_json;
}


/**
 * @brief Gets the WiFiClientSecure client pointer.
 * @return A reference to the WiFiClientSecure client.
 */
WiFiClientSecure & LIBRELINKUP::get_wifisecureclient(void){
    return *llu_client;
}

/**
 * @brief Checks the HTTPS connection to a specified URL.
 * @param url The URL to check the connection for.
 */
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


/**
 * @brief Sets the CA certificate from a file.
 * @param client The WiFiClientSecure client.
 * @param ca_file The path to the CA certificate file.
 * @return 1 if successful, 0 otherwise.
 */
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


/**
 * @brief Shows the CA certificate from a file.
 * @param ca_file The path to the CA certificate file.
 */
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

/**
 * @brief Downloads the root CA certificate from a specified URL and saves it to a file.
 * @param download_url The URL to download the CA certificate from.
 * @param file_name The name of the file to save the CA certificate to.
 * @return 1 if successful, 0 otherwise.
 */
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

/**
 * @brief Reads the content of a file into a string.
 * @param fs The file system to read from.
 * @param path The path to the file.
 * @param myString The buffer to store the string in.
 * @param maxLength The maximum length of the string (including null terminator).
 * @return true if successful, false otherwise.
 */
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

/**
 * @brief Parses a timestamp string into a time_t value.
 * @param timestampStr The timestamp string to parse.
 * @return The parsed time_t value, or -1 if parsing fails.
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
        DBGprint_LLU; Serial.println("strptime() konnte den String nicht parsen.");
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

/**
 * @brief Updates the time zone offset once based on local and factory timestamps.
 * @param ts_local The local timestamp.
 * @param ts_factory The factory timestamp.
 * @return true if successful, false otherwise.
 */
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
