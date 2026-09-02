# Zigbee über den ESP32-H2

Der S3 spricht über UART mit einem zweiten Chip, dem ESP32-H2, der als
Zigbee-Koordinator arbeitet. Der H2 hält das Funknetz, der S3 stellt Bedienung,
Weboberfläche und MQTT bereit.

## Wer was hält

| | ESP32-H2 | ESP32-S3 |
|---|---|---|
| Zigbee-Netz, Bindungen, Attributberichte | ✔ | |
| Geräteregister für Oberfläche und MQTT | | ✔ |
| Kommandos entgegennehmen | ✔ | |
| Weboberfläche, Home-Assistant-Discovery | | ✔ |

Beide Seiten führen eine eigene Geräteliste. Die des S3 (`H2Device` in
`zigbee_h2.h`, 32 Plätze) ist eine **Kopie**, gefüllt aus den Meldungen des H2 —
sie ist nie die Wahrheit, nur der letzte bekannte Stand.

## Das Protokoll

Zeilenweise JSON über UART mit 460800 Baud. Der S3 sendet `{"cmd":"..."}`, der
H2 antwortet mit `{"type":"..."}`.

Wichtige Meldungstypen: `list`, `join`, `status`, `scan`, `motion`, `sensor`,
`ack`, `version`, `chipinfo`, `ota_progress`.

### Zwei Regeln, die man nicht verletzen darf

**Kommandos aus fremden Tasks werden eingereiht, nicht gesendet.** `h2_send()`
schreibt den UART direkt und läuft im `loop()`-Task. Web-Handler laufen im
AsyncTCP-Task — würden sie selbst senden, verschränkten sich zwei Schreiber auf
derselben Leitung. Dafür gibt es `h2_enqueue()`; geleert wird die Warteschlange
am Anfang von `zigbee_h2_poll_uart()`.

**Jede zustandsändernde Aktion muss ein `list` nachziehen.** Das Register lernt
ausschließlich aus `list`- und `join`-Meldungen. Wer schaltet, entfernt oder
pollt, ohne danach eine Liste anzufordern, sieht in der Oberfläche weiter den
alten Wert.

## Fallen, die Zeit gekostet haben

**`on` bedeutet nicht „schaltbar".** Bei IAS-Zone-Sensoren spiegelt der H2 die
Bewegung in `onOff` (`coordinator_zigbee.cpp`). Ein Bewegungsmelder meldet
deshalb ein `on`-Feld, ohne ein Aktor zu sein. Die Oberfläche zeigt Schaltknöpfe
nur bei `on >= 0 && occ < 0`.

**Teilmeldungen dürfen nichts löschen.** Eine `motion`-Meldung trägt kein Modell
und keinen Hersteller. Übernimmt man ihre leeren Felder, verliert das Register
bei jeder Bewegung die Gerätebezeichnung. `h2_copy_field()` überspringt deshalb
leere Zeichenketten, und alle Zahlenfelder werden nur bei `>= 0` übernommen.

**Die Liste kommt gestückelt.** `list` kann mit `idx`/`total` in Teilen
eintreffen. Das Register wird deshalb pro Adresse aktualisiert und nie vorab
geleert — sonst wäre es mitten im Transfer leer.

**Entfernen wirkt nur bei wachem Gerät.** `remove` schickt eine
Leave-Aufforderung, `forget` wirft das Gerät aus der Tabelle des H2. Ein
offline- oder schlafendes Gerät bekommt die Aufforderung nie mit, behält den
Netzwerkschlüssel und taucht wieder auf, sobald es sich meldet. Die Oberfläche
sendet beides und sagt das im Bestätigungsdialog.

**Poll wirkt nicht bei Batteriegeräten.** `pollDevice()` im H2 bricht bei
`!canPoll` ab. Der Knopf steht trotzdem in jeder Zeile — er schadet nicht, und
der Statustext erklärt, warum bei einem PIR nichts passiert.

## HTTP-Schnittstelle

Alle Routen liegen unter `/api/h2/`. Schreibende Routen hängen hinter
`ensureConfigAuth()`, lesende nicht.

| Route | Methode | Auth | Zweck |
|---|---|---|---|
| `devices` | GET | – | Geräteregister als JSON |
| `status` | GET | – | letzter Koordinator-Zustand |
| `scan` | GET | – | Ergebnis des letzten Netzwerk-Scans |
| `scan` | POST | ✔ | Scan starten (`dur`, 1–5 s) |
| `refresh` | POST | – | `status` + `list` anfordern |
| `permit` | POST | ✔ | Pairing-Fenster (`seconds`, 0–254) |
| `remove` | POST | ✔ | `remove` + `forget` + lokal löschen |
| `switch` | POST | ✔ | `on`/`off`/`toggle` |
| `level` | POST | ✔ | Dimmwert 0–100 |
| `led`, `sensitivity` | POST | ✔ | herstellerspezifisch (Philips) |
| `poll` | POST | – | Attribute abfragen |
| `reboot` | POST | ✔ | Koordinator neu starten |
| `ota/status`, `ota/upload` | GET/POST | – | H2-Firmware flashen |

Die schreibenden Handler teilen sich `h2_param()` für Parameterprüfung und
`h2_dispatch()` für Einreihen und Antwort. Die Authentifizierung steht bewusst
als eigene Zeile in jedem Handler: `poll` und `refresh` sind absichtlich offen,
und diese Ausnahme in einem Helfer zu verstecken würde sie unsichtbar machen.

## Home Assistant

Zigbee-Geräte erscheinen als Entitäten **unter dem LibreLinkUp-Gerät**, nicht
als eigene Geräte. Je Gerät entstehen Bewegung (`binary_sensor`) und Batterie
(`sensor`), aber nur für das, was es tatsächlich meldet.

Zustand geht auf ein Topic **je Gerät**:

```
librelinkup/<client>/zigbee/0x29FE  →  {"occ":1,"bat":100}
```

**Bewegung wird sofort veröffentlicht**, direkt aus dem H2-Meldungshandler —
nicht im Publish-Zyklus, der nur minütlich läuft. Ein Melder mit einer Minute
Verzögerung taugt für keine Automatisierung.

**Discovery läuft nicht nur beim Verbinden.** Beim MQTT-Verbindungsaufbau ist
das Register noch leer, weil der H2 seine Liste erst Sekunden später schickt.
`mqtt_sync_zigbee_entities()` meldet deshalb bei jeder `list`-Antwort nach, was
noch keine Entität hat, und merkt sich das Angekündigte, um nicht bei jedem
„Reload List" den ganzen Satz erneut zu senden.

Beim Entfernen räumt `mqtt_remove_zigbee_device()` die Discovery-Topics mit
leeren Retain-Nachrichten weg — sonst bleiben Karteileichen in Home Assistant.
