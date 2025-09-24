
#include "helper.h"
#include <IPAddress.h>

#include "librelinkup.h"
extern LIBRELINKUP librelinkup;

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

//----------------------[Parse IP-Address from String]--------------------
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

//------------------------ [Prints the content of a file to the Serial]------------
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

//------------------------ [Prints Local time]----------------------------
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

//------------------------------------[covert to millis]----------------------------
uint32_t HELPER::convertToMillis(uint8_t hours, uint8_t minutes, uint8_t seconds) {
    return (hours * 3600UL + minutes * 60UL + seconds) * 1000UL;
}

//------------------[Hauptfunktion zum Synchronisieren mit einem 5-Sekunden-Offset]--------------
int32_t HELPER::synchronizeWithServer(uint8_t serverHours, uint8_t serverMinutes, uint8_t serverSeconds, 
                                      uint8_t localHours, uint8_t localMinutes, uint8_t localSeconds) {
    
    int32_t timeDifferenceMs = 0;

    // Server- und lokale Zeit in Millisekunden umrechnen
    uint32_t serverTimeMs = convertToMillis(serverHours, serverMinutes, serverSeconds);
    uint32_t localTimeMs  = convertToMillis(localHours, localMinutes, localSeconds);
    
    // calculate timedifference
    timeDifferenceMs = serverTimeMs - localTimeMs;
    //logger.notice("Timedifference from LibreLinkup Timestamp to ESP32 LocalTime: %dms",timeDifferenceMs);
    
    return timeDifferenceMs;
}

//------------------------[get flash ID functions]--------------------------------
/* String timecode = "12/15/2024 4:52:16 PM";
    long unixtime = convertToUnixTime(timecode);
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


// Funktion zur Umwandlung von Unix-Timestamp in "HH:MM"
// format_time(labels[i], sizeof(labels[i]), timecode_array[i]);
void HELPER::format_time(char *buffer, size_t buffer_size, time_t timestamp) {
    setlocale(LC_TIME, "C"); // Verhindert AM/PM und erzwingt 24h-Format
    struct tm *tm_info = localtime(&timestamp); // Nutze lokale Zeit
    strftime(buffer, buffer_size, "%H:%M", tm_info);
}
//-----------------------[check internet connection with Ping]-------------------
bool HELPER::check_internet_status(){
    
    bool result = 0;
    const IPAddress ping_ip(1,1,1,1);
    
    //Ping Host to check internet connection
    if(Ping.ping(ping_ip)){
        //DBGprint;Serial.printf("Ping %s: OK\n\r", ping_ip.toString());
        //logger.notice("Ping %s: OK", ping_ip.toString());
        result = 1;
    }else{
        Serial.printf("Ping %s: NOK\n\r", ping_ip.toString());
        logger.notice("Ping %s: NOK", ping_ip.toString());
        result = 0;
    }
    return result;
}

//---------------------------[get local time]-----------------------------------
String HELPER::get_esp_time_date(){
    
    uint8_t result = 0;

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
    
    //if (timeinfo.tm_isdst == 1){                            // check for daylight save time
    //    hour_localtime = atoi(timeinfo_hour);// + librelinkup.timezone;
    //}
    //else{
        hour_localtime = atoi(timeinfo_hour) + librelinkup.timezone;
    //}
    
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

//------------------------[get flash ID functions]--------------------------------
String HELPER::get_flashmemory_id(){

    uint32_t g_chipuid = ESP.getEfuseMac();
    String g_chipuid_str = String(g_chipuid, HEX);
    g_chipuid_str.toUpperCase();
    //mqtt_base +=  "_" + g_chipuid_str;
    //DBGprint;Serial.print("Flash UID str:"); Serial.println(g_chipuid_str);
    return g_chipuid_str;
}

//------------------------[get json memory data]--------------------------------
Json_Buffer_Info HELPER::getBufferSize(JsonDocument* doc) {
    Json_Buffer_Info info = {0, 0};  // Standardwerte
    if (doc == nullptr) {
        return info;                // Rückgabe bei Nullpointer
    }
    info.usedCapacity = doc->memoryUsage();
    info.totalCapacity = doc->capacity();
    return info;
}