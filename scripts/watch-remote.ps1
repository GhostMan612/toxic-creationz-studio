param(
    [string]$RepoPath = "C:\toxic-creationz-studio",
    [int]$IntervalSeconds = 30
)

Set-Location $RepoPath
$InboxFile = Join-Path $RepoPath ".hermes_inbox.md"
$LastMainSha = ""
$LastBriefHash = ""

Write-Host "[Heartbeat] Watching origin/main & docs/SHARED_BRIEF.md every ${IntervalSeconds}s..." -ForegroundColor Cyan

while ($true) {
    try {
        git fetch origin --quiet 2>$null

        $remoteMainSha = (git rev-parse origin/main 2>$null).Trim()
        $currentBranch = (git rev-parse --abbrev-ref HEAD 2>$null).Trim()
        $briefBlobHash = (git rev-parse origin/main:docs/SHARED_BRIEF.md 2>$null).Trim()

        $mainChanged  = ($remoteMainSha -and $remoteMainSha -ne $LastMainSha)
        $briefChanged = ($briefBlobHash -and $briefBlobHash -ne $LastBriefHash)

        if ($mainChanged -or $briefChanged) {
            $isDirty = git status --porcelain
            if ($currentBranch -eq "main" -and -not $isDirty) {
                git pull --ff-only origin main --quiet 2>$null
                Write-Host "[Heartbeat] Fast-forwarded local main -> $remoteMainSha" -ForegroundColor Green
            }

            $recentLog = git log -n 3 --oneline origin/main
            $prSummary = gh pr list --state all --limit 4 --json number,title,state,headRefName,mergedAt | ConvertFrom-Json

            $lines = @(
                "# Remote Repo Event ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))"
                "- **origin/main:** ``$remoteMainSha``"
                "- **SHARED_BRIEF.md changed:** $briefChanged"
                ""
                "## Recent Commits on origin/main"
                '```'
                ($recentLog -join "`n")
                '```'
                ""
                "## Recent PRs"
            )
            foreach ($pr in $prSummary) {
                $lines += "- PR #$($pr.number) [$($pr.state)] ``$($pr.headRefName)`` - $($pr.title)"
            }
            $lines += ""
            $lines += "Action: Read ``docs/SHARED_BRIEF.md`` to check Muse's latest status and next Work Order."

            $lines -join "`n" | Set-Content -Path $InboxFile -Encoding UTF8

            if ($LastMainSha -ne "") {
                [console]::Beep(880, 220)
                Write-Host "[ALERT] Remote update detected ($remoteMainSha). Wrote .hermes_inbox.md" -ForegroundColor Yellow
            }

            $LastMainSha = $remoteMainSha
            $LastBriefHash = $briefBlobHash
        }
    }
    catch {
        Write-Host "[Heartbeat] Poll error: $_" -ForegroundColor DarkYellow
    }

    Start-Sleep -Seconds $IntervalSeconds
}
