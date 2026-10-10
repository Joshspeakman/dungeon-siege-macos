#!/usr/bin/env python3
"""Replace a black DS1 save portrait from a save with matching character appearance.

Writes a new file, never overwrites either input. Does not change gameplay resources.
Only the flat, zlib-compressed Tank layout used by DS1 saves is supported.
"""
import argparse
import json
import re
import struct
import zlib
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def u32(data, offset):
    require(0 <= offset <= len(data) - 4, "truncated integer")
    return struct.unpack_from("<I", data, offset)[0]


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


class Save:
    def __init__(self, data):
        self.data = data
        require(len(data) >= 88 and data[:8] == b"DSigTank", "not a DS1 Tank save")
        require(u32(data, 8) == 0x10002, "unsupported Tank version")
        self.dirs, self.fileset, indexsize, self.start = struct.unpack_from("<4I", data, 12)
        require(88 <= self.start <= self.dirs < self.fileset < len(data), "unsupported index layout")
        require(self.dirs + indexsize == len(data), "unsupported index extent")
        require(zlib.crc32(data[self.dirs:]) == u32(data, 80), "index checksum mismatch")
        count = u32(data, self.fileset)
        require(1 <= count <= 64 and self.fileset + 4 + 4 * count <= len(data), "invalid file count")
        self.files = {}
        for i in range(count):
            entry = self.fileset + u32(data, self.fileset + 4 + 4 * i)
            require(self.fileset + 4 + 4 * count <= entry <= len(data) - 30, "invalid entry offset")
            parent, size, offset, crc = struct.unpack_from("<4I", data, entry)
            fmt, flags, namelen = struct.unpack_from("<3H", data, entry + 24)
            require(parent == 0 and fmt == 1 and flags == 0, "only flat zlib save entries are supported")
            require(0 < size <= 128 * 1024 * 1024 and 0 < namelen < 256, "invalid entry size")
            meta = entry + ((30 + namelen + 1 + 3) & ~3)
            require(meta + 8 <= len(data) and data[entry + 30 + namelen] == 0, "truncated name")
            name = data[entry + 30:entry + 30 + namelen].decode("ascii")
            require(name not in self.files and "/" not in name and "\\" not in name, "invalid or duplicate resource name")
            packed, chunk = struct.unpack_from("<2I", data, meta)
            require(chunk > 0, "zero chunk size")
            chunks = (size + chunk - 1) // chunk
            require(meta + 8 + chunks * 16 <= len(data), "truncated chunk table")
            begin = self.start + offset
            require(self.start <= begin < self.dirs and begin + packed <= self.dirs, "invalid resource extent")
            decoded, end = bytearray(), 0
            for j in range(chunks):
                usz, csz, extra, coff = struct.unpack_from("<4I", data, meta + 8 + j * 16)
                require(0 <= extra < usz <= size - len(decoded) and coff == end and coff + csz + extra <= packed,
                        "invalid chunk extent")
                raw = data[begin + coff:begin + coff + csz]
                if csz < usz:
                    decoder = zlib.decompressobj()
                    plain = decoder.decompress(raw, usz - extra + 1)
                    require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                            "incomplete compressed chunk")
                else:
                    require(csz == usz and extra == 0, "invalid raw chunk")
                    plain = raw
                require(len(plain) == usz - extra, "decoded size mismatch")
                decoded.extend(plain)
                decoded.extend(data[begin + coff + csz:begin + coff + csz + extra])
                end = coff + csz + extra
            require(end == packed and len(decoded) == size, "incomplete resource")
            require(zlib.crc32(decoded) == crc, "resource checksum mismatch: " + name)
            self.files[name] = dict(entry=entry, meta=meta, begin=begin, end=begin + packed,
                                    chunks=chunks, content=bytes(decoded))
        self.order = sorted(self.files, key=lambda name: self.files[name]["begin"])
        crc, end = 0, self.start
        for name in self.order:
            f = self.files[name]
            require(f["begin"] >= end, "overlapping resources")
            end = f["end"]
            crc = zlib.crc32(f["content"], crc)
        require(crc == u32(data, 84), "data checksum mismatch")


def appearance(party, slot):
    """Parse the saved GAS hierarchy, without confusing inventory and member keys."""
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|\[[^\]]+\]|[{}=;]|[^\s{}=;]+', party.decode("ascii"))
    pos = 0

    def block(nested=False):
        nonlocal pos
        result = {}
        while pos < len(tokens):
            key = tokens[pos]
            pos += 1
            if key == "}":
                require(nested, "unexpected GAS close")
                return result
            if key.startswith("["):
                key = key[1:-1]
                require(pos < len(tokens) and tokens[pos] == "{", "invalid GAS block")
                pos += 1
                value = block(True)
            else:
                require(pos < len(tokens) and tokens[pos] == "=", "invalid GAS property")
                pos += 1
                values = []
                while pos < len(tokens) and tokens[pos] != ";":
                    require(tokens[pos] not in ("{", "}"), "unterminated GAS value")
                    values.append(tokens[pos])
                    pos += 1
                require(pos < len(tokens), "unterminated GAS property")
                pos += 1
                value = " ".join(values).strip('"')
            require(key not in result, "duplicate GAS key")
            result[key] = value
        require(not nested, "unclosed GAS block")
        return result

    try:
        member = block()["party"]["members"][str(slot)]["member"]
        identity = {k: member[k] for k in ("template_name", "custom_head", "skins")}
        require(isinstance(identity["skins"], dict) and identity["skins"], "missing appearance skins")
        return identity
    except (KeyError, TypeError) as exc:
        raise ValueError("missing party member appearance") from exc


def pixels(bmp):
    require(len(bmp) == 12342 and bmp[:2] == b"BM" and u32(bmp, 2) == len(bmp), "expected 64x64 BMP")
    require(u32(bmp, 10) == 54 and u32(bmp, 14) == 40 and struct.unpack_from("<iiHHI", bmp, 18)
            == (64, 64, 1, 24, 0), "expected uncompressed 24-bit 64x64 BMP")
    return bmp[54:]


def repair(target_data, reference_data, slot=0):
    target, reference = Save(target_data), Save(reference_data)
    name = "portrait-%d.bmp" % slot
    require(name in target.files and name in reference.files and "party.gas" in target.files
            and "party.gas" in reference.files, "missing portrait or party resource")
    identity = appearance(target.files["party.gas"]["content"], slot)
    require(identity == appearance(reference.files["party.gas"]["content"], slot), "character appearance differs")
    portrait = target.files[name]
    new = reference.files[name]["content"]
    require(not any(pixels(portrait["content"])), "target portrait is not entirely black")
    require(any(pixels(new)), "reference portrait is also black")
    meta = portrait["meta"]
    require(portrait["chunks"] == 1 and u32(target_data, meta + 16) == 0
            and u32(target_data, meta + 20) == 0, "unsupported portrait chunk layout")
    packed = zlib.compress(new)
    require(len(packed) < len(new), "reference portrait is not compressible")
    delta = len(packed) - (portrait["end"] - portrait["begin"])
    result = bytearray(target_data[:portrait["begin"]] + packed + target_data[portrait["end"]:])
    put32(result, 12, target.dirs + delta)
    put32(result, 16, target.fileset + delta)
    for resource in target.files.values():
        if resource["begin"] >= portrait["end"]:
            put32(result, resource["entry"] + delta + 8, resource["begin"] - target.start + delta)
    put32(result, portrait["entry"] + delta + 12, zlib.crc32(new))
    put32(result, meta + delta, len(packed))
    put32(result, meta + delta + 12, len(packed))
    put32(result, 80, zlib.crc32(result[target.dirs + delta:]))
    crc = 0
    for resource_name in target.order:
        crc = zlib.crc32(new if resource_name == name else target.files[resource_name]["content"], crc)
    put32(result, 84, crc)
    checked = Save(bytes(result))
    require(checked.files.keys() == target.files.keys(), "resource list changed")
    for resource_name, resource in target.files.items():
        expected = new if resource_name == name else resource["content"]
        require(checked.files[resource_name]["content"] == expected, "unexpected resource change")
    return bytes(result), dict(resource=name, appearance=identity, unchanged_resources=len(target.files) - 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--slot", type=int, default=0)
    args = parser.parse_args()
    try:
        result, report = repair(args.target.read_bytes(), args.reference.read_bytes(), args.slot)
        with args.output.open("xb") as output:
            output.write(result)
        print(json.dumps(report, indent=2))
    except (ValueError, OSError, zlib.error, struct.error) as exc:
        parser.exit(1, "Portrait repair refused: %s\n" % exc)


if __name__ == "__main__":
    main()
