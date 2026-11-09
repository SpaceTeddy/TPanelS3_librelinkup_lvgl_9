# Firmware-Signaturprüfung

Jedes Firmware-Update wird vor der Aktivierung gegen einen fest einkompilierten
öffentlichen Schlüssel geprüft. Ein Image ohne gültige Signatur wird verworfen,
das Gerät behält seinen Boot-Slot und läuft mit der alten Version weiter.

- Verfahren: **ECDSA P‑256** über das **SHA‑256** des rohen `firmware.bin`
- Signatur: 64 Byte roh (`r‖s`)
- Prüfung: im Gerät, **während** des Downloads/Uploads gehasht und **vor**
  `Update.end()` (das den neuen Slot bootbar macht) geprüft
- Code: `app/main/fw_signature.{h,cpp}`, Werkzeug: `tools/fw_sign.py`

Das ist eine Prüfung auf Anwendungsebene, **kein** ESP32 Secure Boot. Wer per
USB flasht, umgeht sie. Sie schützt vor manipulierten Updates über Netz,
Hosting (GitHub Pages) und Uploads über die Weboberfläche.

## Die zwei Schlüssel

Es gibt genau ein Schlüsselpaar. Die beiden Hälften liegen an völlig
verschiedenen Orten:

| | Privater Schlüssel | Öffentlicher Schlüssel |
|---|---|---|
| Datei | `fw_signing_key.pem` (PEM, PKCS#8) | `include/fw_pubkey.h` (65 Byte `FW_PUBKEY`) |
| Zweck | **signiert** Images | **prüft** Images, ist in die Firmware einkompiliert |
| Liegt | außerhalb des Repos, z. B. `~/.keys/`, plus Backup im Passwortmanager | im Repo, committet |
| Wird gebraucht von | CI (Secret `FW_SIGNING_KEY`), lokalem Signieren, `platformio_upload.py` | jedem Build |
| Darf öffentlich sein | **niemals** | ja |

Faustregeln:

- Ein Gerät akzeptiert nur Images, die mit dem privaten Schlüssel signiert
  sind, der zu **seinem** einkompilierten `fw_pubkey.h` gehört. Maßgeblich ist
  also das `fw_pubkey.h` der Firmware, die gerade **läuft**, nicht das im Repo.
- CI-Secret, lokale `.pem` und `include/fw_pubkey.h` müssen zusammenpassen.
  Prüfen geht mit `fw_sign.py checkkey` (siehe [Prüfen](#prüfen)).
- Fehlt `include/fw_pubkey.h`, bricht der Build mit
  `include/fw_pubkey.h missing: run ... keygen` ab. Ein Build ohne Schlüssel
  ist also nicht möglich.

## Python

`tools/fw_sign.py` braucht das Paket `cryptography`. Am einfachsten ist das
Python von PlatformIO, dort ist es schon installiert:

```bash
PY=~/.platformio/penv/bin/python
$PY tools/fw_sign.py --help
```

Ein systemweites `pip install cryptography` scheitert unter Homebrew mit
`externally-managed-environment` (PEP 668). Nicht mit
`--break-system-packages` umgehen. Wer ein anderes Python will, nimmt ein venv.

Alle Befehle unten setzen `PY` wie oben und laufen im Projektverzeichnis.

## Einrichtung (einmalig)

### 1. Schlüsselpaar erzeugen

```bash
mkdir -p ~/.keys && chmod 700 ~/.keys
$PY tools/fw_sign.py keygen --out ~/.keys/fw_signing_key.pem
```

- schreibt den privaten Schlüssel nach `~/.keys/fw_signing_key.pem` (Modus `600`)
- schreibt den öffentlichen Schlüssel nach `include/fw_pubkey.h`
- weigert sich, eine vorhandene `.pem` zu überschreiben (`--force` nur, wenn
  man wirklich einen neuen Schlüssel will, siehe [Schlüsselwechsel](#schlüssel-verloren-oder-wechseln))

Den privaten Schlüssel **nie** ins Repo, nach `Downloads/` oder in einen
synchronisierten Ordner legen.

### 2. Öffentlichen Schlüssel committen

```bash
git add include/fw_pubkey.h
git commit -m "Add firmware signing public key"
```

### 3. Privaten Schlüssel als GitHub-Secret hinterlegen

Beide Workflows (`release.yml`, `ota-pages.yml`) signieren mit dem Secret
**`FW_SIGNING_KEY`** und brechen ab, wenn es fehlt.

```bash
gh secret set FW_SIGNING_KEY < ~/.keys/fw_signing_key.pem
```

oder im Browser: Settings → Secrets and variables → Actions → New repository
secret, Name `FW_SIGNING_KEY`, als Wert den **kompletten** Inhalt der `.pem`
einschließlich der Zeilen `-----BEGIN PRIVATE KEY-----` und
`-----END PRIVATE KEY-----`.

### 4. Backup

Den Inhalt der `.pem` in den Passwortmanager legen. Aus dem GitHub-Secret lässt
sich der Schlüssel **nicht** zurücklesen. Ist er verloren, nehmen ausgelieferte
Geräte nur noch Firmware per USB an.

### 5. Kette prüfen

```bash
$PY tools/fw_sign.py checkkey --key ~/.keys/fw_signing_key.pem
# MATCH: key belongs to include/fw_pubkey.h
```

Nach dem ersten CI-Lauf zusätzlich prüfen, ob das **Secret** passt, siehe
[CI-Secret prüfen](#ci-secret-prüfen).

### 6. Lokalen Upload einrichten

In `platformio.ini` (lokal, Pfad nicht committen):

```ini
upload_protocol = custom
custom_upload_url = http://<geraete-ip>/update
custom_signing_key = ~/.keys/fw_signing_key.pem
```

Alternativ statt `custom_signing_key` die Umgebungsvariable `FW_SIGNING_KEY`
mit dem PEM-**Text** setzen (wie in der CI).

### Reihenfolge beim ersten Ausrollen

Geräte mit Firmware **vor** diesem Feature prüfen noch nicht. Sie nehmen das
erste signierte Release wie gewohnt an (das Manifest-Feld `signature` wird
ignoriert, ein angehängter Trailer stört nicht). **Ab dann** akzeptieren sie
nur noch Images, die zum einkompilierten Schlüssel passen.

Reihenfolge daher: Schlüssel erzeugen → `fw_pubkey.h` committen → Secret setzen
→ `checkkey` → Release bauen.

## Update-Wege

| Weg | Login nötig | Signatur liegt | Hochzuladende Datei |
|---|---|---|---|
| Automatisch / Button „Install“ (Manifest) | – | Feld `signature` (base64) in `manifest.json` | – (CI) |
| VS Code / `pio run -t upload` mit `upload_protocol = custom` | nein | Trailer, `platformio_upload.py` hängt ihn an | `firmware.bin` (wird signiert) |
| ElegantOTA-Seite `/update` | nein | Trailer am Image | `firmware.signed.bin` |
| Weboberfläche `/api/fw/upload` | ja (Config-Login) | Trailer am Image | `firmware.signed.bin` |
| Zigbee‑H2 Upload `/api/h2/ota/upload` | nein | Trailer am Image | `*.signed.bin` |
| USB (`upload_protocol` auskommentiert) | – | **keine Prüfung** | – |

`/update` und `/ota/upload` sind wie auf `main` ohne Login. Die Routen
`/ota/start` und `/ota/upload` überschreiben die von ElegantOTA, weil die
Bibliothek `Update.end()` selbst aufruft, bevor eine Signatur geprüft werden
könnte. Sie laufen in denselben prüfenden Handler wie `/api/fw/upload`.

Trailer-Format: `[Image][Signatur 64 B][Magic "FWS1" 4 B]`. Der Trailer wird nie
in den Flash geschrieben.

Nicht abgedeckt: `/api/fs/upload` (Datendateien, keine Firmware). Das
ElegantOTA-Dateisystem-Update (`mode=fs`) ist abgeschaltet, weil dort ein
Rohimage ungeprüft in die Partition ginge.

## Lokal bauen und einspielen

### Über VS Code / PlatformIO (OTA)

Mit der Einrichtung aus Schritt 6 genügt der normale Upload:

```bash
~/.platformio/penv/bin/pio run -e main -t upload
```

Erwartete Ausgabe:

```text
Signing image (ECDSA P-256) ...
Serverconfiguration: autentication not needed.
Uploading: 100.00% ...
Upload successful
```

Ohne `custom_signing_key` und ohne `FW_SIGNING_KEY` erscheint
`WARNING: no signing key ... uploading UNSIGNED image`, das Gerät lehnt ab.

### Über die Weboberfläche

```bash
~/.platformio/penv/bin/pio run -e main
$PY tools/fw_sign.py sign .pio/build/main/firmware.bin \
    --key ~/.keys/fw_signing_key.pem --signed-out .pio/build/main/firmware.signed.bin
```

Die ausgegebene Zeile ist die base64-Signatur (nur für ein eigenes Manifest
nötig). `firmware.signed.bin` (68 Byte größer als `firmware.bin`) über
`/update` oder die Weboberfläche hochladen.

Aus einem GitHub-Release: **`firmware-main.signed.bin`** nehmen, nicht
`firmware-main.bin`.

### Zigbee-H2

Gleicher Schlüssel, gleiches Werkzeug, auf das `firmware.bin` des H2-Projekts:

```bash
$PY tools/fw_sign.py sign <h2-projekt>/.pio/build/<env>/firmware.bin \
    --key ~/.keys/fw_signing_key.pem --signed-out h2.signed.bin
```

Hochladen über `/api/h2/ota/upload`. Das Image wird komplett ins PSRAM
geladen und erst nach erfolgreicher Prüfung an den H2 übertragen.

`h2_ota_start(url)` (Update von einer URL) prüft genauso, braucht also eine URL
auf die `.signed.bin` (max. 1,5 MB). Derzeit ruft aber keine Route und kein
Konsolenbefehl diese Funktion auf.

### Per USB

`upload_protocol = custom` auskommentieren, dann `pio run -e main -t upload`.
Keine Signaturprüfung. Das ist der Notweg, wenn OTA nicht mehr geht, etwa
nach Schlüsselverlust.

## Prüfen

Alle Prüfungen laufen offline am Rechner.

| Frage | Befehl | Gut |
|---|---|---|
| Passt meine `.pem` zur Firmware? | `$PY tools/fw_sign.py checkkey --key ~/.keys/fw_signing_key.pem` | `MATCH` |
| Ist diese `.signed.bin` gültig? | `$PY tools/fw_sign.py verify firmware.signed.bin --header include/fw_pubkey.h` | `OK` |
| Passt Image + base64-Signatur? | `$PY tools/fw_sign.py verify firmware.bin <sig> --header include/fw_pubkey.h` | `OK` |

`--header` prüft gegen genau den Schlüssel, den die Firmware einkompiliert.
`--key` prüft nur gegen den aus der `.pem` abgeleiteten Schlüssel und sagt
deshalb nichts darüber, ob das Gerät das Image annimmt.

Exit-Code `0` bei Erfolg, `1` bei `MISMATCH`/`INVALID`, also auch in Skripten
verwendbar.

### CI-Secret prüfen

Der Inhalt des Secrets ist nicht einsehbar. Prüfen lässt sich, ob das, was die
CI signiert hat, zum Repo-Schlüssel passt:

```bash
BASE=https://spaceteddy.github.io/TPanelS3_librelinkup_lvgl_9/ota
mkdir -p /tmp/fwcheck
curl -s -o /tmp/fwcheck/firmware.bin  $BASE/firmware.bin
SIG=$(curl -s $BASE/manifest.json | $PY -c 'import json,sys; print(json.load(sys.stdin)["signature"])')
$PY tools/fw_sign.py verify /tmp/fwcheck/firmware.bin "$SIG" --header include/fw_pubkey.h
```

Für den Staging-Kanal `$BASE/staging/...`. `INVALID` heißt: Secret und
`fw_pubkey.h` gehören nicht zusammen, Geräte würden dieses Release ablehnen.

### Am Gerät testen

| Test | Erwartet |
|---|---|
| `firmware.signed.bin` hochladen | Update, Neustart mit neuer Version |
| unsignierte `firmware.bin` über `/update` | Abgelehnt, Log `[SIG] signature trailer missing` |
| manipulierte `.signed.bin` (Befehl unten) | Abgelehnt, Log `[SIG] verify failed (mbedtls -0x…)` |
| mit fremdem Schlüssel signiert | Abgelehnt, `verify failed` |

```bash
cp .pio/build/main/firmware.signed.bin /tmp/fwcheck/tampered.bin
printf 'x' | dd of=/tmp/fwcheck/tampered.bin bs=1 seek=100000 conv=notrunc
```

Nach jeder Ablehnung muss das Gerät mit der alten Version weiterlaufen.

## Entwicklung ohne Schlüssel

`-DFW_SIGNATURE_ENFORCE=0` in `build_flags` schaltet die Prüfung ab: Unsignierte
Images werden mit `[SIG] verification disabled` angenommen. Das gilt erst für
Updates **auf** eine solche Firmware. Ein Gerät, auf dem eine prüfende Firmware
läuft, nimmt auch eine `ENFORCE=0`-Firmware nur signiert an. Niemals in
Release-Builds.

## Schlüssel verloren oder wechseln

**Schlüssel verloren:** Kein OTA-Weg mehr möglich, auch nicht mit
`ENFORCE=0`, denn die laufende Firmware prüft gegen den alten Schlüssel.
Vorgehen:

1. Neues Paar erzeugen: `keygen --out ~/.keys/fw_signing_key.pem --force`
2. `include/fw_pubkey.h` committen, Secret `FW_SIGNING_KEY` ersetzen, Backup
3. Jedes Gerät **einmal per USB** flashen

**Geplanter Wechsel oder Verdacht auf Kompromittierung, alter Schlüssel noch
vorhanden:** Die Geräte lassen sich ohne USB umstellen, indem die Firmware mit
dem **neuen** öffentlichen Schlüssel ein letztes Mal mit dem **alten** privaten
Schlüssel signiert wird:

1. Neues Paar mit **anderem** Dateinamen erzeugen:
   `keygen --out ~/.keys/fw_signing_key_new.pem --header include/fw_pubkey.h --force`
2. Bauen (die Firmware enthält jetzt den neuen öffentlichen Schlüssel)
3. Mit dem **alten** Schlüssel signieren und auf alle Geräte bringen
   (`sign ... --key ~/.keys/fw_signing_key.pem --signed-out ...`)
4. Ab jetzt nur noch den neuen Schlüssel: Secret ersetzen,
   `custom_signing_key` umstellen, `checkkey` mit der neuen `.pem`
5. Alten Schlüssel vernichten, sobald kein Gerät mehr die alte Firmware hat

Achtung bei Schritt 3 über das Manifest: Die CI signiert mit dem Secret. Dafür
muss das Secret für diesen einen Lauf noch den **alten** Schlüssel enthalten.

## Log-Meldungen und Fehlerbilder

| Meldung | Bedeutung |
|---|---|
| `[SIG] verify failed (mbedtls -0x…)` | Signatur passt nicht zum Image bzw. zum einkompilierten Schlüssel |
| `[SIG] signature trailer missing` | unsignierte Datei hochgeladen statt der `.signed.bin` |
| `[SIG] malformed signature` | Manifest-Signatur kein gültiges base64 mit 64 Byte |
| `[SIG] verification disabled` | Firmware mit `FW_SIGNATURE_ENFORCE=0` gebaut |
| `manifest not signed` | Manifest enthält kein `signature`-Feld |
| `install failed: signature invalid` (Display) | Manifest-Update abgelehnt |
| Upload-Antwort `{"status":"failed","error":"signature check failed ..."}` | wie `trailer missing` / `verify failed` |
| Upload-Antwort `{"status":"failed","error":""}` an `/api/fw/upload` | Config-Login fehlt oder falsch |
| `no private key: pass --key FILE ...` (Werkzeug) | `--key` leer, z. B. weil eine Shell-Variable nicht gesetzt ist |

## Bekannte Grenzen

- **Kein Downgrade-Schutz:** Die Signatur bindet nur das Image, nicht die
  Version. Ein Angreifer mit Zugriff auf das Hosting könnte ein altes, signiertes
  Release samt Manifest wieder einspielen. Abhilfe wäre, die Version in
  `esp_app_desc_t` des Images gegen die laufende Version zu prüfen.
- **Ein Schlüssel:** Die Firmware kennt genau einen öffentlichen Schlüssel. Ein
  Wechsel läuft über den Umweg oben.
- `/update` und `/ota/upload` sind ohne Login. Die Signatur verhindert fremde
  Firmware, nicht aber, dass jemand im LAN ein **altes signiertes** Release
  einspielt (siehe Downgrade).
