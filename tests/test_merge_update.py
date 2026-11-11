from paths import ROOT
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def write(path, text='x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class MergeUpdateTests(unittest.TestCase):
    def merge(self, base, update, out):
        return subprocess.run([sys.executable, str(ROOT / 'tools/merge_update.py'), str(base), str(update), str(out)],
                              capture_output=True, text=True)

    def test_ps4_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write(tmp / 'base/eboot.bin', 'old')
            write(tmp / 'base/sce_sys/param.sfo', 'base')
            write(tmp / 'base/sce_module/libc.prx')
            write(tmp / 'patch/eboot.bin', 'new')
            write(tmp / 'patch/sce_sys/param.sfo', 'patch')
            self.assertEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)
            self.assertEqual((tmp / 'out/eboot.bin').read_text(), 'new')
            self.assertEqual((tmp / 'out/sce_sys/param.sfo').read_text(), 'patch')
            self.assertTrue((tmp / 'out/sce_module/libc.prx').is_file())

    def test_pkg_tool_layout(self):
        # PS4 PKG Tool: Image0 holds the files, Sc0 the sce_sys ones; the update was fixed by
        # hand (eboot.bin and param.sfo copied out), which still counts as Image0 + Sc0.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write(tmp / 'base/Image0/eboot.bin', 'old')
            write(tmp / 'base/Image0/sce_module/libc.prx')
            write(tmp / 'base/Image0/memoryconfiguration.igz')
            write(tmp / 'base/Sc0/param.sfo', 'base')
            write(tmp / 'patch/Image0/eboot.bin', 'new')
            write(tmp / 'patch/Sc0/param.sfo', 'patch')
            write(tmp / 'patch/eboot.bin', 'new')
            write(tmp / 'patch/sce_sys/param.sfo', 'patch')
            self.assertEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)
            out = tmp / 'out'
            self.assertEqual((out / 'eboot.bin').read_text(), 'new')
            self.assertEqual((out / 'sce_sys/param.sfo').read_text(), 'patch')
            self.assertTrue((out / 'sce_module/libc.prx').is_file())
            self.assertTrue((out / 'memoryconfiguration.igz').is_file())
            self.assertFalse((out / 'Image0').exists())
            self.assertFalse((out / 'Sc0').exists())

    def merge_fake_game(self, *options):
        """Two minimal dumps merged with the given options (their files are not a real game)."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for name in ('base', 'patch'):
                write(tmp / name / 'eboot.bin')
                write(tmp / name / 'sce_sys/param.sfo')
            return subprocess.run([sys.executable, str(ROOT / 'tools/merge_update.py'), *options,
                                   str(tmp / 'base'), str(tmp / 'patch'), str(tmp / 'out')],
                                  capture_output=True, text=True)

    def test_progress_lines(self):
        result = self.merge_fake_game('--progress')
        self.assertEqual(result.returncode, 0)
        lines = [line for line in result.stdout.splitlines() if line.startswith('progress ')]
        self.assertEqual(lines[-1], 'progress 1.00 Game files ready')
        self.assertIn('progress 0.95 Linking game files (4 of 4)', lines)

    def test_no_progress_lines_without_the_option(self):
        result = self.merge_fake_game()
        self.assertEqual(result.returncode, 0)
        self.assertNotIn('progress', result.stdout)

    def test_check_refuses_a_game_this_port_does_not_run(self):
        result = self.merge_fake_game('--check')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unsupported game files', result.stderr)

    def test_not_a_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'base').mkdir()
            write(tmp / 'patch/eboot.bin')
            self.assertNotEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)


if __name__ == '__main__':
    unittest.main()
