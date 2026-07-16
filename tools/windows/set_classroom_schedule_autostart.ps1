[CmdletBinding()]
param(
    [string]$LauncherPath = "",
    [switch]$Remove
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$startupDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::Startup)
$shortcutPath = Join-Path $startupDirectory "Classroom Schedule Server.lnk"

if ($Remove) {
    if (Test-Path -LiteralPath $shortcutPath) {
        Remove-Item -LiteralPath $shortcutPath -Force
        Write-Host "Removed login auto-start: $shortcutPath"
    } else {
        Write-Host "Login auto-start is already disabled."
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($LauncherPath)) {
    throw "LauncherPath is required when enabling login auto-start."
}

$resolvedLauncher = (Resolve-Path -LiteralPath $LauncherPath).Path
if ([IO.Path]::GetExtension($resolvedLauncher) -notin @(".bat", ".cmd")) {
    throw "LauncherPath must point to a .bat or .cmd file."
}

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $resolvedLauncher
$shortcut.Arguments = "--autostart"
$shortcut.WorkingDirectory = Split-Path -Parent $resolvedLauncher
$shortcut.WindowStyle = 7
$shortcut.Description = "Start the local classroom schedule server after Windows login."
$shortcut.Save()

Write-Host "Installed login auto-start: $shortcutPath"
Write-Host "Target: $resolvedLauncher --autostart"
