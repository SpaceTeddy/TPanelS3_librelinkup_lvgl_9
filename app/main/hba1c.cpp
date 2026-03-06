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
 * @note Uses a global DynamicJsonDocument allocated on heap (optionally PSRAM).
 * @note Uses LittleFS and flush() after writes for persistence.
 */

#include "hba1c.h"

#include "librelinkup.h"
extern LIBRELINKUP librelinkup;

//------------------------[uuid logger]-----------------------------------
/** @brief Module logger instance. */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>
#include <time.h>

//---------------------------[globals]------------------------------------

/**
 * @brief Global JSON document used for parsing/serialization.
 *
 * Allocated on heap to allow larger capacity; can be moved to PSRAM if desired.
 * Cleared frequently to keep memory usage stable.
 */
DynamicJsonDocument* globalJsonDoc = new DynamicJsonDocument(JSON_BUFFER_SIZE);

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
        time_t testTime = now - (i * 24 * 60 * 60);  // i Tage zurück
        localtime_r(&testTime, &timeinfo);

        char filename[20];
        strftime(filename, sizeof(filename), "/%Y-%m-%d.json", &timeinfo);

        DynamicJsonDocument doc(1024);
        JsonArray arr = doc.to<JsonArray>();

        for (int j = 0; j < 10; j++) {
            JsonObject obj = arr.createNestedObject();
            obj["timestamp"] = testTime + (j * 300);  // alle 5 Minuten
            obj["glucose"] = random(80, 180);         // Zufällige Werte zwischen 80 und 180
        }

        File file = LittleFS.open(filename, "w");
        if (file) {
            serializeJson(doc, file);
            file.flush(); // Wichtig für sicheres Speichern
            file.close();
            logger.notice("✅ Testdatei erstellt: %s", filename);
        } else {
            logger.notice("❌ Fehler beim Erstellen der Datei %s!", filename);
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
    updateFilename(now);
    //logger.debug("UpdateFilename: %s", today_json_filename);
}

void HBA1C::updateFilename(time_t timestamp) {
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    strftime(today_json_filename, sizeof(today_json_filename), "/%Y-%m-%d.json", &timeinfo);
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
        logger.notice("❌ Fehler: Datei %s nicht gefunden!", filename);
        return;
    }

    logger.notice("=== Rohdaten aus Datei %s ===", filename);
    Serial.printf("=== Rohdaten aus Datei %s ===\n\r", filename);

    char buffer[257];  // 256 Zeichen + Nullterminator
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

    logger.notice("Debug: Öffne Datei %s", filename);
    Serial.printf("Debug: Öffne Datei %s\n\r", filename);

    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        logger.notice("Fehler: Datei %s nicht gefunden!", filename);
        Serial.printf("Fehler: Datei %s nicht gefunden!\n\r", filename);
        return;
    }

    globalJsonDoc->clear();

    logger.notice("Debug: Lese JSON-Daten...");
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.notice("Fehler beim Lesen von %s: %s", filename, error.c_str());
        Serial.printf("Fehler beim Lesen von %s: %s\n\r", filename, error.c_str());
        return;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.notice("Fehler: JSON-Datei %s ist kein Array!", filename);
        Serial.printf("Fehler: JSON-Datei %s ist kein Array!\n\r", filename);
        return;
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();
    logger.notice("=== Glucose-Werte aus %s (Einträge: %d) ===", filename, arr.size());
    Serial.printf("=== Glucose-Werte aus %s (Einträge: %d) ===\n\r", filename, arr.size());

    for (JsonObject obj : arr) {
        time_t timestamp = obj["timestamp"].as<time_t>();
        uint16_t glucose = obj["glucose"].as<uint16_t>();

        struct tm *timeinfo = localtime(&timestamp);
        char timeString[20];
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeinfo);

        logger.notice("Zeit: %s | Glucose: %d mg/dL", timeString, glucose);
        Serial.printf("Zeit: %s | Glucose: %d mg/dL\n\r", timeString, glucose);

        yield();
    }

    logger.notice("Debug: JSON-Ausgabe abgeschlossen.");
    globalJsonDoc->clear();
}

/**
 * @brief List all JSON files in LittleFS (name and size).
 */
void HBA1C::listJsonFilesTelnet() {
    File root = LittleFS.open("/");
    if (!root) {
        logger.err("Fehler: LittleFS konnte nicht geöffnet werden!");
        return;
    }

    logger.notice("=== Alle gespeicherten JSON-Dateien ===");

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (filename.endsWith(".json")) {
            //logger.debug("Datei: %s | Größe: %d Bytes", filename.c_str(), file.size());
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
void HBA1C::loadJsonFromFile(const char* filename, DynamicJsonDocument &jsonDoc) {
    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.err("📂 Datei %s nicht gefunden, neue Datei wird erstellt!", filename);
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
        return;
    }

    //logger.debug("📂 Lade Datei %s...", filename);

    jsonDoc.clear();
    DeserializationError error = deserializeJson(jsonDoc, file);
    file.close();

    if (error) {
        logger.err("❌ Fehler beim Lesen von %s: %s", filename, error.c_str());
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
    }

    //logger.debug("📄 Datei %s geladen mit %d Einträgen.", filename, jsonDoc.size());
}

/**
 * @brief Save JSON document to file and flush.
 * @param filename Path of JSON file.
 * @param jsonDoc JSON document (must be non-empty JsonArray).
 */
void HBA1C::saveJsonToFile(const char* filename, DynamicJsonDocument &jsonDoc) {
    if (!jsonDoc.is<JsonArray>() || jsonDoc.size() == 0) {
        logger.err("❌ Fehler: JSON-Dokument ist leer oder ungültig! Speichern abgebrochen.");
        return;
    }

    File file = LittleFS.open(filename, "w");
    if (!file) {
        logger.err("❌ Fehler: Datei %s konnte nicht zum Schreiben geöffnet werden!", filename);
        return;
    }

    serializeJson(jsonDoc, file);
    file.flush();
    file.close();

    //logger.debug("✅ Datei %s erfolgreich gespeichert mit %d Einträgen.", filename, jsonDoc.size());
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
            logger.notice("🗑️ Datei %s wurde erfolgreich gelöscht!", filename);
            return true;
        } else {
            logger.notice("❌ Fehler: Datei %s konnte nicht gelöscht werden!", filename);
            return false;
        }
    } else {
        logger.notice("⚠️  Datei %s existiert nicht, kann nicht gelöscht werden.", filename);
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
    updateFilename(timestamp);

    struct tm timeinfo;
    struct tm last_timeinfo;
    localtime_r(&timestamp, &timeinfo);
    localtime_r(&last_timestamp, &last_timeinfo);

    if (last_timeinfo.tm_mday != timeinfo.tm_mday) {
        //logger.debug("🟢 Neuer Tag erkannt, Datei wechseln zu %s...", today_json_filename);
        updateFilename(timestamp);
    }

    last_timestamp = timestamp;

    loadJsonFromFile(today_json_filename, *globalJsonDoc);

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.err("❌ Fehler: JSON-Dokument ist kein Array! Erstelle neues Array.");
        globalJsonDoc->clear();
        globalJsonDoc->to<JsonArray>();
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();

    if (arr.size() > 0) {
        JsonObject lastEntry = arr[arr.size() - 1];
        if (lastEntry.containsKey("glucose") && lastEntry["glucose"] == glucose) {
            logger.debug("new value identical to last entry (%d mg/dL).", glucose);
            return;
        }
    }

    if (arr.size() >= MAX_ENTRIES) {
        arr.remove(0);
    }

    JsonObject obj = arr.createNestedObject();
    obj["timestamp"] = timestamp;
    obj["glucose"]   = glucose;

    saveJsonToFile(today_json_filename, *globalJsonDoc);
    globalJsonDoc->clear();

    //logger.debug("✅ Neuer Wert gespeichert: %ld | Glucose: %d mg/dL in Datei: %s", timestamp, glucose, today_json_filename);
}

/**
 * @brief Check if a new daily file is required at day boundary.
 */
void HBA1C::checkNewDay() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    struct tm last_timeinfo;
    localtime_r(&now, &timeinfo);
    localtime_r(&last_timestamp, &last_timeinfo);

    if (last_timeinfo.tm_mday != timeinfo.tm_mday) {
        Serial.println("Neuer Tag erkannt, Datei wechseln...");
        logger.notice("Neuer Tag erkannt, Datei wechseln...");
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
    if (size == 0) {
        return 0.0f;
    }

    float sum = 0;
    uint16_t valid_count = 0;
    for (int i = 0; i < size; i++) {
        if(values[i] != 0){
            sum += values[i];
            valid_count++;
        }
    }

    if (valid_count == 0) {
        return 0.0f;
    }
    return sum / valid_count;
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
        logger.err("❌ Fehler: Datei %s konnte nicht geöffnet werden!", filename);
        return 0;
    }

    //logger.debug("📂 Lade Datei %s...", filename);

    globalJsonDoc->clear();
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.err("❌ Fehler beim Lesen von %s: %s", filename, error.c_str());
        return 0;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.err("⚠️  Datei %s enthält kein JSON-Array!", filename);
        return 0;
    }

    uint32_t local_count = 0;
    uint32_t sum = 0;
    JsonArray arr = globalJsonDoc->as<JsonArray>();

    for (JsonObject obj : arr) {
        if (obj.containsKey("glucose")) {
            uint16_t glucose = obj["glucose"];
            sum += glucose;
            local_count++;
        }
    }

    count += local_count;
    //logger.debug("📄 Datei %s verarbeitet: %d Werte gefunden.", filename, local_count);
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
            logger.debug("❌ Fehler: Konnte Root-Verzeichnis nicht öffnen!");
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
            logger.debug("❌ Fehler: Datei %s existiert nicht!", filename);
            return 0.0;
        }

        sum += processJsonFile(filename, count);
    }

    if (count == 0) {
        logger.debug("⚠️  Keine gültigen Glucose-Daten gefunden!");
        return 0.0;
    }

    float mean = (float)sum / count;
    //logger.debug("📊 Berechneter Glucose-Mean aus %s: %.2f mg/dL (aus %d Einträgen)", filename, mean, count);
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
        logger.notice("❌ Fehler: Konnte Root-Verzeichnis nicht öffnen!");
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

        if (filename == "config.json" || filename == "/config.json") {
            file = root.openNextFile();
            continue;
        }

        const char* baseName = filename.c_str();
        const char* slash = strrchr(baseName, '/');
        if (slash != nullptr && *(slash + 1) != '\0') {
            baseName = slash + 1;
        }

        int year, month, day;
        if (sscanf(baseName, "%4d-%2d-%2d.json", &year, &month, &day) != 3) {
            logger.notice("⚠️  Datei %s hat kein gültiges Datumsformat!", filename.c_str());
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
            //logger.debug("📂 Einbeziehen: %s (vor %.0f Tagen)", filepath.c_str(), diff_days);
            sum += processJsonFile(filepath.c_str(), count);
        } else {
            //logger.debug("📂 Ignoriert: %s (vor %.0f Tagen)", filepath.c_str(), diff_days);
        }

        file = root.openNextFile();
    }

    if (count == 0) {
        logger.debug("⚠️  Keine Glucose-Daten in den letzten 7 Tagen gefunden!");
        return 0.0;
    }

    float mean = (float)sum / count;
    //logger.debug("📊 Durchschnittlicher Glucosewert der letzten 7 Tage: %.2f mg/dL (aus %d Werten)", mean, count);
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
    if (size == 0) {
        return 0.0f;
    }

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
    if (size == 0) {
        return 0.0f;
    }

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
    if (mean == 0.0f) {
        return 0.0f;
    }
    return (std_dev / mean) * 100.0;
}
