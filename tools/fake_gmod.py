"""Stand-in for the GMod module, so the RDR2 half can be developed without GMod.

Opens the shared memory as the guest and plays a scripted player:

  1. on connect, stands where the host says its player ped is,
  2. walks forward (north) for --walk-seconds,
  3. grabs entity 0 of the host's list, as the physgun would: pulls it to a point two
     metres in front of the eyes and holds it for --hold-seconds,
  4. throws it forward and up, lets go, and then stands still watching it.

    python tools/fake_gmod.py                      run until Ctrl+C
    python tools/fake_gmod.py --seconds 12
    python tools/fake_gmod.py --protocol-version 999    play a mismatched build

Everything it writes is in wire conventions (RDR2 metres and rotations). The unit
conversion the real module does is tested on its own in protocol/tests.
"""

from __future__ import annotations

import argparse
import math
import sys
import time

from gr_shm import DEFAULT_NAME, Link, now_us

WALK_SPEED = 200 * 0.01905      # GMod's default walk speed, 200 units/s, in m/s
EYE_HEIGHT = 64 * 0.01905       # GMod's standing eye height, 64 units, in metres
HOLD_DISTANCE = 2.0             # metres in front of the eyes
PULL_SECONDS = 0.5              # how long the grab takes to pull the entity in
THROW_VELOCITY = (0.0, 12.0, 4.0)
THROW_FLIGHT_SECONDS = 0.3      # the proxy stays GMod-driven this long after the throw
GRAVITY = 9.81


def vec(v):
    return {"x": v[0], "y": v[1], "z": v[2]}


def main() -> int:
    parser = argparse.ArgumentParser(description="Fake GMod guest for the Garry's Redemption bridge.")
    parser.add_argument("--seconds", type=float, default=0.0, help="stop after this long (default: run until Ctrl+C)")
    parser.add_argument("--fps", type=float, default=60.0)
    parser.add_argument("--name", default=DEFAULT_NAME, help="shared memory name (tests use their own)")
    parser.add_argument("--protocol-version", type=int, default=None, help="announce this version instead of the real one")
    parser.add_argument("--walk-seconds", type=float, default=1.5)
    parser.add_argument("--hold-seconds", type=float, default=2.0)
    parser.add_argument("--quiet", action="store_true", help="only print state changes, events and the summary")
    args = parser.parse_args()

    link = Link("guest", args.name, protocol_version=args.protocol_version)
    c = link.layout.const
    link.open()
    print(f"fake_gmod: guest, protocol {link.version}, shm {link.shm_size} bytes, "
          f"{'created' if link.mapping.created else 'opened'} {args.name}", flush=True)

    stats = {"connected": 0, "frames_read": 0, "entities_seen": 0, "grabbed_handle": 0,
             "saw_driven": 0, "saw_ragdoll": 0, "saw_recovered": 0, "walked": 0.0,
             "last_host_frame": 0}
    last_state = "closed"
    phase = "wait"          # wait -> walk -> hold -> flight -> watch
    phase_start = 0.0
    pos = [0.0, 0.0, 0.0]
    start_pos = [0.0, 0.0, 0.0]
    vel = [0.0, 0.0, 0.0]
    held = None             # {"handle", "from", "pos", "vel"}
    anchor_serial = 0       # the host's serial we anchored under, 0 = not anchored
    frame = 0
    dt = 1.0 / args.fps
    start = time.perf_counter()
    next_tick = start
    next_report = 0.0

    def say(t, text):
        print(f"fake_gmod: [{t:7.3f}] {text}", flush=True)

    try:
        while True:
            t = time.perf_counter() - start
            if args.seconds and t >= args.seconds:
                break
            frame += 1

            state = link.tick()
            if state != last_state:
                p = link.peer
                say(t, f"link {last_state} -> {state} (peer protocol {p['protocol_version']}, "
                       f"shm {p['shm_size']}, state {p['state']}, reason {p['reason']})")
                if state == "connected":
                    stats["connected"] += 1
                elif last_state == "connected":
                    # The host went away. Whatever was held is gone with it, and so is
                    # the anchor: start over when it comes back.
                    phase, held, anchor_serial = "wait", None, 0
                    stats["grabbed_handle"] = 0
                last_state = state

            host = link.read()
            if host:
                stats["frames_read"] += 1
                stats["last_host_frame"] = host["frame"]
                stats["entities_seen"] = max(stats["entities_seen"], host["entity_count"])
            entities = host["entities"][:host["entity_count"]] if host else []

            # The host names each stretch of GMod owning the player. A new serial, or the
            # host taking the player itself, means anchoring afresh where its ped stands.
            if phase != "wait" and host and (host["anchor_serial"] != anchor_serial
                                             or host["flags"] & c("GR_HOSTF_RDR2_OWNS_PLAYER")):
                phase, held, anchor_serial = "wait", None, 0
                say(t, "anchor dropped: the host owns the player or handed it back")

            if phase == "wait" and host and host["frame"] > 0 and host["anchor_serial"] != 0                     and not host["flags"] & c("GR_HOSTF_RDR2_OWNS_PLAYER"):
                anchor_serial = host["anchor_serial"]
                pos = [host["ped_pos"]["x"], host["ped_pos"]["y"], host["ped_pos"]["z"]]
                start_pos = list(pos)
                phase, phase_start = "walk", t
                say(t, f"anchored at host ped ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f}), walking north")

            eye = [pos[0], pos[1], pos[2] + EYE_HEIGHT]
            hold_point = [eye[0], eye[1] + HOLD_DISTANCE, eye[2]]  # yaw 0 looks along +Y
            vel = [0.0, 0.0, 0.0]

            if phase == "walk":
                vel = [0.0, WALK_SPEED, 0.0]
                pos[1] += WALK_SPEED * dt
                if t - phase_start >= args.walk_seconds:
                    if entities:
                        e = entities[0]
                        held = {"handle": e["handle"],
                                "from": [e["pos"]["x"], e["pos"]["y"], e["pos"]["z"]],
                                "pos": [e["pos"]["x"], e["pos"]["y"], e["pos"]["z"]],
                                "vel": [0.0, 0.0, 0.0]}
                        stats["grabbed_handle"] = e["handle"]
                        phase, phase_start = "hold", t
                        say(t, f"stopped after {pos[1] - start_pos[1]:.2f} m, grabbing entity 0 (handle {e['handle']})")
                    elif host:
                        phase, phase_start = "watch", t
                        say(t, "host lists no entities, nothing to grab")

            elif phase == "hold":
                k = min(1.0, (t - phase_start) / PULL_SECONDS)
                k = k * k * (3.0 - 2.0 * k)  # ease in and out, like a physgun pull
                target = [held["from"][i] + (hold_point[i] - held["from"][i]) * k for i in range(3)]
                held["vel"] = [(target[i] - held["pos"][i]) / dt for i in range(3)]
                held["pos"] = target
                held["flags"] = c("GR_DRIVE_HELD")
                if t - phase_start >= args.hold_seconds:
                    held["vel"] = list(THROW_VELOCITY)
                    held["flags"] = 0
                    phase, phase_start = "flight", t
                    say(t, f"throwing handle {held['handle']} at "
                           f"{math.sqrt(sum(v * v for v in THROW_VELOCITY)):.2f} m/s")

            elif phase == "flight":
                held["vel"][2] -= GRAVITY * dt
                for i in range(3):
                    held["pos"][i] += held["vel"][i] * dt
                if t - phase_start >= THROW_FLIGHT_SECONDS:
                    say(t, f"released handle {held['handle']}: it is RDR2's again")
                    held = None
                    phase, phase_start = "watch", t

            # What does the host say about the entity we grabbed?
            if stats["grabbed_handle"]:
                for e in entities:
                    if e["handle"] != stats["grabbed_handle"]:
                        continue
                    if e["flags"] & c("GR_ENTF_DRIVEN") and not stats["saw_driven"]:
                        stats["saw_driven"] = 1
                        say(t, "host reports the entity as driven")
                    if e["flags"] & c("GR_ENTF_RAGDOLL") and not stats["saw_ragdoll"]:
                        stats["saw_ragdoll"] = 1
                        say(t, "host reports the entity as ragdolled")
                    if phase == "watch" and stats["saw_ragdoll"] and not stats["saw_recovered"] \
                            and not e["flags"] & (c("GR_ENTF_RAGDOLL") | c("GR_ENTF_DRIVEN")):
                        stats["saw_recovered"] = 1
                        say(t, f"host reports the entity back on its feet at "
                               f"({e['pos']['x']:.2f}, {e['pos']['y']:.2f}, {e['pos']['z']:.2f})")

            stats["walked"] = pos[1] - start_pos[1]
            driven = []
            if held:
                driven.append({"handle": held["handle"], "flags": held.get("flags", c("GR_DRIVE_HELD")),
                               "pos": vec(held["pos"]), "vel": vec(held["vel"])})

            link.publish({
                "frame": frame,
                "time_us": now_us(),
                "host_frame_seen": stats["last_host_frame"],
                "anchor_serial": anchor_serial,
                # Until anchored, pos is meaningless and the host must not follow it.
                "flags": 0 if phase == "wait" else c("GR_GUESTF_PLAYER_VALID") | c("GR_GUESTF_ON_GROUND"),
                "pos": vec(pos),
                "vel": vec(vel),
                "eye_pos": vec([pos[0], pos[1], pos[2] + EYE_HEIGHT]),
                "eye_rot": vec([0.0, 0.0, 0.0]),
                "fov": 0.0,
                "driven_count": len(driven),
                "driven": driven,
            })

            if not args.quiet and host and t >= next_report:
                next_report = t + 1.0
                say(t, f"host frame {host['frame']}, {host['entity_count']} entities, phase {phase}, "
                       f"player at ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f})")

            next_tick += dt
            delay = next_tick - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.perf_counter()
    except KeyboardInterrupt:
        pass

    print(f"summary state={link.state} connected={stats['connected']} frames_read={stats['frames_read']} "
          f"entities_seen={stats['entities_seen']} grabbed_handle={stats['grabbed_handle']} "
          f"saw_driven={stats['saw_driven']} saw_ragdoll={stats['saw_ragdoll']} "
          f"saw_recovered={stats['saw_recovered']} walked={stats['walked']:.2f} "
          f"last_host_frame={stats['last_host_frame']} torn_reads={link.torn_reads}", flush=True)
    link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
