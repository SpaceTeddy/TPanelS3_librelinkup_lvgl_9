/**
 * @file librelinkup.h
 * @brief LibreLinkUp API Client Library for ESP32
 * @version 1.0
 * 
 * Comprehensive interface for Abbott LibreLinkUp CGM system integration
 * Includes SSL/TLS management, sensor data processing, and API communication
 */

#ifndef librelinkup_H
#define librelinkup_H

/**
 * @defgroup includes Included Libraries
 * @brief External dependencies and components
 * @{
 */
#include <Arduino.h>                ///< Arduino core functions
#include <WiFiClientSecure.h>       ///< Secure WiFi client
#include <HTTPClient.h>             ///< HTTP client functionality
#include <ArduinoJson.h>            ///< JSON parsing and generation
#include <StreamUtils.h>            ///< Stream utility extensions
#include <FS.h>                     ///< Filesystem operations

#include <memory>                   ///< Smart pointers
#include <string>                   ///< String operations
#include <vector>                   ///< Vector container
#include <uuid/common.h>            ///< UUID common utilities
#include <uuid/console.h>           ///< Console interaction
#include <uuid/telnet.h>            ///< Telnet server
#include <uuid/log.h>               ///< Logging system
/** @} */

/**
 * @defgroup constants Library Constants
 * @brief Global configuration and debug settings
 * @{
 */
#define VERSION_LIBRELINKUP_LIB 1.0 ///< Library version identifier
#define DBGprint_LLU Serial.printf("[%08dms][%s][%s] ",millis(),__FILE__,__func__); ///< Debug output formatter
#define LIBRELINKUP_DEBUG 0         ///< Global debug flag (0=disabled)
/** @} */

/**
 * @defgroup enums Status Enumerations
 * @brief System state and error codes
 * @{
 */

/**
 * @enum TimeCodeState
 * @brief Timestamp validation states
 */
enum TimeCodeState : uint8_t {
    SENSOR_TIMECODE_ERROR        = 0, ///< Invalid timestamp format
    SENSOR_TIMECODE_VALID        = 1, ///< Valid timestamp within range
    SENSOR_TIMECODE_OUT_OF_RANGE = 2, ///< Timestamp exceeds valid range
    SENSOR_NOT_ACTIVE            = 3, ///< Sensor not initialized
    LOCAL_TIME_ERROR             = 4, ///< System clock mismatch
};

/**
 * @enum SensorState
 * @brief Sensor lifecycle states
 */
enum SensorState : uint8_t {
    SENSOR_NOT_AVAILABLE = 0, ///< No sensor detected
    SENSOR_NOT_STARTED = 1,   ///< Sensor paired but inactive
    SENSOR_STARTING = 2,      ///< Warm-up phase (60 mins)
    SENSOR_READY = 3,         ///< Active and reporting
    SENSOR_EXPIRED = 4,       ///< End of 14-day lifecycle
    SENSOR_SHUT_DOWN = 5,     ///< Manual deactivation
    SENSOR_FAILURE = 6,       ///< Hardware malfunction
};
/** @} */

/**
 * @defgroup constants Library Constants
 * @brief Global configuration of Glucose data
 * @{
 */
#define DATA_POINTS 141 +1          // 12 Stunden * 12 Punkte pro Stunde (5 Minuten Intervalle) +1 for last point
#define INTERVAL_SECONDS 300        // 5 Minuten in Sekunden
#define REQUIRED_DATA_POINTS 141    // Anzahl der vollständigen Daten vom Server
/** @} */



/**
 * @struct Sensor History Data
 * @brief Struct for Sensor History Data
 */
typedef struct {
    time_t Timestamp;
    uint16_t ValueInMgPerDl;
} GlucoseData;
/** @} */


/**
 * @defgroup security Security Configuration
 * @brief SSL/TLS certificate management
 * @{
 */
/// Google Trust Services R4 Root CA for api.libreview.io
static const char API_ROOT_CA[] PROGMEM = R"CERT(...)CERT";
/** @} */

/**
 * @class LIBRELINKUP
 * @brief Main controller class for LibreLinkUp integration
 * 
 * Manages full lifecycle of sensor communication including:
 * - Secure authentication
 * - Data retrieval and parsing
 * - Certificate management
 * - Sensor status monitoring
 */
class LIBRELINKUP {
private:
    /**
     * @brief Convert time components to milliseconds
     * @param hours Hour component (0-23)
     * @param minutes Minute component (0-59)
     * @param seconds Second component (0-59)
     * @return Calculated milliseconds
     */
    uint32_t convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds);

public:

    /**
     * @defgroup timing API Timing
     * @brief Request timing measurements
     * @{
     */
    uint32_t https_llu_api_fetch_time = 0; ///< Time taken for last API fetch in milliseconds
    /** @} */

    /**
     * @defgroup config Timing Constants
     * @brief Time-related configuration parameters
     * @{
     */
    static const int LIBRELINKUPSENSORTIMEOUT = 120000;  ///< 2 minute sensor timeout (ms)
    static const int GRAPHDATAARRAYSIZE_PLUS_ONE = 1;    ///< Graph array size buffer
    static const int GRAPHDATAARRAYSIZE = 141;           ///< Primary data points count
    static const int UNIXTIME1HOUR = 3600;                ///< Seconds per hour
    static const int UNIXTIME14DAYS = 1209600;            ///< Seconds in 14 days
    static const int TIMEFULLGRAPHDATA = 1160100;         ///< Full dataset duration (13d 12h 15m)
    /** @} */

    /**
     * @defgroup endpoints API Endpoints
     * @brief Server URLs and certificate paths
     * @{
     */
    const char* url_dl_DigiCertGlobalRootG2 = "https://cacerts.digicert.com/DigiCertGlobalRootG2.crt.pem"; ///< DigiCert root CA
    const char* url_dl_BaltimoreCyberTrustRoot = "https://cacerts.digicert.com/BaltimoreCyberTrustRoot.crt.pem"; ///< Baltimore CA
    const char* url_dl_GoogleTrustRootR4 = "https://i.pki.goog/r4.pem"; ///< Google Trust Services CA

    const char* url_check_DigiCertGlobalRootG2 = "https://global-root-ca.chain-demos.digicert.com"; ///< DigiCert test endpoint
    const char* url_check_BaltimoreCyberTrustRoot = "https://baltimore-cybertrust-root.chain-demos.digicert.com"; ///< Baltimore test
    const char* url_check_GoogleTrustRootR4 = "https://good.gtsr4.demosite.pki.goog"; ///< Google Trust test

    const char* path_root_ca_baltimore = "/rootCA_BCTR.pem";  ///< Baltimore cert storage path
    const char* path_root_ca_dcgrg2 = "/rootCA_DCGRG2.pem";   ///< DigiCert storage path
    const char* path_root_ca_googler4 = "/rootCA_GoogleR4.pem"; ///< Google Trust storage path

    const char *base_url = "https://api.libreview.io"; ///< API base URL
    const int httpsPort = 443;  
                           ///< HTTPS default port
    /** @} */

    /**
    * @struct llu_login_data
    * @brief Struct for Credentials and session management
    */
    struct {
        String email = "";                      ///< Account email
        String password = "";                   ///< Account password
        String user_id = "";                    ///< User identifier
        String account_id = "";                 ///< Account identifier
        String user_country = "";               ///< User region
        String user_token = "";                 ///< Session token
        String connection_country = "";         ///< Connection region
        int16_t connection_status = 0;          ///< API connection state
        uint32_t user_token_expires = 0;        ///< Token expiration timestamp
        uint8_t user_login_status = 0;          ///< Authentication state
    } llu_login_data;
    /** @} */

    
    /**
    * @struct llu_sensor_history_values
    * @brief Struct for glucose sensor history values
    */
    struct {
        uint16_t graph_data[GRAPHDATAARRAYSIZE+GRAPHDATAARRAYSIZE_PLUS_ONE] = {0}; ///< Glucose history buffer
        uint32_t timestamp[GRAPHDATAARRAYSIZE+GRAPHDATAARRAYSIZE_PLUS_ONE] = {0};  ///< Glucose history buffer    
    } llu_sensor_history_data;
    /** @} */

    /**
     * @defgroup sensor Sensor Data
     * @brief Glucose measurements and status
     * @{
     */
    String url_graph = "/llu/connections/" + llu_login_data.user_id + "/graph"; ///< Graph data endpoint
    String url_connection = "/llu/connections"; ///< Connections endpoint
    String url_user_auth = "/llu/auth/login";    ///< Authentication endpoint
    String url_user_tou = "/auth/continue/tou";  ///< Terms of use endpoint

    //uint16_t graph_data[GRAPHDATAARRAYSIZE+GRAPHDATAARRAYSIZE_PLUS_ONE] = {0}; ///< Glucose history buffer
    //uint32_t timestamp[GRAPHDATAARRAYSIZE+GRAPHDATAARRAYSIZE_PLUS_ONE] = {0};  ///< Glucose history buffer
    
    struct {
        uint16_t glucoseMeasurement = 0;        ///< Current measurement (mg/dL)
        uint8_t trendArrow = 0;                 ///< Trend direction code
        uint8_t measurement_color = 0;          ///< Display color code
        String str_TrendMessage = "";           ///< Trend interpretation
        String str_measurement_timestamp = "";  ///< Formatted timestamp
        String str_trendArrow = "";             ///< Trend direction text

        uint16_t glucosetargetLow = 0;          ///< Lower target range
        uint16_t glucosetargetHigh = 0;         ///< Upper target range
        uint16_t glucoseAlarmLow = 0;           ///< Low glucose threshold
        uint16_t glucoseAlarmHigh = 0;          ///< High glucose threshold
        uint16_t glucosefixedLowAlarmValues = 0;///< Fixed low alarm setting
              
    } llu_glucose_data;

    struct {
        uint8_t sensor_state = 0;               ///< Current sensor state
        String sensor_sn_non_active = "";       ///< Inactive sensor serial
        String sensor_id_non_active = "";       ///< Inactive sensor ID
        uint32_t sensor_non_activ_unixtime = 0; ///< Last activation attempt
        
        String sensor_id = "";                  ///< Active sensor ID
        String sensor_sn = "";                  ///< Active sensor serialsensor_state                = (*json_librelinkup)["data"]["activeSensors"][0]["sensor"]["pt"].as<int>();
        uint32_t sensor_activation_time = 0;    ///< Activation timestamp                
    } llu_sensor_data;
        
    
    
    int8_t timezone = 1;                    ///< UTC offset (UTC+1 default)
    bool sensor_reconnect = 0;              ///< Reconnection flag
    bool sensor_expired = 0;                ///< Expiration status flag
    /** @} */

    /**
     * @defgroup structures Data Structures
     * @brief Complex data types
     * @{
     */
    
    /**
     * @struct LLU_Status
     * @brief Combined sensor status
     */
    struct LLU_Status {
        uint8_t timestamp_status;           ///< Timestamp validation result
        uint8_t sensor_state;               ///< Sensor lifecycle state
        uint32_t last_timestamp_unixtime;   ///< Last valid Unix timestamp
    } llu_status;

    /**
     * @struct LocalTime
     * @brief Local time components
     */
    struct LocalTime {
        uint16_t day = 0;       ///< Day of month (1-31)
        uint16_t month = 0;     ///< Month (1-12)
        uint16_t year = 0;      ///< 4-digit year
        uint16_t hour = 0;      ///< Hour (0-23)
        uint16_t minute = 0;    ///< Minute (0-59)
        uint16_t second = 0;    ///< Second (0-59)
    } localtime;

    /**
     * @struct LibrelinkupTimeCode
     * @brief Raw timestamp from API
     */
    struct LibrelinkupTimeCode {
        int day = 0;        ///< API day value
        int month = 0;      ///< API month value
        int year = 0;       ///< API year value
        int hour = 0;       ///< API hour value
        int minute = 0;     ///< API minute value
        int second = 0;     ///< API second value
    } librelinkuptimecode;

    /**
     * @struct sensor_livetime
     * @brief Sensor remaining lifetime
     */
    struct {
        uint32_t sensor_valid_days = 0;         ///< Valid days remaining
        uint32_t sensor_valid_hours = 0;        ///< Valid hours remaining
        uint32_t sensor_valid_minutes = 0;      ///< Valid minutes remaining
        uint32_t sensor_valid_seconds = 0;      ///< Valid seconds remaining
    } sensor_livetime;
    /** @} */

    /**
     * @defgroup api_functions API Functions
     * @brief Core library functionality
     * @{
     */
    
    /**
     * @brief Initialize LibreLinkUp connection
     * @param use_cert Certificate handling mode:
     * - 0: No certificate verification
     * - 1: Use built-in certificates
     * - 2: Use file-based certificates
     * @return Initialization status:
     * - 0: Failure
     * - 1: Success
     */
    uint8_t begin(uint8_t use_cert);

    /**
     * @brief Get current epoch time
     * @return Current Unix timestamp in seconds
     */
    time_t get_epoch_time();

    /**
     * @brief Generate SHA256 hash of account ID
     * @param user_id User identifier
     * @return Hashed account ID string
     */
    String account_id_sha256(String user_id);

    /**
     * @brief Validate graph data integrity
     * @return Validation status:
     * - 0: Invalid data
     * - 1: Valid data
     */
    uint8_t check_graphdata(void);

    /**
     * @brief Authenticate user credentials
     * @param user_email Account email
     * @param user_password Account password
     * @return HTTP status code:
     * - 200: Success
     * - 401: Unauthorized
     * - 500: Server error
     */
    uint16_t auth_user(String user_email, String user_password);

    /**
     * @brief Accept Terms of Use
     * @return HTTP status code
     */
    uint16_t tou_user(void);

    /**
     * @brief Retrieve connection metadata
     * @return HTTP status code
     */
    uint16_t get_connection_data(void);

    /**
     * @brief Fetch glucose graph data
     * @return HTTP status code
     */
    uint16_t get_graph_data(void);

    /**
     * @brief Download root certificate
     * @param download_url Certificate source URL
     * @param file_name Local storage path
     * @return HTTP status code
     */
    uint16_t download_root_ca_to_file(const char* download_url, const char* file_name);

    /**
     * @brief Configure WiFiClientSecure with certificate
     * @param client WiFiClientSecure reference
     * @param ca_file Certificate file path
     * @return Configuration success
     */
    bool setCAfromfile(WiFiClientSecure &client, const char* ca_file);

    /**
     * @brief Display certificate contents
     * @param ca_file Certificate file path
     */
    void showCAfromfile(const char* ca_file);

    /**
     * @brief Read file to string buffer
     * @param fs Filesystem reference
     * @param path File path
     * @param myString Output buffer
     * @param maxLength Buffer size
     * @return Read success status
     */
    bool read2String(fs::FS &fs, const char * path, char *myString, size_t maxLength);

    /**
     * @brief Test HTTPS connection
     * @param url Endpoint to test
     */
    void check_https_connection(const char* url);

    /**
     * @brief Validate API timestamp
     * @param librelinkup_timestamp API timestamp string
     * @param print_mode Debug output level:
     * - 0: Silent
     * - 1: Basic info
     * - 2: Verbose
     * @return TimeCodeState validation result
     */
    uint8_t check_valid_timestamp(String librelinkup_timestamp, uint8_t print_mode);

    /**
     * @brief Map sensor state code to enum
     * @param state Raw state code
     * @return SensorState enum value
     */
    uint8_t get_sensor_state(uint8_t state);

    /**
     * @brief Calculate sensor remaining lifetime
     * @param unix_activation_time Activation timestamp
     * @return Days remaining (negative if expired)
     */
    int check_sensor_lifetime(uint32_t unix_activation_time);

    /**
     * @brief Calculate sensor remaining warmuptime
     * @param unix_activation_time Activation timestamp
     * @return minutes remaining
     */
    int get_remaining_warmup_time(time_t unix_activation_time);


    /**
     * @brief Validate client state
     * @return Client validity status
     */
    bool check_client();

    /**
     * @brief Get secure client reference
     * @return WiFiClientSecure instance
     */
    WiFiClientSecure & get_wifisecureclient(void);
    /** @} */

    /**
     * @brief converts TimeStamp String in UnixTime format
     * @return TimeStamp in UnixTime format
     */
    time_t parseTimestamp(const char* timestampStr);
    /** @} */
};

#endif