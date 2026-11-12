"""Turns whatever the user picked (.ipa, unpacked folder, Payload folder or .app) into a configured game."""
from __future__ import annotations

import ast
import importlib
import plistlib
import re
import shutil
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import macho_check

Progress = Callable[[float, str], None]
CONFIG_KEYS = ("GAME_ROOT", "APP_NAME", "APP_EXECUTABLE")
IPA_STAMP = ".ipa_stamp"


class Cancelled(Exception):
    """The user pressed Cancel while a long copy was running."""


@dataclass
class Game:
    root: Path
    app: Path
    info: dict

    @property
    def executable(self) -> str:
        return self.info.get("CFBundleExecutable") or self.app.stem

    @property
    def label(self) -> str:
        return (self.info.get("CFBundleDisplayName") or self.info.get("CFBundleName")
                or self.executable).strip() or self.app.stem

    @property
    def version(self) -> str:
        return str(self.info.get("CFBundleShortVersionString") or "")

    @property
    def is_unity(self) -> bool:
        return (self.app / "Frameworks" / "UnityFramework.framework" / "UnityFramework").is_file()

    def size_mb(self) -> int:
        return sum(p.stat().st_size for p in self.app.rglob("*") if p.is_file()) >> 20


def _branding(project: Path):
    tools = str(project / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    return importlib.import_module("apk_branding")


def package_id(project: Path, info: dict) -> str:
    return _branding(project).package_id(info)


def icon_png(project: Path, game: Game) -> bytes | None:
    try:
        branding = _branding(project)
        icon = branding.largest_icon(game.app, game.info)
        return branding.normalize_png(icon.read_bytes()) if icon else None
    except Exception:
        return None


def load_info(app: Path) -> dict:
    try:
        with (app / "Info.plist").open("rb") as handle:
            return plistlib.load(handle)
    except FileNotFoundError:
        raise ValueError(f"{app.name} has no Info.plist, so it is not a complete app.") from None
    except (OSError, plistlib.InvalidFileException) as error:
        raise ValueError(f"I could not read the Info.plist of {app.name}: {error}") from None


def apps_in(payload: Path) -> list[Path]:
    return sorted(p for p in payload.iterdir() if p.is_dir() and p.suffix == ".app") if payload.is_dir() else []


def bundle_candidates(choice: Path) -> list[Path]:
    """Every .app the picked folder could mean; an empty list means it holds no app."""
    if choice.suffix == ".app":
        return [choice]
    if choice.name == "Payload":
        return apps_in(choice)
    return apps_in(choice / "Payload")


def _ipa_stamp(ipa: Path) -> str:
    stat = ipa.stat()
    return f"{stat.st_size}:{stat.st_mtime_ns}"


def _same_ipa(ipa: Path, dest: Path) -> bool:
    """Whether dest was unpacked from this very file; a different .ipa with the same name is unpacked again."""
    try:
        return (dest / IPA_STAMP).read_text(encoding="utf-8").strip() == _ipa_stamp(ipa)
    except FileNotFoundError:
        pass
    except OSError:
        return False
    # Unpacked before stamps existed: trust it unless the .ipa is newer, and remember it from now on.
    try:
        if ipa.stat().st_mtime > (dest / "Payload").stat().st_mtime:
            return False
        (dest / IPA_STAMP).write_text(_ipa_stamp(ipa), encoding="utf-8")
    except OSError:
        return False
    return True


def extract_ipa(ipa: Path, games_dir: Path, progress: Progress,
                cancelled: Callable[[], bool] = lambda: False) -> Path:
    dest = games_dir / re.sub(r"[^\w.-]+", "_", ipa.stem)
    if apps_in(dest / "Payload") and _same_ipa(ipa, dest):
        progress(1.0, "Already unpacked")
        return dest
    partial = dest.with_name(dest.name + ".unpacking")
    shutil.rmtree(partial, ignore_errors=True)
    try:
        with zipfile.ZipFile(ipa) as archive:
            members = archive.infolist()
            total = sum(m.file_size for m in members) or 1
            done = 0
            for member in members:
                if cancelled():
                    raise Cancelled()
                archive.extract(member, partial)
                done += member.file_size
                progress(done / total, f"Unpacking {done >> 20} of {total >> 20} MB")
        (partial / IPA_STAMP).write_text(_ipa_stamp(ipa), encoding="utf-8")
    except Cancelled:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    except (OSError, zipfile.BadZipFile) as error:
        shutil.rmtree(partial, ignore_errors=True)
        raise ValueError(f"I could not unpack {ipa.name}: {error}\n\n"
                         "Check that the file finished downloading and that the disk is not full.") from None
    shutil.rmtree(dest, ignore_errors=True)
    partial.rename(dest)
    return dest


def adopt_loose_app(app: Path, games_dir: Path, progress: Progress) -> Path:
    """A .app outside a Payload folder is copied into one, because the build expects <root>/Payload/<App>.app."""
    if app.parent.name == "Payload":
        return app.parent.parent
    dest = games_dir / app.stem
    # Copied beside dest first and swapped in afterwards: the picked .app may itself live inside dest.
    partial = dest.with_name(dest.name + ".copying")
    progress(0.0, "Copying the app into the project")
    shutil.rmtree(partial, ignore_errors=True)
    try:
        shutil.copytree(app, partial / "Payload" / app.name)
    except OSError as error:
        shutil.rmtree(partial, ignore_errors=True)
        raise ValueError(f"I could not copy {app.name}: {error}") from None
    shutil.rmtree(dest, ignore_errors=True)
    partial.rename(dest)
    progress(1.0, "Copied")
    return dest


def read_config(project: Path) -> Game | None:
    config = project / "tools" / "game_config_local.py"
    # Parsed, never imported: the values are plain string literals.
    values = {}
    try:
        tree = ast.parse(config.read_text(encoding="utf-8"))
    except (OSError, SyntaxError, ValueError):
        return None
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in CONFIG_KEYS and isinstance(node.value, ast.Constant)
                and isinstance(node.value.value, str)):
            values[node.targets[0].id] = node.value.value
    if not values.get("GAME_ROOT") or not values.get("APP_NAME"):
        return None
    root = Path(values["GAME_ROOT"])
    if not root.is_absolute():
        root = project / root
    app = root / "Payload" / f"{values['APP_NAME']}.app"
    try:
        return Game(root, app, load_info(app))
    except ValueError:
        return None


def write_config(project: Path, game: Game) -> None:
    # tools/game_config.py imports this file, and the names come from the game's Info.plist: repr() keeps a quote
    # or backslash in them from ending the string literal.
    (project / "tools" / "game_config_local.py").write_text(
        '"""Written by the xnuroid helper. Git-ignored, so your paths stay local."""\n'
        f'GAME_ROOT = {game.root.resolve().as_posix()!r}\n'
        f'APP_NAME = {game.app.stem!r}\n'
        f'APP_EXECUTABLE = {str(game.executable)!r}\n',
        encoding="utf-8")


def check_encryption(choice: Path, app: Path | None) -> macho_check.AppReport:
    if app is not None:
        return macho_check.check_app(app)
    return macho_check.check_ipa(choice)
