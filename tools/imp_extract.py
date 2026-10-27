#!/usr/bin/env python3
"""Extract Steel Horizon's GAMEDATA.IMP using its GAMEDATA.XML index.

Usage: python3 -I tools/imp_extract.py GAMEDATA.XML GAMEDATA.IMP OUTDIR

Every entry in the index is Compression="none", so each file is the byte range
[Offset, Offset + CompressedSize) of the IMP. Exits non-zero if any entry is
out of range, compressed, or has CompressedSize != FullSize.
"""
import os
import sys
import xml.etree.ElementTree as ET


def entries(xml_path):
    root = ET.parse(xml_path).getroot()
    for f in root.iter("File"):
        yield {
            "path": f.get("Path"),
            "offset": int(f.get("Offset")),
            "size": int(f.get("CompressedSize")),
            "full": int(f.get("FullSize")),
            "compression": f.get("Compression"),
        }


def main(xml_path, imp_path, out_dir):
    imp_size = os.path.getsize(imp_path)
    errors = 0
    count = 0
    with open(imp_path, "rb") as imp:
        for e in entries(xml_path):
            if e["compression"] != "none" or e["size"] != e["full"]:
                print(f"unsupported entry: {e}", file=sys.stderr)
                errors += 1
                continue
            if e["offset"] + e["size"] > imp_size:
                print(f"out of range: {e}", file=sys.stderr)
                errors += 1
                continue
            rel = e["path"].lstrip("/")
            if ".." in rel.split("/"):
                print(f"unsafe path: {e['path']}", file=sys.stderr)
                errors += 1
                continue
            dest = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            imp.seek(e["offset"])
            data = imp.read(e["size"])
            with open(dest, "wb") as out:
                out.write(data)
            count += 1
    print(f"extracted {count} files, {errors} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
