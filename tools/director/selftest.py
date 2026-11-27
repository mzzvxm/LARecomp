"""Run Director offline checks, including the same protocol on restricted hosts."""

import contextlib
import io
import itertools
import json
import os
import queue
import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


class MemoryConnection:
    """A byte stream for loopback tests on hosts that prohibit TCP sockets."""

    def __init__(self, inbox, peer):
        self.inbox, self.peer = inbox, peer
        self.closed = False
        self.timeout = None
        self.pending = b""

    def settimeout(self, value):
        self.timeout = value

    def sendall(self, data):
        if self.closed:
            raise OSError("connection closed")
        self.peer.put(data)

    def recv(self, limit):
        if not self.pending:
            try:
                self.pending = self.inbox.get(timeout=self.timeout)
            except queue.Empty as exc:
                raise socket.timeout() from exc
        result, self.pending = self.pending[:limit], self.pending[limit:]
        return result

    def shutdown(self, _how):
        self.close()

    def close(self):
        if not self.closed:
            self.closed = True
            self.inbox.put(b"")
            self.peer.put(b"")

    def fileno(self):
        return -1 if self.closed else 1


@contextlib.contextmanager
def test_transport(dc):
    try:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            with socket.create_connection(listener.getsockname(), timeout=1):
                pass
    except PermissionError:
        print("TCP loopback restricted; running MockServer tests over an in-memory byte stream")
    else:
        yield
        return

    servers = {}
    ports = itertools.count(1)

    def start(server):
        server.port = next(ports)
        servers[server.port] = server
        dc.threading.Thread(target=server._tick_loop, daemon=True).start()
        return server.port, server.token

    def stop(server):
        servers.pop(server.port, None)
        server._stop = True
        with server.lock:
            for c in server.clients:
                c.sock.close()

    def connect(address, timeout=None):
        server = servers.get(address[1])
        if server is None:
            raise ConnectionRefusedError("no mock at this address")
        a, b = queue.Queue(), queue.Queue()
        client = MemoryConnection(a, b)
        client.settimeout(timeout)
        server._accept_client(MemoryConnection(b, a))
        return client

    with (patch.object(dc.MockServer, "start", start), patch.object(dc.MockServer, "stop", stop),
          patch.object(socket, "create_connection", connect)):
        yield


def session_suite(st, dc):
    class SessionTests(unittest.TestCase):
        def setUp(self):
            temp = tempfile.TemporaryDirectory(prefix="director-tools-")
            self.addCleanup(temp.cleanup)
            self.root = Path(temp.name)
            self.game_dir = self.root / "game with spaces"
            self.game_dir.mkdir()
            self.path = self.game_dir / "director.json"
            self.mock = dc.MockServer(self.game_dir)
            self.mock.start()
            self.addCleanup(self.mock.stop)

        def cli(self, *args):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                result = st.main([*map(str, args), "--json"])
            return result, json.loads(buf.getvalue())

        def test_find_and_attach(self):
            self.mock.write_info(self.path)
            rc, found = self.cli("slots", "--game-dir", self.game_dir, "--attach", self.path)
            self.assertEqual(rc, 0)
            self.assertEqual([item["info"] for item in found["slots"]], [str(self.path)])
            self.assertTrue(found["slots"][0]["alive"])
            self.assertNotIn(self.mock.token, str(found))
            for opts in (["--game-dir", self.game_dir], ["--attach", self.game_dir], ["--attach", self.path]):
                self.assertEqual(self.cli("cmd", "help", *opts)[0], 0)
            with patch.object(st.Path, "cwd", return_value=self.game_dir):
                self.assertEqual(self.cli("cmd", "help")[0], 0)

        def test_ambiguous_and_stale(self):
            self.mock.write_info(self.path)
            second = self.root / "director.json"
            self.mock.write_info(second)
            with patch.object(st.Path, "cwd", return_value=self.root):
                self.assertEqual(self.cli("cmd", "help", "--game-dir", self.game_dir)[0], st.EXIT_USAGE)
                receipt = self.mock.info()
                second.write_text(json.dumps(dict(receipt, pid=0)), encoding="utf-8")
                self.assertEqual(self.cli("cmd", "help", "--game-dir", self.game_dir)[0], 0)
                self.assertEqual(self.cli("cmd", "help", "--attach", second)[0], st.EXIT_ENV)

        def test_launch_argv_and_identity(self):
            exe = self.game_dir / "larecomp.exe"
            exe.touch()
            proc = Mock(pid=os.getpid())
            proc.poll.return_value = None
            scene = st.resolve_scene("orbit-reveal")
            ffmpeg = self.game_dir / "ffmpeg.exe"
            ffmpeg.touch()

            def start(*args, **kwargs):
                self.mock.write_info(self.path)
                return proc

            with patch.object(st.subprocess, "Popen", side_effect=start) as popen:
                rc, receipt = self.cli("launch", "--exe", exe, "--scene", scene, "--ffmpeg", ffmpeg)
                self.assertEqual(rc, 0)
                work = self.game_dir / "director-work"
                self.assertEqual(popen.call_args.args[0], [
                    str(exe), "--mcla_director=true", f"--mcla_director_info={self.path}",
                    f"--mcla_director_root={work}", f"--mcla_capture_dir={work / 'captures'}",
                    f"--mcla_scene={scene}", f"--mcla_capture_ffmpeg={ffmpeg}",
                ])
                self.assertEqual(popen.call_args.kwargs["cwd"], self.game_dir)
                self.assertEqual(receipt["info"], str(self.path))
                self.assertFalse(proc.terminate.called)
                self.assertEqual(self.cli("launch", "--exe", exe)[0], st.EXIT_USAGE)
                popen.assert_called_once()

        def test_launch_refuses_wrong_pid_and_cleans_up(self):
            exe = self.game_dir / "larecomp.exe"
            exe.touch()
            proc = Mock(pid=os.getpid() + 1)
            proc.poll.return_value = None

            def start(*args, **kwargs):
                self.mock.write_info(self.path)
                return proc

            with patch.object(st.subprocess, "Popen", side_effect=start):
                rc, result = self.cli("launch", "--exe", exe, "--timeout-s", "0.01")
            self.assertEqual(rc, st.EXIT_ENV)
            self.assertEqual(result["error"]["code"], "timeout")
            proc.terminate.assert_called_once()

        def test_launch_reports_exit(self):
            exe = self.game_dir / "larecomp.exe"
            exe.touch()
            proc = Mock()
            proc.poll.return_value = 7
            with patch.object(st.subprocess, "Popen", return_value=proc):
                rc, result = self.cli("launch", "--exe", exe)
            self.assertEqual(rc, st.EXIT_ENV)
            self.assertIn("code 7", result["error"]["msg"])
            proc.terminate.assert_not_called()

        def test_stage_preserves_relative_dependencies(self):
            scene = self.root / "camera.scene"
            scene.write_text(
                'scene test\ncam 0 rig flyby path "my roads/example" up 2 yaw 0 pitch 0\n'
                'at 1 capture fps 60\nend 3\n', encoding="utf-8")
            route = self.root / "routes" / "my roads" / "example.route"
            route.parent.mkdir(parents=True)
            route.write_text("0,0,8,0\n10,0,8,0\n", encoding="utf-8")
            draft = scene.read_text(encoding="utf-8")
            lint = st.lint_one(scene, no_src=True, text=draft)
            self.assertEqual(lint["errors"], 0, lint["diagnostics"])
            stage = self.root / "staged"
            stage.mkdir()
            scene.write_text("scene newer\nend 4\n", encoding="utf-8")
            copied, captures = st.stage_scene(scene, stage, draft, lint["_scene"])
            self.assertEqual(captures, [3])
            self.assertIn("# [scene_tool] at 1 capture", copied.read_text(encoding="utf-8"))
            self.assertEqual((stage / route.relative_to(self.root)).read_bytes(), route.read_bytes())

        def test_new_recipe_copies_spots_and_paths(self):
            custom = self.root / "spots.txt"
            custom.write_text("own-spot 10 1 10 0", encoding="utf-8")
            self.assertEqual(self.cli("new", "my.shot", "--recipe", "chase", "--dir", self.root)[0], 0)
            self.assertIn("own-spot", custom.read_text(encoding="utf-8"))
            self.assertEqual(self.cli("lint", self.root / "my.shot", "--strict")[0], 0)

        def test_lint_reads_the_settings_manifest(self):
            manifest = self.root / "settings" / "generated" / "manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "sample": {"type": "f64", "traits": ["rex::Range{0, 5}", "rex::kDebugOnly"]},
                "mode": {"type": "std::string", "traits": ['rex::Allowed("a", "b")', "rex::kRequiresRestart"]},
            }), encoding="utf-8")
            sc = st.scene_check_module()
            settings = sc.scan_cvars(self.root)
            self.assertEqual((settings["sample"].lo, settings["sample"].hi, settings["sample"].debug_only), (0, 5, True))
            self.assertEqual((settings["mode"].allowed, settings["mode"].lifecycle), (["a", "b"], "kRequiresRestart"))
            self.assertTrue(sc.lint("scene sample\nat 0 set sample 6\nend 1\n", settings).diags)

        def test_atlas_output_and_archive_key(self):
            atlas = st.atlas_module()
            self.assertFalse(atlas.ATLAS_ROOT.is_relative_to(st.HERE.parents[1]))
            self.assertEqual(atlas.output_root(self.root), self.root)
            with self.assertRaises(ValueError):
                atlas.output_root(st.HERE)
            game = st.load_module(st.HERE / "atlas_game.py", "atlas_game_test")
            self.assertEqual(len(game.find_rpf_key()), 32)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SessionTests)
    return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()


def run(st):
    dc = st.director_client_module()
    scene_run = st.load_module(st.SCENE_RUN, "scene_run_mod")
    take_diff = st.load_module(st.TAKE_DIFF, "take_diff")
    atlas = st.atlas_module()
    game = st.load_module(st.HERE / "atlas_game.py", "atlas_game")
    offline = (st.scene_check_module().selftest, atlas._selftest, game._selftest, scene_run.selftest, take_diff.selftest)
    outcomes = []
    for check in offline:
        print(f"\n{check.__module__}")
        outcomes.append(check() == 0)
    results = st.selftest_tool()
    with test_transport(dc):
        outcomes.append(dc._selftest() == 0)
        outcomes.append(session_suite(st, dc))
        results += st.load_preview().selftest()
    skipped = failed = 0
    for label, ok, detail in results:
        if label.startswith("SKIP"):
            skipped += 1
            print(label)
            continue
        print(("PASS " if ok else "FAIL ") + label + (f": {detail}" if not ok and detail else ""))
        failed += not ok
    print(f"scene tool and preview: {len(results) - skipped} checks, {failed} failed, {skipped} skipped")
    return int(failed != 0 or not all(outcomes))
