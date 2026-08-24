# Bauen und Flashen

## Plattformwahl

In `platformio.ini` stehen zwei Plattformen zur Auswahl, jeweils eine davon
aktiv:

```ini
;platform = espressif32 @7.0.1                                  ; Arduino 2.0.17 / IDF 4.4
platform = http://github.com/pioarduino/platform-espressif32.git ; Arduino 3.3.11 / IDF 5.5
```

`espressif32 @7.0.1` ist trotz der Versionsnummer der **alte** Stand: Arduino
Core 2.0.17 mit ESP-IDF 4.4. Die offizielle PlatformIO-Plattform ist dort stehen
geblieben; aktuelle Arduino-Releases erscheinen nur noch bei pioarduino.

**Der Code baut auf beiden.** Alle betroffenen Stellen sind mit
`ESP_ARDUINO_VERSION` bzw. `ESP_IDF_VERSION` abgesichert. Diese Eigenschaft ist
bewusst hergestellt und sollte erhalten bleiben — sie macht einen Rückwechsel zu
einer auskommentierten Zeile.

### pioarduino baut die IDF aus Quellen

Das ist der entscheidende Unterschied und der Grund für die Artefakte im
Projektverzeichnis (`managed_components/` mit mehreren hundert MB,
`CMakeLists.txt`, `dependencies.lock`, alle in `.gitignore`).

Es bedeutet aber auch: **jede sdkconfig-Option ist erreichbar**, direkt aus der
`platformio.ini`:

```ini
custom_sdkconfig =
	CONFIG_SPIRAM=y
	CONFIG_SPIRAM_MODE_QUAD=y
	CONFIG_SPIRAM_XIP_FROM_PSRAM=y
```

`CONFIG_SPIRAM_XIP_FROM_PSRAM` legt Code und Rodata ins PSRAM. Damit hängt die
Ausführung nicht mehr am Flash-Cache — was das behebt, steht in
[pitfalls.md](pitfalls.md#display-flimmert-bei-flash-zugriffen).

Bei der klassischen `espressif32`-Plattform greift `custom_sdkconfig` **nicht**:
dort kommen fertig übersetzte Bibliotheken zum Einsatz.

## extra_scripts

```ini
extra_scripts = pre:get_version.py, post:ldflags_compat.py, platformio_upload.py
```

| Skript | Aufgabe |
|---|---|
| `get_version.py` | setzt `APP_FIRMWARE_VERSION` aus `git describe` |
| `ldflags_compat.py` | fügt `-Wl,--no-warn-execstack` hinzu, falls der Linker es kennt |
| `platformio_upload.py` | OTA-Upload, nur aktiv bei `upload_protocol = custom` |

**Diese Zeile darf nicht auskommentiert werden**, auch nicht beim seriellen
Flashen. Ohne `get_version.py` meldet sich das Gerät als `0.0.0-dev`, hält sich
damit für älter als jede veröffentlichte Version und lädt sich alle zehn Minuten
die Release-Firmware herunter — es überschreibt also den Teststand, den man
gerade untersucht.

`ldflags_compat.py` muss ein **`post:`**-Skript sein. In einem `pre:`-Skript ist
`$CC` noch der Host-Compiler, die Fähigkeitsprüfung testet dann stillschweigend
das falsche Werkzeug. Die Prüfung selbst ist nötig, weil der ld 2.35 des
Core-2.x-Toolchains eine unbekannte Option als **harten Fehler** behandelt.

## Flashen

**Seriell** (USB):

```ini
;upload_protocol = custom
;custom_upload_url = http://192.168.0.103/update
```

Beide Zeilen auskommentiert lassen, `extra_scripts` aber aktiv. `pio run -t upload`.

**Über die Luft** (ElegantOTA):

```ini
upload_protocol = custom
custom_upload_url = http://<geraete-ip>/update
```

**Vom GitHub-Manifest** — das Gerät prüft selbständig alle zehn Minuten:

```ini
-DFW_UPDATE_MANIFEST_URL=\"https://spaceteddy.github.io/.../ota/manifest.json\"
-DFW_UPDATE_CHECK_INTERVAL_MS=600000
```

Dieser Weg braucht **kein WireGuard**. Entscheidend ist nur, welche Default-Route
gerade gilt — siehe [pitfalls.md](pitfalls.md#wireguard-und-routing).

## Partitionen

`partitions_16mb_ota3m5.csv`: zwei App-Slots à 3,5 MB, 9,2 MB LittleFS,
128 KB Coredump.

Rollback ist im Bootloader aktiv (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`),
Arduino bestätigt das Image aber bereits in `initArduino()` — also **vor**
`setup()`. Der Bootloader rettet damit nur vor einem Image, das gar nicht
hochkommt, nicht vor einem, das sauber startet und danach nicht funktioniert.

## Coredump auslesen

```bash
pio run -t coredump
```

Setzt einen Absturz voraus, der es in die Coredump-Partition geschafft hat, und
eine serielle Verbindung.
