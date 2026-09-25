"""Lightweight sampler regression checks; does not launch a game or benchmark."""
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('matrix', Path(__file__).with_name('benchmark-matrix.py'))
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


class StatusFreshnessTests(unittest.TestCase):
    def test_fresh_and_stale_snapshots(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'status.txt'
            path.write_text('booted=1\nfps=59.94\nvps=59.94\nspeed=1\nframe_count=120\nstate=running\nlast_error=\n')
            self.assertEqual(matrix.read_status(path)['frame_count'], 120)
            old = time.time() - 3.7
            os.utime(path, (old, old))
            self.assertIsNone(matrix.read_status(path))

    def test_missing_or_partial_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'status.txt'
            self.assertIsNone(matrix.read_status(path))
            path.write_text('booted=1\n')
            self.assertIsNone(matrix.read_status(path))

    def test_future_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'status.txt'
            path.write_text('booted=1\nfps=60\nvps=60\nspeed=1\nframe_count=1\n')
            future = time.time() + 10
            os.utime(path, (future, future))
            self.assertIsNone(matrix.read_status(path))

    def test_replaced_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'status.txt'
            path.write_text('booted=1\nfps=60\nvps=60\nspeed=1\nframe_count=1\n')
            initial = path.stat()
            changed = SimpleNamespace(st_ino=initial.st_ino + 1,
                                      st_mtime_ns=initial.st_mtime_ns,
                                      st_size=initial.st_size)
            with patch.object(Path, 'stat', side_effect=[initial, changed]):
                self.assertIsNone(matrix.read_status(path))


if __name__ == '__main__':
    unittest.main()
