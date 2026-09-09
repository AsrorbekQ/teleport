#!/usr/bin/env python3
"""Edit Teleport habit data from a computer.

The device stores habits in /apps/habits/habits.bin (see src/activities/habits/HabitStore.h).
This tool converts that file to and from a plain text file you can edit by hand:

    # export the device file to text, edit it, then import it back
    python3 scripts/habits_tool.py export "/Volumes/NO NAME/apps/habits/habits.bin" habits.txt
    python3 scripts/habits_tool.py import habits.txt "/Volumes/NO NAME/apps/habits/habits.bin"

Text format: one habit per line, "name: dates". Dates are YYYY-MM-DD, separated by
commas; "a..b" marks every day from a to b inclusive. A habit with no dates is fine.

    Workout: 2026-09-01, 2026-09-03..2026-09-07
    Read 20 pages: 2026-09-05
    Meditate:

Lines starting with # are ignored. Names may not contain ": ". Up to 12 habits, 31 chars each.
The device keeps 400 days of history; older dates are dropped on import with a warning.
"""

import datetime as dt
import os
import struct
import sys

MAGIC = b"CPHB"
VERSION = 1
MAX_HABITS = 12
NAME_LENGTH = 32
WINDOW_DAYS = 400
DAY_BYTES = WINDOW_DAYS // 8
HEADER = struct.Struct("<4sBBHI")
EPOCH = dt.date(1970, 1, 1).toordinal()


def day_index(date: dt.date) -> int:
    return date.toordinal() - EPOCH


def date_of(day: int) -> dt.date:
    return dt.date.fromordinal(day + EPOCH)


def read_bin(path: str):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, count, _reserved, window_start = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        sys.exit(f"{path}: not a Teleport habits file")
    habits = []
    offset = HEADER.size
    for _ in range(count):
        name = data[offset : offset + NAME_LENGTH].split(b"\0", 1)[0].decode("utf-8", "replace")
        bits = data[offset + NAME_LENGTH : offset + NAME_LENGTH + DAY_BYTES]
        days = {window_start + i for i in range(WINDOW_DAYS) if (bits[i // 8] >> (i % 8)) & 1}
        habits.append((name, days))
        offset += NAME_LENGTH + DAY_BYTES
    return habits


def write_bin(path: str, habits, today: dt.date):
    all_days = [d for _n, days in habits for d in days]
    today_index = day_index(today)
    window_start = min(all_days + [today_index]) if habits else 0
    # Leave the device 30 days before it has to shift the window itself.
    earliest_allowed = today_index - (WINDOW_DAYS - 31)
    if window_start < earliest_allowed:
        dropped = sum(1 for d in all_days if d < earliest_allowed)
        print(f"warning: {dropped} check-ins older than {date_of(earliest_allowed)} dropped (400-day limit)")
        window_start = earliest_allowed

    out = bytearray(HEADER.pack(MAGIC, VERSION, len(habits), 0, window_start))
    for name, days in habits:
        encoded = name.encode("utf-8")[: NAME_LENGTH - 1]
        out += encoded.ljust(NAME_LENGTH, b"\0")
        bits = bytearray(DAY_BYTES)
        for d in days:
            i = d - window_start
            if 0 <= i < WINDOW_DAYS:
                bits[i // 8] |= 1 << (i % 8)
        out += bits
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)


def parse_date(text: str) -> int:
    return day_index(dt.date.fromisoformat(text.strip()))


def read_text(path: str):
    habits = []
    with open(path, encoding="utf-8") as f:
        for line_no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                sys.exit(f"{path}:{line_no}: expected 'name: dates'")
            name, _sep, dates = line.partition(":")
            name = name.strip()
            if not name:
                sys.exit(f"{path}:{line_no}: empty habit name")
            days = set()
            for token in dates.split(","):
                token = token.strip()
                if not token:
                    continue
                try:
                    if ".." in token:
                        start, end = token.split("..", 1)
                        a, b = parse_date(start), parse_date(end)
                        if b < a:
                            sys.exit(f"{path}:{line_no}: range {token} ends before it starts")
                        days.update(range(a, b + 1))
                    else:
                        days.add(parse_date(token))
                except ValueError:
                    sys.exit(f"{path}:{line_no}: bad date '{token}', expected YYYY-MM-DD")
            habits.append((name, days))
    if len(habits) > MAX_HABITS:
        sys.exit(f"{path}: {len(habits)} habits, the device supports at most {MAX_HABITS}")
    return habits


def format_days(days) -> str:
    ordered = sorted(days)
    parts = []
    i = 0
    while i < len(ordered):
        j = i
        while j + 1 < len(ordered) and ordered[j + 1] == ordered[j] + 1:
            j += 1
        if j - i >= 2:
            parts.append(f"{date_of(ordered[i])}..{date_of(ordered[j])}")
        else:
            parts.extend(str(date_of(d)) for d in ordered[i : j + 1])
        i = j + 1
    return ", ".join(parts)


def write_text(path: str, habits):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Teleport habits. One per line: name: YYYY-MM-DD, YYYY-MM-DD..YYYY-MM-DD\n")
        for name, days in habits:
            f.write(f"{name}: {format_days(days)}\n")


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in ("export", "import"):
        print(__doc__)
        return 1
    command, src, dst = sys.argv[1:]
    if command == "export":
        habits = read_bin(src)
        write_text(dst, habits)
    else:
        habits = read_text(src)
        write_bin(dst, habits, dt.date.today())
    total = sum(len(days) for _n, days in habits)
    print(f"{command}ed {len(habits)} habits, {total} check-ins -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
