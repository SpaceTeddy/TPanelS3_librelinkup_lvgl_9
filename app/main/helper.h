/**
 * @file helper.h
 * @brief Provides utility functions for ESP32, including time synchronization, IP parsing, JSON handling, and internet status checking.
 */

#ifndef HELPER_H
#define HELPER_H

#include <Arduino.h>
#include <IPAddress.h>
#include <LittleFS.h>
#include <ESP32Ping.h>
#include <ArduinoJson.h>

#include <memory>
#include <string>
#include <vector>

#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

/**
 * @struct Json_Buffer_Info
 * @brief Structure containing information about the JSON buffer usage.
 */
struct Json_Buffer_Info {
    size_t usedCapacity;    ///< Amount of used memory in the buffer.
    size_t totalCapacity;   ///< Total allocated capacity of the buffer.
};

/**
 * @class HELPER
 * @brief Utility class providing various helper functions for the ESP32 environment.
 */
class HELPER {
    public:
        int32_t timedifference = 0;  ///< Time difference between local and server time in milliseconds.
        const int TIME_DIFF_THRESHOLD = -60000; ///< Threshold for time synchronization.

        /**
         * @brief Creates a JSON document in PSRAM.
         * @param size The size of the JSON document in bytes.
         * @return Pointer to the created DynamicJsonDocument.
         */
        DynamicJsonDocument* createJsonInPSRAM(size_t size);
        
        /**
         * @brief Parses a string representation of an IP address into an IPAddress object.
         * @param ipStr The string containing the IP address.
         * @return Parsed IPAddress object.
         */
        static IPAddress parseIPAddress(const String &ipStr);

        /**
         * @brief Prints the contents of a file stored in LittleFS.
         * @param filename The name of the file to be printed.
         */
        void printFile(const char *filename);

        /**
         * @brief Prints the local time to the console.
         * @param mode Boolean flag to specify time format.
         */
        void printLocalTime(bool mode);

        /**
         * @brief Converts hours, minutes, and seconds into milliseconds.
         * @param hours Number of hours.
         * @param minutes Number of minutes.
         * @param seconds Number of seconds.
         * @return Equivalent time in milliseconds.
         */
        uint32_t convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds);

        /**
         * @brief Synchronizes local time with the server time.
         * @param serverHours Server-provided hours.
         * @param serverMinutes Server-provided minutes.
         * @param serverSeconds Server-provided seconds.
         * @param localHours Local device hours.
         * @param localMinutes Local device minutes.
         * @param localSeconds Local device seconds.
         * @return The calculated time difference in milliseconds.
         */
        int32_t synchronizeWithServer(uint8_t serverHours, uint8_t serverMinutes, uint8_t serverSeconds, 
                                      uint8_t localHours, uint8_t localMinutes, uint8_t localSeconds);

        /**
         * @brief convertion of Unix-Timestamp to "HH:MM"
         * @return 
         */
        void format_time(char *buffer, size_t buffer_size, time_t timestamp);
        
        /**
         * @brief Checks if the ESP32 has an active internet connection.
         * @return True if connected to the internet, otherwise false.
         */
        bool check_internet_status();

        /**
         * @brief Retrieves the current ESP32 time and date as a string.
         * @return String representation of the current time and date.
         */
        String get_esp_time_date();

        /**
         * @brief Retrieves the unique flash memory ID of the ESP32.
         * @return String containing the flash memory ID.
         */
        String get_flashmemory_id();

        /**
         * @brief Converts a string representation of date and time to Unix timestamp.
         * @param datetime The date and time string.
         * @return Corresponding Unix timestamp.
         */
        long convertStrToUnixTime(const String& datetime);

        /**
         * @brief Retrieves information about the JSON buffer usage.
         * @param doc Pointer to the JSON document.
         * @return Json_Buffer_Info structure containing buffer details.
         */
        Json_Buffer_Info getBufferSize(JsonDocument* doc);
};

#endif // HELPER_H
