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

## Der Speicherpreis von Core 3.x

Core 3.x hat rund **65 KB weniger freies internes RAM** als Core 2.x, gemessen
mit `esp_status` bei sonst gleicher Firmware (2026-08-28):

| | Core 2.x | Core 3.x |
|---|---|---|
| Internes RAM (8-bit) frei | 138 956 B | 73 876 B |
| Größter interner Block | 90 100 B | **23 540 B** |
| Minimum inkl. Boot | 69 544 B | 18 468 B |
| PSRAM frei | 6 496 071 B | 3 904 984 B |
| LVGL-Pool | 67 %, 44 752/60 084 | 66 %, 44 132/60 100 |

Die Laufzeiten unterscheiden sich (7:17 gegen 2:42), die Zahlen sind also nicht
auf die Sekunde vergleichbar — die Größenordnung schon.

Zwei Werte sind wichtiger als die Summe:

- **Der größte zusammenhängende Block**, nicht das freie Gesamt-RAM, entscheidet
  über TLS: eine Session braucht 16 KB am Stück. 23 540 B unter Core 3.x sind
  ein schmaler Grat, 90 100 B unter Core 2.x sind komfortabel.
- **`min since boot`** stammt aus einem eigenen Zähler (`g_internal_min_runtime`),
  der periodisch abgetastet wird. Er kann deshalb *über* dem aktuellen Freiwert
  liegen, wenn seit der letzten Abtastung allokiert wurde — das ist kein Fehler.
  Der harte Tiefstwert ist „inkl. Boot".

Das fehlende PSRAM (2,6 MB) ist kein Verlust, sondern der Preis von
`CONFIG_SPIRAM_XIP_FROM_PSRAM`: `.text` und `.rodata` werden beim Boot ins PSRAM
kopiert. Genau das behebt den Flicker.

### Wo die 65 KB liegen

`python3 tools/dram_usage.py` wertet die Link-Map aus. Ergebnis für beide Stände:

| | Core 2.x | Core 3.x |
|---|---|---|
| `.bss`/`.data` je Archiv, Summe | 111 734 B | 112 399 B |
| `.iram0.text` | 71 091 B | 81 875 B |
| `.dram0.dummy` | 55 736 B | 66 560 B |
| Heap-Beginn ab `0x3FC80000` | +279 160 B | +292 848 B |

**Der statische Anteil ist praktisch gleich** — 665 Bytes Unterschied, obwohl
Core 3.x NimBLE mit `MEM_ALLOC_MODE_INTERNAL` in den Defaults hat. Bluetooth
wird gar nicht gelinkt, weder `libbt.a` noch `libnimble.a` steuern ein Byte bei.
BT abzuschalten bringt hier also nichts.

Der Rest teilt sich so auf:

- **~13,7 KB** gehen an den Linker: `.iram0.text` wächst unter IDF 5.5 um
  10,8 KB. Auf dem S3 sind IRAM und DRAM dasselbe SRAM, deshalb wächst
  `.dram0.dummy` exakt mit — jedes Byte IRAM kostet ein Byte Heap. Daran ist
  wenig zu drehen: `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT` und
  `LWIP_IRAM_OPTIMIZATION` sind in der Core-3-Config bereits **aus**.
- **~51 KB** werden zur Laufzeit allokiert (Heap-Pool minus frei, gegen Core 2.x
  gerechnet). Hier sitzt der eigentliche Unterschied, und hier ist noch nicht
  geklärt, wer sie hält.

### Was nichts gebracht hat

`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` legt WiFi- und lwIP-Puffer ins PSRAM.
Core 2.x setzt das per Default, Core 3.x nicht — deshalb lag der Verdacht nahe,
dass dort die Differenz steckt. Die Option **greift** nachweislich
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` steht in der generierten
`sdkconfig.main`), bringt aber nur **~8,6 KB** (65 252 → 73 876 B). Sie erklärt
die Lücke nicht.

Die Lehre: bevor man eine sdkconfig-Option als wirkungslos abhakt, in
`sdkconfig.main` nachsehen, ob sie überhaupt angekommen ist. Und in einem
mehrzeiligen INI-Wert beendet eine **Leerzeile** den Wert — steht eine Option
dahinter, wird sie stillschweigend ignoriert. Deshalb ist der Block im
`custom_sdkconfig` lückenlos mit `;`-Zeilen durchgezogen.

Ebenfalls geprüft und verworfen: `CONFIG_MBEDTLS_DYNAMIC_BUFFER` taucht in
**keiner** der beiden sdkconfigs auf, ist also hinter einer Abhängigkeit
versteckt und keine Option.

### Nächster Messschritt

Die offenen ~51 KB findet man nicht durch Raten an sdkconfig-Schaltern. `esp_status`
gibt dafür seit dieser Untersuchung eine zusätzliche Zeile aus:

```
Internal pool: <Summe> Bytes (<alloc> alloc in <n> blocks, <frei> free in <m> blocks)
```

Sie beantwortet zwei Fragen, die aus „frei" und „größter Block" allein nicht
hervorgehen:

- **`free blocks`** trennt Fragmentierung von Regionsgrenzen. Eine Handvoll
  freier Blöcke heißt: der interne Heap ist vom Linker schlicht in Regionen
  zerlegt, die 23 540 B sind normal. Dutzende heißen: echte Fragmentierung, und
  dann lohnt die Suche nach dem Verursacher.
- **`alloc + free`** ist die Pool-Größe. Nur sie trennt die beiden Effekte, die
  sich unter Core 3.x überlagern — kleinerer Pool (das größere `.iram0.text`
  frisst dasselbe SRAM) *und* mehr Laufzeit-Allokation. Zum Vergleich: der
  Linker meldet für den Core-3-Build 147 257 B freies DIRAM, zur Laufzeit sind
  es 73 876 B.

Für die Zuordnung „welcher Task hält was" bräuchte es zusätzlich
`CONFIG_HEAP_TASK_TRACKING=y` und `heap_caps_get_per_task_info()`. Task-Stacks
sind der Hauptverdacht — sie liegen immer im internen RAM.

### Erste Messung (Core 3.x, 2026-08-28)

```
Internal largest block: 19444 Bytes (TLS needs ~16k contiguous)
Internal RAM (8-bit): 73884 Bytes (min since boot: 78216, incl. boot: 18432)
Internal pool: 257168 Bytes (183496 alloc in 569 blocks, 73672 free in 18 blocks)
```

- **183 496 B sind zur Laufzeit allokiert**, in 569 Blöcken zu im Mittel 322 B.
  Das ist der Brocken, nicht die Pool-Größe.
- **18 freie Blöcke** bei 73 672 B frei: die IDF registriert auf dem S3 nur eine
  Handvoll interner 8-Bit-Regionen, es sind also echte Löcher dazwischen — aber
  moderat, keine Zersplitterung in Dutzende Fragmente.
- **Der größte freie Block schwankt**: 23 540 B in einer Messung, 19 444 B in der
  nächsten. Eine TLS-Session braucht 16 384 B am Stück für den IN-Puffer. Das
  passt — knapp. Ein Loch mehr und Handshakes scheitern wieder mit
  „SSL - Memory allocation failed". Das ist das operative Risiko von Core 3.x,
  unabhängig davon, wo die 51 KB stecken.

### Der Heap steht nach dem Boot still

Zwei Messungen desselben Builds, bei 18 s und bei 1:47 Laufzeit, liefern
**exakt** dieselben Werte: 183 496 B allokiert in 569 Blöcken, größter freier
Block 19 444 B. Daraus folgt:

- **Kein Leck und keine kriechende Fragmentierung.** Die 183 KB sind
  Startkosten, kein Wachstum. Wer sie senken will, muss beim Startup ansetzen,
  nicht bei der Laufzeit.
- **Der Zustand nach 18 s ist der Dauerzustand.** Eine Messung direkt nach dem
  Boot genügt, man muss nicht lange laufen lassen.

Der interessante Wert ist stattdessen das Minimum **inkl. Boot**: 15 160 B in
einer Messung, 18 432 B in der anderen — es schwankt von Boot zu Boot. Zu einem
Zeitpunkt während des Starts waren also nur ~15 KB internes RAM frei, gegenüber
73 888 B im Dauerzustand. Irgendetwas belegt kurzzeitig ~58 KB und gibt sie
wieder frei.

Die naheliegende Erklärung ist der erste TLS-Handshake (LibreLinkUp-Login):
16 KB IN-Puffer, 4 KB OUT, dazu Handshake-Zustand und die Verifikation gegen das
Zertifikatsbündel. Das passt in der Größenordnung, ist aber **nicht
nachgewiesen** — zu prüfen, indem man `esp_status` vor dem ersten Fetch aufruft
und danach erneut.

### Was das für TLS bedeutet

Dass der Boot-Tiefstwert bei 15 KB liegt und der Start trotzdem durchläuft,
heißt: eine TLS-Session passt. Knapp. Der größte freie Block ist im
Dauerzustand 19 444 B, eine Session braucht 16 384 B am Stück — 3 KB Reserve.

Die daraus folgende Vorhersage: **zwei gleichzeitige TLS-Sessions passen unter
Core 3.x vermutlich nicht.** Nach dem ersten 16-KB-Puffer bleibt vom größten
Block nichts Brauchbares übrig, und die restlichen 18 freien Blöcke sind alle
kleiner. Unter Core 2.x mit 90 100 B größtem Block ist das kein Thema. Wer also
gleichzeitig das Web-Dashboard offen hat und einen Glukose-Fetch laufen lässt,
ist der wahrscheinlichste Auslöser für „SSL - Memory allocation failed".

Offen bleibt, ob Core 2.x einen *größeren Pool* hat oder schlicht *weniger
allokiert*. Diese Zeile aus einem Core-2-Build beantwortet es — nur die
Pool-Größe trennt die beiden Effekte. Bislang nicht gemessen.

> **Ungeprüft:** die `Internal pool`-Zeile ist nur unter Core 3.x gebaut worden.
> `heap_caps_get_info()` und `multi_heap_info_t` gibt es unverändert seit
> IDF 3.x, das sollte also halten — nachgewiesen ist es für Core 2.x aber nicht.
> Beim nächsten Rückwechsel als Erstes prüfen (siehe die Zweikern-Regel oben).

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
