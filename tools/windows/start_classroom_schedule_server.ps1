[CmdletBinding()]
param(
    [switch]$AutoStart,
    [switch]$CheckOnly,
    [switch]$SkipSessionCheck
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Get-ProcessSetting {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Default = ""
    )

    $value = [Environment]::GetEnvironmentVariable($Name, "Process")
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $Default
    }
    return $value.Trim()
}

function ConvertTo-SettingBoolean {
    param(
        [string]$Value,
        [bool]$Default
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $Default
    }
    switch ($Value.Trim().ToLowerInvariant()) {
        { $_ -in @("1", "true", "yes", "on") } { return $true }
        { $_ -in @("0", "false", "no", "off") } { return $false }
        default { throw "Invalid boolean setting: $Value" }
    }
}

function Resolve-ProjectFile {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectDirectory,
        [Parameter(Mandatory = $true)][string]$PathText
    )

    if ([IO.Path]::IsPathRooted($PathText)) {
        return [IO.Path]::GetFullPath($PathText)
    }
    return [IO.Path]::GetFullPath((Join-Path $ProjectDirectory $PathText))
}

function Get-SessionStorageStatePath {
    param([Parameter(Mandatory = $true)][string]$SessionFile)

    try {
        $config = Get-Content -LiteralPath $SessionFile -Raw | ConvertFrom-Json
        $property = $config.PSObject.Properties | Where-Object { $_.Name -eq "playwright_storage_state" } | Select-Object -First 1
        if ($null -eq $property -or [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            return $null
        }
        $pathText = [string]$property.Value
        if ([IO.Path]::IsPathRooted($pathText)) {
            return [IO.Path]::GetFullPath($pathText)
        }
        return [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $SessionFile) $pathText))
    } catch {
        return $null
    }
}

function Get-PythonCommand {
    $python = Get-Command "python.exe" -ErrorAction SilentlyContinue
    if ($null -ne $python) {
        return [PSCustomObject]@{
            Executable = $python.Source
            Prefix = @()
        }
    }

    $py = Get-Command "py.exe" -ErrorAction SilentlyContinue
    if ($null -ne $py) {
        return [PSCustomObject]@{
            Executable = $py.Source
            Prefix = @("-3")
        }
    }

    throw "Python 3 was not found. Install Python and enable Add Python to PATH."
}

function Invoke-PythonCapture {
    param(
        [Parameter(Mandatory = $true)]$PythonCommand,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutSeconds = 30
    )

    $allArguments = @($PythonCommand.Prefix) + $Arguments
    $quotedArguments = foreach ($argument in $allArguments) {
        '"' + $argument.Replace('"', '\"') + '"'
    }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $PythonCommand.Executable
    $startInfo.Arguments = $quotedArguments -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Python process could not be started."
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
                $process.WaitForExit()
            } catch {
                # The process may have exited between the timeout and Kill().
            }
            return [PSCustomObject]@{
                ExitCode = 124
                Output = ""
                TimedOut = $true
            }
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $output = (@($stdout, $stderr) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join [Environment]::NewLine
        return [PSCustomObject]@{
            ExitCode = $process.ExitCode
            Output = $output.Trim()
            TimedOut = $false
        }
    } finally {
        $process.Dispose()
    }
}

function Test-UsableIPv4Address {
    param([string]$Address)

    $parsed = $null
    if (-not [Net.IPAddress]::TryParse($Address, [ref]$parsed)) {
        return $false
    }
    if ($parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        return $false
    }
    $bytes = $parsed.GetAddressBytes()
    return $bytes[0] -ne 0 -and
        $bytes[0] -ne 127 -and
        -not ($bytes[0] -eq 169 -and $bytes[1] -eq 254)
}

function Get-PreferredLanIPv4Address {
    try {
        $defaultRoutes = Get-NetRoute -AddressFamily IPv4 -DestinationPrefix "0.0.0.0/0" -ErrorAction Stop
        $candidates = foreach ($route in $defaultRoutes) {
            $adapter = Get-NetAdapter -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue
            if ($null -eq $adapter -or -not $adapter.HardwareInterface -or $adapter.Status -ne "Up") {
                continue
            }
            $addresses = Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue
            foreach ($address in @($addresses)) {
                if (Test-UsableIPv4Address $address.IPAddress) {
                    $interfaceMetric = 999999
                    try {
                        $interface = Get-NetIPInterface -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex -ErrorAction Stop
                        $interfaceMetric = [int]$interface.InterfaceMetric
                    } catch {
                        # Interface metric is only used to choose between valid adapters.
                    }
                    [PSCustomObject]@{
                        Address = $address.IPAddress
                        Metric = $interfaceMetric + [int]$route.RouteMetric
                    }
                }
            }
        }
        $selected = $candidates | Sort-Object Metric | Select-Object -First 1
        if ($null -ne $selected) {
            return $selected.Address
        }
    } catch {
        # Fall through to the socket-based method on older Windows versions.
    }

    try {
        $socket = New-Object Net.Sockets.UdpClient
        try {
            $socket.Connect("8.8.8.8", 53)
            $address = ([Net.IPEndPoint]$socket.Client.LocalEndPoint).Address.IPAddressToString
            if (Test-UsableIPv4Address $address) {
                return $address
            }
        } finally {
            $socket.Dispose()
        }
    } catch {
        return $null
    }
    return $null
}

function Get-LanIPv4AddressWithRetry {
    param(
        [int]$Attempts,
        [int]$DelaySeconds
    )

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        $address = Get-PreferredLanIPv4Address
        if (-not [string]::IsNullOrWhiteSpace($address)) {
            return $address
        }
        if ($attempt -lt $Attempts) {
            Start-Sleep -Seconds $DelaySeconds
        }
    }
    throw "No usable LAN IPv4 address was found. Connect this PC to the same network as the ESP32."
}

function Test-ExistingScheduleServer {
    param([int]$Port)

    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2
        if ($response.StatusCode -eq 200) {
            return $true
        }
    } catch {
        return $false
    }
    return $false
}

function Test-CommandLineContainsPath {
    param(
        [string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$ExpectedPath
    )

    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $false
    }
    $normalizedExpected = [IO.Path]::GetFullPath($ExpectedPath).Replace("/", "\").ToLowerInvariant()
    $normalizedCommandLine = $CommandLine.Replace("/", "\").ToLowerInvariant()
    return $normalizedCommandLine.Contains($normalizedExpected)
}

function Get-ScheduleServerPortState {
    param(
        [int]$Port,
        [Parameter(Mandatory = $true)][string]$ServerScript,
        [Parameter(Mandatory = $true)][string]$LauncherScript
    )

    $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
    if ($listeners.Count -eq 0) {
        return [PSCustomObject]@{ Kind = "free"; ProcessId = $null; Healthy = $false }
    }

    $processIds = @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
    if ($processIds.Count -ne 1) {
        return [PSCustomObject]@{ Kind = "foreign"; ProcessId = ($processIds -join ", "); Healthy = $false }
    }

    $processId = [int]$processIds[0]
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return [PSCustomObject]@{ Kind = "foreign"; ProcessId = $processId; Healthy = $false }
    }
    $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.ParentProcessId)" -ErrorAction SilentlyContinue
    $serverMatches = Test-CommandLineContainsPath -CommandLine $process.CommandLine -ExpectedPath $ServerScript
    $launcherMatches = ($null -ne $parent) -and `
        (Test-CommandLineContainsPath -CommandLine $parent.CommandLine -ExpectedPath $LauncherScript)
    if (-not $serverMatches -or -not $launcherMatches) {
        return [PSCustomObject]@{ Kind = "foreign"; ProcessId = $processId; Healthy = $false }
    }

    return [PSCustomObject]@{
        Kind = "project_server"
        ProcessId = $processId
        Healthy = Test-ExistingScheduleServer $Port
        StartedAtUtc = ([datetime]$process.CreationDate).ToUniversalTime()
    }
}

function Stop-VerifiedScheduleServer {
    param(
        [int]$Port,
        [int]$ExpectedProcessId,
        [Parameter(Mandatory = $true)][string]$ServerScript,
        [Parameter(Mandatory = $true)][string]$LauncherScript
    )

    $current = Get-ScheduleServerPortState -Port $Port -ServerScript $ServerScript -LauncherScript $LauncherScript
    if ($current.Kind -ne "project_server" -or $current.ProcessId -ne $ExpectedProcessId) {
        throw "The existing schedule server changed before restart; no process was stopped."
    }

    Stop-Process -Id $ExpectedProcessId -Force -ErrorAction Stop
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        Start-Sleep -Milliseconds 500
        $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
        if ($listeners.Count -eq 0) {
            return
        }
    }
    throw "The previous schedule server did not release port $Port in time."
}

function Test-EamsSession {
    param(
        [Parameter(Mandatory = $true)]$PythonCommand,
        [Parameter(Mandatory = $true)][string]$ServerScript,
        [Parameter(Mandatory = $true)][string]$SessionFile,
        [string]$LoginFile,
        [int]$TimeoutSeconds
    )

    $arguments = @(
        "-B", "-X", "utf8", $ServerScript,
        "--probe-eams",
        "--session-file", $SessionFile,
        "--probe-date", (Get-Date -Format "yyyy-MM-dd"),
        "--upstream-timeout-seconds", "$TimeoutSeconds"
    )
    if (-not [string]::IsNullOrWhiteSpace($LoginFile) -and (Test-Path -LiteralPath $LoginFile -PathType Leaf)) {
        $arguments += @("--eams-login-file", $LoginFile)
    }

    $probe = Invoke-PythonCapture -PythonCommand $PythonCommand -Arguments $arguments -TimeoutSeconds ($TimeoutSeconds + 8)
    if ($probe.TimedOut) {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $true
            CanRefresh = $false
            Message = "The EAMS session check timed out. Check the network and WebVPN reachability, then try again."
        }
    }
    if ($null -ne $probe.ExitCode -and $probe.ExitCode -ne 0) {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $true
            CanRefresh = $false
            Message = "The EAMS session check could not run. Check Python and the local session configuration."
        }
    }

    try {
        $result = $probe.Output | ConvertFrom-Json
    } catch {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $false
            CanRefresh = $false
            Message = "The EAMS session check returned an invalid response."
        }
    }

    $sessionProperty = $result.PSObject.Properties | Where-Object { $_.Name -eq "session" } | Select-Object -First 1
    if ($null -eq $sessionProperty -or $null -eq $sessionProperty.Value) {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $false
            CanRefresh = $false
            Message = "The EAMS session check returned no session result."
        }
    }

    $session = $sessionProperty.Value
    if ($session.status -eq "request_failed") {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $true
            CanRefresh = $false
            Message = "The school system is temporarily unreachable. Check the network and try again."
        }
    }
    if ($session.status -ne "ok") {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $false
            CanRefresh = $session.status -notin @("placeholder_upstream_url", "placeholder_headers")
            Message = "The EAMS session is unavailable (status: $($session.status)). Run the launcher normally to refresh it."
        }
    }
    if ([bool]$session.looks_like_login) {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $false
            CanRefresh = $true
            Message = "The EAMS session has expired and returned to the login page. Run the launcher normally to refresh it."
        }
    }
    if (-not ([bool]$session.looks_like_eams) -and -not ([bool]$session.starts_json)) {
        return [PSCustomObject]@{
            IsValid = $false
            IsRetryable = $false
            CanRefresh = $true
            Message = "The session response is not recognized as EAMS data. Run the launcher normally to refresh it."
        }
    }

    return [PSCustomObject]@{
        IsValid = $true
        IsRetryable = $false
        CanRefresh = $false
        Message = "EAMS session is valid."
    }
}

function Invoke-EamsSessionRefresh {
    param(
        [Parameter(Mandatory = $true)]$PythonCommand,
        [Parameter(Mandatory = $true)][string]$RefreshScript,
        [Parameter(Mandatory = $true)][string]$SessionFile,
        [Parameter(Mandatory = $true)][string]$LoginFile,
        [Parameter(Mandatory = $true)][string]$ProfileDirectory,
        [string]$EdgeExecutable,
        [int]$LoginTimeoutSeconds,
        [int]$UpstreamTimeoutSeconds
    )

    $arguments = @(
        "-B", "-X", "utf8", $RefreshScript,
        "--session-file", $SessionFile,
        "--login-file", $LoginFile,
        "--profile-dir", $ProfileDirectory,
        "--timeout-seconds", "$LoginTimeoutSeconds",
        "--upstream-timeout-seconds", "$UpstreamTimeoutSeconds"
    )
    if (-not [string]::IsNullOrWhiteSpace($EdgeExecutable)) {
        $arguments += @("--edge-executable", $EdgeExecutable)
    }

    $refresh = Invoke-PythonCapture `
        -PythonCommand $PythonCommand `
        -Arguments $arguments `
        -TimeoutSeconds ($LoginTimeoutSeconds + $UpstreamTimeoutSeconds + 45)
    if ($refresh.TimedOut) {
        return [PSCustomObject]@{
            Success = $false
            Message = "The EAMS login window did not finish in time. Run the launcher again to retry."
        }
    }

    $result = $null
    try {
        $result = $refresh.Output | ConvertFrom-Json
    } catch {
        return [PSCustomObject]@{
            Success = $false
            Message = "The EAMS session refresh returned an invalid response."
        }
    }
    if ($null -eq $result) {
        return [PSCustomObject]@{
            Success = $false
            Message = "The EAMS session refresh returned no result."
        }
    }
    $okProperty = $result.PSObject.Properties | Where-Object { $_.Name -eq "ok" } | Select-Object -First 1
    if ($null -eq $okProperty -or -not [bool]$okProperty.Value) {
        $messageProperty = $result.PSObject.Properties | Where-Object { $_.Name -eq "message" } | Select-Object -First 1
        $message = if ($null -ne $messageProperty -and -not [string]::IsNullOrWhiteSpace([string]$messageProperty.Value)) {
            [string]$messageProperty.Value
        } else {
            "The EAMS session refresh failed."
        }
        return [PSCustomObject]@{ Success = $false; Message = $message }
    }
    return [PSCustomObject]@{ Success = $true; Message = "EAMS session updated." }
}

function Show-AutoStartError {
    param([string]$Message)

    if (-not $AutoStart) {
        return
    }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show(
            $Message,
            "Classroom schedule server",
            [Windows.Forms.MessageBoxButtons]::OK,
            [Windows.Forms.MessageBoxIcon]::Warning
        )
    } catch {
        # Console output remains available if a message box cannot be shown.
    }
}

function Invoke-Launcher {
    $scriptDirectory = Split-Path -Parent $PSCommandPath
    $defaultProjectDirectory = [IO.Path]::GetFullPath((Join-Path $scriptDirectory "..\.."))
    $projectDirectory = Get-ProcessSetting -Name "PROJECT_DIR" -Default $defaultProjectDirectory
    $projectDirectory = [IO.Path]::GetFullPath($projectDirectory)
    if (-not (Test-Path -LiteralPath $projectDirectory -PathType Container)) {
        throw "Project directory not found: $projectDirectory"
    }

    $serverScript = Join-Path $projectDirectory "tools\real_classroom_schedule_server.py"
    if (-not (Test-Path -LiteralPath $serverScript -PathType Leaf)) {
        throw "Server script not found: $serverScript"
    }
    $refreshScript = Join-Path $projectDirectory "tools\eams_session_refresh.py"
    if (-not (Test-Path -LiteralPath $refreshScript -PathType Leaf)) {
        throw "EAMS session refresh script not found: $refreshScript"
    }

    $python = Get-PythonCommand
    $serverHost = Get-ProcessSetting -Name "SERVER_HOST" -Default "0.0.0.0"
    $serverPortText = Get-ProcessSetting -Name "SERVER_PORT" -Default "8080"
    $serverPort = 0
    if (-not [int]::TryParse($serverPortText, [ref]$serverPort) -or $serverPort -lt 1 -or $serverPort -gt 65535) {
        throw "SERVER_PORT must be between 1 and 65535."
    }

    $displayAddress = Get-ProcessSetting -Name "DISPLAY_IP"
    if ([string]::IsNullOrWhiteSpace($displayAddress)) {
        $ipAttempts = if ($AutoStart) { 6 } else { 1 }
        $displayAddress = Get-LanIPv4AddressWithRetry -Attempts $ipAttempts -DelaySeconds 5
    } elseif (-not (Test-UsableIPv4Address $displayAddress)) {
        throw "DISPLAY_IP is not a usable IPv4 address: $displayAddress"
    }

    $provider = Get-ProcessSetting -Name "SCHEDULE_PROVIDER" -Default "eams-room-occupancy"
    if ($provider -notin @("fixture", "manual-session", "eams-room-occupancy")) {
        throw "Unsupported SCHEDULE_PROVIDER: $provider"
    }

    $fixtureFile = Resolve-ProjectFile -ProjectDirectory $projectDirectory -PathText (Get-ProcessSetting -Name "FIXTURE_PATH" -Default "tools\fixtures\classroom_schedule_fixture.json")
    $sessionFile = Resolve-ProjectFile -ProjectDirectory $projectDirectory -PathText (Get-ProcessSetting -Name "EAMS_SESSION_FILE" -Default ".local-secrets\eams-session.json")
    $loginFile = Resolve-ProjectFile -ProjectDirectory $projectDirectory -PathText (Get-ProcessSetting -Name "EAMS_LOGIN_FILE" -Default ".local-secrets\eams-login.json")
    $storageStateFile = Get-SessionStorageStatePath -SessionFile $sessionFile
    $timeoutText = Get-ProcessSetting -Name "UPSTREAM_TIMEOUT_SECONDS" -Default "12"
    $timeoutSeconds = 0
    if (-not [int]::TryParse($timeoutText, [ref]$timeoutSeconds) -or $timeoutSeconds -lt 1 -or $timeoutSeconds -gt 120) {
        throw "UPSTREAM_TIMEOUT_SECONDS must be between 1 and 120."
    }
    $loginTimeoutText = Get-ProcessSetting -Name "EAMS_LOGIN_TIMEOUT_SECONDS" -Default "600"
    $loginTimeoutSeconds = 0
    if (-not [int]::TryParse($loginTimeoutText, [ref]$loginTimeoutSeconds) -or
        $loginTimeoutSeconds -lt 30 -or $loginTimeoutSeconds -gt 3600) {
        throw "EAMS_LOGIN_TIMEOUT_SECONDS must be between 30 and 3600."
    }
    $browserProfile = Resolve-ProjectFile `
        -ProjectDirectory $projectDirectory `
        -PathText (Get-ProcessSetting -Name "EAMS_BROWSER_PROFILE" -Default ".local-secrets\eams-edge-profile")
    $edgeExecutable = Get-ProcessSetting -Name "EDGE_EXECUTABLE"
    if (-not [string]::IsNullOrWhiteSpace($edgeExecutable)) {
        $edgeExecutable = Resolve-ProjectFile -ProjectDirectory $projectDirectory -PathText $edgeExecutable
    }
    $refreshSession = ConvertTo-SettingBoolean `
        -Value (Get-ProcessSetting -Name "REFRESH_EAMS_SESSION" -Default "1") `
        -Default $true

    if ($provider -eq "fixture") {
        if (-not (Test-Path -LiteralPath $fixtureFile -PathType Leaf)) {
            throw "Fixture file not found: $fixtureFile"
        }
    } elseif (-not (Test-Path -LiteralPath $sessionFile -PathType Leaf)) {
        throw "EAMS session file not found: $sessionFile"
    }

    $checkSession = ConvertTo-SettingBoolean -Value (Get-ProcessSetting -Name "CHECK_EAMS_SESSION" -Default "1") -Default $true
    if ($SkipSessionCheck) {
        $checkSession = $false
    }

    $portState = Get-ScheduleServerPortState `
        -Port $serverPort `
        -ServerScript $serverScript `
        -LauncherScript $PSCommandPath
    if ($portState.Kind -eq "foreign") {
        throw "Port $serverPort is used by a process that is not the managed schedule server (PID: $($portState.ProcessId))."
    }

    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Classroom schedule server" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Project:  $projectDirectory"
    Write-Host "LAN IP:   $displayAddress" -ForegroundColor Green
    Write-Host "Device:   http://${displayAddress}:$serverPort"
    Write-Host "Provider: $provider"
    Write-Host "Tokens:   configured (values are hidden)"

    $sessionRefreshed = $false
    if ($provider -ne "fixture" -and $checkSession) {
        Write-Host "Session:  checking EAMS..." -NoNewline
        $probeAttempts = if ($AutoStart) { 2 } else { 1 }
        $probe = $null
        for ($attempt = 1; $attempt -le $probeAttempts; $attempt++) {
            $probe = Test-EamsSession -PythonCommand $python -ServerScript $serverScript -SessionFile $sessionFile -LoginFile $loginFile -TimeoutSeconds $timeoutSeconds
            if ($probe.IsValid -or -not $probe.IsRetryable -or $attempt -eq $probeAttempts) {
                break
            }
            Start-Sleep -Seconds 5
        }
        if (-not $probe.IsValid) {
            Write-Host " failed" -ForegroundColor Red
            if ($probe.CanRefresh -and $refreshSession -and -not $CheckOnly) {
                Write-Host "Session:  open Edge and complete the slider/login within $loginTimeoutSeconds seconds." -ForegroundColor Yellow
                $refresh = Invoke-EamsSessionRefresh `
                    -PythonCommand $python `
                    -RefreshScript $refreshScript `
                    -SessionFile $sessionFile `
                    -LoginFile $loginFile `
                    -ProfileDirectory $browserProfile `
                    -EdgeExecutable $edgeExecutable `
                    -LoginTimeoutSeconds $loginTimeoutSeconds `
                    -UpstreamTimeoutSeconds $timeoutSeconds
                if (-not $refresh.Success) {
                    $fallbackProbe = Test-EamsSession `
                        -PythonCommand $python `
                        -ServerScript $serverScript `
                        -SessionFile $sessionFile `
                        -LoginFile $loginFile `
                        -TimeoutSeconds $timeoutSeconds
                    if (-not $fallbackProbe.IsValid) {
                        throw $refresh.Message
                    }
                }

                Write-Host "Session:  verifying updated EAMS session..." -NoNewline
                $probe = Test-EamsSession `
                    -PythonCommand $python `
                    -ServerScript $serverScript `
                    -SessionFile $sessionFile `
                    -LoginFile $loginFile `
                    -TimeoutSeconds $timeoutSeconds
                if (-not $probe.IsValid) {
                    Write-Host " failed" -ForegroundColor Red
                    throw $probe.Message
                }
                $sessionRefreshed = $true
                Write-Host " valid" -ForegroundColor Green
            } else {
                throw $probe.Message
            }
        } else {
            Write-Host " valid" -ForegroundColor Green
        }
    } elseif ($provider -ne "fixture") {
        Write-Host "Session:  check skipped" -ForegroundColor Yellow
    }

    if ($CheckOnly) {
        if ($portState.Kind -eq "project_server" -and -not $portState.Healthy) {
            throw "The managed schedule server is listening on port $serverPort but its health check failed."
        }
        Write-Host "Status:   checks passed; server was not started" -ForegroundColor Green
        return 0
    }

    $storageStateIsNewer = $false
    if ($portState.Kind -eq "project_server" -and
        -not [string]::IsNullOrWhiteSpace($storageStateFile) -and
        (Test-Path -LiteralPath $storageStateFile -PathType Leaf)) {
        $storageStateIsNewer = (Get-Item -LiteralPath $storageStateFile).LastWriteTimeUtc -gt $portState.StartedAtUtc
    }

    if ($portState.Kind -eq "project_server") {
        if ($sessionRefreshed -or $storageStateIsNewer) {
            Write-Host "Status:   restarting the managed server to load the updated session" -ForegroundColor Yellow
            Stop-VerifiedScheduleServer `
                -Port $serverPort `
                -ExpectedProcessId $portState.ProcessId `
                -ServerScript $serverScript `
                -LauncherScript $PSCommandPath
            $portState = Get-ScheduleServerPortState `
                -Port $serverPort `
                -ServerScript $serverScript `
                -LauncherScript $PSCommandPath
            if ($portState.Kind -ne "free") {
                throw "Port $serverPort was not released after restarting the managed schedule server."
            }
        } else {
            if (-not $portState.Healthy) {
                throw "The managed schedule server is listening on port $serverPort but its health check failed."
            }
            Write-Host "Status:   server is already running on port $serverPort" -ForegroundColor Green
            return 0
        }
    }

    $env:SCHEDULE_HOST = $serverHost
    $env:SCHEDULE_PORT = "$serverPort"
    $env:SCHEDULE_API_PATH = Get-ProcessSetting -Name "SCHEDULE_API_PATH" -Default "/classroom-schedule/today"
    $env:PARENT_CALL_ALERT_API_PATH = Get-ProcessSetting -Name "PARENT_CALL_ALERT_API_PATH" -Default "/parent-call-alert/trigger"
    $env:PARENT_CALL_ALERT_MODE = Get-ProcessSetting -Name "PARENT_CALL_ALERT_MODE" -Default "mock"
    $env:SCHEDULE_PROVIDER = $provider
    $env:SCHEDULE_FIXTURE = $fixtureFile
    $env:SCHEDULE_EAMS_SESSION_FILE = $sessionFile
    $env:SCHEDULE_EAMS_LOGIN_FILE = $loginFile
    $env:SCHEDULE_UPSTREAM_TIMEOUT_SECONDS = "$timeoutSeconds"

    Write-Host "Status:   starting; keep this window open" -ForegroundColor Green
    Write-Host "Press Ctrl+C to stop." -ForegroundColor DarkGray
    Write-Host "========================================" -ForegroundColor Cyan

    Push-Location $projectDirectory
    try {
        $arguments = @($python.Prefix) + @("-B", "-X", "utf8", $serverScript)
        & $python.Executable @arguments
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

try {
    $exitCode = Invoke-Launcher
    exit $exitCode
} catch {
    $message = $_.Exception.Message
    Write-Host ""
    Write-Host "[ERROR] $message" -ForegroundColor Red
    Show-AutoStartError -Message $message
    exit 1
}
