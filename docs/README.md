# Dokumentation

Firmware für das LilyGO T-Panel S3: Glukoseanzeige aus der LibreLinkUp-API mit
Weboberfläche, MQTT-Verteilung, optionalem WireGuard-Tunnel und OTA-Update.

| Dokument | Inhalt |
|---|---|
| [architecture.md](architecture.md) | Task-Aufteilung, Zustandsmaschine, Datenfluss, Module |
| [build-and-flash.md](build-and-flash.md) | Plattformwahl, `custom_sdkconfig`, Skripte, Upload-Wege |
| [configuration.md](configuration.md) | `/config.json`, Konsole, Weboberfläche, MQTT |
| [pitfalls.md](pitfalls.md) | **Feinheiten und Stolperfallen** |

## Wo anfangen

- **Etwas funktioniert nicht wie erwartet** → [pitfalls.md](pitfalls.md). Die
  meisten Überraschungen dieses Projekts stehen dort, mit Symptom und Ursache.
- **Erster Kontakt mit dem Code** → [architecture.md](architecture.md),
  besonders die Task-Aufteilung: sie erklärt, warum die Telnet-Konsole
  erreichbar bleibt, während die Anzeige steht.
- **Bauen oder flashen** → [build-and-flash.md](build-and-flash.md). Vor allem
  der Abschnitt zu `extra_scripts` — die Zeile auszukommentieren hat
  Nebenwirkungen, die man nicht erwartet.

## Zwei Cores

Die Firmware baut auf Arduino Core 2.x **und** 3.x. Alle betroffenen Stellen
sind mit `ESP_ARDUINO_VERSION` bzw. `ESP_IDF_VERSION` abgesichert; Toolchain-
Flags werden vor dem Setzen auf Verfügbarkeit geprüft. Diese Eigenschaft ist
bewusst hergestellt — sie macht einen Plattformwechsel zu einer auskommentierten
Zeile statt zu einem Projekt.

## Hardware

Die Board-Dokumentation von LilyGO (Pinout, Varianten, Bezugsquellen) steht
unverändert in der [README.md](../README.md) im Wurzelverzeichnis.
