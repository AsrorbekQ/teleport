#!/usr/bin/env python3
"""Convert an Anki .apkg export into the compact CrossPoint flashcard deck format.

The firmware cannot parse SQLite, so this runs on a computer once:

    python3 scripts/anki_to_deck.py GregMat_Vocabulary_List.apkg gre.deck

Then copy gre.deck to /apps/flashcards/ on the SD card.

Deck format (little-endian, version 1):
    header (16 bytes):
        char[4]  magic "CPFC"
        uint8    version (1)
        uint8    field count (5)
        uint16   card count
        uint32   offset table position (always 16)
        uint32   reserved (0)
    offset table: uint32 * card count, absolute file offset of each record
    record:
        uint8    set id (0 = no set / "Double Duty")
        uint16   payload length in bytes
        payload: field count NUL-terminated UTF-8 strings

Field order for the GregMat note type: word, definition 1, definition 2,
example, synonyms. Images and HTML are stripped; <br>/<div>/<hr> become
newlines. Records longer than MAX_PAYLOAD are truncated so the firmware can
read any card into a fixed buffer.
"""

import html
import json
import os
import re
import sqlite3
import struct
import sys
import tempfile
import zipfile

MAGIC = b"CPFC"
VERSION = 1
FIELD_COUNT = 5
MAX_PAYLOAD = 1536  # must match FlashcardDeck::MAX_PAYLOAD in the firmware

FIELD_NAMES = ["Word", "Definition 1", "Definition 2", "Example", "Synonym"]


def clean_html(text: str) -> str:
    text = re.sub(r"<img[^>]*>", "", text, flags=re.I)
    text = re.sub(r"<\s*(br|hr)\s*/?>", "\n", text, flags=re.I)
    text = re.sub(r"</?\s*(div|p)[^>]*>", "\n", text, flags=re.I)
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text)
    text = text.replace("\xa0", " ")
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r" *\n *", "\n", text)
    text = re.sub(r"\n{2,}", "\n", text)
    return text.strip().rstrip(",").strip()


def set_id_for_deck(name: str) -> int:
    m = re.search(r"Set\s+(\d+)", name)
    return int(m.group(1)) if m else 0


def open_collection(apkg_path: str):
    z = zipfile.ZipFile(apkg_path)
    names = z.namelist()
    if "collection.anki21b" in names:
        sys.exit("This .apkg uses the zstd-compressed format; export it from Anki "
                 "with 'Support older Anki versions' checked and retry.")
    db_name = "collection.anki21" if "collection.anki21" in names else "collection.anki2"
    tmp_dir = tempfile.mkdtemp()
    z.extract(db_name, tmp_dir)
    return sqlite3.connect(os.path.join(tmp_dir, db_name))


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    apkg_path, out_path = sys.argv[1], sys.argv[2]
    con = open_collection(apkg_path)

    models_json, decks_json = con.execute("select models, decks from col").fetchone()
    models = json.loads(models_json)
    decks = json.loads(decks_json)
    deck_sets = {int(k): set_id_for_deck(v["name"]) for k, v in decks.items()}

    field_index = {}
    for mid, model in models.items():
        names = [f["name"] for f in model["flds"]]
        field_index[int(mid)] = [names.index(n) if n in names else None for n in FIELD_NAMES]

    rows = con.execute(
        "select c.did, c.due, n.mid, n.flds from cards c join notes n on n.id = c.nid "
        "where c.ord = 0 order by c.due"
    ).fetchall()

    records = []
    truncated = 0
    for did, _due, mid, flds in rows:
        fields = flds.split("\x1f")
        idx = field_index[mid]
        values = [clean_html(fields[i]) if i is not None and i < len(fields) else "" for i in idx]
        if not values[0]:
            continue
        payload = b"".join(v.encode("utf-8") + b"\0" for v in values)
        if len(payload) > MAX_PAYLOAD:
            truncated += 1
            payload = payload[: MAX_PAYLOAD - FIELD_COUNT]
            # Ensure every field still terminates: drop a partial UTF-8 tail, pad NULs.
            while payload and (payload[-1] & 0xC0) == 0x80:
                payload = payload[:-1]
            missing = FIELD_COUNT - payload.count(b"\0")
            payload += b"\0" * max(missing, 1)
        records.append((deck_sets.get(did, 0), payload))

    records.sort(key=lambda r: (r[0] if r[0] else 999, 0))

    header_size = 16
    table_size = 4 * len(records)
    offset = header_size + table_size
    offsets = []
    for _set_id, payload in records:
        offsets.append(offset)
        offset += 3 + len(payload)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<4sBBHII", MAGIC, VERSION, FIELD_COUNT, len(records), header_size, 0))
        f.write(struct.pack("<%dI" % len(offsets), *offsets))
        for set_id, payload in records:
            f.write(struct.pack("<BH", set_id, len(payload)))
            f.write(payload)

    sizes = [len(p) for _s, p in records]
    print(f"cards: {len(records)}  file: {offset} bytes  "
          f"payload avg/max: {sum(sizes) // len(sizes)}/{max(sizes)}  truncated: {truncated}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
