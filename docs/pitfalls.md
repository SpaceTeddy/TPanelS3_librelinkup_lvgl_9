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

**Symptom:** Der Fetch scheitert mit `[HTTP] GET... failed, error: connection
refused`, im Detail `tls=SSL - Memory allocation failed` — obwohl `esp_status`
58 KB freien internen Speicher meldet. Tritt auf, sobald mehrere Telnet-
Sitzungen oder das Dashboard offen sind.

**Ursache:** mbedTLS braucht **zwei zusammenhängende 16-KB-Blöcke**
(`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, je einer pro Richtung). Der interne
Heap fragmentiert unter Last: 58 KB frei, aber nur 17,4 KB am Stück. Der erste
Puffer passt gerade noch, der zweite nicht mehr.

**Fix:** `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` über `custom_sdkconfig`. mbedTLS
allokiert dann im PSRAM, wo ~3,9 MB in einem Stück verfügbar sind.

Wirkung, gemessen:

| | vorher | nachher |
|---|---|---|
| größter interner Block | 17.396 | **30.708** |
| internes RAM gesamt | 58.736 | 66.124 |
| Fetch mit 2 Telnet-Sitzungen | scheitert | läuft |

Dass die Instruktionen bereits über `CONFIG_SPIRAM_XIP_FROM_PSRAM` aus dem PSRAM
kommen, macht die zusätzlichen TLS-Puffer dort bandbreitenmäßig unerheblich.

### Zwei Messfallen

- **`MALLOC_CAP_INTERNAL` allein** zählt das IRAM mit — einen nur
  32-bit-adressierbaren, zu 100 % belegten Heap. Richtig ist
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- **Die Summe sagt nichts.** Entscheidend ist
  `heap_caps_get_largest_free_block()`. `esp_status` zeigt beides; nur der
  größte Block beantwortet die Frage, ob eine TLS-Verbindung zustande kommt.

`main.cpp` führt zusätzlich mit `g_internal_min_runtime` einen eigenen
Tiefstand, gemessen **vor** jedem Fetch-Versuch. Der IDF-Wert
`heap_caps_get_minimum_free_size()` ist von einem Boot-Ausreißer dominiert und
als Frühwarnung unbrauchbar. Der eigene Wert ist eine Stichprobe, kein exaktes
Minimum — er kann kurzzeitig über dem aktuellen Stand liegen.

Reserve, falls es je wieder eng wird: `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096`
oder `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`. Den Eingangspuffer bei 16384 belassen,
sonst brechen Server mit großen TLS-Records die Verbindung ab.

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
