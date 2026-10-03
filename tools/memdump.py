"""Prints the live bridge shared memory as text.

Read-only and passive: it opens the mapping only if one of the sides has already created
it, and never writes to it. Use it before asking anyone to describe what they see in-game.

    python tools/memdump.py              one snapshot
    python tools/memdump.py --watch      refresh twice a second until Ctrl+C
    python tools/memdump.py --json       one snapshot as JSON
    python tools/memdump.py --all        also show rotation and bounds of every entity
"""

from __future__ import annotations

import argparse
import json
import sys
import time

from gr_layout import Layout
from gr_shm import DEFAULT_NAME, Mapping, seq_read

PEER_STATES = {0: "absent", 1: "alive", 2: "refused", 3: "gone"}
REASONS = {0: "none", 1: "protocol version mismatch", 2: "multiplayer session"}
ENTITY_TYPES = {0: "none", 1: "ped", 2: "animal", 3: "horse", 4: "vehicle", 5: "object"}


def flag_names(layout: Layout, prefix: str, value: int) -> str:
    names = [name[len(prefix):].lower() for name, bit in layout.constants.items()
             if name.startswith(prefix) and value & bit]
    return "|".join(names) if names else "-"


def v3(v: dict) -> str:
    return f"({v['x']:10.3f}, {v['y']:10.3f}, {v['z']:8.3f})"


def snapshot(layout: Layout, name: str) -> dict:
    """Reads everything that can be read. Samples the heartbeats twice to see who is beating."""
    mapping = Mapping(name, create=False)
    try:
        header_size = layout.const("GR_HEADER_SIZE")
        first = layout.unpack("GrHeader", mapping.read(0, header_size))
        time.sleep(0.25)
        header = layout.unpack("GrHeader", mapping.read(0, header_size))
        out = {"name": name, "mapped_bytes": mapping.size, "header": header, "beating": {}}
        for side in ("host", "guest"):
            out["beating"][side] = header[side]["heartbeat"] != first[side]["heartbeat"]

        # The frames can only be decoded if their writer uses this checkout's layout.
        mine = (layout.const("GR_PROTOCOL_VERSION"), layout.const("GR_SHM_SIZE"))
        for side, struct_name in (("host", "GrHostFrame"), ("guest", "GrGuestFrame")):
            peer = header[side]
            if peer["magic"] != layout.const("GR_MAGIC"):
                out[side] = None
                continue
            if (peer["protocol_version"], peer["shm_size"]) != mine or mapping.size < mine[1]:
                out[side] = "layout-mismatch"
                continue
            data = seq_read(mapping, layout.offset("GrShared", side), layout.sizeof(struct_name), tries=64)
            out[side] = layout.unpack(struct_name, data) if data else "torn"
        return out
    finally:
        mapping.close()


def render(layout: Layout, snap: dict, show_all: bool) -> str:
    lines = [f"bridge {snap['name']}  ({snap['mapped_bytes']} bytes mapped, this checkout: protocol "
             f"{layout.const('GR_PROTOCOL_VERSION')}, {layout.const('GR_SHM_SIZE')} bytes)"]

    for side, label in (("host", "host  (RDR2)"), ("guest", "guest (GMod)")):
        p = snap["header"][side]
        if p["magic"] != layout.const("GR_MAGIC"):
            lines.append(f"  {label}: never opened the link")
            continue
        beat = "beating" if snap["beating"][side] else "SILENT"
        reason = f", reason: {REASONS.get(p['reason'], p['reason'])}" if p["reason"] else ""
        lines.append(f"  {label}: {PEER_STATES.get(p['state'], p['state'])}{reason}, {beat}, "
                     f"protocol {p['protocol_version']}, shm {p['shm_size']}, pid {p['pid']}, "
                     f"heartbeat {p['heartbeat']}, epoch {p['epoch']:08x}")

    for side in ("host", "guest"):
        frame = snap[side]
        if frame is None:
            continue
        if frame == "layout-mismatch":
            lines.append(f"\n{side} frame: written with a different layout than this checkout, not decoded")
            continue
        if frame == "torn":
            lines.append(f"\n{side} frame: could not get a consistent copy (writer stuck mid-write?)")
            continue
        lines.append("")
        lines.extend(render_host(layout, frame, show_all) if side == "host"
                     else render_guest(layout, frame))
    return "\n".join(lines)


def render_host(layout: Layout, f: dict, show_all: bool) -> list[str]:
    inp = f["input"]
    down = [f"0x{vk:02X}" for vk in range(256) if inp["keys"][vk // 8] >> (vk % 8) & 1]
    lines = [
        f"host frame {f['frame']}  seq {f['seq']}  time {f['game_time_ms']} ms  dt {f['dt'] * 1000:.2f} ms  "
        f"flags {flag_names(layout, 'GR_HOSTF_', f['flags'])}  fov {f['cam_fov']:.1f}",
        f"  player ped  pos {v3(f['ped_pos'])}  rot {v3(f['ped_rot'])}",
        f"  input       keys down [{' '.join(down) if down else 'none'}]  mouse total ({inp['mouse_dx']}, {inp['mouse_dy']})  "
        f"wheel {inp['wheel']}  cursor ({inp['cursor_x']}, {inp['cursor_y']}) of {inp['screen_w']}x{inp['screen_h']}  "
        f"flags {flag_names(layout, 'GR_INPUT_', inp['flags'])}",
        f"  entities    {f['entity_count']}",
    ]
    for e in f["entities"][:min(f["entity_count"], len(f["entities"]))]:
        lines.append(f"    {e['handle']:8d}  {ENTITY_TYPES.get(e['type'], e['type']):8s} model {e['model']:08X}  "
                     f"pos {v3(e['pos'])}  vel {v3(e['vel'])}  hp {e['health']:6.1f}  "
                     f"{flag_names(layout, 'GR_ENTF_', e['flags'])}")
        if show_all:
            lines.append(f"              rot {v3(e['rot'])}  bounds {v3(e['bounds_min'])} .. {v3(e['bounds_max'])}")
    return lines


def render_guest(layout: Layout, f: dict) -> list[str]:
    lines = [
        f"guest frame {f['frame']}  seq {f['seq']}  last host frame seen {f['host_frame_seen']}  "
        f"flags {flag_names(layout, 'GR_GUESTF_', f['flags'])}  fov {f['fov']:.1f}",
        f"  player      pos {v3(f['pos'])}  vel {v3(f['vel'])}",
        f"  eyes        pos {v3(f['eye_pos'])}  rot {v3(f['eye_rot'])}",
        f"  driven      {f['driven_count']}",
    ]
    for d in f["driven"][:min(f["driven_count"], len(f["driven"]))]:
        lines.append(f"    {d['handle']:8d}  pos {v3(d['pos'])}  vel {v3(d['vel'])}  "
                     f"{flag_names(layout, 'GR_DRIVE_', d['flags'])}")
    return lines


def to_jsonable(value):
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, dict):
        return {k: to_jsonable(v) for k, v in value.items()}
    if isinstance(value, list):
        return [to_jsonable(v) for v in value]
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Print the Garry's Redemption bridge shared memory.")
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--watch", action="store_true", help="refresh until Ctrl+C")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--all", action="store_true", help="show every entity field")
    args = parser.parse_args()

    layout = Layout()
    try:
        while True:
            try:
                snap = snapshot(layout, args.name)
            except FileNotFoundError:
                text = (f"bridge {args.name}: the shared memory does not exist.\n"
                        "Neither the RDR2 plugin nor the GMod module (nor a fake) is running.")
                if not args.watch:
                    print(text)
                    return 1
            else:
                if args.json:
                    for side in ("host", "guest"):  # trim the unused tail of the big arrays
                        if isinstance(snap[side], dict):
                            key, count = ("entities", "entity_count") if side == "host" else ("driven", "driven_count")
                            snap[side][key] = snap[side][key][:snap[side][count]]
                    text = json.dumps(to_jsonable(snap), indent=2)
                else:
                    text = render(layout, snap, args.all)
            if not args.watch:
                print(text)
                return 0
            print("\x1b[2J\x1b[H" + text, flush=True)
            time.sleep(0.25)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
