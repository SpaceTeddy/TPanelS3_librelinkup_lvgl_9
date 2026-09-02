# Konfiguration und Bedienung

## `/config.json`

Die gesamte Laufzeitkonfiguration liegt als JSON auf LittleFS. Sie wird von
`settings.cpp` gelesen und geschrieben und überlebt ein OTA-Update — dieses
schreibt nur die App-Partition.

Gruppen: LibreLinkUp-Zugang, WLAN, MQTT, WireGuard, Anzeige, Zeitzone, OTA.

> **Achtung:** Ein fehlender Schlüssel wird nicht durch den Default aus
> `settings.h` ersetzt, sondern zu 0 bzw. `""`. Siehe
> [pitfalls.md](pitfalls.md#konfiguration-überschreibt-ihre-eigenen-defaults).

Die Datei im Repo unter `data/` ist eine Vorlage. Die echte Konfiguration
liegt auf dem Gerät und wird über `/api/config` oder die Konsole gepflegt.

## Wege, die Konfiguration zu ändern

| Weg | Speichert? |
|---|---|
| Touchscreen-Buttons | ✅ |
| Weboberfläche / `/api/…` | ✅ |
| Konsolen-Kommandos | teils — siehe unten |

`wireguard enable` hat lange **nicht** gespeichert: der Wert lebte nur im RAM,
nach einem Neustart stand wieder der alte Zustand in der Datei. Inzwischen
behoben, aber ein guter Grund, bei neuen Kommandos auf
`settings.saveConfiguration()` zu achten.

## Konsole

Erreichbar über Telnet (Port aus `telnet_port`, Standard 23) und über die
Weboberfläche. `help` listet alles auf; die wichtigsten:

| Kommando | Zweck |
|---|---|
| `esp_status` | Heap, PSRAM, LVGL-Speicher, Zähler, Netzwerkzustand |
| `log_level all` | Debug-Ausgaben einschalten |
| `llu statistics` | Glukose-Mittelwerte, Standardabweichung |
| `wifi`, `ping` | Netzwerkdiagnose |
| `wireguard enable\|disable` | Tunnel schalten |
| `fw_update`, `ota` | Firmware-Update anstoßen |
| `screens` | Screen weiterschalten |
| `list_json_files`, `print_json_file` | Glukose-Tagesdateien einsehen |
| `reboot` | Neustart |

### `esp_status` lesen

```
Internal RAM (8-bit): 65252 Bytes (min since boot: 45528, incl. boot: 180)
LVGL mem: 71% used, max_used 47240/59908 Bytes, frag 2%
```

- **`min since boot`** ist der aussagekräftige Wert. Unter ~45 KB wird es eng,
  eine TLS-Verbindung braucht ~32 KB.
- **`incl. boot`** meldet dauerhaft ~200 Bytes und ist kein Alarmzeichen.
- **LVGL** läuft in einem eigenen Pool, nicht im ESP-Heap. Ein Anstieg von
  `max_used` oder eine zweistellige Fragmentierung über die Zeit ist der
  Vorbote von Darstellungsproblemen.

### `/status` — der stille Vertrag zur Weboberfläche

`/status` liefert mehr als die gespeicherte Konfiguration; einige Felder sind
Laufzeitzustand, den die Config-Seite braucht. Wer daran etwas ändert, muss
beide Seiten anfassen.

| Feld | Bedeutung |
|---|---|
| `brightness` | **Ist-Wert** der Hintergrundbeleuchtung. Dimmen und Automatik schreiben ihn laufend. |
| `brightness_set` | **Sollwert** (`fsm.brightness_before_dim`). Übersteht die Dimmrampe, folgt der Automatik, ist das Ziel beim Aufhellen. **Daran hängt der Regler.** |
| `dim_active` | ob gerade eine Dimmrampe läuft |
| `auto_brightness` | Automatik ein/aus |
| `auto_bri_min` / `auto_bri_max` | Endpunkte der Helligkeitskurve bei 1 lx und 1000 lx |
| `lux_available` | ob ein gepaartes Zigbee-Gerät Beleuchtungsstärke meldet |
| `lux` | aktueller Umgebungswert, `-1` wenn keiner |

**Der Regler zeigt den Sollwert, nie den Ist-Wert.** Während einer Dimmrampe
fällt der Ist-Wert bis auf 0; ein Regler, der ihm folgt, zieht dem Nutzer seine
Einstellung weg. Der Ist-Wert erscheint nur als Zusatz, und nur wenn er
abweicht — als `(dimmed to N)` oder `(auto)`.

**`/status` darf die Konfiguration nicht aus dem Flash nachladen.** Das tat es
bis 2026-09-02 und verwarf damit jeden nicht gespeicherten Laufzeitwert:
`/setBrightness` schreibt bewusst nur ins RAM, weil bei jedem Reglerschritt zu
speichern das Dateisystem zermürben würde. Jeder Statusabruf setzte die
Helligkeit deshalb auf den zuletzt gespeicherten Wert zurück.

## Weboberfläche

- **Dashboard** mit Glukose-Chart, Cursor beim Überfahren
- **Konfiguration** aller Einstellungen
- **Telnet-Terminal** als WebSocket-Brücke — für dieses Gerät `127.0.0.1`
  eintragen, nicht die LAN-Adresse
- **ElegantOTA** unter `/update`

## MQTT

`librelinkup/MASTER/data` trägt den kompletten Graphen (~14 KB). Ein Gerät mit
`mqtt_master_mode = 1` holt die Daten von der API und veröffentlicht sie, alle
anderen hören mit.

Home-Assistant-Discovery wird bei aktivem `ha_discovery` automatisch
veröffentlicht.

## Zeit

NTP beim Start, danach eine Zeitzonen-Erkennung aus den API-Zeitstempeln
(`tz lock (1-sample): off_s=7200`). Eine falsche Zeit fällt zuerst bei
WireGuard auf: dessen Handshake verwirft Zeitstempel, die zu weit abweichen.
