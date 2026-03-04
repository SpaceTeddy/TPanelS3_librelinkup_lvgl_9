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

/**
 * @brief Generates a SHA-256 hash of the LibreLinkUp user ID.
 *
 * This function computes the SHA-256 digest of the provided
 * LibreLinkUp @p user_id and returns the result as a lowercase
 * hexadecimal string (64 characters).
 *
 * The resulting hash is typically used as the Account-ID header
 * when communicating with the LibreLinkUp API.
 *
 * @param user_id
 *        The LibreLinkUp user identifier (plain string).
 *
 * @return
 *        A 64-character lowercase hexadecimal string representing
 *        the SHA-256 hash of the input.
 *
 * @note
 *        This function uses mbedtls_sha256() in SHA-256 mode (not SHA-224).
 *
 * @warning
 *        The function does not validate whether @p user_id is empty.
 *        An empty string will still produce a valid SHA-256 hash.
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
 * @brief Checks whether the underlying TLS client connection is still active.
 *
 * This function verifies if the internal @c WiFiClientSecure instance
 * used for LibreLinkUp communication is currently connected.
 *
 * If the client is not connected, the function performs cleanup by:
 *  - Stopping the TLS client
 *  - Ending the associated HTTPClient session
 *
 * @return
 *        true  if the TLS client is still connected,
 *        false if the connection is closed and cleanup was performed.
 *
 * @note
 *        This function ensures that stale or half-open TLS connections
 *        do not remain active before issuing new HTTP requests.
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
 * @brief Retrieves the current Unix epoch time.
 *
 * This function attempts to obtain the current local time using
 * getLocalTime(). If successful, it converts the system time to
 * epoch format using time().
 *
 * If getLocalTime() fails (e.g., NTP not yet synchronized),
 * the function falls back to time(nullptr) to still provide
 * a best-effort epoch value.
 *
 * @return
 *        Current Unix epoch time (seconds since 1970-01-01 00:00:00 UTC).
 *
 * @note
 *        If NTP synchronization has not yet occurred, the returned
 *        value may be invalid or near zero depending on system state.
 *
 * @warning
 *        Callers should verify that the returned value is plausible
 *        (e.g., greater than a known minimum epoch threshold) before
 *        relying on it for time-sensitive calculations.
 */
time_t LIBRELINKUP::get_epoch_time() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        logger.debug("could not get local time. Fallback `time(nullptr)`.");
        now = time(nullptr);  // Falls getLocalTime() fehlschlägt, nutze time()
    } else {
        time(&now);
    }
    return now;
}

/**
 * @brief Compares two sensor serial number strings lexicographically.
 *
 * This function performs a standard C-string comparison using strcmp()
 * and returns a normalized comparison result.
 *
 * @param s1
 *        First sensor serial string.
 *
 * @param s2
 *        Second sensor serial string.
 *
 * @return
 *        -1 if s1 is lexicographically smaller than s2,
 *         0 if both strings are equal,
 *         1 if s1 is lexicographically greater than s2.
 *
 * @note
 *        The comparison is purely lexicographical (byte-wise) and does
 *        not interpret the serial numbers numerically.
 *
 * @warning
 *        Both input pointers must be valid null-terminated strings.
 *        Passing nullptr results in undefined behavior.
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
 * @brief Gets the state of a LibreLinkUp sensor.
 *
 * This function takes a sensor state identifier and returns the corresponding
 * human-readable string for logging purposes.
 *
 * @param state
 *        The sensor state identifier.
 *
 * @return
 *        The sensor state string.
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
 * @brief Evaluates the lifecycle state of the active Libre sensor.
 *
 * This function determines the current sensor state based on:
 *  - The activation timestamp (@p unix_activation_time)
 *  - The configured sensor runtime (@p sensor_runtime)
 *  - The current system time
 *
 * It distinguishes between:
 *  - SENSOR_NOT_AVAILABLE (no active sensor detected or no valid time)
 *  - SENSOR_STARTING      (within warm-up phase, typically first 60 minutes)
 *  - SENSOR_READY         (active and within valid runtime window)
 *  - SENSOR_EXPIRED       (runtime exceeded)
 *
 * When the sensor is in READY state, the remaining lifetime is calculated
 * and stored in @c sensor_livetime (days, hours, minutes, seconds).
 *
 * @param unix_activation_time
 *        Sensor activation time in Unix epoch seconds.
 *
 * @param sensor_runtime
 *        Total allowed runtime of the sensor in seconds
 *        (e.g., 14 or 15 days expressed in seconds).
 *
 * @return
 *        One of the following sensor states:
 *        - SENSOR_NOT_AVAILABLE
 *        - SENSOR_STARTING
 *        - SENSOR_READY
 *        - SENSOR_EXPIRED
 *
 * @note
 *        The function requires a valid system time (NTP synchronized).
 *        If time retrieval fails, SENSOR_NOT_AVAILABLE is returned.
 *
 * @warning
 *        This function evaluates lifecycle state only.
 *        It does NOT determine data freshness or signal validity.
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
 * @brief Determines the Libre sensor type based on the serial number prefix.
 *
 * This function compares the currently active sensor serial number
 * against a predefined reference prefix (e.g. LIBRE3PLUS_SERIAL_START)
 * to distinguish between Libre 3 and Libre 3 Plus sensors.
 *
 * Based on the comparison result, the sensor runtime is configured:
 *  - Libre 3       → 14 days
 *  - Libre 3 Plus  → 15 days
 *
 * The check is intended to be performed only once per session.
 *
 * @return
 *        1  if the sensor is identified as Libre 3 Plus,
 *       -1  if identified as Libre 3,
 *        0  if no sensor is available, already checked,
 *           or the type is unknown.
 *
 * @note
 *        The detection is based on lexicographical comparison of
 *        serial number prefixes and assumes consistent formatting.
 *
 * @warning
 *        The function depends on a valid non-empty sensor serial number.
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
 * @brief Calculates the remaining sensor warm-up time in minutes.
 *
 * Libre sensors typically require a fixed warm-up period (60 minutes)
 * after activation before they begin delivering valid measurements.
 *
 * This function determines how many minutes remain until the warm-up
 * period has completed, based on the activation timestamp and the
 * current system time.
 *
 * @param unix_activation_time
 *        Sensor activation time in Unix epoch seconds.
 *
 * @return
 *        Remaining warm-up time in minutes.
 *        Returns 0 if the warm-up period has already completed.
 *
 * @note
 *        This function assumes that the system time is valid and
 *        synchronized (e.g. via NTP).
 *
 * @warning
 *        If system time is not properly initialized, the returned
 *        value may be incorrect.
 */
int LIBRELINKUP::get_remaining_warmup_time(time_t unix_activation_time) {
    time_t current_time = time(NULL);  // Aktuelle Zeit holen (Unix-Zeit)
    int remaining_time = (unix_activation_time + (60 * 60)) - current_time;  // 60 Minuten Warmup

    // Falls die Zeit bereits abgelaufen ist, auf 0 setzen
    if (remaining_time < 0) return 0;

    return remaining_time / 60;  // Sekunden in Minuten umrechnen
}

/**
 * @brief Validates LibreView measurement timestamps and updates data freshness state.
 *
 * This function validates the provided factory and cloud timestamps against
 * the current system time and determines whether the measurement is still valid.
 *
 * The function performs the following steps:
 *  - Verifies that system time (NTP) is initialized.
 *  - Parses both factory and cloud timestamp strings.
 *  - Derives the local timezone offset (including DST).
 *  - Computes the measurement age in milliseconds.
 *  - Updates the internal data freshness state via update_data_state_from_diff().
 *
 * Based on the computed age, the function classifies the timestamp as:
 *  - SENSOR_TIMECODE_VALID
 *  - SENSOR_TIMECODE_OUT_OF_RANGE
 *  - SENSOR_TIMECODE_ERROR
 *  - LOCAL_TIME_ERROR
 *
 * @param factory_ts
 *        Factory timestamp string received from LibreView.
 *
 * @param cloud_ts
 *        Cloud timestamp string received from LibreView.
 *
 * @param print_mode
 *        If set to 1, detailed debug information is logged.
 *
 * @return
 *        SENSOR_TIMECODE_VALID         if the timestamp is valid and within range,
 *        SENSOR_TIMECODE_OUT_OF_RANGE  if the measurement is too old or in the future,
 *        SENSOR_TIMECODE_ERROR         if parsing fails,
 *        LOCAL_TIME_ERROR              if system time is not properly initialized.
 *
 * @note
 *        The function also updates:
 *          - llu_status.data_age_ms
 *          - llu_status.data_state
 *          - tz_locked and tz_offset_s_locked
 *
 * @warning
 *        Requires a valid and synchronized system time (e.g., via NTP).
 *        If system time is invalid, all further time comparisons are unreliable.
 */
uint8_t LIBRELINKUP::check_valid_timestamp_factory(
    const String& factory_ts,
    const String& cloud_ts,
    uint8_t print_mode)
{
    time_t now = time(nullptr);
    if (now < 1700000000) {
        logger.notice("Failed to obtain valid time (NTP?)");
        llu_status.data_state = DataState::INVALID_TIME;
        llu_status.data_age_ms = 0;
        return LOCAL_TIME_ERROR;
    }

    time_t tCloud  = parseTimestamp(cloud_ts.c_str());
    time_t tFactory = parseTimestamp(factory_ts.c_str());

    if (tFactory <= 0 || tCloud <= 0) {
        logger.notice("Timestamp parse failed (factory='%s' cloud='%s')",
                      factory_ts.c_str(), cloud_ts.c_str());
        llu_status.data_state = DataState::INVALID_TIME;
        llu_status.data_age_ms = 0;
        return SENSOR_TIMECODE_ERROR;
    }

    // Determine offset from current TZ/DST state
    struct tm lt{};
    localtime_r(&now, &lt);
    int32_t offset_s = (lt.tm_isdst > 0) ? 7200 : 3600;

    tz_locked = 1;
    tz_offset_s_locked = offset_s;

    int32_t diff_ms = (int32_t)(now - (tFactory + (time_t)offset_s)) * 1000;

    update_data_state_from_diff(diff_ms);

    if (print_mode == 1) {
        logger.debug("tz_locked=%d offset_s=%ld", (int)tz_locked, (long)tz_offset_s_locked);
        logger.debug("ESP32 now epoch                   : %ld", (long)now);
        logger.debug("Factory epoch                     : %ld", (long)tFactory);
        logger.debug("Cloud TS epoch                    : %ld", (long)tCloud);
        logger.debug("diff_ms                           : %ld (warn=%ld stale=%ld)",
                      (long)diff_ms, (long)LIBRELINKUPDATAWARNMS, (long)LIBRELINKUPSENSORTIMEOUT);
    }

    if (diff_ms < 0) return SENSOR_TIMECODE_OUT_OF_RANGE;
    if ((uint32_t)diff_ms > LIBRELINKUPSENSORTIMEOUT) return SENSOR_TIMECODE_OUT_OF_RANGE;
    return SENSOR_TIMECODE_VALID;
}

/**
 * @brief Updates the internal data freshness state based on measurement age.
 *
 * This function evaluates the age of the latest glucose measurement
 * (in milliseconds) and updates the internal data status accordingly.
 *
 * The following states are assigned:
 *  - DataState::OK           → Measurement is within the valid time window
 *  - DataState::STALE_WARN   → Measurement exceeds warning threshold
 *  - DataState::STALE_LOST   → Measurement exceeds timeout threshold
 *
 * If @p diff_ms is negative, the measurement is treated as invalid
 * and classified as STALE_LOST.
 *
 * Additionally, the computed age is stored in:
 *   @c llu_status.data_age_ms
 *
 * @param diff_ms
 *        Age of the measurement in milliseconds
 *        (typically computed as: now - measurement_time).
 *
 * @note
 *        The threshold values are defined by:
 *          - LIBRELINKUPDATAWARNMS
 *          - LIBRELINKUPSENSORTIMEOUT
 *
 * @warning
 *        This function only evaluates data freshness.
 *        It does not validate sensor lifecycle state.
 */
void LIBRELINKUP::update_data_state_from_diff(int32_t diff_ms)
{
    // diff_ms is the age of the measurement in milliseconds
    if (diff_ms < 0) {
        llu_status.data_age_ms = 0;
        llu_status.data_state = DataState::STALE_LOST;
        return;
    }

    llu_status.data_age_ms = (uint32_t)diff_ms;

    if (llu_status.data_age_ms > LIBRELINKUPSENSORTIMEOUT) {
        llu_status.data_state = DataState::STALE_LOST;
    } else if (llu_status.data_age_ms > LIBRELINKUPDATAWARNMS) {
        llu_status.data_state = DataState::STALE_WARN;
    } else {
        llu_status.data_state = DataState::OK;
    }
}

/**
 * @brief Counts valid glucose values in the historical graph data buffer.
 *
 * This function iterates through the internal graph data array and
 * counts all entries that are non-zero.
 *
 * Zero values are treated as invalid or unused data points.
 *
 * @return
 *        The number of valid (non-zero) glucose measurements
 *        currently stored in the graph data buffer.
 *
 * @note
 *        The size of the buffer is defined by GRAPHDATAARRAYSIZE.
 *
 * @warning
 *        A value of zero is assumed to indicate missing or invalid data.
 *        If zero can be a valid measurement in future sensor revisions,
 *        this logic must be adjusted accordingly.
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
 * @brief Maps a LibreLinkUp region code to the corresponding API base URL.
 *
 * LibreLinkUp uses region-specific API endpoints. This function
 * translates a given region identifier into the appropriate
 * base URL used for API communication.
 *
 * Currently supported mappings:
 *  - "de" or "eu" → https://api-de.libreview.io
 *  - Any other value → https://api.libreview.io (default)
 *
 * @param region
 *        Region identifier returned by the LibreLinkUp authentication API
 *        (e.g. "de", "eu", "us").
 *
 * @return
 *        The base URL string corresponding to the given region.
 *
 * @note
 *        If the region is unknown or not explicitly handled,
 *        the default global API endpoint is returned.
 */
String LIBRELINKUP::regionToBaseUrl(const String& region) {
    if (region == "de" || region == "eu") return "https://api-de.libreview.io";
    return "https://api.libreview.io";
}


/**
 * @brief Sends a "Terms of Use" acceptance request to the LibreLinkUp API.
 *
 * This function performs an authenticated HTTP POST request to the
 * LibreLinkUp "accept terms" endpoint. It is typically required when
 * the API indicates that the user must accept updated Terms of Use
 * before normal API access can continue.
 *
 * The function:
 *  - Initializes an HTTPS connection
 *  - Adds required authentication headers (Bearer token)
 *  - Sends a POST request
 *  - Parses the JSON response
 *  - Updates user-related fields (user_id, country, login status)
 *  - Cleans up HTTP and TLS resources
 *
 * @return
 *        1 if the request was initiated successfully,
 *        0 if HTTPS initialization failed.
 *
 * @note
 *        A successful return value does not necessarily mean that the
 *        Terms of Use were accepted — it only indicates that the request
 *        was processed. HTTP status codes should be checked in logs.
 *
 * @warning
 *        Requires a valid and non-expired user_token in
 *        @c llu_login_data.user_token.
 *
 * @details
 *        On HTTP 200 or 301 responses, the function parses the returned
 *        JSON and updates internal login state accordingly.
 */
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

/**
 * @brief Authenticates a LibreLinkUp user and retrieves an access token.
 *
 * This function performs an HTTPS POST request to the LibreLinkUp
 * authentication endpoint using the provided email and password.
 *
 * On success, it parses the JSON response and updates internal login data:
 *  - user_login_status
 *  - user_country
 *  - user_id
 *  - user_token
 *  - user_token_expires
 *  - account_id (SHA-256 hash of user_id)
 *
 * The LibreLinkUp API may request a region redirect. In that case, the function
 * updates @c base_url to the redirected region endpoint and retries the login once.
 *
 * @param user_email
 *        LibreLinkUp account email address.
 *
 * @param user_password
 *        LibreLinkUp account password.
 *
 * @return
 *        1 if authentication succeeded and token data was extracted,
 *        0 if the HTTPS request could not be started or authentication failed.
 *
 * @note
 *        This function resets the underlying HTTP/TLS state at the beginning
 *        of each call by stopping the client and ending the HTTPClient session.
 *
 * @warning
 *        This function handles credentials. Avoid logging sensitive values.
 *
 * @details
 *        If the response indicates "redirect=true", the function switches
 *        to a region-specific endpoint (e.g., https://api-<region>.libreview.io)
 *        and recursively retries authentication once.
 */
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


/**
 * @brief Fetches the current connection measurement data from the LibreLinkUp API.
 *
 * This function performs an authenticated HTTPS GET request to the
 * LibreLinkUp "connections" endpoint and extracts the most recent
 * glucose measurement data (current value + trend information).
 *
 * If no valid authentication token is available, the function triggers
 * a login via auth_user(). If the API indicates that Terms of Use must
 * be accepted (login status == 4), tou_user() is called.
 *
 * On successful response (HTTP 200/301), the JSON response is parsed using
 * an ArduinoJson filter to minimize memory usage. The following fields are
 * populated:
 *  - llu_glucose_data.glucoseMeasurement
 *  - llu_glucose_data.trendArrow
 *  - llu_glucose_data.measurement_color
 *  - llu_glucose_data.str_TrendMessage
 *  - llu_glucose_data.str_measurement_timestamp
 *  - llu_glucose_data.str_trendArrow (mapped arrow string)
 *
 * The function also clears intermediate JSON documents and cleans up
 * HTTP/TLS resources after the request.
 *
 * @return
 *        1 if the request succeeded and data was parsed,
 *        0 if the request failed or could not be started.
 *
 * @note
 *        HTTP 401 (unauthorized) triggers re-authorization via auth_user().
 *        The function does not automatically retry the GET after reauth.
 *
 * @warning
 *        Requires a valid network connection and proper TLS configuration.
 *        The function may block while performing HTTPS operations.
 */
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

/**
 * @brief Fetches historical glucose graph data from the LibreLinkUp API.
 *
 * This function performs an authenticated HTTPS GET request to the
 * LibreLinkUp "graph" endpoint to retrieve historical glucose measurements.
 *
 * If no valid authentication token is available, the function triggers
 * a login via auth_user(). If the API indicates that Terms of Use must
 * be accepted (login status == 4), tou_user() is called.
 *
 * On successful response (HTTP 200/301), the JSON response is parsed using
 * an ArduinoJson filter to minimize memory usage. The relevant fields are
 * extracted and stored in internal data structures.
 *
 * The function also measures and logs the time taken for the API call and
 * cleans up HTTP/TLS resources after the request.
 *
 * @return
 *        1 if the request succeeded and data was parsed,
 *        0 if the request failed or could not be started.
 *
 * @note
 *        HTTP 401 (unauthorized) triggers re-authorization via auth_user().
 *        The function does not automatically retry the GET after reauth.
 *
 * @warning
 *        Requires a valid network connection and proper TLS configuration.
 *        The function may block while performing HTTPS operations.
 */
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

/**
 * @brief Parses the internal JSON document containing LibreLinkUp graph data.
 *
 * This function extracts relevant fields from the internal JSON document
 * (populated by get_graph_data()) and updates the corresponding internal
 * data structures for glucose measurements, sensor info, and historical data.
 *
 * The function also updates the timezone offset based on the measurement timestamps
 * if it has not been locked yet.
 *
 * @return
 *        true if parsing succeeded and data was extracted,
 *        false if required fields were missing or parsing failed.
 *
 * @note
 *        This function assumes that the internal JSON document is already
 *        populated with valid data from the LibreLinkUp API. It does not
 *        perform any HTTP operations or JSON deserialization itself.
 *
 * @warning
 *        If the structure of the JSON document changes in future API versions,
 *        this parsing logic may need to be updated accordingly.
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

/**
 * @brief Parses the internal JSON document to extract LibreLinkUp graph data.
 *
 * This function reads the internal JSON document (populated by get_graph_data()
 * or ingest_graph_json()) and extracts relevant fields to populate internal
 * data structures for glucose measurements, sensor info, and historical data.
 *
 * The function also updates the timezone offset based on the measurement timestamps
 * if it has not been locked yet.
 *
 * @return
 *        true if parsing succeeded and data was extracted,
 *        false if required fields were missing or parsing failed.
 *
 * @note
 *        This function assumes that the internal JSON document is already
 *        populated with valid data from the LibreLinkUp API. It does not
 *        perform any HTTP operations or JSON deserialization itself.
 *
 * @warning
 *        If the structure of the JSON document changes in future API versions,
 *        this parsing logic may need to be updated accordingly.
 */
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
 *
 * @return const String& Reference to the last graph JSON data.
 */
const String& LIBRELINKUP::get_last_graph_json() const {
    return last_graph_json;
}

/**
 * @brief Gets the WiFiClientSecure client pointer.
 *
 * @return WiFiClientSecure& Reference to the WiFiClientSecure client.
 */
WiFiClientSecure & LIBRELINKUP::get_wifisecureclient(void){
    return *llu_client;
}

/**
 * @brief Checks the HTTPS connection to a given URL and logs the result.
 *
 * This function attempts to establish an HTTPS connection to the specified URL
 * using the internal WiFiClientSecure instance. It performs a simple GET request
 * and logs whether the connection was successful along with the HTTP response code.
 *
 * @param url The URL to check the HTTPS connection against.
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
 *
 * @param client Reference to the WiFiClientSecure instance.
 * @param ca_file Path to the CA certificate file.
 * @return true if the CA certificate was set successfully, false otherwise.
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
 * @brief Reads a CA certificate from a file and logs its content.
 *
 * This function reads the CA certificate from the specified file and logs
 * its content line by line using the internal logger. It also prints the
 * entire certificate to the serial console for debugging purposes.
 *
 * @param ca_file Path to the CA certificate file.
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
 *
 * This function performs an HTTPS GET request to the specified URL to download
 * the root CA certificate. The downloaded certificate is then saved to a file
 * in the LittleFS filesystem. The function logs the progress and any errors
 * encountered during the download process.
 *
 * @param download_url The URL from which to download the root CA certificate.
 * @param file_name The name of the file where the downloaded certificate will be saved.
 * @return 1 if the download and file save were successful, 0 otherwise.
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
 * @brief Reads the content of a file into a string buffer.
 *
 * This function opens the specified file from the given filesystem, reads its
 * content character by character, and stores it in the provided string buffer.
 * The function ensures that the buffer does not overflow by respecting the
 * specified maximum length. It also handles file opening errors and ensures
 * that the buffer is null-terminated.
 *
 * @param fs Reference to the filesystem (e.g., LittleFS) from which to read the file.
 * @param path The path to the file to be read.
 * @param myString The buffer where the file content will be stored as a string.
 * @param maxLength The maximum length of the string buffer (including null terminator).
 * @return true if the file was read successfully, false if there was an error (e.g., file not found).
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
 * @brief Parses a LibreView timestamp string into Unix epoch time.
 *
 * This function converts a timestamp string in the format:
 *
 *     "MM/DD/YYYY HH:MM:SS AM"
 *     "MM/DD/YYYY HH:MM:SS PM"
 *
 * into a time_t (Unix epoch seconds).
 *
 * The function:
 *  - Forces the "C" locale to ensure reliable AM/PM parsing
 *  - Removes the AM/PM suffix manually
 *  - Parses the remaining date/time using strptime()
 *  - Applies manual 12h → 24h conversion
 *  - Uses mktime() to convert struct tm into epoch time
 *
 * Daylight saving time (DST) is automatically handled by setting
 * tm_isdst = -1 before calling mktime().
 *
 * @param timestampStr
 *        Null-terminated timestamp string received from LibreView.
 *
 * @return
 *        Unix epoch time in seconds on success.
 *        Returns -1 if parsing fails.
 *
 * @note
 *        This function assumes US-style date formatting (MM/DD/YYYY).
 *        It does not support ISO-8601 timestamps or IPv6-style date strings.
 *
 * @warning
 *        The function modifies temporary internal buffers but does not
 *        modify the original input string.
 */
time_t LIBRELINKUP::parseTimestamp(const char* timestampStr)
{
    if (!timestampStr || !*timestampStr) {
        return (time_t)-1;
    }

    // NOTE: setting locale is global; do it once if you really need it.
    // setlocale(LC_TIME, "C");

    struct tm tm_time{};
    memset(&tm_time, 0, sizeof(tm_time));

    char timeStr[64];
    strncpy(timeStr, timestampStr, sizeof(timeStr) - 1);
    timeStr[sizeof(timeStr) - 1] = '\0';

    // Detect AM/PM
    const bool has_pm = (strstr(timeStr, "PM") != nullptr);
    const bool has_am = (strstr(timeStr, "AM") != nullptr);

    // Create clean buffer without AM/PM suffix
    char clean_timeStr[64];
    strncpy(clean_timeStr, timeStr, sizeof(clean_timeStr) - 1);
    clean_timeStr[sizeof(clean_timeStr) - 1] = '\0';

    if (char* am_pm = strstr(clean_timeStr, "AM")) *am_pm = '\0';
    if (char* am_pm = strstr(clean_timeStr, "PM")) *am_pm = '\0';

    // Trim trailing spaces (strptime is picky sometimes)
    for (int i = (int)strlen(clean_timeStr) - 1; i >= 0; --i) {
        if (clean_timeStr[i] == ' ' || clean_timeStr[i] == '\t') clean_timeStr[i] = '\0';
        else break;
    }

    // Parse "MM/DD/YYYY HH:MM:SS" (12-hour clock)
    char* ret = strptime(clean_timeStr, "%m/%d/%Y %I:%M:%S", &tm_time);
    if (!ret) {
        logger.debug("parseTimestamp: strptime failed for '%s'", timestampStr);
        return (time_t)-1;
    }

    // Manual AM/PM handling (only if suffix was present)
    if (has_pm && tm_time.tm_hour != 12) {
        tm_time.tm_hour += 12;
    } else if (has_am && tm_time.tm_hour == 12) {
        tm_time.tm_hour = 0;
    }

    tm_time.tm_isdst = -1;

    time_t ts = mktime(&tm_time);
    if (ts <= 0) {
        logger.debug("parseTimestamp: mktime failed for '%s' (ts=%ld)", timestampStr, (long)ts);
        return (time_t)-1;
    }

    return ts;
}

/**
 * @brief Derives and locks the timezone offset from a single timestamp sample.
 *
 * This function calculates the timezone offset between a local LibreLinkUp
 * timestamp (@p ts_local) and the corresponding factory timestamp (@p ts_factory).
 *
 * The difference between both timestamps represents the effective timezone
 * offset (including DST if applicable). The computed offset is then:
 *
 * - Stored in seconds (tz_offset_s_locked)
 * - Stored in rounded hours (tz_offset_h_locked)
 * - Marked as locked via @c tz_locked
 *
 * A sanity check ensures the offset remains within ±15 hours to prevent
 * invalid results caused by parsing errors or corrupted timestamps.
 *
 * @param ts_local
 *        Timestamp string representing the localized measurement time.
 *
 * @param ts_factory
 *        Timestamp string representing the factory (base) measurement time.
 *
 * @return
 *        true  if the timezone offset was successfully calculated and locked,
 *        false if parsing failed or the calculated offset was implausible.
 *
 * @note
 *        This function is intended to be called once per session. After
 *        successful execution, tz_locked remains true and subsequent
 *        updates are typically unnecessary.
 */
bool LIBRELINKUP::update_tz_offset_once(const String& ts_local, const String& ts_factory)
{
    time_t tLocal   = parseTimestamp(ts_local.c_str());
    time_t tFactory = parseTimestamp(ts_factory.c_str());

    if (tLocal <= 0 || tFactory <= 0) {
        logger.debug("tz: parse failed (local=%ld factory=%ld)", (long)tLocal, (long)tFactory);
        return false;
    }

    int32_t off_s = (int32_t)difftime(tLocal, tFactory);

    if (abs(off_s) > 15 * 3600) {
        logger.notice("tz: offset implausible: %ld s", (long)off_s);
        return false;
    }

    int16_t off_h = (int16_t)((off_s >= 0) ? ((off_s + 1800) / 3600)
                                           : ((off_s - 1800) / 3600));

    tz_offset_s_locked = off_s;
    tz_offset_h_locked = off_h;
    tz_locked = true;

    logger.debug("tz lock (1-sample): off_s=%ld off_h=%d",
                 (long)tz_offset_s_locked, (int)tz_offset_h_locked);

    return true;
}