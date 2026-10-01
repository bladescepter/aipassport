"""Host tests of production HTTP policy; not tests of ESP-IDF/TLS/task APIs."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MusicHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            raise unittest.SkipTest("host C compiler missing; HTTP policy tests not run")
        cls.temporary = tempfile.TemporaryDirectory(prefix="music-http-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = pathlib.Path(cls.temporary.name) / "music_http_cases"
        sources = [ROOT / "tests/music_http_cases.c"] + [ROOT / "main" / name for name in (
            "music_http_protocol.c", "music_source.c", "music_stream_buffer.c", "music_frame_reader.c",
        )]
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(ROOT / "main"), *map(str, sources), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise RuntimeError(f"HTTP policy C compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_https_origin_and_relative_path(self):
        self.run_case("url_allowed")

    def test_external_urls_credentials_and_path_traversal_rejected(self):
        self.run_case("url_rejected")

    def test_url_length_and_output_capacity(self):
        self.run_case("url_boundaries")

    def test_content_length_numbers_and_overflow(self):
        self.run_case("header_numbers")

    def test_content_range_structure_and_bounds(self):
        self.run_case("header_ranges")

    def test_critical_header_duplicates_rejected(self):
        self.run_case("header_duplicates")

    def test_compressed_and_chunked_responses_rejected(self):
        self.run_case("header_encoding")

    def test_header_limits_and_injection(self):
        self.run_case("header_limits")

    def test_initial_response_size_and_status(self):
        self.run_case("response_initial")

    def test_range_resume_and_ignored_range(self):
        self.run_case("response_resume")

    def test_auth_redirect_missing_and_server_errors(self):
        self.run_case("response_statuses")

    def test_retry_count_and_recovery_deadline(self):
        self.run_case("recovery_budget")

    def test_progress_and_pause_do_not_restore_retries(self):
        self.run_case("recovery_progress")

    def test_resume_keeps_buffered_partial_frame(self):
        self.run_case("buffered_resume")


if __name__ == "__main__":
    unittest.main()
