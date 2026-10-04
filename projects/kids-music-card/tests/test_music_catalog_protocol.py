"""Run production catalog validation on host, using SDK's portable cJSON only.

No ESP-IDF APIs or mocks. Explicit skip without SDK cJSON sources and host cc.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class CatalogProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('gcc')
        source = pathlib.Path(os.environ.get('IDF_PATH', '/nonexistent')) / 'components/json/cJSON'
        if not cc or not (source / 'cJSON.c').is_file():
            raise unittest.SkipTest('Requires host cc and IDF_PATH for portable cJSON sources')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = pathlib.Path(cls.directory.name) / 'catalog_cases'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-I', str(ROOT / 'main'), '-I', str(source),
                        str(ROOT / 'main/music_catalog_protocol.c'),
                        str(ROOT / 'main/music_policy.c'), str(source / 'cJSON.c'),
                        str(ROOT / 'tests/music_catalog_cases.c'), '-lm', '-o', str(cls.executable)], check=True)

    def test_current_origin_path_and_duplicate_fields(self):
        subprocess.run([str(self.executable), 'current'], check=True)

    def test_manifest_format_counts_empty_and_limit(self):
        subprocess.run([str(self.executable), 'manifest'], check=True)

    def test_page_pin_lengths_utf8_hash_path_and_duplicates(self):
        subprocess.run([str(self.executable), 'page'], check=True)
