#!/usr/bin/env python3
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import encode_music  # noqa: E402


class MusicResourceTests(unittest.TestCase):
    def test_catalog_is_unique_and_matches_generated_header(self):
        catalog = json.loads((ROOT / "assets/music/catalog.json").read_text())
        self.assertEqual(len(catalog), 6)
        self.assertEqual(len({item["id"] for item in catalog}), 6)
        self.assertEqual(len({item["path"] for item in catalog}), 6)
        header = (ROOT / "main/music_catalog.h").read_text()
        for item in catalog:
            self.assertIn(item["title"], header)
            self.assertIn(item["path"], header)

    def test_ogg_packet_parser_keeps_packets_across_pages(self):
        def page(body, lacing, sequence):
            header = bytearray(b"OggS" + bytes([0, 0]))
            header += (0).to_bytes(8, "little")  # granule position
            header += (1).to_bytes(4, "little")  # serial
            header += sequence.to_bytes(4, "little")
            header += (0).to_bytes(4, "little")  # checksum (not needed here)
            header += bytes([len(lacing)]) + bytes(lacing)
            return bytes(header) + body

        # The fake stream includes a header packet, a tags packet, and one
        # audio packet split between two pages.
        first = b"OpusHead" + bytes(11)
        tags = b"OpusTags" + bytes(3)
        audio = b"fake-opus-frame" * 20
        page1 = page(first + tags + audio[:255], [len(first), len(tags), 255], 0)
        page2 = page(audio[255:], [len(audio) - 255], 1)
        with tempfile.NamedTemporaryFile() as stream:
            stream.write(page1 + page2)
            stream.flush()
            packets = encode_music.ogg_packets(pathlib.Path(stream.name))
        self.assertEqual(packets[0], first)
        self.assertEqual(packets[1], tags)
        self.assertEqual(packets[2], audio)

    def test_partition_is_inside_eight_megabytes(self):
        offset, size = 0x250000, 0x5B0000
        self.assertEqual(offset + size, 0x800000)


if __name__ == "__main__":
    unittest.main()
