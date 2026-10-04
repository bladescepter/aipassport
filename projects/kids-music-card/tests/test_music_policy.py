"""Host tests for the production cross-page cache and playlist policy."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MusicPolicyTests(unittest.TestCase):
    def test_cache_limit_cross_page_and_playlist_navigation(self):
        cc = shutil.which('cc') or shutil.which('gcc')
        if not cc:
            self.skipTest('Host C compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / 'music_policy_cases'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-I', str(ROOT / 'main'), str(ROOT / 'main/music_policy.c'),
                            str(ROOT / 'tests/music_policy_cases.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)
