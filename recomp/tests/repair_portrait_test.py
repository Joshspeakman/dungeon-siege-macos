"""Synthetic save rewrite tests. No game assets or user saves are required."""
import importlib.util
import struct
import unittest
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location("repair", Path(__file__).parents[1] / "tools/repair_portrait.py")
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


def bmp(visible):
    data = bytearray(12342)
    data[:2] = b"BM"
    struct.pack_into("<I", data, 2, len(data))
    struct.pack_into("<IIiiHHI", data, 10, 54, 40, 64, 64, 1, 24, 0)
    if visible:
        data[54:] = bytes(i % 251 for i in range(12288))
    return bytes(data)


def save(visible=False, skin="skin_1", skew=0):
    party = ("[party] { [members] { [0] { [member] { template_name = farmgirl; "
             "custom_head = default; [skins] { 0 = %s,hair_1; } } } } }" % skin).encode()
    # Deliberately put the portrait before gameplay bytes; the rewrite must update later offsets.
    resources = [("party.gas", party), ("portrait-0.bmp", bmp(visible)),
                 ("world.xdat", b"Gameplay state must stay unchanged." * 1000)]
    data = bytearray(88 + skew)
    data[:8] = b"DSigTank"
    struct.pack_into("<I", data, 8, 0x10002)
    struct.pack_into("<I", data, 24, len(data))
    records, crc = {}, 0
    for name, plain in resources:
        offset = len(data) - (88 + skew)
        chunks = bytearray()
        packed = bytearray()
        for start in range(0, len(plain), 16384):
            part = plain[start:start + 16384]
            extra = 16 if len(part) == 16384 else 0
            chunk = zlib.compress(part[:-extra] if extra else part)
            chunks.extend(struct.pack("<4I", len(part), len(chunk), extra, len(packed)))
            packed.extend(chunk)
            if extra:
                packed.extend(part[-extra:])
        data.extend(packed)
        record = bytearray(30)
        struct.pack_into("<4I", record, 0, 0, len(plain), offset, zlib.crc32(plain))
        struct.pack_into("<3H", record, 24, 1, 0, len(name))
        record.extend(name.encode() + b"\0")
        record.extend(b"\0" * (-len(record) % 4))
        record.extend(struct.pack("<2I", len(packed), 16384) + chunks)
        records[name] = record
        crc = zlib.crc32(plain, crc)
    dirs = len(data)
    data.extend(b"\0" * 4)
    fileset = len(data)
    data.extend(struct.pack("<I", len(records)) + b"\0" * (4 * len(records)))
    for i, name in enumerate(sorted(records)):
        struct.pack_into("<I", data, fileset + 4 + i * 4, len(data) - fileset)
        data.extend(records[name])
    struct.pack_into("<3I", data, 12, dirs, fileset, len(data) - dirs)
    struct.pack_into("<2I", data, 80, zlib.crc32(data[dirs:]), crc)
    return bytes(data)


class PortraitRepairTest(unittest.TestCase):
    def test_rewrite_all_alignments_and_raw_tails(self):
        for skew in range(4):
            original, reference = save(skew=skew), save(True, skew=skew)
            result, report = repair.repair(original, reference)
            before, after = repair.Save(original), repair.Save(result)
            self.assertEqual(after.files["portrait-0.bmp"]["content"], bmp(True))
            for name in ("party.gas", "world.xdat"):
                self.assertEqual(before.files[name]["content"], after.files[name]["content"])
            self.assertGreater(after.files["world.xdat"]["begin"], before.files["world.xdat"]["begin"])
            self.assertEqual(report["unchanged_resources"], 2)

    def test_refuses_wrong_character(self):
        with self.assertRaisesRegex(ValueError, "appearance differs"):
            repair.repair(save(), save(True, skin="skin_2"))

    def test_refuses_visible_target(self):
        with self.assertRaisesRegex(ValueError, "not entirely black"):
            repair.repair(save(True), save(True))

    def test_refuses_black_reference(self):
        with self.assertRaisesRegex(ValueError, "also black"):
            repair.repair(save(), save())

    def test_refuses_corruption(self):
        for offset in (88, -1):
            broken = bytearray(save())
            broken[offset] ^= 0xff
            with self.assertRaises((ValueError, zlib.error)):
                repair.repair(bytes(broken), save(True))

    def test_refuses_missing_member(self):
        with self.assertRaisesRegex(ValueError, "missing portrait"):
            repair.repair(save(), save(True), 1)


if __name__ == "__main__":
    unittest.main()
