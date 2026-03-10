/**
 * @file hba1c.cpp
 * @brief Implementation of glucose history storage on LittleFS and HbA1c related metrics.
 *
 * This module stores glucose samples as daily JSON files on LittleFS.
 * Each daily file is named "/YYYY-MM-DD.json" and contains a JSON array of objects:
 * @code
 * [
 *   { "timestamp": 1700000000, "glucose": 123 },
 *   ...
 * ]
 * @endcode
 *
 * The module provides:
 * - Storage helpers (load/save/delete, daily file switching)
 * - Debug output helpers (raw dump, formatted print, list files)
 * - Statistics (mean glucose, estimated HbA1c, TIR, standard deviation, CV)
 *
 * @note Uses a global JsonDocument allocated on heap (optionally PSRAM).
 * @note Uses LittleFS and flush() after writes for persistence.
 */

#include "hba1c.h"

#include <librelinkup.h>
extern LIBRELINKUP librelinkup;

//------------------------[uuid logger]-----------------------------------
/** @brief Module logger instance. */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

//---------------------------[globals]------------------------------------

/**
 * @brief Global JSON document used for parsing/serialization.
 *
 * Allocated on heap to allow larger capacity; can be moved to PSRAM if desired.
 * Cleared frequently to keep memory usage stable.
 */
JsonDocument* globalJsonDoc = new JsonDocument();

/**
 * @brief Current day's JSON filename ("/YYYY-MM-DD.json").
 *
 * Updated via HBA1C::updateFilename().
 */
char today_json_filename[20];

/**
 * @brief Last stored timestamp (Unix epoch seconds).
 *
 * Used to detect day boundaries and decide when to switch daily files.
 */
time_t last_timestamp = 0;

//---------------------------[functions]----------------------------------

/**
 * @brief Create multiple test JSON files (last 7 days) with random glucose data.
 *
 * For each of the last 7 days, creates a file "/YYYY-MM-DD.json" containing 10 samples
 * spaced by 5 minutes. Glucose values are random in [80..180] mg/dL.
 *
 * @warning Overwrites existing files with the same names.
 */
void HBA1C::createTestJsonFiles() {
    time_t now = time(nullptr);
    struct tm timeinfo;

    for (int i = 0; i < 7; i++) {
        time_t testTime = now - (i * 24 * 60 * 60);  // i days back
        localtime_r(&testTime, &timeinfo);

        char filename[20];
        strftime(filename, sizeof(filename), "/%Y-%m-%d.json", &timeinfo);

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (int j = 0; j < 10; j++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["timestamp"] = testTime + (j * 300);  // every 5 minutes
            obj["glucose"] = random(80, 180);         // random values between 80 and 180
        }

        File file = LittleFS.open(filename, "w");
        if (file) {
            serializeJson(doc, file);
            file.flush(); // Important for safe persistence
            file.close();
            logger.notice("Test file created: %s", filename);
        } else {
            logger.notice("Error creating file %s!", filename);
        }
    }
}

/**
 * @brief Generate the current daily JSON filename ("/YYYY-MM-DD.json").
 *
 * Uses local time.
 */
void HBA1C::updateFilename() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    strftime(today_json_filename, sizeof(today_json_filename), "/%Y-%m-%d.json", timeinfo);
    //logger.debug("UpdateFilename: %s", today_json_filename);
}

/**
 * @brief Dump raw file contents block-wise (no JSON parsing).
 * @param filename Path of the file to read.
 *
 * Outputs both to logger and Serial.
 */
void HBA1C::debugRawFileContents(const char* filename) {
    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.notice("Error: file %s not found!", filename);
        return;
    }

    logger.notice("=== Raw data from file %s ===", filename);
    Serial.printf("=== Raw data from file %s ===\n\r", filename);

    char buffer[257];  // 256 chars + null terminator
    size_t bytesRead;

    while ((bytesRead = file.readBytes(buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        logger.notice("%s", buffer);
        Serial.printf("%s", buffer);
    }

    file.close();
}

/**
 * @brief Print a JSON file as formatted entries (timestamp + glucose).
 * @param filename File name/path. If missing leading '/', it will be added.
 *
 * Expects JSON array with objects containing:
 * - "timestamp" (Unix epoch seconds)
 * - "glucose" (mg/dL)
 */
void HBA1C::printJsonFileTelnet(const char* filename) {

    String path = filename;
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    logger.notice("Debug: opening file %s", filename);
    Serial.printf("Debug: opening file %s\n\r", filename);

    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        logger.notice("Error: file %s not found!", filename);
        Serial.printf("Error: file %s not found!\n\r", filename);
        return;
    }

    globalJsonDoc->clear();

    logger.notice("Debug: reading JSON data...");
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.notice("Error reading %s: %s", filename, error.c_str());
        Serial.printf("Error reading %s: %s\n\r", filename, error.c_str());
        return;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.notice("Error: JSON file %s is not an array!", filename);
        Serial.printf("Error: JSON file %s is not an array!\n\r", filename);
        return;
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();
    logger.notice("=== Glucose values from %s (entries: %d) ===", filename, arr.size());
    Serial.printf("=== Glucose values from %s (entries: %d) ===\n\r", filename, arr.size());

    for (JsonObject obj : arr) {
        time_t timestamp = obj["timestamp"].as<time_t>();
        uint16_t glucose = obj["glucose"].as<uint16_t>();

        struct tm *timeinfo = localtime(&timestamp);
        char timeString[20];
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeinfo);

        logger.notice("Time: %s | Glucose: %d mg/dL", timeString, glucose);
        Serial.printf("Time: %s | Glucose: %d mg/dL\n\r", timeString, glucose);

        yield();
    }

    logger.notice("Debug: JSON output completed.");
    globalJsonDoc->clear();
}

/**
 * @brief List all JSON files in LittleFS (name and size).
 */
void HBA1C::listJsonFilesTelnet() {
    File root = LittleFS.open("/");
    if (!root) {
        logger.err("Error: could not open LittleFS!");
        return;
    }

    logger.notice("=== All stored JSON files ===");

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (filename.endsWith(".json")) {
            //logger.debug("File: %s | Size: %d bytes", filename.c_str(), file.size());
        }
        file = root.openNextFile();
    }
}

/**
 * @brief Load JSON from file into jsonDoc.
 * @param filename Path of JSON file.
 * @param jsonDoc JSON document to fill.
 *
 * If the file is missing or unreadable, jsonDoc becomes an empty JsonArray.
 */
void HBA1C::loadJsonFromFile(const char* filename, JsonDocument &jsonDoc) {
    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.err("File %s not found, creating new file!", filename);
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
        return;
    }

    //logger.debug("Loading file %s...", filename);

    jsonDoc.clear();
    DeserializationError error = deserializeJson(jsonDoc, file);
    file.close();

    if (error) {
        logger.err("Error reading %s: %s", filename, error.c_str());
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
    }

    //logger.debug("File %s loaded with %d entries.", filename, jsonDoc.size());
}

/**
 * @brief Save JSON document to file and flush.
 * @param filename Path of JSON file.
 * @param jsonDoc JSON document (must be non-empty JsonArray).
 */
void HBA1C::saveJsonToFile(const char* filename, JsonDocument &jsonDoc) {
    if (!jsonDoc.is<JsonArray>() || jsonDoc.size() == 0) {
        logger.err("Error: JSON document is empty or invalid! Save aborted.");
        return;
    }

    File file = LittleFS.open(filename, "w");
    if (!file) {
        logger.err("Error: could not open file %s for writing!", filename);
        return;
    }

    serializeJson(jsonDoc, file);
    file.flush();
    file.close();

    //logger.debug("File %s saved successfully with %d entries.", filename, jsonDoc.size());
    jsonDoc.clear();
}

/**
 * @brief Delete a JSON file from LittleFS.
 * @param filename Path of the JSON file.
 * @return true if deletion succeeded, otherwise false.
 */
bool HBA1C::deleteJsonFile(const char* filename) {
    if (LittleFS.exists(filename)) {
        if (LittleFS.remove(filename)) {
            logger.notice("File %s was deleted successfully!", filename);
            return true;
        } else {
            logger.notice("Error: could not delete file %s!", filename);
            return false;
        }
    } else {
        logger.notice("Warning: file %s does not exist, cannot delete.", filename);
        return false;
    }
}

/**
 * @brief Add a glucose sample to the current day's JSON file.
 * @param timestamp Unix epoch seconds.
 * @param glucose Glucose in mg/dL.
 *
 * - Switches daily file when day changes.
 * - Avoids storing duplicate consecutive glucose values.
 * - Enforces MAX_ENTRIES by removing oldest entry.
 */
void HBA1C::addGlucoseValue(time_t timestamp, uint16_t glucose) {
    static time_t last_timestamp = 0;
    updateFilename();

    struct tm *timeinfo = localtime(&timestamp);
    struct tm *last_timeinfo = localtime(&last_timestamp);

    if (last_timeinfo->tm_mday != timeinfo->tm_mday) {
        //logger.debug("New day detected, switching file to %s...", today_json_filename);
        updateFilename();
    }

    last_timestamp = timestamp;

    loadJsonFromFile(today_json_filename, *globalJsonDoc);

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.err("Error: JSON document is not an array! Creating a new array.");
        globalJsonDoc->clear();
        globalJsonDoc->to<JsonArray>();
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();

    if (arr.size() > 0) {
        JsonObject lastEntry = arr[arr.size() - 1];
        if (lastEntry["glucose"].is<uint16_t>() && lastEntry["glucose"] == glucose) {
            logger.debug("Warning: new value equals last entry (%d mg/dL). Save skipped.", glucose);
            return;
        }
    }

    if (arr.size() >= MAX_ENTRIES) {
        arr.remove(0);
    }

    JsonObject obj = arr.add<JsonObject>();
    obj["timestamp"] = timestamp;
    obj["glucose"]   = glucose;

    saveJsonToFile(today_json_filename, *globalJsonDoc);
    globalJsonDoc->clear();

    //logger.debug("New value saved: %ld | Glucose: %d mg/dL in file: %s", timestamp, glucose, today_json_filename);
}

/**
 * @brief Check if a new daily file is required at day boundary.
 */
void HBA1C::checkNewDay() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    struct tm *last_timeinfo = localtime(&last_timestamp);

    if (last_timeinfo->tm_mday != timeinfo->tm_mday) {
        Serial.println("New day detected, switching file...");
        logger.notice("New day detected, switching file...");
        updateFilename();
    }

    last_timestamp = now;
}

/**
 * @brief Calculate mean glucose from a history array.
 * @param values Array of glucose values (mg/dL).
 * @param size Number of elements.
 * @return Mean glucose (mg/dL).
 *
 * @note Current implementation ignores 0 values in the sum, but still divides by size.
 */
float HBA1C::calculateGlucoseMeanFromHistory(uint16_t values[], uint16_t size) {
    float sum = 0;
    for (int i = 0; i < size; i++) {
        if(values[i] != 0){
            sum += values[i];
        }
    }
    return sum / size;
}

/**
 * @brief Parse a JSON file and compute sum of glucose values.
 * @param filename Path of JSON file.
 * @param[out] count Increased by number of entries.
 * @return Sum of glucose values (mg/dL).
 */
uint32_t HBA1C::processJsonFile(const char* filename, uint32_t &count) {
    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.err("Error: could not open file %s!", filename);
        return 0;
    }

    //logger.debug("Loading file %s...", filename);

    globalJsonDoc->clear();
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.err("Error reading %s: %s", filename, error.c_str());
        return 0;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.err("Warning: file %s does not contain a JSON array!", filename);
        return 0;
    }

    uint32_t local_count = 0;
    uint32_t sum = 0;
    JsonArray arr = globalJsonDoc->as<JsonArray>();

    for (JsonObject obj : arr) {
        if (obj["glucose"].is<uint16_t>()) {
            uint16_t glucose = obj["glucose"];
            sum += glucose;
            local_count++;
        }
    }

    count += local_count;
    //logger.debug("Processed file %s: %d values found.", filename, local_count);
    globalJsonDoc->clear();
    return sum;
}

/**
 * @brief Calculate mean glucose from a JSON file or all JSON files.
 * @param filename Path to JSON file or "*" for all JSON files.
 * @return Mean glucose (mg/dL), or 0.0 if no data.
 */
float HBA1C::calculateGlucoseMeanFromJson(const char* filename) {
    uint32_t sum = 0;
    uint32_t count = 0;

    if (strcmp(filename, "*") == 0) {
        File root = LittleFS.open("/");
        if (!root || !root.isDirectory()) {
            logger.debug("Error: could not open root directory!");
            return 0.0;
        }

        File file = root.openNextFile();
        while (file) {
            String currentFile = file.name();
            if (currentFile.endsWith(".json")) {
                sum += processJsonFile(currentFile.c_str(), count);
            }
            file = root.openNextFile();
        }
    } else {
        if (!LittleFS.exists(filename)) {
            logger.debug("Error: file %s does not exist!", filename);
            return 0.0;
        }

        sum += processJsonFile(filename, count);
    }

    if (count == 0) {
        logger.debug("Warning: no valid glucose data found!");
        return 0.0;
    }

    float mean = (float)sum / count;
    //logger.debug("Calculated glucose mean from %s: %.2f mg/dL (from %d entries)", filename, mean, count);
    return mean;
}

/**
 * @brief Calculate mean glucose across JSON files from the last 7 days.
 * @return Mean glucose (mg/dL) or 0.0 if no data found.
 */
float HBA1C::calculateGlucoseMeanForLast7Days() {
    uint32_t sum = 0;
    uint32_t count = 0;

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        logger.notice("Error: could not open root directory!");
        return 0.0;
    }

    time_t now = time(nullptr);

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();

        if (!filename.endsWith(".json")) {
            file = root.openNextFile();
            continue;
        }

        String filepath = filename;
        if (!filepath.startsWith("/")) {
            filepath = "/" + filepath;
        }

        if (filename == "config.json") {
            file = root.openNextFile();
            continue;
        }

        int year, month, day;
        if (sscanf(filename.c_str(), "%4d-%2d-%2d.json", &year, &month, &day) != 3) {
            logger.notice("Warning: file %s has invalid date format!", filename.c_str());
            file = root.openNextFile();
            continue;
        }

        struct tm file_tm = {0};
        file_tm.tm_year = year - 1900;
        file_tm.tm_mon  = month - 1;
        file_tm.tm_mday = day;
        time_t file_time = mktime(&file_tm);

        double diff_days = difftime(now, file_time) / (60 * 60 * 24);
        if (diff_days >= 0 && diff_days <= 7) {
            //logger.debug("Including: %s (%.0f days ago)", filepath.c_str(), diff_days);
            sum += processJsonFile(filepath.c_str(), count);
        } else {
            //logger.debug("Ignored: %s (%.0f days ago)", filepath.c_str(), diff_days);
        }

        file = root.openNextFile();
    }

    if (count == 0) {
        logger.debug("Warning: no glucose data found in the last 7 days!");
        return 0.0;
    }

    float mean = (float)sum / count;
    //logger.debug("Average glucose in last 7 days: %.2f mg/dL (from %d values)", mean, count);
    return mean;
}

/**
 * @brief Calculate estimated HbA1c (%) from mean glucose (mg/dL).
 * @param mean_glucose Mean glucose in mg/dL.
 * @return Estimated HbA1c in percent.
 */
float HBA1C::calculate_hba1c(float mean_glucose) {
    return (mean_glucose + 46.7) / 28.7;
}

/**
 * @brief Calculate Time in Range percentage.
 * @param values Glucose values (mg/dL).
 * @param size Array size.
 * @param min_range Inclusive minimum (mg/dL).
 * @param max_range Inclusive maximum (mg/dL).
 * @return Percentage [0..100] in range.
 */
float HBA1C::calculate_time_in_range(uint16_t values[], uint16_t size, int min_range, int max_range) {
    int count = 0;
    for (int i = 0; i < size; i++) {
        if (values[i] >= min_range && values[i] <= max_range) {
            count++;
        }
    }
    return (count / (double)size) * 100.0;
}

/**
 * @brief Calculate standard deviation of glucose values.
 * @param values Glucose values (mg/dL).
 * @param size Array size.
 * @param mean Mean glucose (mg/dL).
 * @return Standard deviation (mg/dL).
 */
float HBA1C::calculate_standard_deviation(uint16_t values[], uint16_t size, float mean) {
    double sum = 0;
    for (int i = 0; i < size; i++) {
        sum += pow(values[i] - mean, 2);
    }
    return sqrt(sum / size);
}

/**
 * @brief Calculate coefficient of variation (CV%).
 * @param std_dev Standard deviation (mg/dL).
 * @param mean Mean glucose (mg/dL).
 * @return CV in percent.
 */
float HBA1C::calculate_coefficient_of_variation(float std_dev, float mean) {
    return (std_dev / mean) * 100.0;
}
