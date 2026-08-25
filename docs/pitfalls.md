# Feinheiten und Stolperfallen

Dinge, die man diesem Projekt nicht ansieht und die jeweils einen Abend Suche
gekostet haben. Aufbau: Symptom → Ursache → Konsequenz.

## Konfiguration überschreibt ihre eigenen Defaults

**Symptom:** Ein Feld ist 0, obwohl in `settings.h` ein sinnvoller Default steht.

`loadConfiguration()` kopiert bedingungslos aus dem JSON-Dokument:

```c
config.mqtt_port = doc["mqtt_port"];   // fehlt der Schlüssel → 0
```

Ein leeres oder unvollständiges `JsonDocument` liefert für jedes Feld 0 bzw. den
leeren String — der Default `uint16_t mqtt_port = 1883` aus dem Struct wird also
zerstört. Zudem meldet die Funktion einen Lesefehler zwar, **bricht aber nicht
ab** und kopiert weiter.

**Das kostete konkret:** Die Gesundheitsprüfung der FSM verband sich auf **Port
0**, scheiterte zwangsläufig, erklärte den WireGuard-Tunnel für krank und
startete das Gerät alle fünf Minuten neu. MQTT selbst lief die ganze Zeit —
weil es `mqtt.mqtt_port` benutzt, eine *andere* Variable.

**Fix:** `doc["x"] | config.x` behält den Default, wenn der Schlüssel fehlt. Ein
explizit gesetztes `0` im JSON bleibt dabei erhalten.

> Bisher nur für `mqtt_port` umgesetzt. **Alle übrigen Felder haben dieselbe
> Schwäche** — `telnet_port`, `brightness`, `timezone`, `wg_mode`.

## Zwei Quellen für denselben Wert

MQTT verbindet über `mqtt.mqtt_server` / `mqtt.mqtt_port`, die Gesundheitsprüfung
liest `settings.config.*`. Laufen die auseinander, funktioniert der Betrieb,
während die Diagnose Alarm schlägt — der irreführendste Fehlerfall überhaupt.

## WireGuard und Routing

Ist WireGuard aktiv, wird sein Interface zur **Default-Route**. Das WG-Interface
selbst bekommt eine Host-Route (`255.255.255.255`), beansprucht also kein
Subnetz. Daraus folgt eine Regel, die viel erklärt:

| Ziel | Weg |
|---|---|
| im eigenen Subnetz (z. B. lokaler MQTT-Broker) | **direkt über WiFi**, der Tunnel bleibt außen vor |
| alles andere (z. B. GitHub, 1.1.1.1) | über die Default-Route, also durch den Tunnel |

**Konsequenz für die Gesundheitsprüfung:** Ein Broker im eigenen LAN wird nie
durch den Tunnel erreicht. Eine TCP-Probe dorthin sagt über WireGuard also
**nichts** aus — obwohl an ihrem Ergebnis der Neustart hängt. Der
`INTERNET_CHECK` gegen `1.1.1.1` ist das ehrliche Ende-zu-Ende-Signal.

**Konsequenz für Firmware-Updates:** Der Update-Pfad braucht WireGuard nicht.
Aber ein Tunnel, der *existiert und nicht trägt*, ist schlimmer als gar keiner —
die Default-Route zeigt hinein, und GitHub wird unerreichbar. Kommt ein Update
bei defektem Tunnel nicht durch: erst WG abschalten, dann updaten.

## Das Gerät erreicht seine eigene IP nicht

**Symptom:** Die Telnet-Konsole der Weboberfläche meldet `connect failed,
errno=119 (EINPROGRESS)` nach etwa drei Sekunden, obwohl `telnet <ip> 23` vom PC
aus einwandfrei funktioniert.

`errno 119` ist kein Fehler, sondern der Normalzustand eines nicht-blockierenden
`connect()`. Dass er am Ende noch dasteht, heißt: das anschließende `select()`
ist in den Timeout gelaufen.

**Ursache:** Der lwIP-Port setzt `LWIP_HAVE_LOOPIF=1`. Damit wird in lwIP die
Abkürzung „Ziel ist meine eigene Adresse → intern zurückschleifen"
wegkompiliert:

```c
#if LWIP_NETIF_LOOPBACK && !LWIP_HAVE_LOOPIF
    if (ip4_addr_cmp(dest, netif_ip4_addr(netif))) { ... }
#endif
```

Das Paket geht stattdessen über WLAN hinaus, der Access Point spiegelt es nicht
zurück, und der SYN versickert.

**Fix:** `127.0.0.1` benutzen — das ist die Adresse, die das `lo0`-Interface
bedient. Die Brücke in `webpage.cpp` ersetzt die eigene STA-/AP-Adresse
inzwischen automatisch.

## Display flimmert bei Flash-Zugriffen

**Symptom:** Streifen und Bildversatz beim Schreiben ins LittleFS, besonders
stark während eines OTA-Updates. Das Bild normalisiert sich erst beim nächsten
Neuzeichnen.

**Ursache:** Ein Flash-Schreibvorgang schaltet den Cache ab. Die DMA des
RGB-Panels bekommt aus dem PSRAM keine Daten nach, der Zeilenpuffer läuft leer.

**Fix:** `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` über `custom_sdkconfig`. Code und
Rodata liegen dann im PSRAM, die Ausführung hängt nicht mehr am Flash-Cache.

**Was nicht half** (geprüft und verworfen, nicht wiederholen):

- Bounce-Buffer (`bounce_buffer_size_px`, `bb_invalidate_cache`) — nachweislich
  aktiv, ohne Wirkung. Die Nachfüll-ISR ist nicht IRAM-sicher.
- LVGL-Zeichenpuffer ins interne RAM verschieben — kostete 44 KB und ließ
  mbedTLS mit `(-32512) SSL - Memory allocation failed` scheitern.
- Erzwungenes `lv_obj_invalidate()` nach Schreibzugriffen.
- `LCD_CLK_SRC_DEFAULT` gegen `LCD_CLK_SRC_PLL160M` tauschen — auf dem ESP32-S3
  sind das identische Werte.

Der Auslöser im Normalbetrieb ist `hba1c.addGlucoseValue()`: es liest die
komplette Tagesdatei, hängt einen Eintrag an und schreibt **alles neu** — jede
Minute, bei einer auf 300 Einträge gedeckelten Datei also rund 12 KB. Anhängen
statt Neuschreiben wäre der eigentliche Hebel, ist aber nicht umgesetzt.

## WireGuard-Bibliothek: Reihenfolge und Sperren

Zwei Korrekturen im eigenen Fork, beide auf beiden Cores relevant:

**`wireguard_platform_init()` muss vor `netif_add()` laufen.** `netif_add()`
führt `wireguardif_init` als Callback aus, das über `wireguard_random_bytes()`
das Cookie-Secret erzeugt — der DRBG muss also vorher geseedet sein. Vorher stand
die Initialisierung 25 Zeilen später: unter mbedTLS 2.x lieferte der genullte
Kontext still deterministischen „Zufall", unter mbedTLS 3.x springt der Code in
einen NULL-Funktionszeiger (`PC = 0x00000000`, `InstrFetchProhibited`).

**Rohe lwIP-Aufrufe brauchen `LOCK_TCPIP_CORE()`.** IDF 5 baut lwIP mit
`CONFIG_LWIP_TCPIP_CORE_LOCKING` und `CONFIG_LWIP_CHECK_THREAD_SAFETY`; ohne
Sperre schlägt eine Assertion zu. Die Makros sind bei abgeschaltetem Core-Locking
leer definiert, ein `#ifdef` ist also nicht nötig. Die DNS-Auflösung muss
**außerhalb** der Sperre bleiben — sie ist Socket-API und würde blockieren.

## Speicher: nicht die Summe, sondern der größte Block

**Symptom:** Der Fetch scheitert mit `connection refused`, im Detail
`tls=SSL - Memory allocation failed` — obwohl `esp_status` ~58 KB freien
internen Speicher meldet. Tritt auf, sobald Telnet-Sitzungen oder das Dashboard
offen sind.

**Ursache:** mbedTLS braucht **zusammenhängende** Puffer. Der interne Heap
fragmentiert unter Last: 58 KB frei, aber nur 17,4 KB am Stück.

**Fix:** `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` mit `IN=16384` /
`OUT=4096`. ESP-IDF hat das ohnehin als Standard, der Arduino-Build schaltet es
ab und erzwingt 16 KB in **beide** Richtungen. Wiedereinschalten senkt den
Bedarf von 2×16 KB auf 16 KB + 4 KB und spart 12 KB pro Verbindung.

`IN` bei 16384 belassen: TLS erlaubt Records bis 16 KB, ein Server der einen
solchen schickt würde die Verbindung abbrechen.

### Nicht mit `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` lösen

Die Puffer ins PSRAM zu legen behebt die Allokation — der größte interne Block
stieg von 17.396 auf 30.708 — **bringt aber das Display-Flimmern zurück**, beim
Fetch wie beim Scrollen über den Graphen. Der PSRAM-Bus trägt bereits den
Framebuffer der LCD-DMA (~10,6 MB/s bei 480×480×2 und ~23 Hz), die
LVGL-Zeichenpuffer und seit XIP auch Code und Rodata. Zusätzlicher Verkehr
dort lässt die DMA verhungern.

**Merksatz:** Bedarf senken, nicht auf den PSRAM-Bus verschieben.

### Warum 120 MHz PSRAM nicht geht

`CONFIG_SPIRAM_SPEED_120M` wäre +50 % auf genau diesem Engpass, und IDF nennt
Quad-PSRAM bei 120 MHz stabil. Flash und PSRAM teilen sich auf dem ESP32-S3
aber den MSPI-Takt, und `mspi_timing_tuning_configs.h` besteht per
`ESP_STATIC_ASSERT` darauf, dass der PSRAM-Takt ein Vielfaches des Flash-Takts
ist. Bei Flash mit 80 MHz scheidet 120 aus; es ginge nur mit Flash auf 120 MHz
(eigenes Risiko) oder 40 MHz — was LittleFS-Schreibvorgänge verlangsamt und
damit gegenläufig wäre.

### Zwei Messfallen

- **`MALLOC_CAP_INTERNAL` allein** zählt das IRAM mit, einen nur
  32-bit-adressierbaren, voll belegten Heap. Richtig ist
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- **Die Summe sagt nichts.** Entscheidend ist
  `heap_caps_get_largest_free_block()`. `esp_status` zeigt beides.

`heap_caps_get_minimum_free_size()` wird von einem Boot-Ausreißer dominiert;
`main.cpp` führt deshalb mit `g_internal_min_runtime` einen eigenen Tiefstand,
gemessen **vor** jedem Fetch-Versuch. Es ist eine Stichprobe, kein exaktes
Minimum.

## custom_sdkconfig wirkt additiv

Eine Zeile aus `custom_sdkconfig` zu **entfernen** stellt den alten Wert nicht
wieder her — die generierte `sdkconfig.main` behält ihn. Zum Zurücknehmen
entweder den Gegenwert explizit setzen oder `sdkconfig.main` und
`sdkconfig.defaults` löschen und neu bauen.

Bei einer Kconfig-`choice` reicht es außerdem nicht, die gewünschte Alternative
zu setzen; die bestehende muss explizit deaktiviert werden
(`CONFIG_SPIRAM_SPEED_80M=n` neben `CONFIG_SPIRAM_SPEED_120M=y`).

Und: eine Änderung an `custom_sdkconfig` löst eine IDF-Neukonfiguration aus,
bei der der **erste** Build-Lauf an einem veralteten CMake-Cache scheitern kann.
Ein zweiter Aufruf läuft dann durch.

## Chart-Cursor: Neuzeichnen drosseln

`touch_event_cb()` läuft bei jedem `LV_EVENT_PRESSING`, also alle paar
Millisekunden. Bei 400 px Chartbreite und 141 Punkten (~2,8 px pro Punkt) landen
viele dieser Ereignisse auf **demselben** Datenpunkt. Ohne frühen Ausstieg wird
dann jedes Mal die volle Cursor-Positionierung ausgeführt — und weil die
Cursor-Linie volle Chart-Höhe hat, invalidiert das einen Streifen über die
gesamte Höhe und zwingt LVGL, den 141-Punkte-Linienzug neu zu zeichnen.

Zwei Regeln: bei unverändertem Index sofort zurückkehren, und
`lv_obj_move_foreground()` nur beim Sichtbarwerden aufrufen, nicht bei jeder
Bewegung.

## Chart: Versatz zwischen Chart-ID und Datenindex

`draw_chart_glucose_data()` füllt im Normalfall (Sensor länger als ~12 h aktiv)
**rechtsbündig**: `graph_data[data_count-1-i]` landet auf Chart-ID `140-i`. Bei
`data_count < 141` entsteht dadurch ein Versatz von `141 - data_count` zwischen
Chart-ID und `graph_data`/`timestamp`-Index, während `add_axis_labels()` von der
umgekehrten Annahme ausgeht.

**Das ist so gewollt** und wurde bewusst nicht geändert. Im eingeschwungenen
Zustand ist `data_count == 141` und der Versatz null.

## Upload-Skript kapert den seriellen Upload

`platformio_upload.py` ersetzte `UPLOADCMD` bedingungslos — auch bei
`upload_protocol = esptool`. Der serielle Upload landete dadurch im OTA-Pfad und
scheiterte an der auskommentierten `custom_upload_url`. Das sah aus, als ob
`extra_scripts` den *Build* bricht, während in Wahrheit nur `-t upload` betroffen
war. Die Registrierung ist inzwischen an `upload_protocol == "custom"` gebunden.
