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
    [string]$GameDir = "",
    [switch]$Push,
    [switch]$SkipBuild,
    [switch]$Quick   # skip the slow static scans; disassembly regions, xrefs and the device runs still happen
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
function Get-GameRoot {
    # Resolve the extracted-IPA folder, in order: -GameDir, LOADER_GAME_DIR, or
    # the git-ignored game_config_local.py (GAME_ROOT = "...").
    if ($GameDir) { return $GameDir }
    if ($env:LOADER_GAME_DIR) { return $env:LOADER_GAME_DIR }
    $local = Join-Path $PSScriptRoot "game_config_local.py"
    if (Test-Path $local) {
        $m = Select-String -Path $local -Pattern '^\s*GAME_ROOT\s*=\s*["'']([^"'']*)["'']' | Select-Object -First 1
        if ($m) { return $m.Matches[0].Groups[1].Value }
    }
    return ""
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
$localDataForScan = $null
$fw = $null
$gameRoot = Get-GameRoot
if ($gameRoot) {
    $gamePath = Join-Path $PSScriptRoot $gameRoot
    if (Test-Path $gamePath) {
        $fw = Get-ChildItem -Recurse -Filter UnityFramework -File -Path $gamePath -ErrorAction SilentlyContinue | Select-Object -First 1
    }
}
if ($fw) {
    $localDataForScan = Join-Path (Split-Path (Split-Path $fw.DirectoryName)) "Data"
    if (-not $Quick) {
    python il2cpp_exports.py $fw.FullName "_Unity" | Out-File -Encoding utf8 (Join-Path $logs "exports_Unity.txt")
    python il2cpp_exports.py $fw.FullName "_" | Where-Object { $_ -notmatch '^_il2cpp_' } | Out-File -Encoding utf8 (Join-Path $logs "exports_non_il2cpp.txt")
    python macho_symbols.py $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs "symbols_unity.txt")
    python macho_objc.py $fw.FullName "^Unity(?!PLCrash)" 3 | Out-File -Encoding utf8 (Join-Path $logs "objc_unity.txt")
    }
    $objdump = $null
    foreach ($c in @((Get-Command llvm-objdump -ErrorAction SilentlyContinue).Source, "C:\Program Files\LLVM\bin\llvm-objdump.exe", (Join-Path (Find-Ndk) "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe"))) { if ($c -and (Test-Path $c)) { $objdump = $c; break } }
    if ($objdump) {
        # disasm_regions.txt: "name 0xstart 0xend" per line (image offsets); edit/push it to request more code
        $regions = @()
        foreach ($line in (Get-Content (Join-Path $PSScriptRoot "disasm_regions.txt") -ErrorAction SilentlyContinue)) {
            $t = ($line -split '#')[0].Trim()
            if ($t) { $p = $t -split '\s+'; $regions += ,@($p[0], [Convert]::ToInt64($p[1], 16), [Convert]::ToInt64($p[2], 16)) }
        }
        foreach ($r in $regions) {
            & $objdump -d --no-show-raw-insn ("--start-address=0x{0:x}" -f $r[1]) ("--stop-address=0x{0:x}" -f $r[2]) $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs ("disasm_" + $r[0] + ".txt"))
        }
    } else { Write-Host "llvm-objdump not found: skipped disassembly dumps" }
    if ($localDataForScan -and (Test-Path $localDataForScan)) { python scan_data.py $localDataForScan | Out-File -Encoding utf8 (Join-Path $logs "data_scan.txt") }
    if (Test-Path xref_targets.txt) { python macho_xref.py $fw.FullName xref_targets.txt | Out-File -Encoding utf8 (Join-Path $logs "xref.txt") }
    if (Test-Path funcs_targets.txt) { python macho_objc.py $fw.FullName --funcs funcs_targets.txt 2 | Out-File -Encoding utf8 (Join-Path $logs "funcs.txt") }
    if (-not $Quick) { python compact_unwind.py $fw.FullName | Out-File -Encoding utf8 (Join-Path $logs "unwind_summary.txt") }
    if (Test-Path (Join-Path $logs 'exports_Unity.txt')) { Write-Host "exports: $((Get-Content (Join-Path $logs 'exports_Unity.txt')).Count) _Unity* symbols" }
}

# ---- deploy ----
& $adb devices | Tee-Object -FilePath (Join-Path $logs "adb_devices.txt")
& $adb push xnuroid_loader /data/local/tmp/xnuroid_loader | Out-Null
& $adb push throw_frames.txt /data/local/tmp/throw_frames.txt | Out-Null
& $adb shell chmod 755 /data/local/tmp/xnuroid_loader

# ---- sync game data: push files that exist in the IPA's Data folder but are missing on the phone ----
if ($fw) {
    $appDir = Split-Path (Split-Path $fw.DirectoryName)
    $localData = Join-Path $appDir "Data"
    if (Test-Path $localData) {
        $remote = @{}
        (& $adb shell 'cd /data/local/tmp/game/Data 2>/dev/null && find . -type f') | ForEach-Object { $r = $_.Trim() -replace '^\./', ''; if ($r) { $remote[$r] = $true } }
        $root = (Resolve-Path $localData).Path
        $pushed = New-Object System.Collections.Generic.List[string]
        foreach ($f in (Get-ChildItem $localData -Recurse -File)) {
            $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
            if (-not $remote.ContainsKey($rel)) {
                $dst = "/data/local/tmp/game/Data/$rel"
                & $adb shell ("mkdir -p '" + (Split-Path $dst -Parent).Replace('\', '/') + "'") | Out-Null
                $src = $f.FullName
                if ($src.Length -ge 240) {
                    # adb cannot open Windows paths over MAX_PATH; stage through a short temp file (\\?\ prefix lifts the limit)
                    $tmp = Join-Path $env:TEMP "adb_sync_stage.tmp"
                    try { [System.IO.File]::Copy("\\?\" + $src, $tmp, $true); $src = $tmp }
                    catch { $pushed.Add("FAILED (long path, copy): $rel"); continue }
                }
                & $adb push $src $dst 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) { $pushed.Add("FAILED (adb push): $rel"); continue }
                $pushed.Add("$rel ($($f.Length) bytes)")
            }
        }
        Write-Host "data sync: $($remote.Count) files already on phone, pushed $($pushed.Count)"
        $pushed | Out-File -Encoding utf8 (Join-Path $logs "data_sync.txt")
    } else { Write-Host "data sync skipped: $localData not found" }
}

# ---- test matrix ----
$runs = @()
$matrix = Join-Path $PSScriptRoot "run_matrix.txt"   # "name|ENV=1 ENV2=x" per line; '#' comments
if (Test-Path $matrix) {
    foreach ($line in (Get-Content $matrix)) {
        $t = ($line -split '#')[0].Trim()
        if ($t) { $p = $t -split '\|', 2; $runs += @{ name = $p[0].Trim(); env = $(if ($p.Count -gt 1) { $p[1].Trim() } else { "" }) } }
    }
}
if ($runs.Count -eq 0) { $runs = @( @{ name = "1_default"; env = "" } ) }
& $adb shell svc power stayon true | Out-Null
& $adb shell input keyevent KEYCODE_WAKEUP | Out-Null
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("run_phases $stamp  commit: $(git rev-parse --short HEAD)")
foreach ($r in $runs) {
    $n = $r.name
    Write-Host "=== $n ($($r.env)) ==="
    & $adb logcat -c
    # Run detached on the phone (a dropped wireless-adb connection must not kill the test) and poll for the exit file.
    $sh = "cd /data/local/tmp`n" + $r.env + " timeout 240 ./xnuroid_loader > run_$n.out 2> run_$n.err`necho EXIT=`$? > run_$n.exit`n"
    $tmpSh = Join-Path $env:TEMP "run_$n.sh"
    [System.IO.File]::WriteAllText($tmpSh, $sh)
    & $adb shell "rm -f /data/local/tmp/run_$n.exit" | Out-Null
    & $adb push $tmpSh "/data/local/tmp/run_$n.sh" | Out-Null
    & $adb shell "nohup sh /data/local/tmp/run_$n.sh >/dev/null 2>&1 &" | Out-Null
    $exit = ""
    for ($i = 0; $i -lt 180; $i++) {
        Start-Sleep -Seconds 2
        $o = (& $adb shell "cat /data/local/tmp/run_$n.exit 2>/dev/null") 2>$null
        if ($o -match "EXIT=") { $exit = ($o | Select-Object -Last 1).Trim(); break }
    }
    if (-not $exit) { $exit = "EXIT=?(timeout waiting for device)" }
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
& $adb shell svc power stayon false | Out-Null
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
