"""vcs.exe reads a disc the same from an .iso and from a .chd (docs/DISC.md "CHD images").

VCS_DISC_COMPARE=<other image> makes vcs.exe open its disc and the other one, compare every sector, then read the
other one again from 4 threads, print `[disc] compare` lines and exit (0: equal, 1: different) before the game starts.
The images here are generated (tests/test_chd.py's tiny ISO 9660 disc, wrapped in a DVD CHD and a CD-mode CHD): no
game data. The player's whole disc is a check, opt-in (VCS_TEST_PLAYER_DISC=1, test_chd.PLAYER_CHD / PLAYER_ISO).
The same runs show the `[disc] raw fallback` line (an .iso's path, never a .chd's); VCS_DISC_FIND=<folder> shows
the disc a folder search takes there.

Skips when the game is not built, or built before VCS_DISC_COMPARE existed (it would start the game instead).
vcs.exe prints UTF-8 (its manifest), so its output is read as UTF-8.
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import toolchain as tc  # noqa: E402
import test_chd as t  # noqa: E402

VCS = tc.VCS_EXE
SCATTERED = "[disc] compare: 4 threads, 256 scattered reads equal"


def run_compare(disc, other, timeout=600, env_extra=None, prefix="[disc] compare"):
    env = dict(os.environ, VCS_DISC_COMPARE=other, VCS_KEEP_SETTINGS="1", PS2X_HIDDEN="1", **(env_extra or {}))
    p = subprocess.run([VCS, disc], env=env, capture_output=True, encoding="utf-8", errors="replace", timeout=timeout)
    lines = [l for l in (p.stdout + p.stderr).splitlines() if l.startswith(prefix)]
    return p.returncode, lines


class DiscBackends(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(VCS):
            raise unittest.SkipTest("the game is not built (%s)" % VCS)
        cls.dir = tempfile.TemporaryDirectory()
        sectors = t.iso_sectors()
        cls.iso = os.path.join(cls.dir.name, "disc.iso")
        with open(cls.iso, "wb") as f:
            f.write(b"".join(sectors))
        # a build without the compare mode would boot the game from this disc: give it seconds, not minutes
        try:
            _, lines = run_compare(cls.iso, cls.iso, timeout=60)
        except subprocess.TimeoutExpired:
            lines = []
        if not lines:
            cls.dir.cleanup()
            raise unittest.SkipTest("%s has no VCS_DISC_COMPARE (an older build): python tools/build.py" % VCS)
        cls.dvd = os.path.join(cls.dir.name, "disc_dvd.chd")
        with open(cls.dvd, "wb") as f:
            f.write(t.dvd_chd(sectors, [("c", i % 2) for i in range(len(sectors) // 2)]))
        cls.cd = os.path.join(cls.dir.name, "disc_cd.chd")
        with open(cls.cd, "wb") as f:
            f.write(t.cd_chd(sectors, [("c", i % 3) for i in range(len(sectors) // 2)]))
        bad = list(sectors)
        bad[21] = bytes(2048)
        cls.bad_iso = os.path.join(cls.dir.name, "bad.iso")
        with open(cls.bad_iso, "wb") as f:
            f.write(b"".join(bad))

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def test_iso_against_itself(self):
        code, lines = run_compare(self.iso, self.iso)
        self.assertEqual((code, lines), (0, ["[disc] compare: 22 sectors equal", SCATTERED]))

    def test_dvd_chd_equals_the_iso(self):
        code, lines = run_compare(self.dvd, self.iso)
        self.assertEqual((code, lines), (0, ["[disc] compare: 22 sectors equal", SCATTERED]))

    def test_cd_mode_chd_equals_the_iso(self):
        code, lines = run_compare(self.cd, self.iso)
        self.assertEqual((code, lines), (0, ["[disc] compare: 22 sectors equal", SCATTERED]))

    # 1 and 4 decoders. The compare's second pass reads the CHD from 4 threads at once; a cache of 3 hunks makes them
    # evict each other's hunks while they wait for theirs.
    def test_parallel_decoders_read_the_same(self):
        for chd in (self.dvd, self.cd):
            for decoders in ("1", "4"):
                env = {"VCS_CHD_DECODERS": decoders, "VCS_CHD_CACHE_HUNKS": "3"}
                code, lines = run_compare(self.iso, chd, env_extra=env, prefix="[disc]")
                self.assertEqual(code, 0, (chd, decoders, lines))
                self.assertIn("[disc] chd: decoders %s, cache 3 hunks" % decoders, lines, (chd, decoders))
                self.assertIn("[disc] compare: 22 sectors equal", lines, (chd, decoders))
                self.assertIn(SCATTERED, lines, (chd, decoders))
                self.assertFalse([l for l in lines if "a decoder failed to open" in l], (chd, decoders))

    def test_a_damaged_hunk_fails_the_read_with_parallel_decoders(self):
        sectors = t.iso_sectors()
        bad = os.path.join(self.dir.name, "bad_hunk.chd")
        with open(bad, "wb") as f:  # hunk 5 (sectors 10-11) fails its CRC check
            f.write(t.dvd_chd(sectors, [("c", i % 2) for i in range(len(sectors) // 2)], break_crc=5))
        for decoders in ("1", "4"):
            code, lines = run_compare(self.iso, bad, env_extra={"VCS_CHD_DECODERS": decoders}, prefix="[disc]")
            self.assertEqual(code, 1, (decoders, lines))
            self.assertIn("[disc] compare: cannot read sectors 0-21", lines, decoders)
            self.assertTrue([l for l in lines if l.startswith("[disc] CHD hunk 5:")], (decoders, lines))

    def test_dvd_chd_with_chdmans_default_codecs_equals_the_iso(self):
        # lzma, zlib, huff, flac: what chdman createdvd writes by default; libchdr reads the test writer's hunks
        sectors = t.iso_sectors()
        for endian in ("L", "B"):
            path = os.path.join(self.dir.name, "default_%s.chd" % endian)
            with open(path, "wb") as f:
                f.write(t.dvd_chd(sectors, [("c", i % 4) for i in range(len(sectors) // 2)],
                                  compressors=t.DVD_DEFAULT_CODECS, enc=t.cb.Encoding(flac_endian=endian)))
            code, lines = run_compare(path, self.iso)
            self.assertEqual((code, lines), (0, ["[disc] compare: 22 sectors equal", SCATTERED]), endian)

    # After a failed read, the emulated IOP reads the image file as raw sectors (iop_cdvd.cpp readSectors): the game
    # hands it an .iso's path only, never a .chd's compressed bytes
    def test_the_raw_fallback_is_an_iso_only(self):
        for disc, want in ((self.iso, self.iso), (self.dvd, "none"), (self.cd, "none")):
            _, lines = run_compare(disc, self.iso, prefix="[disc] raw fallback")
            self.assertEqual(lines, ["[disc] raw fallback: " + want], disc)

    # VCS_DISC_FIND=<folder>: the disc a folder search picks (the exe's folder, then the current one), then exit
    def test_a_folder_search_takes_an_iso_before_a_chd(self):
        def find(names):
            with tempfile.TemporaryDirectory() as folder, tempfile.TemporaryDirectory() as data:
                for name in names:
                    with open(os.path.join(folder, name), "wb") as f:
                        f.write(b"x")
                # a data folder of its own: a build without VCS_DISC_FIND starts the game
                env = dict(os.environ, VCS_DISC_FIND=folder, VCS_DATA_DIR=data, VCS_KEEP_SETTINGS="1", PS2X_HIDDEN="1")
                p = subprocess.run([VCS], env=env, capture_output=True, encoding="utf-8", errors="replace", timeout=60)
                lines = [l for l in (p.stdout + p.stderr).splitlines() if l.startswith("[disc] find")]
                return p.returncode, [l.replace(folder + os.sep, "") for l in lines]
        self.assertEqual(find(["a.chd", "b.iso", "notes.txt"]), (0, ["[disc] find: b.iso"]))
        self.assertEqual(find(["game.CHD"]), (0, ["[disc] find: game.CHD"]))
        self.assertEqual(find(["notes.txt"]), (0, ["[disc] find: none"]))

    def test_a_difference_is_found(self):
        code, lines = run_compare(self.dvd, self.bad_iso)
        self.assertEqual((code, lines), (1, ["[disc] compare: sector 21 differs"]))

    @unittest.skipUnless(t.PLAYER_DISC, t.PLAYER_DISC_WHY)
    def test_players_whole_disc(self):
        sectors = os.path.getsize(t.PLAYER_ISO) // 2048
        code, lines = run_compare(t.PLAYER_CHD, t.PLAYER_ISO, timeout=1800)
        self.assertEqual((code, lines), (0, ["[disc] compare: %d sectors equal" % sectors, SCATTERED]))


if __name__ == "__main__":
    unittest.main()
