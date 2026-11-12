import os
import plistlib
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "gui"))
sys.path.insert(0, str(ROOT / "tools"))
import apk_branding  # noqa: E402
import game as games  # noqa: E402


def quiet(_fraction, _text):
    pass


def make_app(folder: Path, executable: str = "Demo") -> Path:
    folder.mkdir(parents=True)
    with (folder / "Info.plist").open("wb") as handle:
        plistlib.dump({"CFBundleExecutable": executable, "CFBundleName": folder.stem}, handle)
    (folder / "data.bin").write_bytes(b"payload")
    return folder


def make_ipa(path: Path, marker: bytes) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("Payload/Demo.app/Info.plist", plistlib.dumps({"CFBundleExecutable": "Demo"}))
        archive.writestr("Payload/Demo.app/marker", marker)
    return path


class ConfigTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name)
        (self.project / "tools").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def roundtrip(self, executable: str) -> games.Game:
        app = make_app(self.project / "games" / "demo" / "Payload" / "It's a game.app", executable)
        game = games.Game(app.parent.parent, app, games.load_info(app))
        games.write_config(self.project, game)
        loaded = games.read_config(self.project)
        self.assertIsNotNone(loaded)
        self.assertEqual(loaded.app, game.app)
        self.assertEqual(loaded.executable, executable)
        return loaded

    def test_plain_names(self):
        self.roundtrip("Demo")

    def test_quotes_and_backslashes_stay_data(self):
        hostile = 'x" + __import__("os").environ.setdefault("XNUROID_TEST_INJECTED", "1") + "\\'
        self.roundtrip(hostile)
        source = (self.project / "tools" / "game_config_local.py").read_text(encoding="utf-8")
        scope: dict = {}
        exec(compile(source, "game_config_local.py", "exec"), scope)   # what tools/game_config.py does on import
        self.assertEqual(scope["APP_EXECUTABLE"], hostile)
        self.assertNotIn("XNUROID_TEST_INJECTED", os.environ)

    def test_reads_the_older_raw_string_form(self):
        app = make_app(self.project / "old" / "Payload" / "Demo.app")
        (self.project / "tools" / "game_config_local.py").write_text(
            f'"""doc"""\nGAME_ROOT = r"{(self.project / "old").as_posix()}"\nAPP_NAME = "Demo"\n', encoding="utf-8")
        self.assertEqual(games.read_config(self.project).app, app)


class UnpackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.games = self.root / "games"
        self.games.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def marker(self, dest: Path) -> bytes:
        return (dest / "Payload" / "Demo.app" / "marker").read_bytes()

    def test_same_ipa_is_reused_and_a_changed_one_is_unpacked_again(self):
        ipa = make_ipa(self.root / "Demo.ipa", b"first")
        dest = games.extract_ipa(ipa, self.games, quiet)
        self.assertEqual(self.marker(dest), b"first")
        (dest / "Payload" / "Demo.app" / "marker").write_bytes(b"kept")
        self.assertEqual(self.marker(games.extract_ipa(ipa, self.games, quiet)), b"kept")
        make_ipa(ipa, b"second, longer")
        self.assertEqual(self.marker(games.extract_ipa(ipa, self.games, quiet)), b"second, longer")

    def test_cancel_leaves_nothing_behind(self):
        ipa = make_ipa(self.root / "Demo.ipa", b"first")
        with self.assertRaises(games.Cancelled):
            games.extract_ipa(ipa, self.games, quiet, lambda: True)
        self.assertEqual(list(self.games.iterdir()), [])

    def test_loose_app_inside_its_own_destination_survives(self):
        app = make_app(self.games / "Demo" / "Demo.app")
        dest = games.adopt_loose_app(app, self.games, quiet)
        self.assertEqual(dest, self.games / "Demo")
        self.assertEqual((dest / "Payload" / "Demo.app" / "data.bin").read_bytes(), b"payload")


class PackageIdTest(unittest.TestCase):
    def test_ascii_name_is_unchanged(self):
        self.assertEqual(apk_branding.package_id({"CFBundleName": "Pocket God", "CFBundleIdentifier": "com.a.b"}),
                         "com.xnuroid.pocketgod")

    def test_non_ascii_name_falls_through(self):
        self.assertEqual(apk_branding.package_id({"CFBundleName": "ゲーム", "CFBundleExecutable": "Game-JP"}),
                         "com.xnuroid.gamejp")
        self.assertEqual(apk_branding.package_id({"CFBundleName": "ゲーム", "CFBundleIdentifier": "jp.co.studio.game"}),
                         "com.xnuroid.jp_co_studio_game")
        self.assertEqual(apk_branding.package_id({}), "com.xnuroid.loader")


if __name__ == "__main__":
    unittest.main()
