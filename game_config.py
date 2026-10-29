"""Game bundle configuration shared by the analysis scripts.

This repository ships with empty defaults so that no game-specific path, bundle
name, or executable name is ever committed. Fill the values in for your own
decrypted IPA in either of two ways:

  * create a local, git-ignored ``game_config_local.py`` next to this file that
    assigns ``GAME_ROOT``, ``APP_NAME`` and (optionally) ``APP_EXECUTABLE``; or
  * set the ``LOADER_GAME_DIR``, ``LOADER_APP_NAME`` and ``LOADER_APP_EXECUTABLE``
    environment variables.

Example ``game_config_local.py``::

    GAME_ROOT = "path/to/extracted-ipa"   # folder that contains Payload/
    APP_NAME = "YourApp"                  # YourApp.app lives inside Payload/
    APP_EXECUTABLE = "YourApp"            # optional; defaults to APP_NAME
"""

import os
from pathlib import Path

# Directory that contains the extracted Payload/ folder of your decrypted IPA.
GAME_ROOT = os.environ.get("LOADER_GAME_DIR", "")

# Name of the *.app bundle inside Payload/, without the .app suffix.
APP_NAME = os.environ.get("LOADER_APP_NAME", "")

# File name of the decrypted executable inside the .app bundle.
APP_EXECUTABLE = os.environ.get("LOADER_APP_EXECUTABLE", "")

# Optional local overrides. game_config_local.py is git-ignored, so your real
# paths never leave your machine.
try:
    import game_config_local as _local
except ImportError:
    _local = None
if _local is not None:
    for _name in ("GAME_ROOT", "APP_NAME", "APP_EXECUTABLE"):
        _value = getattr(_local, _name, None)
        if _value:
            globals()[_name] = _value

# Default the executable name to the bundle name when it was left blank.
APP_EXECUTABLE = APP_EXECUTABLE or APP_NAME


def require_config():
    """Raise a helpful error if the game paths have not been configured yet."""
    if not GAME_ROOT or not APP_NAME:
        raise RuntimeError(
            "Game paths are not configured. Set LOADER_GAME_DIR and LOADER_APP_NAME, "
            "or create game_config_local.py next to game_config.py."
        )


def app_bundle_dir():
    """Path to the <App>.app bundle inside Payload/."""
    require_config()
    return Path(GAME_ROOT) / "Payload" / f"{APP_NAME}.app"


def app_executable_path():
    """Path to the decrypted executable inside the app bundle."""
    return app_bundle_dir() / APP_EXECUTABLE


def unity_framework_path():
    """Path to the bundled UnityFramework binary."""
    return (
        app_bundle_dir()
        / "Frameworks"
        / "UnityFramework.framework"
        / "UnityFramework"
    )


def guest_bundle_path():
    """Bundle path as seen by the guest process inside the emulated device."""
    require_config()
    return f"/Payload/{APP_NAME}.app"
