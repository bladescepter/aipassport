"""Compile and run the production streaming C core on the host, without IDF.

The existing tools/validate.sh --static discovers these tests automatically.
A missing host compiler is reported as a skip, not firmware validation.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MusicStreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            raise unittest.SkipTest("host C compiler missing; streaming C tests not run")
        cls.temporary = tempfile.TemporaryDirectory(prefix="music-stream-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = pathlib.Path(cls.temporary.name) / "music_stream_cases"
        sources = [
            ROOT / "tests/music_stream_cases.c",
            ROOT / "main/music_source.c",
            ROOT / "main/music_file_source.c",
            ROOT / "main/music_frame_reader.c",
            ROOT / "main/music_stream_buffer.c",
        ]
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(ROOT / "main"), *map(str, sources), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise RuntimeError(f"host C compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name):
        result = subprocess.run(
            [str(self.binary), name, str(pathlib.Path(self.temporary.name) / "track.opus")],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_single_byte_short_reads(self):
        self.run_case("short_reads")

    def test_little_endian_frame_length(self):
        self.run_case("little_endian")

    def test_eof_at_each_frame_boundary(self):
        self.run_case("eof_boundaries")

    def test_zero_oversized_and_maximum_frame(self):
        self.run_case("bad_lengths")

    def test_starvation_preserves_header_and_payload(self):
        self.run_case("starvation")

    def test_deadline_includes_partial_progress(self):
        self.run_case("deadline")

    def test_long_pause_preserves_partial_frame(self):
        self.run_case("pause_resume")

    def test_cancel_close_and_new_generation(self):
        self.run_case("cancellation")

    def test_transport_error_is_not_eof(self):
        self.run_case("transport_error")

    def test_source_contract_validation(self):
        self.run_case("source_contract")

    def test_bounded_buffer_wrap_and_backpressure(self):
        self.run_case("ring_wrap")

    def test_buffer_terminal_results_and_cancel(self):
        self.run_case("ring_terminal")

    def test_buffer_against_byte_sequence_model(self):
        self.run_case("ring_model")

    def test_incremental_buffered_frames(self):
        self.run_case("buffered_frames")

    def test_real_file_source_and_truncated_header(self):
        self.run_case("file_source")


if __name__ == "__main__":
    unittest.main()
