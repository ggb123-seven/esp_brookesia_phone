param(
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $ProjectRoot "build"

Write-Host "Project root: $ProjectRoot"

if (Test-Path -LiteralPath $BuildDir) {
    $resolvedBuild = Resolve-Path -LiteralPath $BuildDir
    $projectPrefix = $ProjectRoot.Path.TrimEnd('\') + '\'

    if (-not $resolvedBuild.Path.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove build directory outside the project: $resolvedBuild"
    }

    Write-Host "Removing build cache: $resolvedBuild"
    Remove-Item -LiteralPath $resolvedBuild.Path -Recurse -Force
} else {
    Write-Host "No build directory found."
}

if ($Reconfigure) {
    Write-Host "Reconfiguring ESP-IDF project for esp32p4..."
    & idf.py set-target esp32p4
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & idf.py reconfigure
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Host "Done."
