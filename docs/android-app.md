# Android app host

The Android app is the playable host. It runs the same loader as a `NativeActivity` behind
`libxnuroid.so`, so the guest image executes inside a normal app process rather than from an
`adb shell`.

## Build

```bash
./build_apk.sh
./build_apk.sh install
```

The first command builds `build/xnuroid.apk`. The `install` command builds, installs, copies
game files if missing, and launches the app.

The APK package is `com.xnuroid.<name>`, the native library is `libxnuroid.so`, and the
activity is `android.app.NativeActivity`. The install script pulls the device's
`/system/lib64/libc++.so` and ships it as `libsysc++.so`, because an app linker namespace cannot
open the system C++ library directly.

The package id, launcher label, and icon come from the game bundle configured in
`tools/game_config.py`. `tools/apk_branding.py` reads `CFBundleDisplayName` (or `CFBundleName`)
from its `Info.plist` as the label, derives `<name>` from `CFBundleName` (lowercase letters,
digits, and underscores), takes the largest loose `AppIcon` PNG listed under `CFBundleIcons`,
converts Apple's CgBI PNG format to a standard PNG, and wraps it in an adaptive icon. Without a configured bundle the label
is `xnuroid`, the package is `com.xnuroid.loader`, and the system default icon is used. Changing
games therefore installs a separate app instead of replacing the previous one.

## Running

`./build_apk.sh install` installs the APK, copies `/data/local/tmp/game` into the app's internal
files directory with `run-as` if needed, and starts the activity.

The app uses this layout:

| Path | Contents |
| --- | --- |
| `<internalDataPath>/game/` | `UnityFramework`, `Info.plist`, and `Data/` |
| `<internalDataPath>/sandbox/` | Guest-visible `Documents`, `Library`, and `Caches` tree |
| `<externalDataPath>/loader.env` | Optional `KEY=VALUE` environment overrides |
| `<externalDataPath>/out.txt`, `err.txt` | Loader standard output and error |
| `<externalDataPath>/exit.txt` | `EXIT=<code>` after the loader thread returns |

Pull logs with (replace `<name>` with the package suffix printed by the build):

```bash
adb pull /sdcard/Android/data/com.xnuroid.<name>/files/out.txt
adb pull /sdcard/Android/data/com.xnuroid.<name>/files/err.txt
```

Example `loader.env`:

```dotenv
LOADER_FRAMES=300
LOADER_FRAME_DIR=/sdcard/Android/data/com.xnuroid.<name>/files/frames
LOADER_GL_TRACE=1
```

The app sets `LOADER_ENGINE=8`, `LOADER_FDE=all`, and a very large `LOADER_FRAMES` before reading
`loader.env`, so values in that file override those defaults.
