#!/usr/bin/env python3
# Rewrites the publisher field of a generated PPSSPP.nacp to UTF-8.
#
# Why this exists: nx_generate_nacp passes the author to nacptool as a command line argument, and
# on Windows that argument goes through the system ANSI codepage. A UTF-8 author name therefore
# arrives as codepage bytes (GBK on a Chinese Windows) and the console shows mojibake under the app
# icon. nacptool itself writes whatever bytes it is given, so the fix is to write the UTF-8 bytes
# ourselves afterwards.
#
# NACP layout: 16 language slots, each 0x300 bytes = name (0x200) + publisher (0x100), followed by
# the version string at 0x3060. Only the publisher slots are touched here; the name and version come
# from nacptool and are ASCII.

import sys

AUTHOR_UTF8 = "龙魂猪猪".encode("utf-8")

SLOT_COUNT = 16
SLOT_SIZE = 0x300
PUBLISHER_OFFSET = 0x200
PUBLISHER_SIZE = 0x100


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_nacp_author.py <PPSSPP.nacp>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < SLOT_COUNT * SLOT_SIZE:
        print("unexpected nacp size %d" % len(data), file=sys.stderr)
        return 1
    if len(AUTHOR_UTF8) + 1 > PUBLISHER_SIZE:
        print("author name too long for the nacp field", file=sys.stderr)
        return 1

    for slot in range(SLOT_COUNT):
        start = slot * SLOT_SIZE + PUBLISHER_OFFSET
        data[start:start + PUBLISHER_SIZE] = AUTHOR_UTF8 + b"\0" * (PUBLISHER_SIZE - len(AUTHOR_UTF8))

    with open(path, "wb") as f:
        f.write(data)

    print("nacp publisher patched to UTF-8 '%s'" % AUTHOR_UTF8.decode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
