import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('publish_catalog', ROOT / 'tools/publish_catalog.py')
PUBLISH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PUBLISH)


class PublishCatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.audio = self.root / 'audio'
        self.audio.mkdir()
        self.catalog = self.root / 'catalog.json'
        self.characters = self.root / 'characters.json'
        self.characters.write_text(json.dumps({'characters': 'song0123456789'}))
        # TOC indicates 20 ms mono; this fixture tests container validation,
        # not whether Opus can decode these synthetic packet payloads.
        (self.audio / 'one.opus').write_bytes(b'\x02\x00\x98x' * 2)
        self.output = self.root / 'public'

    def publish(self, rows):
        self.catalog.write_text(json.dumps(rows))
        return PUBLISH.publish(self.catalog, self.audio, self.output, 'r1', self.characters)

    def test_pagination_hash_paths_and_independent_local_catalog(self):
        rows = [{'id': f'song{i}', 'title': 'song', 'path': 'one.opus'} for i in range(21)]
        before = json.dumps(rows)
        self.assertEqual(self.publish(rows), 21)
        self.assertEqual(self.catalog.read_text(), before)
        release = self.output / 'v1/releases/r1'
        manifest = json.loads((release / 'manifest.json').read_text())
        self.assertEqual(manifest['total_pages'], 2)
        page = json.loads((release / 'tracks/all/0001.json').read_text())
        self.assertEqual(len(page['tracks']), 1)
        track = page['tracks'][0]
        self.assertEqual(track['size_bytes'], 8)
        self.assertEqual(track['duration_ms'], 40)
        self.assertTrue((self.output / track['audio_path'].lstrip('/')).is_file())

    def test_duplicate_and_unsupported_titles_fail_before_output(self):
        row = {'id': 'song', 'title': 'song', 'path': 'one.opus'}
        with self.assertRaises(ValueError):
            self.publish([row, row])
        self.assertFalse(self.output.exists())
        with self.assertRaises(ValueError):
            self.publish([{**row, 'title': '缺字'}])
        self.assertFalse(self.output.exists())

    def test_refuses_overwrite_path_traversal_and_oversize_catalog(self):
        with self.assertRaises(ValueError):
            self.publish([{'id': 'song', 'title': 'song', 'path': '../one.opus'}])
        with self.assertRaises(ValueError):
            self.publish([{}] * 1001)
        self.output.mkdir()
        with self.assertRaises(ValueError):
            self.publish([])

    def test_stream_empty_truncation_lengths_and_packet_duration(self):
        path = self.audio / 'bad.opus'
        for data in (b'', b'\x02', b'\x00\x00', b'\x02\x00x', b'\x02\x00\x80x'):
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                PUBLISH.inspect_audio(path)

    def test_empty_catalog(self):
        self.assertEqual(self.publish([]), 0)
        manifest = json.loads((self.output / 'v1/releases/r1/manifest.json').read_text())
        self.assertEqual(manifest['total_pages'], 0)

    def test_json_size_limit(self):
        with self.assertRaises(ValueError):
            PUBLISH.document({'data': 'a' * 16384})
