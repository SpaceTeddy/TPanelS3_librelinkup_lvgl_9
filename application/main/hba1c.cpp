#include "hba1c.h"

#include "librelinkup.h"
extern LIBRELINKUP librelinkup;

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>   // für pow/sqrt

// ---------------------------------------------------------------------
// KEINE globalen PSRAM/Json-Allokationen mehr!
// Stattdessen: Lazy-Init auf dem Heap nach Start (setup()).
// Größe kommt weiterhin aus hba1c.h (JSON_BUFFER_SIZE).
// ---------------------------------------------------------------------

DynamicJsonDocument* globalJsonDoc = nullptr;


static inline void hba1c_ensure_init() {
    if (!globalJsonDoc) {
        globalJsonDoc = new DynamicJsonDocument(JSON_BUFFER_SIZE);
        if (!globalJsonDoc) {
            // Sehr unwahrscheinlich; aber sauber loggen
            logger.err("❌ Konnte globalJsonDoc nicht allokieren (JSON_BUFFER_SIZE=%u)!", (unsigned)JSON_BUFFER_SIZE);
        }
    }
}

// Dateiname wie gehabt
char today_json_filename[20];  // z.B. "/2025-09-20.json"

// „Letzter gespeicherter Zeitstempel“ (tageswechsel-Logik)
static time_t last_timestamp = 0;

// ---------------------------------------------------------------------
// Öffentliche Init der Klasse
// ---------------------------------------------------------------------
void HBA1C::begin() {
    hba1c_ensure_init();
    // optional: direkt Dateiname initialisieren
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    if (timeinfo) {
        strftime(today_json_filename, sizeof(today_json_filename), "/%Y-%m-%d.json", timeinfo);
    } else {
        snprintf(today_json_filename, sizeof(today_json_filename), "/%Y-%m-%d.json");
    }
    logger.debug("HBA1C::begin() -> Startdatei: %s", today_json_filename);
}

// ---------------------------------------------------------------------
// Hilfsfunktionen / Tools
// ---------------------------------------------------------------------

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
            obj["glucose"] = random(80, 180);         // 80..180
        }

        File file = LittleFS.open(filename, "w");
        if (file) {
            serializeJson(doc, file);
            file.flush();
            file.close();
            logger.notice("✅ Testdatei erstellt: %s", filename);
        } else {
            logger.notice("❌ Fehler beim Erstellen der Datei %s!", filename);
        }
    }
}

// Aktuellen Tages-Dateinamen aktualisieren
void HBA1C::updateFilename() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    if (timeinfo) {
        strftime(today_json_filename, sizeof(today_json_filename), "/%Y-%m-%d.json", timeinfo);
        logger.debug("UpdateFilename: %s", today_json_filename);
    } else {
        logger.notice("⚠️ Konnte lokale Zeit nicht lesen; behalte %s", today_json_filename);
    }
}

void HBA1C::debugRawFileContents(const char* filename) {
    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.notice("❌ Fehler: Datei %s nicht gefunden!", filename);
        return;
    }

    logger.notice("=== Rohdaten aus Datei %s ===", filename);
    Serial.printf("=== Rohdaten aus Datei %s ===\n\r", filename);

    char buffer[257];
    size_t bytesRead;

    while ((bytesRead = file.readBytes(buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        logger.notice("%s", buffer);
        Serial.printf("%s", buffer);
        yield();
    }

    file.close();
}

void HBA1C::printJsonFileTelnet(const char* filename) {
    hba1c_ensure_init();

    String path = filename;
    if (!path.startsWith("/")) path = "/" + path;
    
    logger.notice("Debug: Öffne Datei %s", path.c_str());
    Serial.printf("Debug: Öffne Datei %s\n\r", path.c_str());

    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        logger.notice("Fehler: Datei %s nicht gefunden!", path.c_str());
        Serial.printf("Fehler: Datei %s nicht gefunden!\n\r", path.c_str());
        return;
    }

    globalJsonDoc->clear();

    logger.notice("Debug: Lese JSON-Daten...");
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.notice("Fehler beim Lesen von %s: %s", path.c_str(), error.c_str());
        Serial.printf("Fehler beim Lesen von %s: %s\n\r", path.c_str(), error.c_str());
        return;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.notice("Fehler: JSON-Datei %s ist kein Array!", path.c_str());
        Serial.printf("Fehler: JSON-Datei %s ist kein Array!\n\r", path.c_str());
        return;
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();
    logger.notice("=== Glucose-Werte aus %s (Einträge: %d) ===", path.c_str(), arr.size());
    Serial.printf("=== Glucose-Werte aus %s (Einträge: %d) ===\n\r", path.c_str(), arr.size());

    for (JsonObject obj : arr) {
        time_t ts = obj["timestamp"].as<time_t>();
        uint16_t glucose = obj["glucose"].as<uint16_t>();

        struct tm *timeinfo = localtime(&ts);
        char timeString[20];
        if (timeinfo) {
            strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeinfo);
        } else {
            strcpy(timeString, "invalid time");
        }

        logger.notice("Zeit: %s | Glucose: %d mg/dL", timeString, glucose);
        Serial.printf("Zeit: %s | Glucose: %d mg/dL\n\r", timeString, glucose);
        yield();
    }

    logger.notice("Debug: JSON-Ausgabe abgeschlossen.");
    globalJsonDoc->clear();
}

void HBA1C::listJsonFilesTelnet() {
    File root = LittleFS.open("/");
    if (!root) {
        logger.notice("Fehler: LittleFS konnte nicht geöffnet werden!");
        return;
    }

    logger.notice("=== Alle gespeicherten JSON-Dateien ===");

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (filename.endsWith(".json")) {
            logger.notice("Datei: %s | Größe: %d Bytes", filename.c_str(), file.size());
        }
        file = root.openNextFile();
    }
}

void HBA1C::loadJsonFromFile(const char* filename, DynamicJsonDocument &jsonDoc) {
    hba1c_ensure_init();

    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.debug("📂 Datei %s nicht gefunden, neue Datei wird erstellt!", filename);
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
        return;
    }

    logger.notice("📂 Lade Datei %s...", filename);

    jsonDoc.clear();
    DeserializationError error = deserializeJson(jsonDoc, file);
    file.close();

    if (error) {
        logger.err("❌ Fehler beim Lesen von %s: %s", filename, error.c_str());
        jsonDoc.clear();
        jsonDoc.to<JsonArray>();
    }

    logger.debug("📄 Datei %s geladen mit %d Einträgen.", filename, jsonDoc.size());
}

void HBA1C::saveJsonToFile(const char* filename, DynamicJsonDocument &jsonDoc) {
    hba1c_ensure_init();

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

    logger.debug("✅ Datei %s erfolgreich gespeichert mit %d Einträgen.", filename, jsonDoc.size());
    jsonDoc.clear();
}

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

// Neuen Wert speichern
void HBA1C::addGlucoseValue(time_t timestamp, uint16_t glucose) {
    hba1c_ensure_init();

    updateFilename();

    struct tm *timeinfo = localtime(&timestamp);
    struct tm *last_timeinfo = localtime(&last_timestamp);

    if (timeinfo && last_timeinfo && (last_timeinfo->tm_mday != timeinfo->tm_mday)) {
        logger.debug("🟢 Neuer Tag erkannt, Datei wechseln zu %s...", today_json_filename);
        updateFilename();
    }

    last_timestamp = timestamp;

    loadJsonFromFile(today_json_filename, *globalJsonDoc);

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.err("❌ Fehler: JSON-Dokument ist kein Array! Erstelle neues Array.");
        globalJsonDoc->clear();
        globalJsonDoc->to<JsonArray>();
    }

    JsonArray arr = globalJsonDoc->as<JsonArray>();

    // Doppelte direkt nacheinander vermeiden
    if (arr.size() > 0) {
        JsonObject lastEntry = arr[arr.size() - 1];
        if (lastEntry.containsKey("glucose") && lastEntry["glucose"] == glucose) {
            logger.debug("⚠️  Neuer Wert ist identisch zum letzten Eintrag (%d mg/dL). Speichern übersprungen.", glucose);
            globalJsonDoc->clear();
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

    logger.debug("✅ Neuer Wert gespeichert: %ld | Glucose: %d mg/dL in Datei: %s", (long)timestamp, glucose, today_json_filename);
}

// Prüfen, ob eine neue Datei um 00:00 benötigt wird
void HBA1C::checkNewDay() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    struct tm *last_timeinfo = localtime(&last_timestamp);

    if (timeinfo && last_timeinfo && (last_timeinfo->tm_mday != timeinfo->tm_mday)) {
        Serial.println("Neuer Tag erkannt, Datei wechseln...");
        logger.notice("Neuer Tag erkannt, Datei wechseln...");
        updateFilename();
    }

    last_timestamp = now;
}

// Mittelwert aus History-Array
float HBA1C::calculateGlucoseMeanFromHistory(uint16_t values[], uint16_t size){
    float sum = 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < size; i++) {
        if(values[i] != 0){
            sum += values[i];
            n++;
        }
    }
    return n ? (sum / n) : 0.0f;
}

uint32_t HBA1C::processJsonFile(const char* filename, uint32_t &count) {
    hba1c_ensure_init();

    File file = LittleFS.open(filename, "r");
    if (!file) {
        logger.err("❌ Fehler: Datei %s konnte nicht geöffnet werden!", filename);
        return 0;
    }

    logger.notice("📂 Lade Datei %s...", filename);

    globalJsonDoc->clear();
    DeserializationError error = deserializeJson(*globalJsonDoc, file);
    file.close();

    if (error) {
        logger.err("❌ Fehler beim Lesen von %s: %s", filename, error.c_str());
        return 0;
    }

    if (!globalJsonDoc->is<JsonArray>()) {
        logger.notice("⚠️  Datei %s enthält kein JSON-Array!", filename);
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
    logger.notice("📄 Datei %s verarbeitet: %d Werte gefunden.", filename, local_count);
    globalJsonDoc->clear();
    return sum;
}

float HBA1C::calculateGlucoseMeanFromJson(const char* filename) {
    hba1c_ensure_init();

    uint32_t sum = 0;
    uint32_t count = 0;

    if (strcmp(filename, "*") == 0) {
        // Alle JSON-Dateien
        File root = LittleFS.open("/");
        if (!root || !root.isDirectory()) {
            logger.debug("❌ Fehler: Konnte Root-Verzeichnis nicht öffnen!");
            return 0.0f;
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
        // Nur die angegebene Datei
        if (!LittleFS.exists(filename)) {
            logger.debug("❌ Fehler: Datei %s existiert nicht!", filename);
            return 0.0f;
        }
        sum += processJsonFile(filename, count);
    }

    if (count == 0) {
        logger.debug("⚠️  Keine gültigen Glucose-Daten gefunden!");
        return 0.0f;
    }

    float mean = (float)sum / count;
    logger.debug("📊 Berechneter Glucose-Mean aus %s: %.2f mg/dL (aus %d Einträgen)", filename, mean, count);
    return mean;
}

float HBA1C::calculateGlucoseMeanForLast7Days() {
    hba1c_ensure_init();

    uint32_t sum = 0;
    uint32_t count = 0;

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        logger.notice("❌ Fehler: Konnte Root-Verzeichnis nicht öffnen!");
        return 0.0f;
    }

    time_t now = time(nullptr);

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        
        if (!filename.endsWith(".json")) {
            file = root.openNextFile();
            continue;
        }

        if (filename == "config.json") {
            file = root.openNextFile();
            continue;
        }

        // Datumsformat (YYYY-MM-DD.json) extrahieren
        int year, month, day;
        if (sscanf(filename.c_str(), "%4d-%2d-%2d.json", &year, &month, &day) != 3) {
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
            String path = filename;
            if (!path.startsWith("/")) path = "/" + path;
            logger.notice("📂 Einbeziehen: %s (vor %.0f Tagen)", path.c_str(), diff_days);
            sum += processJsonFile(path.c_str(), count);
        } else {
            logger.notice("📂 Ignoriert: %s (vor %.0f Tagen)", filename.c_str(), diff_days);
        }

        file = root.openNextFile();
    }

    if (count == 0) {
        logger.notice("⚠️  Keine Glucose-Daten in den letzten 7 Tagen gefunden!");
        return 0.0f;
    }

    float mean = (float)sum / count;
    logger.notice("📊 Durchschnittlicher Glucosewert der letzten 7 Tage: %.2f mg/dL (aus %d Werten)", mean, count);
    return mean;
}

// HbA1c
float HBA1C::calculate_hba1c(float mean_glucose) {
    return (mean_glucose + 46.7f) / 28.7f;
}

// Time in Range
float HBA1C::calculate_time_in_range(uint16_t values[], uint16_t size, int min_range, int max_range) {
    int hit = 0;
    for (uint16_t i = 0; i < size; i++) {
        if (values[i] >= min_range && values[i] <= max_range) hit++;
    }
    return size ? (hit / (double)size) * 100.0 : 0.0;
}

// Standardabweichung
float HBA1C::calculate_standard_deviation(uint16_t values[], uint16_t size, float mean) {
    if (size == 0) return 0.0f;
    double sum = 0;
    for (uint16_t i = 0; i < size; i++) {
        double d = (double)values[i] - (double)mean;
        sum += d * d;
    }
    return sqrt(sum / size);
}

// Variationskoeffizient
float HBA1C::calculate_coefficient_of_variation(float std_dev, float mean) {
    return (mean != 0.0f) ? ((std_dev / mean) * 100.0f) : 0.0f;
}
