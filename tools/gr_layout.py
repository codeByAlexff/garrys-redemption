"""Reads the shared memory layout out of protocol/gr_protocol.h.

The header is the single source of truth. The Python tools never carry their own copy of
a struct: they parse the region between GR_LAYOUT_BEGIN and GR_LAYOUT_END and compute
offsets with the same rules the compiler uses (each field aligned to its own alignment,
struct padded to its largest member's alignment).

The protocol build proves the parse right: `--emit-check` writes a C++ file of
static_asserts, one per offset, size and constant, and protocol/CMakeLists.txt compiles it.

    python tools/gr_layout.py                 print the layout
    python tools/gr_layout.py --emit-check F  write the C++ check file
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "protocol" / "gr_protocol.h"

# C type -> (struct format character, size). Alignment equals size for all of these.
SCALARS = {
    "uint8_t": ("B", 1),
    "int8_t": ("b", 1),
    "uint16_t": ("H", 2),
    "int16_t": ("h", 2),
    "uint32_t": ("I", 4),
    "int32_t": ("i", 4),
    "uint64_t": ("Q", 8),
    "int64_t": ("q", 8),
    "float": ("f", 4),
    "double": ("d", 8),
}

_CONST_RE = re.compile(r"^inline\s+constexpr\s+\w+\s+(\w+)\s*=\s*(.+?);$")
_STRUCT_RE = re.compile(r"^struct\s+(\w+)\s*\{$")
_FIELD_RE = re.compile(r"^(\w+)\s+(\w+)(?:\[(\w+)\])?;$")
_SAFE_EXPR_RE = re.compile(r"^[\w\s()+\-*|<>]+$")


class LayoutError(Exception):
    pass


@dataclass
class Field:
    name: str
    type: str          # a key of SCALARS, or a struct name
    count: int | None  # None for a plain field, N for `type name[N]`
    offset: int
    size: int          # total, including all array elements


@dataclass
class Struct:
    name: str
    fields: list[Field] = field(default_factory=list)
    size: int = 0
    align: int = 1

    def __getitem__(self, name: str) -> Field:
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(f"{self.name} has no field {name}")


class Layout:
    """Constants and structs from the header, plus pack/unpack of whole structs."""

    def __init__(self, header: Path = HEADER):
        self.header = Path(header)
        self.constants: dict[str, int] = {}
        self.structs: dict[str, Struct] = {}
        self._formats: dict[str, struct.Struct] = {}
        self._parse(self.header.read_text(encoding="utf-8"))

    # ---- parsing ----

    def _parse(self, text: str) -> None:
        try:
            body = text.split("// GR_LAYOUT_BEGIN", 1)[1].split("// GR_LAYOUT_END", 1)[0]
        except IndexError:
            raise LayoutError(f"{self.header}: GR_LAYOUT_BEGIN / GR_LAYOUT_END markers not found")

        current: Struct | None = None
        offset = 0
        for number, raw in enumerate(body.splitlines(), 1):
            line = raw.split("//", 1)[0].strip()
            if not line:
                continue

            if current is None:
                if m := _CONST_RE.match(line):
                    self.constants[m.group(1)] = self._eval(m.group(2))
                elif m := _STRUCT_RE.match(line):
                    current = Struct(m.group(1))
                    offset = 0
                else:
                    raise LayoutError(f"layout region line {number}: cannot parse {line!r}")
                continue

            if line == "};":
                current.size = _align_up(offset, current.align)
                self.structs[current.name] = current
                current = None
                continue

            m = _FIELD_RE.match(line)
            if not m:
                raise LayoutError(f"layout region line {number}: cannot parse field {line!r}")
            type_name, name, count_text = m.groups()
            elem_size, elem_align = self._size_align(type_name)
            count = None if count_text is None else self._eval(count_text)
            offset = _align_up(offset, elem_align)
            size = elem_size * (1 if count is None else count)
            current.fields.append(Field(name, type_name, count, offset, size))
            current.align = max(current.align, elem_align)
            offset += size

        if current is not None:
            raise LayoutError(f"struct {current.name} is not closed")

    def _size_align(self, type_name: str) -> tuple[int, int]:
        if type_name in SCALARS:
            size = SCALARS[type_name][1]
            return size, size
        if type_name in self.structs:
            s = self.structs[type_name]
            return s.size, s.align
        raise LayoutError(f"unknown type {type_name!r} (structs must be defined before use)")

    def _eval(self, expr: str) -> int:
        """Evaluates an integer constant expression from the header."""
        if not _SAFE_EXPR_RE.match(expr):
            raise LayoutError(f"unsupported constant expression {expr!r}")
        py = re.sub(r"sizeof\s*\(\s*(\w+)\s*\)", lambda m: str(self.structs[m.group(1)].size), expr)
        py = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]{0,2}\b", r"\1", py)
        try:
            return int(eval(py, {"__builtins__": {}}, dict(self.constants)))
        except Exception as e:
            raise LayoutError(f"cannot evaluate {expr!r}: {e}") from None

    # ---- lookups ----

    def const(self, name: str) -> int:
        return self.constants[name]

    def sizeof(self, struct_name: str) -> int:
        return self.structs[struct_name].size

    def offset(self, struct_name: str, path: str) -> int:
        """Offset of a dotted field path, e.g. offset('GrShared', 'header.guest.heartbeat')."""
        total = 0
        s = self.structs[struct_name]
        parts = path.split(".")
        for i, part in enumerate(parts):
            f = s[part]
            total += f.offset
            if i + 1 < len(parts):
                s = self.structs[f.type]
        return total

    # ---- pack / unpack ----

    def _flat_format(self, struct_name: str) -> str:
        """struct-module format of the struct with explicit padding, no alignment magic."""
        s = self.structs[struct_name]
        out = []
        at = 0
        for f in s.fields:
            if f.offset > at:
                out.append(f"{f.offset - at}x")
            n = 1 if f.count is None else f.count
            if f.type in SCALARS:
                out.append(f"{n}{SCALARS[f.type][0]}")
            else:
                out.append(self._flat_format(f.type) * n)
            at = f.offset + f.size
        if s.size > at:
            out.append(f"{s.size - at}x")
        return "".join(out)

    def format(self, struct_name: str) -> struct.Struct:
        if struct_name not in self._formats:
            self._formats[struct_name] = struct.Struct("<" + self._flat_format(struct_name))
            assert self._formats[struct_name].size == self.structs[struct_name].size
        return self._formats[struct_name]

    def unpack(self, struct_name: str, data, offset: int = 0) -> dict:
        """Bytes -> nested dict. Arrays become lists; uint8 arrays become bytes."""
        values = iter(self.format(struct_name).unpack_from(data, offset))
        return self._build(struct_name, values)

    def _build(self, struct_name: str, values) -> dict:
        out = {}
        for f in self.structs[struct_name].fields:
            if f.type in SCALARS:
                if f.count is None:
                    out[f.name] = next(values)
                else:
                    items = [next(values) for _ in range(f.count)]
                    out[f.name] = bytes(items) if f.type == "uint8_t" else items
            elif f.count is None:
                out[f.name] = self._build(f.type, values)
            else:
                out[f.name] = [self._build(f.type, values) for _ in range(f.count)]
        return out

    def pack(self, struct_name: str, value: dict | None = None) -> bytes:
        """Nested dict -> bytes. Missing fields are zero, short arrays are zero-padded."""
        flat: list = []
        self._flatten(struct_name, value or {}, flat)
        return self.format(struct_name).pack(*flat)

    def _flatten(self, struct_name: str, value: dict, flat: list) -> None:
        s = self.structs[struct_name]
        unknown = set(value) - {f.name for f in s.fields}
        if unknown:
            raise KeyError(f"{struct_name} has no field(s) {sorted(unknown)}")
        for f in s.fields:
            v = value.get(f.name)
            if f.type in SCALARS:
                zero = 0.0 if SCALARS[f.type][0] in "fd" else 0
                if f.count is None:
                    flat.append(zero if v is None else v)
                else:
                    items = list(v or [])
                    if len(items) > f.count:
                        raise ValueError(f"{struct_name}.{f.name}: {len(items)} items, room for {f.count}")
                    flat.extend(items + [zero] * (f.count - len(items)))
            elif f.count is None:
                self._flatten(f.type, v or {}, flat)
            else:
                items = list(v or [])
                if len(items) > f.count:
                    raise ValueError(f"{struct_name}.{f.name}: {len(items)} items, room for {f.count}")
                for item in items:
                    self._flatten(f.type, item, flat)
                for _ in range(f.count - len(items)):
                    self._flatten(f.type, {}, flat)

    # ---- output ----

    def describe(self) -> str:
        lines = ["constants:"]
        for name, value in self.constants.items():
            lines.append(f"  {name} = {value} (0x{value:X})")
        for s in self.structs.values():
            lines.append(f"\nstruct {s.name}  size {s.size}  align {s.align}")
            for f in s.fields:
                suffix = "" if f.count is None else f"[{f.count}]"
                lines.append(f"  {f.offset:6d}  {f.type} {f.name}{suffix}  ({f.size} bytes)")
        return "\n".join(lines)

    def emit_check(self) -> str:
        out = [
            "// Generated by tools/gr_layout.py --emit-check. Do not edit.",
            "// If this fails to compile, the Python parse of gr_protocol.h disagrees with",
            "// the compiler: fix the parser or simplify the header, do not edit this file.",
            '#include "gr_protocol.h"',
            "",
        ]
        for name, value in self.constants.items():
            out.append(f"static_assert(static_cast<unsigned long long>({name}) == {value}ull);")
        for s in self.structs.values():
            out.append(f"static_assert(sizeof({s.name}) == {s.size});")
            out.append(f"static_assert(alignof({s.name}) == {s.align});")
            for f in s.fields:
                out.append(f"static_assert(offsetof({s.name}, {f.name}) == {f.offset});")
                out.append(f"static_assert(sizeof((({s.name}*)nullptr)->{f.name}) == {f.size});")
        return "\n".join(out) + "\n"


def _align_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--header", type=Path, default=HEADER)
    parser.add_argument("--emit-check", type=Path, metavar="FILE",
                        help="write a C++ file of static_asserts for the parsed layout")
    args = parser.parse_args()

    try:
        layout = Layout(args.header)
    except (LayoutError, OSError) as e:
        print(f"gr_layout: {e}", file=sys.stderr)
        return 1

    if args.emit_check:
        args.emit_check.parent.mkdir(parents=True, exist_ok=True)
        args.emit_check.write_text(layout.emit_check(), encoding="utf-8")
    else:
        print(layout.describe())
    return 0


if __name__ == "__main__":
    sys.exit(main())
