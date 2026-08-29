#!/usr/bin/env python3
"""Zeigt, welche Bibliothek wie viel *internes* DRAM statisch belegt.

Hintergrund: Core 3.x (pioarduino/IDF 5.5) hat rund 74 KB weniger freies
internes RAM als Core 2.x. Um zu klaeren, ob so ein Unterschied statisch
(.bss/.data im Image) oder erst zur Laufzeit entsteht, reicht `esp_status`
nicht -- das meldet nur die Summe. Dieses Skript liest die Link-Map und
schluesselt den statischen Anteil nach Archiv auf.

    python3 tools/dram_usage.py                       # aktueller Build
    python3 tools/dram_usage.py a.map --compare b.map  # zwei Builds vergleichen

Faellt der statische Anteil in beiden Builds aehnlich aus, sitzt der
Unterschied in Laufzeit-Allokationen (WiFi/lwIP, mbedTLS) -- siehe den
Kommentarblock zu CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP in platformio.ini.

Gezaehlt wird nur, was im internen DRAM-Fenster des ESP32-S3 landet
(0x3FC88000..0x3FD00000). Alles in PSRAM (0x3C000000..) oder Flash bleibt
aussen vor -- genau darum geht es ja.
"""

import argparse
import collections
import os
import re
import sys

# Internes SRAM des ESP32-S3, wie es der Linker fuer Daten vergibt.
DRAM_START = 0x3FC80000
DRAM_END = 0x3FD00000

DEFAULT_MAP = os.path.join(".pio", "build", "main", "firmware.map")

# Eine Eingabezeile der Map, z.B.:
#  .bss.foo       0x3fc9a1b0       0x28 .pio/build/main/liblvgl.a(lv_obj.c.o)
# Der Dateiname steht je nach Symbollaenge auf derselben oder der naechsten
# Zeile, deshalb das optionale \n im Muster.
ENTRY = re.compile(
    r"^ (\.(?:bss|data)[\w.$]*)\s*\n?\s*0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)",
    re.MULTILINE,
)


def per_archive(map_path):
    """Bytes internes DRAM je Archiv/Objektdatei aus einer Link-Map."""
    try:
        with open(map_path, errors="ignore") as handle:
            text = handle.read()
    except OSError as error:
        sys.exit(f"Map nicht lesbar: {error}")

    totals = collections.Counter()
    for match in ENTRY.finditer(text):
        address = int(match.group(2), 16)
        size = int(match.group(3), 16)
        if not DRAM_START <= address < DRAM_END or size == 0:
            continue
        # ".../liblvgl.a(lv_obj.c.o)" -> "liblvgl.a", "src/main.cpp.o" -> "main.cpp.o"
        name = os.path.basename(match.group(4)).split("(")[0]
        totals[name] += size
    return totals


def print_single(totals, limit):
    for name, size in totals.most_common(limit):
        print(f"{size:>9,}  {name}")
    print(f"{sum(totals.values()):>9,}  GESAMT")


def print_compare(left, right, left_name, right_name, limit):
    print(f"{'delta':>9}  {'links':>9}  {'rechts':>9}  Archiv")
    print(f"{'':>9}  {left_name:>9.9}  {right_name:>9.9}")
    keys = set(left) | set(right)
    for name in sorted(keys, key=lambda k: abs(right[k] - left[k]), reverse=True)[:limit]:
        delta = right[name] - left[name]
        if delta == 0:
            continue
        print(f"{delta:>+9,}  {left[name]:>9,}  {right[name]:>9,}  {name}")
    total = sum(right.values()) - sum(left.values())
    print(f"{total:>+9,}  {sum(left.values()):>9,}  {sum(right.values()):>9,}  GESAMT")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("map", nargs="?", default=DEFAULT_MAP,
                        help=f"Link-Map (Standard: {DEFAULT_MAP})")
    parser.add_argument("--compare", metavar="MAP",
                        help="zweite Link-Map; zeigt die Differenz je Archiv")
    parser.add_argument("--limit", type=int, default=25,
                        help="Anzahl Zeilen (Standard: 25)")
    args = parser.parse_args()

    totals = per_archive(args.map)
    if not totals:
        sys.exit(f"Keine DRAM-Eintraege in {args.map} gefunden -- richtige Map? "
                 f"Erwartet werden Adressen in {DRAM_START:#x}..{DRAM_END:#x}.")

    if args.compare:
        print_compare(totals, per_archive(args.compare),
                      os.path.basename(args.map), os.path.basename(args.compare),
                      args.limit)
    else:
        print_single(totals, args.limit)


if __name__ == "__main__":
    main()
