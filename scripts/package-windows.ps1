param(
    [string]$BuildConfig = "Release",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$ExePath = Join-Path "build\AIDAW_artefacts" "$BuildConfig\AI DAW.exe"
if (-not (Test-Path $ExePath)) {
    throw "Compiled executable not found at $ExePath"
}

$StageDir = Join-Path $OutputDir "ToxicCreationzStudio-win64"
$ZipPath  = Join-Path $OutputDir "ToxicCreationzStudio-win64.zip"

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
if (Test-Path $ZipPath)  { Remove-Item -Force $ZipPath }

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "mcp") | Out-Null

Copy-Item $ExePath (Join-Path $StageDir "Toxic Creationz Studio.exe")
Copy-Item "mcp\daw_mcp_server.py" (Join-Path $StageDir "mcp\daw_mcp_server.py")
Copy-Item "mcp\requirements.txt"  (Join-Path $StageDir "mcp\requirements.txt")
if (Test-Path "README.md") {
    Copy-Item "README.md" (Join-Path $StageDir "README.md")
}

Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath -Force
Write-Host "[PACKAGE] Created $ZipPath ($((Get-Item $ZipPath).Length) bytes)" -ForegroundColor Green
