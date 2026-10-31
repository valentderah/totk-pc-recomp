#!/usr/bin/env python3
"""
find_mains.py - search a folder tree for Switch NSO files named 'main'.

Usage:
  python tools\find_mains.py C:\Users\Chandler
  python tools\find_mains.py D:\ --name main

Prints path, file size, and the NSO build ID (module ID) for every match.
"""
import argparse
import os
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="folder to search, e.g. C:\\Users\\Chandler")
    ap.add_argument("--name", default="main", help="file name to look for (default: main)")
    a = ap.parse_args()

    found = 0
    for dirpath, _dirs, files in os.walk(a.root, onerror=lambda e: None):
        for f in files:
            if f.lower() != a.name.lower():
                continue
            p = os.path.join(dirpath, f)
            try:
                with open(p, "rb") as fh:
                    head = fh.read(0x60)
                if head[:4] != b"NSO0":
                    continue
                build_id = head[0x40:0x60].hex()
                size = os.path.getsize(p)
                text_size = struct.unpack_from("<I", head, 0x18)[0]
                print("%s\n   size=%d bytes  text=%#x  build_id=%s" % (p, size, text_size, build_id))
                found += 1
            except OSError:
                pass
    print("\nfound %d NSO file(s)" % found)


if __name__ == "__main__":
    main()
