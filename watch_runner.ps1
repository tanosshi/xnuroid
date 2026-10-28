# Leave this running in a terminal. Every 20 s it fetches this branch; when there is a new commit it
# fast-forwards, rebuilds, deploys, runs run_matrix.txt on the phone and pushes the logs to branch
# "device-logs" (run_phases.ps1 -Quick -Push). Stop with Ctrl+C.
#
#   powershell -ExecutionPolicy Bypass -File watch_runner.ps1
#
# NOTE: it builds and runs whatever is committed on the branch below, on your PC and your phone.
param([string]$Branch = "main", [int]$Seconds = 20)
Set-Location $PSScriptRoot
$last = ""
Write-Host "watching origin/$Branch (Ctrl+C to stop)"
while ($true) {
    git fetch -q origin $Branch 2>$null
    $remote = (git rev-parse "origin/$Branch").Trim()
    if ($remote -and $remote -ne $last) {
        Write-Host "$(Get-Date -Format HH:mm:ss) new commit $remote"
        git checkout -q $Branch 2>$null
        git merge -q --ff-only "origin/$Branch" 2>&1 | Out-Host
        powershell -ExecutionPolicy Bypass -File run_phases.ps1 -Quick -Push
        $last = $remote
        Write-Host "$(Get-Date -Format HH:mm:ss) done, waiting for the next commit"
    }
    Start-Sleep -Seconds $Seconds
}
