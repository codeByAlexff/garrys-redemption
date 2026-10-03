"""End-to-end tests of the bridge over real shared memory, in separate processes.

  - the two Python fakes against each other,
  - each Python fake against gr_probe, which is the real C++ link code,
  - whatever real binaries have been built (the .asi in its test harness),
  - memdump against a live session.

Each test uses its own mapping name, so they never disturb a real running bridge.
The C++ parts are skipped, loudly, if they have not been built yet.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import time
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent.parent
REPO = TOOLS.parent
PROBE = REPO / "protocol" / "build" / "Release" / "gr_probe.exe"
ASI_RUNNER = REPO / "rdr2" / "build" / "harness" / "Release" / "asi_runner.exe"
ASI = REPO / "rdr2" / "build" / "Release" / "GarrysRedemption.asi"

ASI_LOG = ASI.with_name("GarrysRedemption.log")  # the plugin logs next to itself

sys.path.insert(0, str(TOOLS))
from gr_layout import Layout  # noqa: E402

PROTOCOL = Layout().const("GR_PROTOCOL_VERSION")

_counter = 0


def read_asi_log() -> str:
    return ASI_LOG.read_text(encoding="utf-8", errors="replace") if ASI_LOG.exists() else ""


def unique_name() -> str:
    global _counter
    _counter += 1
    return f"Local\\GRPyTest_{os.getpid()}_{_counter}"


def spawn(args, env=None) -> subprocess.Popen:
    return subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, cwd=str(REPO), env=env)


def py(script: str, *args) -> list:
    return [sys.executable, TOOLS / script, *args]


def finish(proc: subprocess.Popen, timeout: float = 40.0) -> tuple[str, dict]:
    """Waits for the process and returns (all output, the key=value pairs of its summary line)."""
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        raise AssertionError(f"process did not exit:\n{out}")
    summary = {}
    for line in out.splitlines():
        if line.startswith("summary "):
            summary = dict(pair.split("=", 1) for pair in line.split()[1:])
    return out, summary


class FakesTogether(unittest.TestCase):
    def test_scripted_scene_plays_out(self):
        name = unique_name()
        host = spawn(py("fake_rdr2.py", "--seconds", "9", "--name", name, "--quiet"))
        time.sleep(0.5)
        guest = spawn(py("fake_gmod.py", "--seconds", "8", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        host_out, h = finish(host)
        detail = f"\n--- fake_rdr2 ---\n{host_out}\n--- fake_gmod ---\n{guest_out}"

        self.assertEqual(g["state"], "connected", detail)
        self.assertEqual(g["connected"], "1", detail)
        self.assertEqual(g["entities_seen"], "4", detail)
        self.assertEqual(g["grabbed_handle"], "1001", detail)
        self.assertEqual((g["saw_driven"], g["saw_ragdoll"], g["saw_recovered"]), ("1", "1", "1"), detail)
        # A torn read is legal: the reader gave up after its bounded retries and kept its
        # previous frame. On a busy machine one can happen, so only require them to be rare.
        for side in (g, h):
            self.assertLessEqual(int(side["torn_reads"]), int(side["frames_read"]) // 100, detail)

        self.assertEqual((h["grabs"], h["releases"], h["recoveries"]), ("1", "1", "1"), detail)
        # The throw is 12 m/s forward and 4 up; gravity has had 0.3 s at it by release.
        self.assertGreater(float(h["max_release_speed"]), 11.0, detail)
        # The guest anchored to the host's ped rather than the other way round, and the
        # host followed it north.
        self.assertIn("anchored at host ped (1500.00, -2200.00, 60.00)", guest_out)
        self.assertAlmostEqual(float(h["player_north"]), float(g["walked"]), delta=0.2, msg=detail)
        self.assertGreater(float(g["walked"]), 5.0, detail)
        # The guest left first, cleanly, and the host noticed.
        self.assertEqual(h["state"], "waiting", detail)

    def test_guest_first_then_host(self):
        name = unique_name()
        guest = spawn(py("fake_gmod.py", "--seconds", "4", "--name", name, "--quiet"))
        time.sleep(0.7)
        host = spawn(py("fake_rdr2.py", "--seconds", "2.5", "--name", name, "--quiet"))
        host_out, h = finish(host)
        guest_out, g = finish(guest)
        detail = f"\n--- fake_rdr2 ---\n{host_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertIn("created", guest_out.splitlines()[0], detail)
        self.assertEqual(h["connected"], "1", detail)
        self.assertEqual(g["connected"], "1", detail)
        self.assertEqual(g["state"], "waiting", detail)

    def test_version_mismatch_is_refused_by_both(self):
        name = unique_name()
        host = spawn(py("fake_rdr2.py", "--seconds", "3", "--name", name, "--quiet"))
        time.sleep(0.4)
        guest = spawn(py("fake_gmod.py", "--seconds", "2", "--name", name, "--quiet", "--protocol-version", "999"))
        guest_out, g = finish(guest)
        host_out, h = finish(host)
        detail = f"\n--- fake_rdr2 ---\n{host_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertIn("-> version-mismatch (peer protocol 999", host_out, detail)
        self.assertIn(f"-> version-mismatch (peer protocol {PROTOCOL},", guest_out, detail)
        self.assertEqual(g["state"], "version-mismatch", detail)
        self.assertEqual((h["connected"], g["connected"]), ("0", "0"), detail)
        self.assertEqual((h["frames_read"], g["frames_read"]), ("0", "0"), detail)

    def test_multiplayer_host_refuses(self):
        name = unique_name()
        host = spawn(py("fake_rdr2.py", "--seconds", "3", "--name", name, "--quiet", "--multiplayer"))
        time.sleep(0.4)
        guest = spawn(py("fake_gmod.py", "--seconds", "2", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        host_out, h = finish(host)
        detail = f"\n--- fake_rdr2 ---\n{host_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertEqual(h["state"], "refusing", detail)
        self.assertEqual(g["state"], "peer-refused", detail)
        self.assertIn("reason 2", guest_out, detail)
        self.assertEqual(g["frames_read"], "0", detail)


class Memdump(unittest.TestCase):
    def test_nothing_running(self):
        result = subprocess.run(py("memdump.py", "--name", unique_name()), capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not exist", result.stdout)

    def test_live_session(self):
        name = unique_name()
        host = spawn(py("fake_rdr2.py", "--seconds", "5", "--name", name, "--quiet", "--hold-key", "W"))
        time.sleep(0.4)
        guest = spawn(py("fake_gmod.py", "--seconds", "4", "--name", name, "--quiet"))
        time.sleep(2.5)  # the guest is holding entity 1001 by now
        text = subprocess.run(py("memdump.py", "--name", name), capture_output=True, text=True).stdout
        as_json = subprocess.run(py("memdump.py", "--name", name, "--json"), capture_output=True, text=True).stdout
        finish(guest)
        finish(host)

        self.assertRegex(text, rf"host  \(RDR2\): alive, beating, protocol {PROTOCOL}")
        self.assertRegex(text, rf"guest \(GMod\): alive, beating, protocol {PROTOCOL}")
        self.assertIn("keys down [0x57]", text)
        self.assertRegex(text, r"1001\s+ped .*ragdoll\|driven")
        self.assertRegex(text, r"2001\s+horse")
        self.assertRegex(text, r"driven\s+1\n\s+1001 .* held")
        self.assertIn("flags player_valid|on_ground", text)

        snap = json.loads(as_json)
        self.assertEqual(snap["host"]["entity_count"], 4)
        self.assertEqual(len(snap["host"]["entities"]), 4)
        self.assertEqual(snap["guest"]["driven"][0]["handle"], 1001)
        self.assertTrue(snap["beating"]["host"] and snap["beating"]["guest"])


@unittest.skipUnless(PROBE.exists(), f"gr_probe not built ({PROBE}): run tools\\build.ps1")
class PythonAgainstCpp(unittest.TestCase):
    """The same handshake and layout, with real C++ on one side."""

    def test_cpp_guest_reads_the_python_host(self):
        name = unique_name()
        host = spawn(py("fake_rdr2.py", "--seconds", "4", "--name", name, "--quiet"))
        time.sleep(0.4)
        probe = spawn([PROBE, "--role", "guest", "--seconds", "2.5", "--name", name])
        probe_out, p = finish(probe)
        host_out, h = finish(host)
        detail = f"\n--- fake_rdr2 ---\n{host_out}\n--- gr_probe ---\n{probe_out}"
        self.assertEqual(p["ever_connected"], "1", detail)
        self.assertEqual(p["peer_entities"], "4", detail)
        self.assertGreater(int(p["frames_read"]), 60, detail)
        self.assertGreater(int(p["last_peer_frame"]), 60, detail)
        self.assertEqual(p["torn_reads"], "0", detail)
        self.assertEqual(h["connected"], "1", detail)
        self.assertGreater(int(h["frames_read"]), 60, detail)

    def test_cpp_host_reads_the_python_guest(self):
        name = unique_name()
        probe = spawn([PROBE, "--role", "host", "--seconds", "5", "--name", name])
        time.sleep(0.4)
        # No entities from the probe, so the fake has nothing to grab, but it must
        # still connect, anchor and publish frames the C++ side can read.
        guest = spawn(py("fake_gmod.py", "--seconds", "3", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        probe_out, p = finish(probe)
        detail = f"\n--- gr_probe ---\n{probe_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertEqual(p["ever_connected"], "1", detail)
        self.assertGreater(int(p["frames_read"]), 60, detail)
        self.assertEqual(g["connected"], "1", detail)
        self.assertIn("host lists no entities", guest_out, detail)
        self.assertEqual(p["state"], "waiting", detail)  # saw the guest leave

    def test_cpp_refuses_a_mismatched_python_peer(self):
        name = unique_name()
        probe = spawn([PROBE, "--role", "host", "--seconds", "3", "--name", name])
        time.sleep(0.4)
        guest = spawn(py("fake_gmod.py", "--seconds", "2", "--name", name, "--quiet", "--protocol-version", "7"))
        guest_out, g = finish(guest)
        probe_out, p = finish(probe)
        detail = f"\n--- gr_probe ---\n{probe_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertIn("-> version-mismatch (peer protocol=7 ", probe_out, detail)
        self.assertEqual(p["ever_connected"], "0", detail)
        self.assertEqual(g["state"], "version-mismatch", detail)
        # The C++ side published its refusal where the Python side could read it.
        self.assertIn("state 2, reason 1", guest_out, detail)

    def test_python_sees_a_cpp_host_refusing_multiplayer(self):
        name = unique_name()
        probe = spawn([PROBE, "--role", "host", "--seconds", "3", "--name", name, "--refuse-multiplayer"])
        time.sleep(0.4)
        guest = spawn(py("fake_gmod.py", "--seconds", "2", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        probe_out, p = finish(probe)
        detail = f"\n--- gr_probe ---\n{probe_out}\n--- fake_gmod ---\n{guest_out}"
        self.assertEqual(p["state"], "refusing", detail)
        self.assertEqual(g["state"], "peer-refused", detail)


@unittest.skipUnless(ASI_RUNNER.exists() and ASI.exists(),
                     f"RDR2 plugin not built ({ASI}): run tools\\build.ps1")
class RealAsiInHarness(unittest.TestCase):
    """The actual GarrysRedemption.asi, loaded by tools/fake_scripthook's runner."""

    def run_asi(self, name, seconds, *extra):
        env = dict(os.environ, GR_SHM_NAME_OVERRIDE=name)
        return spawn([ASI_RUNNER, "--asi", ASI, "--seconds", str(seconds), *extra], env=env)

    def test_asi_connects_to_the_fake_guest(self):
        name = unique_name()
        runner = self.run_asi(name, 6)
        time.sleep(1.0)
        guest = spawn(py("fake_gmod.py", "--seconds", "3", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        runner_out, r = finish(runner)
        log = read_asi_log()
        detail = f"\n--- asi_runner ---\n{runner_out}\n--- log ---\n{log}\n--- fake_gmod ---\n{guest_out}"

        self.assertEqual(g["connected"], "1", detail)
        self.assertGreater(int(g["frames_read"]), 60, detail)
        self.assertIn("link: waiting -> connected", log, detail)
        self.assertIn("link: connected -> waiting", log, detail)
        self.assertRegex(log, rf"protocol {PROTOCOL}, shared memory \d+ bytes", detail)
        # What the plugin drew on screen, as recorded by the fake Script Hook.
        self.assertIn("waiting for GMod", runner_out, detail)
        self.assertIn("connected to GMod", runner_out, detail)
        self.assertEqual(r["unloaded"], "1", detail)
        # The plugin called no native the fake Script Hook has not been taught.
        self.assertEqual(r["unknown_natives"], "0", detail)

    def test_asi_refuses_a_multiplayer_session(self):
        name = unique_name()
        runner = self.run_asi(name, 4, "--multiplayer-after", "1.0")
        time.sleep(0.5)
        guest = spawn(py("fake_gmod.py", "--seconds", "3", "--name", name, "--quiet"))
        guest_out, g = finish(guest)
        runner_out, r = finish(runner)
        log = read_asi_log()
        detail = f"\n--- asi_runner ---\n{runner_out}\n--- log ---\n{log}\n--- fake_gmod ---\n{guest_out}"
        self.assertEqual(g["state"], "peer-refused", detail)
        self.assertIn("multiplayer session detected", log, detail)
        self.assertIn("story mode only", log, detail)
        # Once refused, the plugin touches nothing in the game: no native at all, which
        # is also why it cannot draw a message about it.
        self.assertEqual(r["natives_after_multiplayer"], "0", detail)
        self.assertNotIn("draw:", runner_out.split("multiplayer session started")[1], detail)

    def test_asi_reports_a_mismatched_guest(self):
        name = unique_name()
        runner = self.run_asi(name, 4)
        time.sleep(1.0)
        guest = spawn(py("fake_gmod.py", "--seconds", "2", "--name", name, "--quiet", "--protocol-version", "999"))
        guest_out, g = finish(guest)
        runner_out, r = finish(runner)
        log = read_asi_log()
        detail = f"\n--- asi_runner ---\n{runner_out}\n--- log ---\n{log}\n--- fake_gmod ---\n{guest_out}"
        self.assertEqual(g["state"], "version-mismatch", detail)
        self.assertRegex(log, r"version mismatch.*999", detail)
        self.assertIn("version mismatch", runner_out, detail)


if __name__ == "__main__":
    unittest.main()
