# Architektur

Firmware für ein LilyGO T-Panel S3 (ESP32-S3, 480×480 RGB-Panel, PSRAM), die
Glukosewerte aus der LibreLinkUp-API holt, sie auf dem Display und einer
Weboberfläche darstellt und optional per MQTT verteilt.

## Zwei Tasks, bewusst getrennt

Die Firmware läuft in zwei unabhängigen FreeRTOS-Tasks. Diese Trennung ist die
wichtigste Strukturentscheidung des Projekts:

| Task | Läuft dort | Warum getrennt |
|---|---|---|
| Arduino `loop()` | FSM, Glukose-Fetch, MQTT-Publish, LVGL-Timer | blockiert bei Netzwerkaufrufen |
| `LoopTask` (gepinnt) | ElegantOTA, Telnet-Konsole, Hänger-Erkennung | bleibt erreichbar, wenn `loop()` steht |

Blockiert `loop()` in einem Netzwerkaufruf, läuft `LoopTask` weiter — deshalb
bleibt die Telnet-Konsole bedienbar, während die FSM scheinbar tot ist, und
deshalb kann `LoopTask` den Stillstand überhaupt melden. `loop()` aktualisiert
dazu den Zeitstempel `g_loop_alive_ms`, den `LoopTask` beobachtet.

Ein OTA-Update ist der Grund, warum diese Heartbeat-Logik nicht naiv sein darf:
`httpUpdate.update()` blockiert `loop()` für die gesamte Dauer, ohne
zurückzukehren. Der Fortschritts-Callback füttert den Heartbeat deshalb selbst
(`fw_ui_progress_update()` in `http_update.cpp`).

## Die Zustandsmaschine

`app_fsm.cpp` orchestriert den Betrieb. Zustände (siehe `AppState` in
`app_fsm.h`):

```
BOOT → WIFI_CONNECT → VPN_CHECK → MQTT_CONNECT → RUN_IDLE
                                                    │
              ┌─────────────────────────────────────┤
              ├→ RUN_FETCH → RUN_PUBLISH ───────────┤
              ├→ INTERNET_CHECK ────────────────────┤
              ├→ DISPLAY_DIM / DISPLAY_UNDIM ───────┤
              ├→ FW_CHECKING → FW_INSTALLING        │
              ├→ OTA_MODE (FSM ausgesetzt)          │
              └→ BACKOFF ───────────────────────────┘
```

`RUN_IDLE` ist die Drehscheibe: von dort werden alle periodischen Aufgaben
angestoßen, und dorthin kehrt jeder Zweig zurück. Jeder Übergang wird mit Grund
und Verweildauer geloggt, was die Fehlersuche über Telnet erheblich erleichtert:

```
[FSM]  4 RUN_IDLE  ->  5 RUN_FETCH  | reason=MASTER TICK  | run= 57693ms
```

## Datenfluss

Zwei Betriebsarten, gesteuert über `mqtt_master_mode`:

**Master** holt die Daten selbst von der LibreLinkUp-API (Bibliothek
`LibreLinkUpESP32`, HTTPS) und veröffentlicht den kompletten Graphen auf
`librelinkup/MASTER/data`.

**Client** holt gar nichts, sondern hört auf demselben Topic mit. Ein Client
ohne MQTT-Daten bleibt leer — im Log erkennbar an
`get_graph_data: Auth User: no user_id available!`.

Der Sinn: nur ein Gerät belastet die API, alle weiteren teilen sich dessen
Daten. Das umgeht auch die Rate-Limits der API.

## Module

| Datei | Aufgabe |
|---|---|
| `main.cpp` | Setup-Reihenfolge, `loop()`, `LoopTask`, Statusausgabe |
| `app_fsm.cpp` | Zustandsmaschine, periodische Aufgaben, Gesundheitsprüfungen |
| `webpage.cpp` | Async-Webserver, Dashboard, Chart, WebSocket-Telnet-Brücke |
| `commands.cpp` | Kommandos der uuid-Konsole (Telnet/Seriell) |
| `ui.c`, `ui_display.cpp` | LVGL-Oberfläche: Screens, Chart, Labels |
| `hba1c.cpp` | Glukose-Tagesdateien, Mittelwerte, Statistik |
| `mqtt_handler.cpp`, `mqtt.cpp` | MQTT-Verbindung, Publish, Home-Assistant-Discovery |
| `http_update.cpp` | OTA-Pull: Manifest von GitHub Pages prüfen und installieren |
| `ota_handler.cpp` | OTA-Push: ElegantOTA über die Weboberfläche |
| `settings.cpp` | `/config.json` auf LittleFS lesen und schreiben |
| `helper.cpp` | Zeit, DNS, TCP-Proben, Formatierung |
| `zigbee_h2.cpp`, `h2_ota.cpp` | IPC zum ESP32-H2 (Zigbee) inklusive dessen Firmware-Update |

## Anzeige

LVGL 9.5 auf einem RGB-Panel über Arduino_GFX. Sechs Screens: Welcome, Main,
Debug, FWInfo, FWUpdate, Login. Die Navigation läuft über Wischgesten
(`screen_rotation_next()` in `ui_display.cpp`) und den `screens`-Konsolenbefehl.

Der Framebuffer liegt im PSRAM, ebenso die LVGL-Zeichenpuffer. Das ist relevant
für das Flimmern beim Flash-Zugriff — siehe [pitfalls.md](pitfalls.md).

## Speicherung

Alles auf LittleFS:

- `/config.json` — die gesamte Konfiguration
- Tagesdateien mit Glukosewerten für Statistik und HbA1c-Schätzung
- `data/cert/x509_crt_bundle.bin` — als `board_build.embed_files` ins Image
  eingebettet, nicht auf dem Dateisystem

## Netzwerk

WiFi mit optionalem WireGuard-Tunnel. Ist WireGuard aktiv, wird es zur
Default-Route — mit Konsequenzen für alles, was danach eine Verbindung aufbaut.
Welche das sind und warum ein Ziel im eigenen Subnetz den Tunnel *nicht*
benutzt, steht in [pitfalls.md](pitfalls.md).

TLS gegen die LibreLinkUp-API und GitHub Pages über das eingebettete
Mozilla-CA-Bundle.
