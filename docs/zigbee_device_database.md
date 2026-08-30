# Zigbee device database

The H2 coordinator speaks ZCL but carries no per-device knowledge — there is no
room for it beside the Zigbee stack. When a device joins and reports its
manufacturer and model, the H2 asks the S3 for that device's profile, and the
S3 answers from a database in LittleFS.

So supporting a new Zigbee device is a **database refresh plus a filesystem
upload** — neither chip needs a firmware build.

## Where the database comes from

It is generated in the **coordinator repo**, not here:

```
../TPanelH2_Zigbee_Coordinator/tools/update_db.sh            # pinned versions
../TPanelH2_Zigbee_Coordinator/tools/update_db.sh --latest   # upgrade first
```

That imports [ZHA's quirks](https://github.com/zigpy/zha-device-handlers)
(Apache-2.0) and packs them into two files. Copy them into this repo's LittleFS
image and upload:

```sh
cp ../TPanelH2_Zigbee_Coordinator/data/littlefs/zha_{idx,db}.bin data/
pio run --target uploadfs
```

`data/zha_idx.bin` is ~18 KB, `data/zha_db.bin` ~350 KB, against a 9.2 MB
LittleFS partition — the database is not a space concern.

Devices ZHA does not know go in `data/local_devices.json` **in the coordinator
repo**, which survives a refresh. See `tools/README.md` there.

## How the lookup works

`app/main/zha_db.cpp`. The records are stored pre-rendered as the exact JSON
the H2 expects, so answering needs no JSON parsing and no large buffer:

1. `fnv1a64(manufacturer + '\0' + model)`
2. binary search `zha_idx.bin` — ~11 seeks in an 18 KB file
3. verify the record really names this device, guarding against a hash collision
4. stream the bytes to the UART between a fixed prefix and `}\n`

If nothing matches, or the database is missing entirely, the S3 answers
`found:false` immediately. The H2 then uses its own built-in heuristics, which
is what it did before the database existed — a missing or unresponsive S3
degrades the coordinator, it does not stall it.

## Wire protocol

The H2 asks, **unsolicited** — this is the one message where the S3 is the
responder rather than the initiator:

```json
{"type":"profile_req","rid":42,"addr":6699,
 "manufacturerName":"_TZE200_ztc6ggyl","modelId":"TS0601"}
```

The S3 answers on the same `rid`:

```json
{"cmd":"profile","rid":42,"found":true,"p":{"m":"...","d":"...","setup":[...]}}
{"cmd":"profile","rid":42,"found":false}
```

The H2 retries twice at 2 s intervals, then gives up on the database for that
device. Answers arriving after that are ignored — the `rid` no longer matches.

`zha_db_answer()` writes the UART directly, so it must run on the task that
owns it. It is called from `h2_handle_message()`, which already satisfies that.

## Checking it works

Serial log on boot:

```
[ZHA] device database ready: 1170 devices, 349651 B
```

`{"cmd":"zha"}` to the H2 lists per device whether it resolved:

```json
{"type":"zha","addr":6699,"manufacturerName":"_TZE200_ztc6ggyl",
 "modelId":"TS0601","state":"matched","actions":5,"attrs":2,"dps":3}
```

`state` is `matched` (database), `unknown` (not found or no answer), `waiting`
or `idle`. The device list from `{"cmd":"list"}` carries the same under `zha`.

To check a device against the database from a workstation, without hardware:

```sh
cd ../TPanelH2_Zigbee_Coordinator
python3 tools/zha_lookup.py --wire _TZE200_ztc6ggyl TS0601
```

That prints the exact line the S3 should produce.

## Version coupling

`zha_db.cpp` checks the index schema and refuses a mismatch rather than
misreading it, so a stale upload is a clear log line and not a subtle bug. If
the schema in the coordinator repo's `tools/zha_pack.py` ever changes, bump
`kSchema` here to match.
