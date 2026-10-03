"""Turns a frame written by gr_overlay_dump (GMod console) into two PNGs: the colour, and
the alpha channel as grey.

    python tools/overlay_dump.py FRAME.raw OUT_PREFIX
"""
import struct, sys
from PIL import Image

raw = open(sys.argv[1], "rb").read()
w, h, fmt, _ = struct.unpack_from("<4I", raw, 0)
img = Image.frombytes("RGBA", (w, h), raw[16:16 + w * h * 4], "raw", "BGRA")
img.convert("RGB").save(sys.argv[2] + "_rgb.png")
img.getchannel("A").save(sys.argv[2] + "_a.png")
alpha = img.getchannel("A").histogram()
print(f"{w}x{h} format {fmt}; alpha 0: {alpha[0]} px, 255: {alpha[255]} px, between: {sum(alpha[1:255])} px")
