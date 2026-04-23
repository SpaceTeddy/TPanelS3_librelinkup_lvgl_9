/**
 * @file helper.cpp
 * @brief General-purpose utility functions for the TPanelS3 firmware.
 *
 * Covers:
 * - IP address and hostname resolution
 * - Date/time formatting and conversion (Unix epoch ↔ string, local time)
 * - NTP synchronisation helper
 * - Internet connectivity check (TCP probe)
 * - Flash memory ID generation
 * - LittleFS file printing
 * - ArduinoJson document buffer inspection
 */
#include "helper.h"
#include <IPAddress.h>
#include <WiFi.h>

#include <librelinkup.h>
extern LIBRELINKUP librelinkup;

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

/**
 * @brief Parse a dotted-decimal IPv4 string into an IPAddress.
 * @param ipStr IPv4 string, e.g. "192.168.1.1".
 * @return Parsed IPAddress; returns 0.0.0.0 on malformed input.
 */
IPAddress HELPER::parseIPAddress(const String &ipStr) {
    uint8_t octets[4] = {0}; // Array for 4 Oktetten IP
    int idx = 0;

    // Split String at dots
    int start = 0;
    for (int i = 0; i < ipStr.length() && idx < 4; i++) {
        if (ipStr[i] == '.' || i == ipStr.length() - 1) {
            // last Oktett
            if (i == ipStr.length() - 1) i++;

            // convert to Integer
            octets[idx++] = ipStr.substring(start, i).toInt();
            start = i + 1;
        }
    }
    // create IP-Address
    return IPAddress(octets[0], octets[1], octets[2], octets[3]);
}

/**
 * @brief Print the raw contents of a LittleFS file to Serial.
 * @param filename Path to the file (e.g. "/settings.json").
 */
void HELPER::printFile(const char *filename) {
  // Open file for reading

  File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open data file");
    return;
  }
  // Extract each characters by one by one
  while (file.available()) {
    Serial.print((char)file.read());
  }
  Serial.println();

  // Close the file
  file.close();
}

/**
 * @brief Print the current local time to Serial.
 * @param mode 0 = full date+time string ("Monday, January 06 2025 14:30:00"),
 *             1 = extracts HH/MM/SS into local buffers (no Serial output).
 */
void HELPER::printLocalTime(bool mode){
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
        return;
    }
    if(mode == 0){
        Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
        return;
    }else if(mode == 1){
        
        char timeHour[3];
        strftime(timeHour,3, "%H", &timeinfo);

        char timeMinute[3];
        strftime(timeMinute,3, "%M", &timeinfo);

        char timeSecond[3];
        strftime(timeSecond,3, "%S", &timeinfo);

        //DBGprint;Serial.printf("[ESP32 Time: %s:%s:%s]\n\r",timeHour,timeMinute,timeSecond);
    }
}

/**
 * @brief Convert hours, minutes, and seconds to milliseconds.
 * @param hours   Hours component.
 * @param minutes Minutes component.
 * @param seconds Seconds component.
 * @return Total duration in milliseconds as uint32_t.
 */
uint32_t HELPER::convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    return (hours * 3600UL + minutes * 60UL + seconds) * 1000UL;
}

/**
 * @brief Calculate the signed drift between local and server clocks.
 * @param server_epoch Unix timestamp from the remote server.
 * @param local_epoch  Unix timestamp from the local RTC/NTP clock.
 * @return Drift in seconds (local − server). Returns 0 if either argument is 0.
 */
int32_t HELPER::syncWithServerEpoch(time_t server_epoch, time_t local_epoch)
{
    if (server_epoch == 0 || local_epoch == 0) return 0;
    return (int32_t)difftime(local_epoch, server_epoch);
}

/**
 * @brief Convert a LibreLink-style date/time string to a Unix timestamp.
 *
 * Expected format: "MM/DD/YYYY H:MM:SS AM|PM" (e.g. "12/15/2024 4:52:16 PM").
 * Uses mktime() with the local timezone set on the device.
 *
 * @param datetime Input date/time string.
 * @return Unix timestamp (seconds since epoch), or -1 on parse error.
 */
long HELPER::convertStrToUnixTime(const String& datetime) {
    struct tm timeinfo = {0};

    // Parse the date and time from the input string
    int month, day, year, hour, minute, second;
    char meridian[3];
    if (sscanf(datetime.c_str(), "%d/%d/%d %d:%d:%d %2s", &month, &day, &year, &hour, &minute, &second, meridian) != 7) {
        Serial.println("Error parsing date/time");
        return -1;
    }

    // Adjust for PM
    if (strcmp(meridian, "PM") == 0 && hour != 12) {
        hour += 12;
    } else if (strcmp(meridian, "AM") == 0 && hour == 12) {
        hour = 0;
    }

    // Fill the tm structure
    timeinfo.tm_year = year - 1900; // Years since 1900
    timeinfo.tm_mon  = month - 1;    // Months since January (0-11)
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min  = minute;
    timeinfo.tm_sec  = second;

    // Convert to Unix time
    return mktime(&timeinfo);
}

/**
 * @brief Format a Unix timestamp as a "HH:MM" string in local time.
 *
 * Uses 24-hour format; locale is forced to "C" to prevent AM/PM output.
 *
 * @param buffer      Output buffer.
 * @param buffer_size Size of @p buffer in bytes (minimum 6).
 * @param timestamp   Unix epoch seconds to format.
 */
void HELPER::format_time(char *buffer, size_t buffer_size, time_t timestamp) {
    setlocale(LC_TIME, "C"); // Prevent AM/PM and enforce 24h format
    struct tm *tm_info = localtime(&timestamp); // Use local time
    strftime(buffer, buffer_size, "%H:%M", tm_info);
}

/**
 * @brief Probe internet reachability by opening a TCP connection.
 * @param ip   IP address to connect to (e.g. a known DNS resolver).
 * @param port TCP port (e.g. 53 for DNS, 80 for HTTP).
 * @return true if the TCP handshake succeeds within 1000 ms, false otherwise.
 */
bool HELPER::check_internet_status(IPAddress ip, uint16_t port)
{

   WiFiClient testClient;
   
   bool result;

    if (!testClient.connect(IPAddress(ip), port, 1000)) {
        //logger.debug("TCP connect to broker failed!");
        testClient.stop();
        result = false;
        
    } else {
        //logger.debug("TCP connect to broker OK!");
        testClient.stop();
        result = true;
    }
    return result;
    
}

/**
 * @brief Resolve a hostname to an IPv4 address, with retries.
 *
 * If @p hostname is already a dotted-decimal address, it is parsed directly
 * without a DNS lookup. Otherwise WiFi.hostByName() is tried up to
 * @p max_attempts times with @p retry_delay_ms between attempts.
 *
 * @param hostname       Hostname or dotted-decimal IPv4 string.
 * @param resolved_ip    Output: resolved IP address on success.
 * @param max_attempts   Maximum DNS attempts (clamped to at least 1).
 * @param retry_delay_ms Delay in ms between failed attempts.
 * @return true if resolution succeeded, false otherwise.
 */
bool HELPER::resolveHostnameIPv4(const String &hostname, IPAddress &resolved_ip, uint8_t max_attempts, uint16_t retry_delay_ms)
{
    IPAddress parsed_ip;
    if (parsed_ip.fromString(hostname))
    {
        resolved_ip = parsed_ip;
        return true;
    }

    if (max_attempts == 0)
        max_attempts = 1;

    for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        if (WiFi.hostByName(hostname.c_str(), resolved_ip) == 1)
            return true;

        if (attempt < max_attempts)
            delay(retry_delay_ms);
    }
    return false;
}

/**
 * @brief Check whether the system clock appears to be set to a plausible value.
 * @param min_valid_epoch Minimum acceptable Unix timestamp (default: year 2024).
 * @return true if time(nullptr) >= @p min_valid_epoch.
 */
bool HELPER::timeLooksValid(time_t min_valid_epoch)
{
    return time(nullptr) >= min_valid_epoch;
}

/**
 * @brief Ensure the system clock is NTP-synchronised, triggering a sync if needed.
 *
 * Returns immediately if timeLooksValid() is already true. Otherwise calls
 * configTime() against pool.ntp.org, ntp.nict.jp, and time.google.com, then
 * polls up to @p max_attempts times.
 *
 * @param max_attempts   Maximum polling attempts after configTime() (min 1).
 * @param retry_delay_ms Delay in ms between polls.
 * @return true if the clock is valid after the function returns.
 */
bool HELPER::ensureTimeSynced(uint8_t max_attempts, uint16_t retry_delay_ms)
{
    if (timeLooksValid())
        return true;

    configTime(0, 0, "pool.ntp.org", "ntp.nict.jp", "time.google.com");

    if (max_attempts == 0)
        max_attempts = 1;

    for (uint8_t i = 0; i < max_attempts; ++i)
    {
        if (timeLooksValid())
            return true;
        delay(retry_delay_ms);
    }
    return timeLooksValid();
}

/**
 * @brief Return the current local date and time as a formatted string.
 * @return String in the format "DD.MM.YYYY HH:MM:SS", or "null" if the
 *         system clock is not yet available.
 */
String HELPER::get_esp_time_date(){
    struct tm timeinfo;
    
    char timeinfo_day[3];
    char timeinfo_month[3];
    char timeinfo_year[5];
    char timeinfo_hour[3];
    char timeinfo_minute[3];
    char timeinfo_second[3];

    uint8_t day_localtime = 0;
    uint8_t month_localtime = 0;
    uint16_t year_localtime = 0;

    uint8_t hour_localtime = 0;
    uint8_t minute_localtime = 0;
    uint8_t second_localtime = 0;

    // get local time as int ------------------
    if(!getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
        return "null";
    }

    strftime(timeinfo_day,3, "%d", &timeinfo);
    day_localtime = atoi(timeinfo_day);

    strftime(timeinfo_month,3, "%m", &timeinfo);
    month_localtime = atoi(timeinfo_month);

    strftime(timeinfo_year,5, "%Y", &timeinfo);
    year_localtime = atoi(timeinfo_year);

    strftime(timeinfo_hour,3, "%H", &timeinfo);
    hour_localtime = atoi(timeinfo_hour);
    
    strftime(timeinfo_minute,3, "%M", &timeinfo);
    minute_localtime = atoi(timeinfo_minute);

    strftime(timeinfo_second,3, "%S", &timeinfo);
    second_localtime = atoi(timeinfo_second);
    //----------------------------------------------
    
    char buf_label1[20];
    
    snprintf(buf_label1, 20, "%02d.%02d.%04d %02d:%02d:%02d",day_localtime,month_localtime,year_localtime,hour_localtime,minute_localtime,second_localtime);
    //lv_label_set_text(ui_Label_DebugTime, buf_label1);
    String esp32_time_date = buf_label1;
    /*
    DBGprint_LLU;
    Serial.printf("ESP32 Timestamp: %02d.%02d.%04d ",day_localtime,month_localtime,year_localtime);        Serial.printf("%02d:%02d:%02d\r\n",hour_localtime,minute_localtime,second_localtime);
    logger.notice("ESP32 Timestamp: %02d.%02d.%04d %02d:%02d:%02d",day_localtime,month_localtime,year_localtime,hour_localtime,minute_localtime,second_localtime);
    */

   return esp32_time_date;
}

/**
 * @brief Derive a short, unique device identifier from the ESP32 eFuse MAC.
 *
 * Folds the 48-bit MAC into a 32-bit value via XOR and formats it as an
 * 8-character uppercase hex string (e.g. "A3F2C1B0").
 *
 * @return 8-character hex string.
 */
String HELPER::get_flashmemory_id() {
    uint64_t chipid = ESP.getEfuseMac();   // 48-Bit MAC

    // Fold 48-bit to 32-bit
    uint32_t shortid = (uint32_t)(chipid ^ (chipid >> 32));

    char buf[9];  
    snprintf(buf, sizeof(buf), "%08X", shortid);  // 8 chars, HEX
    return String(buf);
}

/**
 * @brief Inspect the memory usage of an ArduinoJson document.
 *
 * ArduinoJson v7 no longer exposes memoryUsage()/capacity() directly.
 * This wrapper returns doc->size() as usedCapacity and uses overflowed()
 * as a binary overflow indicator (totalCapacity == 1 means overflow occurred).
 *
 * @param doc Pointer to the JsonDocument to inspect; nullptr is handled safely.
 * @return Json_Buffer_Info with usedCapacity and totalCapacity fields.
 */
Json_Buffer_Info HELPER::getBufferSize(JsonDocument* doc) {
    Json_Buffer_Info info = {0, 0};  // Default values
    if (doc == nullptr) {
        return info;                // Return for null pointer
    }
    // ArduinoJson 7 removed memoryUsage()/capacity() on JsonDocument.
    // Keep this API for callers and expose lightweight runtime indicators.
    info.usedCapacity = doc->size();
    info.totalCapacity = doc->overflowed() ? 1 : 0;
    return info;
}
