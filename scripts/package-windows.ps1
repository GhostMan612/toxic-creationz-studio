param(
    [string]$BuildConfig = "Release",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$ExePath = Join-Path "build\AIDAW_artefacts" "$BuildConfig\AI DAW.exe"
if (-not (Test-Path $ExePath)) {
    throw "Compiled executable not found at $ExePath"
}

# A zero-filled / partially linked exe passes Test-Path but silently poisons
# the portable release. Verify the PE MZ signature before packaging.
$Stream = [System.IO.File]::OpenRead((Resolve-Path $ExePath).Path)
try {
    $Header = New-Object byte[] 2
    if ($Stream.Read($Header, 0, 2) -ne 2 -or $Header[0] -ne 0x4D -or $Header[1] -ne 0x5A) {
        throw "Compiled executable is not a valid PE image: $ExePath"
    }
} finally {
    $Stream.Dispose()
}

$StageDir = Join-Path $OutputDir "ToxicCreationzStudio-win64"
$ZipPath  = Join-Path $OutputDir "ToxicCreationzStudio-win64.zip"

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
if (Test-Path $ZipPath)  { Remove-Item -Force $ZipPath }

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "mcp") | Out-Null

Copy-Item $ExePath (Join-Path $StageDir "Toxic Creationz Studio.exe")
Copy-Item "mcp\daw_mcp_server.py"  (Join-Path $StageDir "mcp\daw_mcp_server.py")
Copy-Item "mcp\ai_audio_worker.py" (Join-Path $StageDir "mcp\ai_audio_worker.py")
Copy-Item "mcp\requirements.txt"   (Join-Path $StageDir "mcp\requirements.txt")
Copy-Item "mcp\audio_identify_worker.py" (Join-Path $StageDir "mcp\audio_identify_worker.py")
Copy-Item "mcp\voice_clone_worker.py"    (Join-Path $StageDir "mcp\voice_clone_worker.py")
if (Test-Path "README.md") {
    Copy-Item "README.md" (Join-Path $StageDir "README.md")
}
if (Test-Path "docs\GHOST_KNOWLEDGE.md") {
    Copy-Item "docs\GHOST_KNOWLEDGE.md" (Join-Path $StageDir "GHOST_KNOWLEDGE.md")
}

Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $ZipPath).Path)
try {
    if ($Archive.Entries.Count -lt 1) { throw "Release zip has no entries: $ZipPath" }
} finally {
    $Archive.Dispose()
}

Write-Host "[PACKAGE] Created $ZipPath ($((Get-Item $ZipPath).Length) bytes)" -ForegroundColor Green
