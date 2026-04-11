/**
 * @file hba1c.h
 * @brief Glucose history storage on LittleFS and HbA1c related calculations.
 *
 * The class stores glucose readings (mg/dL) as daily JSON files on LittleFS.
 * Each file is named "/YYYY-MM-DD.json" and contains a JSON array of objects
 * with keys "timestamp" (Unix epoch seconds) and "glucose" (mg/dL).
 */

#ifndef HBA1C_H
#define HBA1C_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <uuid/log.h>

#define MAX_ENTRIES 300         ///< Maximum entries per day file (e.g., 288 for 24h @ 5min + buffer)
#define JSON_BUFFER_SIZE 12000  ///< Capacity for JSON document used during load/modify/store

/**
 * @brief Global JSON document used for parsing/serialization.
 *
 * Allocated in the corresponding .cpp file. This is shared by multiple functions and
 * cleared frequently to keep memory usage stable.
 */
extern JsonDocument globalJsonDoc;

/**
 * @brief Current daily JSON filename in the form "/YYYY-MM-DD.json".
 *
 * Updated by HBA1C::updateFilename().
 */
extern char today_json_filename[20];

/**
 * @class HBA1C
 * @brief Manages glucose storage, retrieval, and calculation of HbA1c-related metrics.
 *
 * Responsibilities:
 * - Maintain one JSON file per day with glucose samples.
 * - Avoid duplicate consecutive glucose values.
 * - Keep per-file size bounded (MAX_ENTRIES) by removing oldest entries.
 * - Provide helpers to print/list files for debugging.
 * - Provide calculations: mean glucose, estimated HbA1c, TIR, standard deviation, CV.
 *
 * @note This class performs synchronous file I/O on LittleFS.
 * @note Not thread-safe; do not call concurrently from multiple tasks without protection.
 */
class HBA1C {
public:
  /**
   * @brief Create multiple test JSON files (last 7 days) with random glucose values.
   *
   * Generates 7 files named "/YYYY-MM-DD.json" containing 10 samples each,
   * spaced by 5 minutes, with glucose in [80..180] mg/dL.
   *
   * @warning Overwrites existing files with the same names.
   */
  void createTestJsonFiles();

  /**
   * @brief Update the filename for the current day (today_json_filename).
   *
   * Uses local time (time(nullptr) + localtime()) and writes into the global buffer.
   */
  void updateFilename();

  /**
   * @brief Print raw file contents for debugging (block-wise).
   * @param filename Path to file (e.g. "/2026-01-08.json").
   *
   * Outputs both to the uuid logger and Serial.
   */
  void debugRawFileContents(const char* filename);

  /**
   * @brief Print decoded JSON file entries via logger and Serial/Telnet-friendly output.
   * @param filename File name or path; if not starting with '/', it will be prefixed.
   *
   * Expects a JSON array with objects containing:
   * - "timestamp": Unix epoch seconds
   * - "glucose": mg/dL (uint16)
   */
  void printJsonFileTelnet(const char* filename);

  /**
   * @brief List all JSON files in LittleFS (name and size).
   *
   * Only files with ".json" suffix are printed.
   */
  void listJsonFilesTelnet();

  /**
   * @brief Delete a JSON file from LittleFS.
   * @param filename Path to file (e.g. "/2026-01-08.json")
   * @return true if file existed and deletion succeeded, otherwise false.
   */
  bool deleteJsonFile(const char* filename);

  /**
   * @brief Add a glucose sample to the current day's JSON file.
   * @param timestamp Unix epoch seconds of measurement time.
   * @param glucose Glucose value in mg/dL.
   *
   * Behavior:
   * - Ensures filename corresponds to the day of @p timestamp.
   * - Loads the daily JSON array, appends {timestamp, glucose}.
   * - Avoids storing if the last stored glucose value equals @p glucose.
   * - Enforces a maximum length of MAX_ENTRIES by removing the oldest entry.
   * - Writes back to file and flushes for persistence.
   */
  void addGlucoseValue(time_t timestamp, uint16_t glucose);

  /**
   * @brief Check day boundary and update daily filename if day has changed.
   *
   * Uses an internal last timestamp to detect changes in tm_mday.
   */
  void checkNewDay();

  /**
   * @brief Process a JSON file and accumulate sum and count of glucose entries.
   * @param filename Path to the JSON file.
   * @param[out] count Incremented by number of valid glucose entries found.
   * @return Sum of glucose values (mg/dL) for this file.
   *
   * A valid entry is any object containing the key "glucose".
   */
  uint32_t processJsonFile(const char* filename, uint32_t& count);

  /**
   * @brief Calculate the mean glucose value from a JSON file or from all JSON files.
   * @param filename Path to file, or "*" to process all JSON files in LittleFS root.
   * @return Mean glucose (mg/dL). Returns 0.0 if no valid data found.
   */
  float calculateGlucoseMeanFromJson(const char* filename);

  /**
   * @brief Calculate mean glucose from an in-memory history array.
   * @param values Array of glucose values (mg/dL).
   * @param size Number of elements in @p values.
   * @return Mean glucose (mg/dL).
   *
   * @note Current implementation ignores elements that are 0 in the sum,
   * but still divides by @p size.
   */
  float calculateGlucoseMeanFromHistory(uint16_t values[], uint16_t size);

  /**
   * @brief Calculate mean glucose across JSON files from the last 7 days.
   * @return Mean glucose (mg/dL). Returns 0.0 if no valid data found.
   */
  float calculateGlucoseMeanForLast7Days();

  /**
   * @brief Estimate HbA1c (%) using the ADAG equation.
   * @param mean_glucose Mean glucose in mg/dL.
   * @return Estimated HbA1c in percent.
   *
   * Formula: (mean_glucose + 46.7) / 28.7
   */
  float calculate_hba1c(float mean_glucose);

  /**
   * @brief Calculate Time-In-Range percentage.
   * @param values Glucose values (mg/dL).
   * @param size Number of values.
   * @param min_range Lower bound (mg/dL), inclusive.
   * @param max_range Upper bound (mg/dL), inclusive.
   * @return Percentage [0..100] of values within [min_range, max_range].
   */
  float calculate_time_in_range(uint16_t values[], uint16_t size, int min_range, int max_range);

  /**
   * @brief Calculate standard deviation of glucose values.
   * @param values Glucose values (mg/dL).
   * @param size Number of values.
   * @param mean Mean glucose (mg/dL).
   * @return Standard deviation (mg/dL).
   */
  float calculate_standard_deviation(uint16_t values[], uint16_t size, float mean);

  /**
   * @brief Calculate coefficient of variation (CV%).
   * @param std_dev Standard deviation (mg/dL).
   * @param mean Mean glucose (mg/dL).
   * @return CV in percent.
   */
  float calculate_coefficient_of_variation(float std_dev, float mean);

private:
  /**
   * @brief Load JSON data from a file into a document.
   * @param filename Path to JSON file.
   * @param[out] jsonDoc Document to populate.
   *
   * If file is missing or unreadable, jsonDoc becomes an empty JSON array.
   */
  void loadJsonFromFile(const char* filename, JsonDocument& jsonDoc);

  /**
   * @brief Save a JSON document to a file and flush.
   * @param filename Path to JSON file.
   * @param jsonDoc Document to write (must be non-empty JsonArray).
   */
  void saveJsonToFile(const char* filename, JsonDocument& jsonDoc);
};

#endif  // HBA1C_H
