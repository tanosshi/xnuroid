# Build the loader with the NDK, push it to the phone, run the exception/FDE test matrix, and
# collect everything under device_logs\<timestamp>\ with a SUMMARY.txt.
#
#   powershell -ExecutionPolicy Bypass -File run_phases.ps1
#   powershell -ExecutionPolicy Bypass -File run_phases.ps1 -Ndk "C:\path\to\ndk\26.1.10909125" -Push
#
# -Push uploads the log folder to branch "device-logs" of this repo's origin (separate throwaway
# clone, your working tree is untouched) so it can be read without copy/paste.
param(
    [string]$Ndk = "",
    [string]$Adb = "",
    [switch]$Push,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Continue"  # native tools write warnings to stderr; failures are checked explicitly
Set-Location $PSScriptRoot

function Find-Ndk {
    if ($Ndk) { return $Ndk }
    foreach ($v in @($env:ANDROID_NDK_HOME, $env:ANDROID_NDK_ROOT, $env:NDK_HOME)) { if ($v -and (Test-Path $v)) { return $v } }
    $root = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk"
    if (Test-Path $root) { return (Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName }
    throw "NDK not found. Pass -Ndk <path to ndk\<version>>."
}
function Find-Adb {
    if ($Adb) { return $Adb }
    $c = Get-Command adb -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
    if (Test-Path $p) { return $p }
    throw "adb not found. Pass -Adb <path to adb.exe>."
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logs = Join-Path $PSScriptRoot "device_logs\$stamp"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$adb = Find-Adb
Write-Host "adb: $adb"

# ---- build (exact command from the gate-2 commit message) ----
if (-not $SkipBuild) {
    $ndk = Find-Ndk
    $clang = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android31-clang++.cmd"
    if (-not (Test-Path $clang)) { $clang = $clang -replace "\.cmd$", ".exe" }
    Write-Host "NDK: $ndk"
    & $clang -std=c++17 -O1 -g -fno-omit-frame-pointer src/android_loader.cpp src/apple_string.cpp src/trampolines.s -ldl -o xnuroid_loader *> (Join-Path $logs "build.log")
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $logs "build.log") | Select-Object -Last 60
        throw "BUILD FAILED (log: $logs\build.log). Paste the last lines of that file."
    }
    Write-Host "build OK"
}

# ---- static inspection of the guest (no device needed): Unity entry points ----
$fw = Get-ChildItem -Recurse -Filter UnityFramework -File -Path (Join-Path $PSScriptRoot "MyGame-v1-7-2") -ErrorAction SilentlyContinue | Select-Object -First 1
if ($fw) {
    python il2cpp_exports.py $fw.FullName "_Unity" | Out-File -Encoding utf8 (Join-Path $logs "exports_Unity.txt")
    python il2cpp_exports.py $fw.FullName "_" | Where-Object { $_ -notmatch '^_il2cpp_' } | Out-File -Encoding utf8 (Join-Path $logs "exports_non_il2cpp.txt")
    python macho_symbols.py $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs "symbols_unity.txt")
    python macho_objc.py $fw.FullName "^Unity(?!PLCrash)" 3 | Out-File -Encoding utf8 (Join-Path $logs "objc_unity.txt")
    $objdump = $null
    foreach ($c in @((Get-Command llvm-objdump -ErrorAction SilentlyContinue).Source, "C:\Program Files\LLVM\bin\llvm-objdump.exe", (Join-Path (Find-Ndk) "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe"))) { if ($c -and (Test-Path $c)) { $objdump = $c; break } }
    if ($objdump) {
        $regions = @( @("didFinishLaunching", 0x11e4c, 0x12100), @("startUnity", 0x11638, 0x11984), @("helper_1516c", 0x1516c, 0x15400),
                      @("init_nographics_7cc3d4", 0x7cc3d4, 0x7cc800), @("startup_7cde30", 0x7cde30, 0x7cdf40), @("repaint", 0x16e3c, 0x16ee0),
                      @("getter_31d488", 0x31d488, 0x31d5c0), @("use_31db04", 0x31db04, 0x31dc00), @("fn_7cc11c", 0x7cc11c, 0x7cc260),
                      @("fn_7caa74", 0x7caa74, 0x7cab60), @("alloc_359a54", 0x359a54, 0x359b88), @("helper_tail_15240", 0x15240, 0x152a0) )
        foreach ($r in $regions) {
            & $objdump -d --no-show-raw-insn ("--start-address=0x{0:x}" -f $r[1]) ("--stop-address=0x{0:x}" -f $r[2]) $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs ("disasm_" + $r[0] + ".txt"))
        }
    } else { Write-Host "llvm-objdump not found: skipped disassembly dumps" }
    python compact_unwind.py $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs "unwind_summary.txt")
    Write-Host "exports: $((Get-Content (Join-Path $logs 'exports_Unity.txt')).Count) _Unity* symbols"
}

# ---- deploy ----
& $adb devices | Tee-Object -FilePath (Join-Path $logs "adb_devices.txt")
& $adb push xnuroid_loader /data/local/tmp/xnuroid_loader | Out-Null
& $adb push throw_frames.txt /data/local/tmp/throw_frames.txt | Out-Null
& $adb shell chmod 755 /data/local/tmp/xnuroid_loader

# ---- test matrix ----
$runs = @(
    @{ name = "1_default";          env = "" },
    @{ name = "2_exc_no_fde";       env = "LOADER_EXCEPTION_TEST=1" },
    @{ name = "3_exc_fde_subset";   env = "LOADER_EXCEPTION_TEST=1 LOADER_FDE_ONLY=/data/local/tmp/throw_frames.txt LOADER_FDE_DUMP=1" },
    @{ name = "4_exc_fde_all";      env = "LOADER_EXCEPTION_TEST=1 LOADER_FDE=all LOADER_EXCEPTION_BENCH=1" },
    @{ name = "5_default_fde_all";  env = "LOADER_FDE=all" },
    @{ name = "6_engine1";          env = "LOADER_FDE=all LOADER_ENGINE=1" },
    @{ name = "7_engine2";          env = "LOADER_FDE=all LOADER_ENGINE=2" }
)
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("run_phases $stamp  commit: $(git rev-parse --short HEAD)")
foreach ($r in $runs) {
    $n = $r.name
    Write-Host "=== $n ($($r.env)) ==="
    & $adb logcat -c
    $cmd = 'cd /data/local/tmp; ' + $r.env + ' timeout 240 ./xnuroid_loader >run_' + $n + '.out 2>run_' + $n + '.err; echo EXIT=$?'
    $exit = (& $adb shell $cmd | Select-Object -Last 1)
    & $adb pull "/data/local/tmp/run_$n.out" (Join-Path $logs "$n.out") 2>&1 | Out-Null
    & $adb pull "/data/local/tmp/run_$n.err" (Join-Path $logs "$n.err") 2>&1 | Out-Null
    & $adb logcat -d -t 600 | Out-File -Encoding utf8 (Join-Path $logs "$n.logcat")
    $summary.Add("")
    $summary.Add("##### $n   env: $($r.env)   $exit")
    $files = @((Join-Path $logs "$n.out"), (Join-Path $logs "$n.err"), (Join-Path $logs "$n.logcat")) | Where-Object { Test-Path $_ }
    $pat = 'Bind addends|Addended-bind|bit-63|\[FDE\]|\[BENCH\]|\[7/5\]|7A |7B |caught managed|NO exception|terminate|uncaught|CRASH|Signal \d+|FATAL|_il2cpp_init returned|Unresolved imports|\[E[12]\]|ENGINE|engine'
    $hits = Select-String -Path $files -Pattern $pat | ForEach-Object { $_.Line.Trim() } | Select-Object -First 90
    foreach ($h in $hits) { $summary.Add($h) }
    $poison = (Select-String -Path (Join-Path $logs "$n.err") -Pattern 'POISON' -ErrorAction SilentlyContinue | Measure-Object).Count
    $summary.Add("poison lines in stderr: $poison")
}
$summary | Out-File -Encoding utf8 (Join-Path $logs "SUMMARY.txt")
Write-Host ""
Write-Host "Done. Logs: $logs"
Write-Host "Paste SUMMARY.txt (or run again with -Push)."

if ($Push) {
    $url = git remote get-url origin
    $tmp = Join-Path $env:TEMP "devlogs_$stamp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Copy-Item -Recurse $logs (Join-Path $tmp $stamp)
    git -C $tmp init -q
    git -C $tmp checkout -q -b device-logs
    git -C $tmp add -A
    git -C $tmp -c user.name=runner -c user.email=runner@localhost commit -q -m "device logs $stamp"
    git -C $tmp push -q --force $url device-logs
    Write-Host "Pushed to branch device-logs"
}
