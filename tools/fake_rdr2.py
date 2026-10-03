"""Stand-in for the RDR2 plugin, so the GMod half can be developed without RDR2.

Creates the shared memory as the host and plays a small scene on flat ground: three peds
(one standing, one walking a circle, one pacing) and one horse. It reacts to the guest
the way the real plugin is meant to:

  - the hidden player ped follows the guest's player position,
  - an entity the guest lists as driven is ragdolled and follows the proxy,
  - when it leaves the driven list it keeps its last velocity, falls, slides to a stop,
    and a moment later "stands up" and goes back to what it was doing.

    python tools/fake_rdr2.py                      run until Ctrl+C
    python tools/fake_rdr2.py --seconds 10
    python tools/fake_rdr2.py --hold-key W         hold a key down in the input block
    python tools/fake_rdr2.py --protocol-version 999    play a mismatched build
    python tools/fake_rdr2.py --multiplayer        play a host that refuses to run

The models are made-up names, not RDR2 models: this fake ships and needs no game data.
"""

from __future__ import annotations

import argparse
import math
import sys
import time

from gr_shm import DEFAULT_NAME, Link, joaat, now_ms, now_us

# An arbitrary spot a few kilometres from the world origin, so anything that forgets the
# floating origin shows up straight away.
SCENE = (1500.0, -2200.0, 60.0)
GROUND_Z = SCENE[2]
GRAVITY = 9.81
SETTLE_SPEED = 0.2      # m/s. Below this a released ragdoll counts as at rest.
SETTLE_SECONDS = 1.0    # At rest this long, it stands up and RDR2 owns it again.


class Entity:
    def __init__(self, handle, model, type_name, home, bounds, health, mover):
        self.handle = handle
        self.model = joaat(model)
        self.type_name = type_name
        self.home = home
        self.bounds_min, self.bounds_max = bounds
        self.health = health
        self.mover = mover          # f(entity, t) -> (pos, vel, yaw) while RDR2 owns it
        self.pos = list(home)
        self.vel = [0.0, 0.0, 0.0]
        self.rot = [0.0, 0.0, 0.0]  # pitch, roll, yaw
        self.mode = "ai"            # ai | driven | ragdoll
        self.frozen = False
        self.rest_time = 0.0

    def half_height(self):
        return -self.bounds_min[2]


def stand(e, t):
    return (e.home[0], e.home[1], e.home[2]), (0.0, 0.0, 0.0), 180.0


def circle(radius, speed):
    def mover(e, t):
        a = t * speed / radius
        pos = (e.home[0] + radius * math.cos(a), e.home[1] + radius * math.sin(a), e.home[2])
        vel = (-speed * math.sin(a), speed * math.cos(a), 0.0)
        # RDR2 heading 0 faces +Y and grows anticlockwise.
        return pos, vel, math.degrees(math.atan2(-vel[0], vel[1]))
    return mover


def pace(length, speed):
    def mover(e, t):
        period = 2.0 * length / speed
        phase = (t % period) / period
        going = phase < 0.5
        along = (phase * 2.0 if going else 2.0 - phase * 2.0) * length
        pos = (e.home[0] + along, e.home[1], e.home[2])
        return pos, (speed if going else -speed, 0.0, 0.0), -90.0 if going else 90.0
    return mover


def build_scene():
    ped = ((-0.3, -0.25, -0.9), (0.3, 0.25, 0.9))
    horse = ((-0.35, -1.2, -0.85), (0.35, 1.2, 0.85))
    x, y, z = SCENE
    return [
        Entity(1001, "gr_fake_ped_standing", "PED", (x, y + 6.0, z + 0.9), ped, 100.0, stand),
        Entity(1002, "gr_fake_ped_circling", "PED", (x + 8.0, y + 4.0, z + 0.9), ped, 100.0, circle(3.0, 1.2)),
        Entity(1003, "gr_fake_ped_pacing", "PED", (x - 8.0, y + 2.0, z + 0.9), ped, 100.0, pace(6.0, 1.4)),
        Entity(2001, "gr_fake_horse", "HORSE", (x + 3.0, y + 10.0, z + 0.85), horse, 300.0, stand),
    ]


def vec(v):
    return {"x": v[0], "y": v[1], "z": v[2]}


def main() -> int:
    parser = argparse.ArgumentParser(description="Fake RDR2 host for the Garry's Redemption bridge.")
    parser.add_argument("--seconds", type=float, default=0.0, help="stop after this long (default: run until Ctrl+C)")
    parser.add_argument("--fps", type=float, default=60.0)
    parser.add_argument("--name", default=DEFAULT_NAME, help="shared memory name (tests use their own)")
    parser.add_argument("--protocol-version", type=int, default=None, help="announce this version instead of the real one")
    parser.add_argument("--multiplayer", action="store_true", help="refuse to run, as the real plugin does in a multiplayer session")
    parser.add_argument("--hold-key", default=None, metavar="KEY", help="hold this key down in the input block: a letter, or a virtual-key code like 0x20")
    parser.add_argument("--quiet", action="store_true", help="only print state changes and the summary")
    args = parser.parse_args()

    link = Link("host", args.name, protocol_version=args.protocol_version)
    c = link.layout.const
    if args.multiplayer:
        link.refuse(c("GR_REASON_MULTIPLAYER"))
    link.open()
    print(f"fake_rdr2: host, protocol {link.version}, shm {link.shm_size} bytes, "
          f"{'created' if link.mapping.created else 'opened'} {args.name}", flush=True)

    keys = bytearray(c("GR_KEY_BYTES"))
    if args.hold_key:
        vk = int(args.hold_key, 0) if args.hold_key[:1].isdigit() else ord(args.hold_key.upper())
        keys[vk // 8] |= 1 << (vk % 8)

    type_id = {"PED": c("GR_ENT_PED"), "HORSE": c("GR_ENT_HORSE")}
    entities = build_scene()
    by_handle = {e.handle: e for e in entities}
    ped_pos = [SCENE[0], SCENE[1], SCENE[2]]
    ped_rot = [0.0, 0.0, 0.0]
    # Like the real plugin: a fresh, never-zero serial per run, so a guest still anchored
    # to an earlier run cannot drive this one's ped.
    anchor_serial = (time.perf_counter_ns() & 0x7FFFFFFF) | 1

    stats = {"connected": 0, "grabs": 0, "releases": 0, "recoveries": 0, "frames_read": 0,
             "last_guest_frame": 0, "max_release_speed": 0.0, "player_y": 0.0}
    last_state = "closed"
    driven = {}
    frame = 0
    dt = 1.0 / args.fps
    start = time.perf_counter()
    next_tick = start
    next_report = 0.0

    try:
        while True:
            t = time.perf_counter() - start
            if args.seconds and t >= args.seconds:
                break
            frame += 1

            state = link.tick()
            if state != last_state:
                p = link.peer
                print(f"fake_rdr2: [{t:7.3f}] link {last_state} -> {state} "
                      f"(peer protocol {p['protocol_version']}, shm {p['shm_size']}, "
                      f"state {p['state']}, reason {p['reason']})", flush=True)
                if state == "connected":
                    stats["connected"] += 1
                last_state = state

            guest = link.read()
            if guest:
                stats["frames_read"] += 1
                stats["last_guest_frame"] = guest["frame"]
                driven = {d["handle"]: d for d in guest["driven"][:guest["driven_count"]]}
                # The real plugin teleports the hidden player ped to the GMod player, but
                # only once the guest has a position of its own to offer.
                valid = bool(guest["flags"] & c("GR_GUESTF_PLAYER_VALID"))                     and guest["anchor_serial"] == anchor_serial
                if valid:
                    ped_pos = [guest["pos"]["x"], guest["pos"]["y"], guest["pos"]["z"]]
                    ped_rot = [0.0, 0.0, guest["eye_rot"]["z"]]
                    stats["player_y"] = ped_pos[1] - SCENE[1]
                if not args.quiet and t >= next_report:
                    next_report = t + 1.0
                    print(f"fake_rdr2: [{t:7.3f}] guest frame {guest['frame']} player "
                          f"{'at' if valid else 'not anchored yet, ped stays at'} "
                          f"({ped_pos[0]:.2f}, {ped_pos[1]:.2f}, {ped_pos[2]:.2f}), "
                          f"{guest['driven_count']} driven", flush=True)
            elif state != "connected":
                # No guest: nothing is driven any more, so anything held gets dropped.
                driven = {}
            # else: connected but the read was torn. Keep last frame's driven list.

            for e in entities:
                d = driven.get(e.handle)
                if d is not None:
                    if e.mode != "driven":
                        e.mode = "driven"
                        stats["grabs"] += 1
                        print(f"fake_rdr2: [{t:7.3f}] entity {e.handle} grabbed -> ragdoll, following proxy", flush=True)
                    e.frozen = bool(d["flags"] & c("GR_DRIVE_FROZEN"))
                    e.pos = [d["pos"]["x"], d["pos"]["y"], d["pos"]["z"]]
                    e.vel = [0.0, 0.0, 0.0] if e.frozen else [d["vel"]["x"], d["vel"]["y"], d["vel"]["z"]]
                    e.rot = [d["rot"]["x"], d["rot"]["y"], d["rot"]["z"]]
                elif e.mode == "driven":
                    e.mode = "ragdoll"
                    e.frozen = False
                    e.rest_time = 0.0
                    speed = math.sqrt(sum(v * v for v in e.vel))
                    stats["releases"] += 1
                    stats["max_release_speed"] = max(stats["max_release_speed"], speed)
                    print(f"fake_rdr2: [{t:7.3f}] entity {e.handle} released at {speed:.2f} m/s "
                          f"({e.vel[0]:.2f}, {e.vel[1]:.2f}, {e.vel[2]:.2f})", flush=True)

                if e.mode == "ragdoll":
                    floor = GROUND_Z + e.half_height()
                    e.vel[2] -= GRAVITY * dt
                    for i in range(3):
                        e.pos[i] += e.vel[i] * dt
                    if e.pos[2] <= floor:
                        e.pos[2] = floor
                        e.vel[2] = 0.0
                        drag = max(0.0, 1.0 - 4.0 * dt)  # sliding friction
                        e.vel[0] *= drag
                        e.vel[1] *= drag
                    speed = math.sqrt(sum(v * v for v in e.vel))
                    e.rest_time = e.rest_time + dt if speed < SETTLE_SPEED else 0.0
                    if e.rest_time >= SETTLE_SECONDS:
                        e.mode = "ai"
                        e.home = (e.pos[0], e.pos[1], floor)
                        stats["recoveries"] += 1
                        print(f"fake_rdr2: [{t:7.3f}] entity {e.handle} settled -> stands up, RDR2 owns it again", flush=True)
                elif e.mode == "ai":
                    pos, vel, yaw = e.mover(e, t)
                    e.pos, e.vel, e.rot = list(pos), list(vel), [0.0, 0.0, yaw]

            out = []
            for e in entities:
                flags = 0
                if e.mode in ("driven", "ragdoll"):
                    flags |= c("GR_ENTF_RAGDOLL")
                if e.mode == "driven":
                    flags |= c("GR_ENTF_DRIVEN")
                if e.frozen:
                    flags |= c("GR_ENTF_FROZEN")
                out.append({"handle": e.handle, "model": e.model, "type": type_id[e.type_name],
                            "flags": flags, "pos": vec(e.pos), "rot": vec(e.rot), "vel": vec(e.vel),
                            "bounds_min": vec(e.bounds_min), "bounds_max": vec(e.bounds_max),
                            "health": e.health})

            link.publish({
                "frame": frame,
                "time_us": now_us(),
                "anchor_serial": anchor_serial,
                "game_time_ms": now_ms() & 0xFFFFFFFF,
                "flags": c("GR_HOSTF_FOCUSED"),
                "dt": dt,
                "cam_fov": 55.0,
                "ped_pos": vec(ped_pos),
                "ped_rot": vec(ped_rot),
                "input": {"keys": bytes(keys), "screen_w": 1920, "screen_h": 1080,
                          "cursor_x": 960, "cursor_y": 540,
                          "flags": c("GR_INPUT_CAPTURED")},
                "entity_count": len(out),
                "entities": out,
            })

            next_tick += dt
            delay = next_tick - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.perf_counter()
    except KeyboardInterrupt:
        pass

    print(f"summary state={link.state} connected={stats['connected']} grabs={stats['grabs']} "
          f"releases={stats['releases']} recoveries={stats['recoveries']} "
          f"frames_read={stats['frames_read']} last_guest_frame={stats['last_guest_frame']} "
          f"max_release_speed={stats['max_release_speed']:.2f} player_north={stats['player_y']:.2f} "
          f"torn_reads={link.torn_reads}", flush=True)
    link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
