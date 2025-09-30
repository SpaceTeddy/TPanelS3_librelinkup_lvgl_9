/**
 * @file hba1c.h
 * @brief Declaration of HbA1c value calculation and storage of glucose data on LittleFS.
 */

#ifndef HBA1C_H
#define HBA1C_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <memory>
#include <string>
#include <vector>
#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

#define MAX_ENTRIES 300         ///< Maximum of 24 hours with 5-minute intervals (288 = 12*24) + some extra
#define JSON_BUFFER_SIZE 12000  ///< Buffer size for JSON storage

extern DynamicJsonDocument* globalJsonDoc;
extern char today_json_filename[20];

/**
 * @class HBA1C
 * @brief Manages glucose data storage, calculations, and retrieval.
 */
class HBA1C {
    public:
        /**
         * @brief Debug function to print raw JSON file contents.
         * @param filename Path of the JSON file.
         */
        void debugRawFileContents(const char* filename);

        /**
         * @brief Print JSON file contents via Telnet.
         * @param filename Path of the JSON file.
         */
        void printJsonFileTelnet(const char* filename);

        /**
         * @brief List all available JSON files in LittleFS via Telnet.
         */
        void listJsonFilesTelnet();

        /**
         * @brief Delete a JSON file from LittleFS.
         * @param filename Path of the JSON file.
         * @return True if deletion was successful, false otherwise.
         */
        bool deleteJsonFile(const char* filename);

        /**
         * @brief Add a new glucose value to the JSON data storage.
         * @param timestamp Unix timestamp of the measurement.
         * @param glucose Glucose level in mg/dL.
         */
        void addGlucoseValue(time_t timestamp, uint16_t glucose);

        /**
         * @brief Checks if a new JSON file should be created for a new day.
         */
        void checkNewDay();

        /**
         * @brief Update the filename for the current day's JSON file.
         */
        void updateFilename();

        /**
         * @brief Process JSON data from a file and calculate total glucose sum and count.
         * @param filename Path of the JSON file.
         * @param count Reference to store the number of entries.
         * @return The total sum of glucose values.
         */
        uint32_t processJsonFile(const char* filename, uint32_t &count);

        /**
         * @brief Create test JSON files with random glucose data.
         */
        void createTestJsonFiles();

        /**
         * @brief Calculate the average glucose value from a JSON file.
         * @param filename Path of the JSON file.
         * @return The mean glucose value.
         */
        float calculateGlucoseMeanFromJson(const char* filename);

        /**
         * @brief Calculate the average glucose value from an array.
         * @param values Array of glucose values.
         * @param size Size of the array.
         * @return The mean glucose value.
         */
        float calculateGlucoseMeanFromHistory(uint16_t values[], uint16_t size);

        /**
         * @brief Calculate the average glucose value from the last 7 days.
         * @return The mean glucose value over the past week.
         */
        float calculateGlucoseMeanForLast7Days();

        /**
         * @brief Calculate the estimated HbA1c based on mean glucose.
         * @param mean_glucose The mean glucose value.
         * @return The estimated HbA1c.
         */
        float calculate_hba1c(float mean_glucose);

        /**
         * @brief Calculate the percentage of time in range (70-180 mg/dL).
         * @param values Array of glucose values.
         * @param size Size of the array.
         * @param min_range Minimum range value.
         * @param max_range Maximum range value.
         * @return The percentage of values within the range.
         */
        float calculate_time_in_range(uint16_t values[], uint16_t size, int min_range, int max_range);

        /**
         * @brief Calculate the standard deviation of glucose values.
         * @param values Array of glucose values.
         * @param size Size of the array.
         * @param mean Mean glucose value.
         * @return The standard deviation.
         */
        float calculate_standard_deviation(uint16_t values[], uint16_t size, float mean);

        /**
         * @brief Calculate the coefficient of variation (CV%) of glucose values.
         * @param std_dev Standard deviation.
         * @param mean Mean glucose value.
         * @return The coefficient of variation.
         */
        float calculate_coefficient_of_variation(float std_dev, float mean);

    private:
        /**
         * @brief Load JSON data from file into a given JSON document.
         * @param filename Path of the JSON file.
         * @param jsonDoc Reference to the JSON document to store data.
         */
        void loadJsonFromFile(const char* filename, DynamicJsonDocument &jsonDoc);

        /**
         * @brief Save a JSON document to a file.
         * @param filename Path of the JSON file.
         * @param jsonDoc Reference to the JSON document to save.
         */
        void saveJsonToFile(const char* filename, DynamicJsonDocument &jsonDoc);

};

#endif // HBA1C_H