"""Finds the tools the build needs on this PC and talks to the phone through adb."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

NDK_VERSION = "28.2.13676358"
PLATFORM = "android-36"
MIN_PHONE_SDK = 31
SDK_PACKAGES = [f"ndk;{NDK_VERSION}", "build-tools;36.1.0", f"platforms;{PLATFORM}", "platform-tools"]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

GIT_URL = "https://git-scm.com/download/win"
PYTHON_URL = "https://www.python.org/downloads/windows/"
STUDIO_URL = "https://developer.android.com/studio"


def run_quiet(cmd: list, timeout: float = 30, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout, creationflags=NO_WINDOW, **kwargs)


def is_project_root(path: Path) -> bool:
    return (path / "build_apk.sh").is_file() and (path / "tools" / "game_config.py").is_file()


def _first_file(candidates) -> Path | None:
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def find_bash() -> Path | None:
    local = Path(os.environ.get("LOCALAPPDATA", ""))
    return _first_file([
        os.environ.get("GIT_BASH"),
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
        local / "Programs" / "Git" / "bin" / "bash.exe",
    ])


def find_sdk() -> Path | None:
    for candidate in (os.environ.get("ANDROID_SDK"), os.environ.get("ANDROID_HOME"),
                      os.environ.get("ANDROID_SDK_ROOT"),
                      Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"):
        if candidate and any((Path(candidate) / part).is_dir() for part in ("platform-tools", "build-tools")):
            return Path(candidate)
    return None


def python_version(bash: Path | None) -> str:
    """Version of the `python` the build script runs; asked through Git Bash so PATH matches the build.

    The Microsoft Store placeholder prints nothing and fails, so it reads as missing.
    """
    probe = "import sys;print(sys.version_info[:2])"
    if bash:
        cmd = [bash, "-lc", f'python -c "{probe}"']
    else:
        found = shutil.which("python")
        if not found:
            return ""
        cmd = [found, "-c", probe]
    try:
        result = run_quiet(cmd, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    match = re.search(r"\((\d+), (\d+)\)", result.stdout)
    return f"{match[1]}.{match[2]}" if result.returncode == 0 and match else ""


def find_keytool() -> Path | None:
    studio = [Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Android" / "Android Studio",
              Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Android Studio"]
    studio = [root / "jbr" / "bin" / "keytool.exe" for root in studio]
    java_home = os.environ.get("JAVA_HOME")
    return _first_file(studio + [Path(java_home) / "bin" / "keytool.exe" if java_home else None,
                                 shutil.which("keytool")])


def find_sdkmanager(sdk: Path) -> Path | None:
    tools = sdk / "cmdline-tools"
    candidates = [tools / "latest" / "bin" / "sdkmanager.bat"]
    if tools.is_dir():
        candidates += sorted(tools.glob("*/bin/sdkmanager.bat"), reverse=True)
    return _first_file(candidates)


def debug_keystore() -> Path:
    return Path.home() / ".android" / "debug.keystore"


@dataclass
class Check:
    key: str
    title: str
    ok: bool
    detail: str
    fix: str = ""


def check_computer() -> tuple[list[Check], dict]:
    found: dict = {}
    checks = []

    bash = find_bash()
    found["bash"] = bash
    checks.append(Check("bash", "Git for Windows", bash is not None,
                        "Installed" if bash else "Needed to run the build script", "" if bash else "git"))

    version = python_version(bash)
    major_minor = tuple(int(x) for x in version.split(".")) if version else (0, 0)
    python_ok = major_minor >= (3, 10)
    detail = (f"Python {version}" if python_ok else
              f"Python {version} is too old, 3.10 or newer is needed" if version else
              "Needed by the build script")
    checks.append(Check("python", "Python", python_ok, detail, "" if python_ok else "python"))

    sdk = find_sdk()
    found["sdk"] = sdk
    checks.append(Check("sdk", "Android Studio (SDK)", sdk is not None,
                        str(sdk) if sdk else "Needed to make Android apps", "" if sdk else "studio"))
    if sdk:
        found["adb"] = sdk / "platform-tools" / "adb.exe"
        missing = []
        if not (sdk / "ndk" / NDK_VERSION).is_dir():
            missing.append(f"NDK {NDK_VERSION}")
        build_tools = sdk / "build-tools"
        if not build_tools.is_dir() or not any(build_tools.iterdir()):
            missing.append("Build-Tools")
        if not (sdk / "platforms" / PLATFORM / "android.jar").is_file():
            missing.append(f"SDK Platform {PLATFORM}")
        if not found["adb"].is_file():
            missing.append("Platform-Tools")
        checks.append(Check("sdk_parts", "Android SDK parts", not missing,
                            "All installed" if not missing else "Missing: " + ", ".join(missing),
                            "" if not missing else "sdk_parts"))
    else:
        adb = shutil.which("adb")
        found["adb"] = Path(adb) if adb else None

    keystore = debug_keystore()
    checks.append(Check("keystore", "App signing key", keystore.is_file(),
                        "Ready" if keystore.is_file() else "Needed to sign the app",
                        "" if keystore.is_file() else "keystore"))
    return checks, found


def create_debug_keystore() -> str:
    keytool = find_keytool()
    if keytool is None:
        raise RuntimeError("I could not find keytool. Install Android Studio first, then try again.")
    keystore = debug_keystore()
    keystore.parent.mkdir(parents=True, exist_ok=True)
    result = run_quiet([keytool, "-genkeypair", "-keystore", keystore, "-storepass", "android",
                        "-alias", "androiddebugkey", "-keypass", "android", "-keyalg", "RSA",
                        "-keysize", "2048", "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"],
                       timeout=120)
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip() or "keytool failed")
    return str(keystore)


@dataclass
class Phone:
    serial: str
    state: str
    model: str = ""
    abi: str = ""
    sdk: int = 0

    @property
    def usable(self) -> bool:
        return self.state == "device" and self.abi.startswith("arm64") and self.sdk >= MIN_PHONE_SDK

    @property
    def label(self) -> str:
        return f"{self.model or 'Android phone'} ({self.serial})"

    def problem(self) -> str:
        if self.state == "unauthorized":
            return "Look at your phone and tap 'Allow' on the USB debugging prompt."
        if self.state == "offline":
            return "The phone is not answering. Unplug it and plug it back in."
        if self.state != "device":
            return f"The phone reports '{self.state}'. Unplug it and plug it back in."
        if self.abi and not self.abi.startswith("arm64"):
            return f"This phone has a {self.abi} processor. A 64-bit ARM (arm64) phone is needed."
        if self.sdk and self.sdk < MIN_PHONE_SDK:
            return "This phone runs an Android version older than 12. Android 12 or newer is needed."
        return ""


class Adb:
    def __init__(self, path: Path):
        self.path = path
        self._details: dict[str, tuple[str, int]] = {}

    def run(self, *args, serial: str = "", timeout: float = 30) -> subprocess.CompletedProcess:
        prefix = [self.path] + (["-s", serial] if serial else [])
        return run_quiet(prefix + list(args), timeout=timeout)

    def shell(self, serial: str, command: str, timeout: float = 30) -> str:
        return self.run("shell", command, serial=serial, timeout=timeout).stdout.strip()

    def phones(self) -> list[Phone]:
        result = self.run("devices", "-l", timeout=15)
        phones = []
        for line in result.stdout.splitlines()[1:]:
            parts = line.split()
            if len(parts) < 2 or line.startswith("*"):
                continue
            phone = Phone(parts[0], parts[1])
            model = next((p.split(":", 1)[1] for p in parts[2:] if p.startswith("model:")), "")
            phone.model = model.replace("_", " ")
            if phone.state == "device":
                if phone.serial not in self._details:
                    abi = self.shell(phone.serial, "getprop ro.product.cpu.abi", timeout=10)
                    sdk = self.shell(phone.serial, "getprop ro.build.version.sdk", timeout=10)
                    self._details[phone.serial] = (abi, int(sdk) if sdk.isdigit() else 0)
                phone.abi, phone.sdk = self._details[phone.serial]
            phones.append(phone)
        return phones

    def read_app_file(self, serial: str, package: str, name: str) -> str | None:
        result = self.run("shell", f"cat /sdcard/Android/data/{package}/files/{name} 2>/dev/null || echo __missing__",
                          serial=serial, timeout=30)
        text = result.stdout
        return None if text.strip() == "__missing__" else text

    def launch(self, serial: str, package: str) -> str:
        return self.shell(serial, f"rm -f /sdcard/Android/data/{package}/files/exit.txt; "
                                  f"am start -S -n {package}/android.app.NativeActivity")

    def stop(self, serial: str, package: str) -> None:
        self.shell(serial, f"am force-stop {package}")

    def is_running(self, serial: str, package: str) -> bool:
        return bool(self.shell(serial, f"pidof {package}", timeout=10))

    def is_installed(self, serial: str, package: str) -> bool:
        return f"package:{package}" in self.shell(serial, f"pm list packages {package}").split()

    def uninstall(self, serial: str, package: str) -> str:
        return self.run("uninstall", package, serial=serial, timeout=60).stdout.strip()
