param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$Avd,
    [string]$Output = (Join-Path $PSScriptRoot '../../build/m4-emulator')
)
$ErrorActionPreference = 'Stop'
$taskEmulator = Join-Path $SdkRoot 'emulator/emulator.exe'
$taskAdb = Join-Path $SdkRoot 'platform-tools/adb.exe'
$taskAvds = @(& $taskEmulator -list-avds)
if ($taskAvds -notcontains $Avd) { throw "AVD does not exist: $Avd" }
$taskDevices = @(& $taskAdb devices)
if ($taskDevices | Where-Object { $_ -match '^emulator-' }) {
    throw 'An emulator is already running. Select it explicitly; this launcher never stops existing devices.'
}
$taskOutput = [System.IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskSavedProxy = @{}
try {
    foreach ($taskName in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY')) {
        $taskSavedProxy[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process')
        [Environment]::SetEnvironmentVariable($taskName, $null, 'Process')
    }
    # The emulator itself reads http_proxy, independently of the SDK client's proxy setting.
    Start-Process -FilePath $taskEmulator -ArgumentList @('-avd', $Avd, '-no-window', '-no-audio',
        '-no-boot-anim', '-no-snapshot-load', '-no-snapshot-save', '-gpu', 'swiftshader_indirect') `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskOutput 'stdout.log') `
        -RedirectStandardError (Join-Path $taskOutput 'stderr.log')
} finally {
    foreach ($taskName in $taskSavedProxy.Keys) {
        [Environment]::SetEnvironmentVariable($taskName, $taskSavedProxy[$taskName], 'Process')
    }
}
Write-Output 'Started the existing AVD without inherited HTTP proxy variables; inspect logs and wait for boot before testing.'
