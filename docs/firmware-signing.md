# Firmware-Signaturprüfung

Jedes Firmware-Update wird vor der Aktivierung gegen einen fest einkompilierten
öffentlichen Schlüssel geprüft. Ein Image ohne gültige Signatur wird verworfen,
das laufende Gerät behält seinen Boot-Slot.

- Verfahren: **ECDSA P‑256** über das **SHA‑256** des rohen `firmware.bin`
- Signatur: 64 Byte roh (`r‖s`)
- Prüfung: im Gerät, **während** des Downloads/Uploads gehasht und **vor**
  `Update.end()` (das den neuen Slot bootbar macht) geprüft
- Code: `app/main/fw_signature.{h,cpp}`, Tool: `tools/fw_sign.py`

Das ist eine Prüfung auf Anwendungsebene, **kein** ESP32 Secure Boot. Wer den
Flash physisch beschreibt, umgeht sie. Sie schützt vor manipulierten Updates
über Netz, Hosting (GitHub Pages) und manuelle Uploads.

## Update-Wege

| Weg | Wo die Signatur liegt | Datei |
|---|---|---|
| Automatisch / Button „Install“ (Manifest) | Feld `signature` (base64) in `manifest.json` | `firmware.bin` |
| Weboberfläche `/api/fw/upload` | Trailer am Image | `firmware-main.signed.bin` |
| ElegantOTA `/update` | Trailer am Image | `firmware-main.signed.bin` |
| Zigbee‑H2 Upload / URL | Trailer am Image | `*.signed.bin` |

Trailer-Format: `[Image][Signatur 64 B][Magic "FWS1" 4 B]`. Der Trailer wird nie
in den Flash geschrieben.

Nicht abgedeckt: `/api/fs/upload` (Datendateien, keine Firmware). Das
ElegantOTA‑Dateisystem‑Update (`mode=fs`) ist abgeschaltet, weil dort ein
Rohimage ungeprüft in die Partition ginge.

## Einmalige Einrichtung

```bash
pip install cryptography
python3 tools/fw_sign.py keygen --out ~/fw_signing_key.pem   # Pfad AUSSERHALB des Repos
```

1. Erzeugt den privaten Schlüssel (`chmod 600`) und `include/fw_pubkey.h`.
2. `include/fw_pubkey.h` committen. Ohne die Datei bricht der Build mit einer
   klaren Fehlermeldung ab.
3. Inhalt von `~/fw_signing_key.pem` als GitHub‑Secret **`FW_SIGNING_KEY`**
   anlegen (Settings → Secrets and variables → Actions). Danach die Datei
   sicher wegsperren (Passwortmanager) – geht der Schlüssel verloren, können
   ausgelieferte Geräte nur noch per USB neu geflasht werden.

Die CI bricht ab, wenn das Secret fehlt.

### Erstes Update auf Signaturprüfung

Geräte mit alter Firmware prüfen noch nicht. Das erste Release mit diesem
Feature kann sie also noch normal erreichen (Manifest ignoriert das neue Feld
bzw. ein angehängter Trailer stört den Flash nicht). **Ab dann** akzeptiert das
Gerät nur noch signierte Images. Reihenfolge: Schlüssel erzeugen → Secret
setzen → Release bauen.

## Ablauf aus Sicht des Anwenders

**Automatisches Update:** nichts zu tun. Die CI signiert, schreibt die Signatur
ins Manifest; das Gerät lädt, prüft, installiert. Bei ungültiger Signatur
erscheint auf dem Update‑Bildschirm `install failed: signature invalid`, der
Status in `/api/fw/status` ist `error`, das Gerät läuft mit der alten Version
weiter.

**Manuelles Update:** statt `firmware-main.bin` die Datei
**`firmware-main.signed.bin`** aus dem GitHub‑Release hochladen. Eine
unsignierte `.bin` wird abgelehnt (`signature trailer missing`).

**Lokal gebaute Firmware testen:**

```bash
python3 tools/fw_sign.py sign .pio/build/main/firmware.bin \
    --key ~/fw_signing_key.pem --signed-out /tmp/firmware.signed.bin
```

Gibt die base64‑Signatur aus (für ein Test‑Manifest) und schreibt die
`.signed.bin` für den Upload.

**Entwicklung ohne Schlüssel:** `-DFW_SIGNATURE_ENFORCE=0` in `build_flags`
schaltet die Prüfung ab (unsignierte Images werden mit Warnung akzeptiert).
Niemals in Release‑Builds.

## Log-Meldungen

| Meldung | Bedeutung |
|---|---|
| `[SIG] verify failed (mbedtls -0x…)` | Signatur passt nicht zum Image / Schlüssel |
| `[SIG] signature trailer missing` | Upload war nicht die `.signed.bin` |
| `[SIG] malformed signature` | Manifest‑Signatur kein gültiges base64 mit 64 Byte |
| `manifest not signed` | Manifest enthält kein `signature`‑Feld |

## Bekannte Grenzen

- **Kein Downgrade‑Schutz:** Die Signatur bindet nur das Image, nicht die
  Version. Ein Angreifer mit Zugriff auf Hosting könnte ein altes, signiertes
  Release samt Manifest wieder einspielen. Abhilfe wäre, die Version in
  `esp_app_desc_t` des Images gegen die laufende Version zu prüfen.
- **Kein Schlüsselwechsel:** Es gibt einen Schlüssel. Rotation heißt: neues
  Release, das beide Schlüssel kennt – derzeit nicht implementiert.
- `/update` (ElegantOTA) war und ist ohne Passwortschutz, sofern keine
  Config‑Authentifizierung gesetzt ist; die neue Route nutzt `ensureConfigAuth`.
