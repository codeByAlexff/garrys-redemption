"""The header parser and the pack/unpack built on it.

The parse itself is proved against the compiler by the protocol build (layout_check.cpp).
These tests cover the Python side of it.
"""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from gr_layout import Layout, LayoutError  # noqa: E402
from gr_shm import joaat  # noqa: E402


class LayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = Layout()

    def test_frozen_header(self):
        # These never change, in any protocol version.
        self.assertEqual(self.layout.sizeof("GrPeer"), 32)
        self.assertEqual(self.layout.sizeof("GrHeader"), 64)
        self.assertEqual(self.layout.offset("GrShared", "header.host.magic"), 0)
        self.assertEqual(self.layout.offset("GrShared", "header.guest.magic"), 32)
        self.assertEqual(self.layout.offset("GrShared", "header.guest.heartbeat"), 60)
        self.assertEqual(self.layout.const("GR_MAGIC").to_bytes(4, "little"), b"GRBR")

    def test_constants_and_sizes(self):
        c = self.layout.const
        self.assertEqual(c("GR_SHM_SIZE"), self.layout.sizeof("GrShared"))
        self.assertEqual(c("GR_HEADER_SIZE"), 64)
        self.assertEqual(c("GR_ENTF_FROZEN"), 8)
        self.assertEqual(self.layout.offset("GrShared", "host"), 64)
        self.assertEqual(self.layout.offset("GrShared", "guest"), 64 + self.layout.sizeof("GrHostFrame"))

    def test_pack_unpack_round_trip(self):
        frame = {
            "frame": 12,
            "cam_fov": 55.0,
            "ped_pos": {"x": 1.5, "y": -2.25, "z": 3.0},
            "input": {"keys": bytes([0x80] + [0] * 31), "mouse_dx": -5, "screen_w": 1920},
            "entity_count": 2,
            "entities": [
                {"handle": 1001, "model": 0xDEADBEEF, "type": 1, "flags": 6,
                 "pos": {"x": 10.0, "y": 20.0, "z": 30.0}, "health": 99.5},
                {"handle": -7, "vel": {"x": 0.25, "y": 0.0, "z": -9.5}},
            ],
        }
        data = self.layout.pack("GrHostFrame", frame)
        self.assertEqual(len(data), self.layout.sizeof("GrHostFrame"))
        back = self.layout.unpack("GrHostFrame", data)
        self.assertEqual(back["frame"], 12)
        self.assertEqual(back["ped_pos"], {"x": 1.5, "y": -2.25, "z": 3.0})
        self.assertEqual(back["input"]["keys"][0], 0x80)
        self.assertEqual(back["input"]["mouse_dx"], -5)
        self.assertEqual(back["entities"][0]["model"], 0xDEADBEEF)
        self.assertEqual(back["entities"][0]["health"], 99.5)
        self.assertEqual(back["entities"][1]["handle"], -7)
        self.assertEqual(back["entities"][1]["vel"]["z"], -9.5)
        self.assertEqual(len(back["entities"]), self.layout.const("GR_MAX_ENTITIES"))
        self.assertEqual(back["entities"][2]["handle"], 0)

    def test_pack_rejects_what_does_not_fit(self):
        with self.assertRaises(KeyError):
            self.layout.pack("GrGuestFrame", {"no_such_field": 1})
        too_many = [{"handle": i} for i in range(self.layout.const("GR_MAX_DRIVEN") + 1)]
        with self.assertRaises(ValueError):
            self.layout.pack("GrGuestFrame", {"driven": too_many})

    def test_padding_follows_the_compiler_rules(self):
        # Not a shape the real header uses, but the parser must not silently get it wrong
        # if a later protocol version does.
        text = """
// GR_LAYOUT_BEGIN
inline constexpr uint32_t N = 1u << 1;
struct Inner {
    uint8_t a;
    double b;
};
struct Outer {
    uint16_t x;
    Inner inner[N];
    uint8_t tail;
};
// GR_LAYOUT_END
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "h.h"
            path.write_text(text, encoding="utf-8")
            layout = Layout(path)
        self.assertEqual(layout.sizeof("Inner"), 16)
        self.assertEqual(layout.structs["Outer"]["inner"].offset, 8)
        self.assertEqual(layout.structs["Outer"]["tail"].offset, 40)
        self.assertEqual(layout.sizeof("Outer"), 48)
        back = layout.unpack("Outer", layout.pack("Outer", {"x": 7, "inner": [{"a": 1, "b": 2.5}], "tail": 9}))
        self.assertEqual((back["x"], back["inner"][0]["b"], back["tail"]), (7, 2.5, 9))

    def test_unparseable_header_is_an_error_not_a_guess(self):
        text = "// GR_LAYOUT_BEGIN\nstruct A {\n    int* p;\n};\n// GR_LAYOUT_END\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "h.h"
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(LayoutError):
                Layout(path)

    def test_joaat(self):
        # Published test vectors for Jenkins one-at-a-time.
        self.assertEqual(joaat("a"), 0xCA2E9442)
        self.assertEqual(joaat("The quick brown fox jumps over the lazy dog".lower()), joaat("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"))


if __name__ == "__main__":
    unittest.main()
