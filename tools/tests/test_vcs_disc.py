"""vcs_disc.py: the advice for a .chd follows the kind of CHD (docs/DISC.md "CHD images").

The CHD files here are headers built by the test (no game data): a v5 or v4 header and one metadata entry, which is
all the check reads.

    python -m unittest discover tools/tests
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chd  # noqa: E402
import vcs_disc  # noqa: E402

CD_META = b"TRACK:1 TYPE:MODE1 SUBTYPE:NONE FRAMES:2025968 PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0\0"


def chd_bytes(version, tag, meta=b"\0"):
    """A CHD header whose first metadata entry has `tag`, as chdman writes them (big-endian)."""
    meta_entry = tag + struct.pack(">I", len(meta)) + struct.pack(">Q", 0) + meta
    if version == 5:
        header_len = 124
        header = bytearray(header_len)
        header[0:8] = b"MComprHD"
        struct.pack_into(">II", header, 8, header_len, 5)
        header[16:20] = b"cdlz"
        struct.pack_into(">QQQ", header, 32, 4959569664, 0, header_len)  # logical bytes, map, metadata offsets
        struct.pack_into(">II", header, 56, 19584, 2448)
    elif version == 4:
        header_len = 108
        header = bytearray(header_len)
        header[0:8] = b"MComprHD"
        struct.pack_into(">II", header, 8, header_len, 4)
        struct.pack_into(">Q", header, 36, header_len)  # metadata offset (v3 / v4)
    else:
        raise ValueError(version)
    return bytes(header) + meta_entry


class ChdAdvice(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_kind_cd_v5(self):
        self.assertEqual(chd.kind(self.write("a.chd", chd_bytes(5, b"CHT2", CD_META))), "cd")

    def test_kind_cd_older_tags(self):
        for tag in (b"CHTR", b"CHCD", b"CHGD"):
            with self.subTest(tag=tag):
                self.assertEqual(chd.kind(self.write("t.chd", chd_bytes(5, tag))), "cd")

    def test_kind_dvd_v5_and_v4(self):
        self.assertEqual(chd.kind(self.write("d5.chd", chd_bytes(5, b"DVD "))), "dvd")
        self.assertEqual(chd.kind(self.write("d4.chd", chd_bytes(4, b"DVD "))), "dvd")

    def test_kind_unknown(self):
        self.assertIsNone(chd.kind(self.write("h.chd", chd_bytes(5, b"GDDD"))))
        self.assertIsNone(chd.kind(self.write("x.chd", b"not a chd at all")))
        self.assertIsNone(chd.kind(self.write("e.chd", b"")))
        self.assertIsNone(chd.kind(os.path.join(self.dir.name, "missing.chd")))

    def test_check_cd_advises_extractcd(self):
        r = vcs_disc.check(self.write("game.chd", chd_bytes(5, b"CHT2", CD_META)))
        self.assertFalse(r["ok"])
        self.assertEqual(r["code"], "not_iso")
        self.assertIn("chdman extractcd -i game.chd -o game.cue -ob game.iso", r["message"])
        self.assertNotIn("extractdvd", r["message"])

    def test_check_dvd_advises_extractdvd(self):
        r = vcs_disc.check(self.write("game.chd", chd_bytes(5, b"DVD ")))
        self.assertEqual(r["code"], "not_iso")
        self.assertIn("chdman extractdvd -i game.chd -o game.iso", r["message"])
        self.assertNotIn("extractcd", r["message"])

    def test_check_unknown_chd_names_both(self):
        r = vcs_disc.check(self.write("game.chd", b"garbage"))
        self.assertEqual(r["code"], "not_iso")
        self.assertIn("extractdvd", r["message"])
        self.assertIn("extractcd", r["message"])

    def test_other_formats_unchanged(self):
        r = vcs_disc.check(self.write("game.cso", b"CISO"))
        self.assertEqual(r["code"], "not_iso")
        self.assertIn("maxcso", r["message"])

    def test_readable_chd_is_checked_as_a_disc(self):
        # a CHD that reads (tests/test_chd.py's tiny disc): the normal disc check runs on it, no convert advice
        import test_chd
        sectors = test_chd.iso_sectors()
        path = self.write("tiny.chd", test_chd.dvd_chd(sectors, [("c", i % 2) for i in range(len(sectors) // 2)]))
        r = vcs_disc.check(path)
        self.assertNotEqual(r["code"], "not_iso", r)
        self.assertNotIn("chdman", r["message"])

    @unittest.skipUnless(os.environ.get("VCS_TEST_PLAYER_DISC") == "1", "the player's disc: VCS_TEST_PLAYER_DISC=1")
    def test_players_cd_mode_chd(self):
        import test_chd
        path = test_chd.PLAYER_CHD
        self.assertEqual(chd.kind(path), "cd")
        r = vcs_disc.check(path)
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["boot"], vcs_disc.ELF_NAME)


if __name__ == "__main__":
    unittest.main()
