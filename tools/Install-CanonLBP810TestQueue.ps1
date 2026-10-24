param(
    [switch]$RemoveQueue,
    # Deletes and re-adds the queue so Windows drops cached capabilities and default settings.
    [switch]$RecreateQueue
)

$ErrorActionPreference = 'Stop'
$queueName = 'Canon LBP-810 CAPT App Queue'
$driverName = 'Canon LBP-810 CAPT'
$taskName = 'CanonLBP810 CAPT Bridge'
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageDirectory = Join-Path $projectRoot 'driver-package'
$installDirectory = Join-Path $env:ProgramFiles 'CanonLBP810'
$installedExecutable = Join-Path $installDirectory 'lbp810_spool.exe'
$captureDirectory = Join-Path $env:ProgramData 'CanonLBP810\Jobs'
$capturePath = Join-Path $captureDirectory 'from-windows-app.pwg'
$infPath = Join-Path $packageDirectory 'CanonLBP810.inf'
$catalogPath = Join-Path $packageDirectory 'CanonLBP810.cat'

function Stop-Bridge {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($task) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    }
    # The task may have been started from an older build directory.
    Get-Process -Name 'lbp810_spool' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
}

function Find-KitTool([string]$name, [string]$architecture) {
    $kitBin = 'C:\Program Files (x86)\Windows Kits\10\bin'
    $tool = Get-ChildItem -Path $kitBin -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "$architecture\$name" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $tool) {
        throw "$name was not found in the Windows SDK/WDK under $kitBin."
    }
    return $tool
}

$isAdministrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdministrator) {
    throw 'Run this script from an elevated PowerShell session.'
}

if ($RemoveQueue) {
    Stop-Bridge
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
    if (Get-Printer -Name $queueName -ErrorAction SilentlyContinue) {
        Remove-Printer -Name $queueName
    }
    Remove-PrinterPort -Name $capturePath -ErrorAction SilentlyContinue
    Remove-Item -Path $installDirectory -Recurse -Force -ErrorAction SilentlyContinue
    return
}

$executable = @('build-release', 'build-msvc') |
    ForEach-Object { Join-Path $projectRoot "$_\lbp810_spool.exe" } |
    Where-Object { Test-Path $_ } |
    Sort-Object { (Get-Item $_).LastWriteTime } -Descending |
    Select-Object -First 1
if (-not $executable) {
    throw 'lbp810_spool.exe was not found. Build the project first (see README_RU.md).'
}
foreach ($requiredPath in @($infPath, (Join-Path $packageDirectory 'CanonLBP810.xml'),
                            (Join-Path $packageDirectory 'CanonLBP810-manifest.ini'))) {
    if (-not (Test-Path $requiredPath)) {
        throw "Required file not found: $requiredPath"
    }
}
$signTool = Find-KitTool 'signtool.exe' 'x64'
$inf2Cat = Find-KitTool 'Inf2Cat.exe' 'x86'

# The catalog must match the current INF/XML/manifest, so it is regenerated on every install.
Remove-Item $catalogPath -Force -ErrorAction SilentlyContinue
& $inf2Cat "/driver:$packageDirectory" '/os:10_x64'
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE."
}

$certificateSubject = 'CN=Canon LBP-810 Local Test Signing'
$certificate = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -eq $certificateSubject -and $_.NotAfter -gt (Get-Date).AddDays(7) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1
if (-not $certificate) {
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certificateSubject `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(2)
}

$certificateFile = Join-Path $env:TEMP ("CanonLBP810-{0}.cer" -f $certificate.Thumbprint)
Export-Certificate -Cert $certificate -FilePath $certificateFile -Force | Out-Null
try {
    Import-Certificate -FilePath $certificateFile -CertStoreLocation Cert:\LocalMachine\Root -ErrorAction SilentlyContinue | Out-Null
    Import-Certificate -FilePath $certificateFile -CertStoreLocation Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue | Out-Null
} finally {
    Remove-Item $certificateFile -Force -ErrorAction SilentlyContinue
}

& $signTool sign /sm /v /fd SHA256 /sha1 $certificate.Thumbprint $catalogPath
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE."
}

& pnputil.exe /add-driver $infPath
if ($LASTEXITCODE -ne 0) {
    throw "pnputil failed with exit code $LASTEXITCODE."
}

# Always (re)register the newest staged package so an update replaces the previous version.
$publishedInf = Get-WindowsDriver -Online -All |
    Where-Object { $_.OriginalFileName -match 'canonlbp810\.inf$' } |
    Sort-Object { [version]$_.Version } -Descending |
    Select-Object -First 1 -ExpandProperty Driver
if (-not $publishedInf) {
    throw 'The Canon v4 INF was staged but its published oem INF could not be found.'
}
Add-PrinterDriver -Name $driverName -InfPath (Join-Path $env:windir "INF\$publishedInf") `
    -PrinterEnvironment 'Windows x64'

Stop-Bridge
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -Path $executable -Destination $installedExecutable -Force

New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
try {
    Add-PrinterPort -Name $capturePath -ErrorAction Stop
} catch {
    if (-not (Get-PrinterPort -Name $capturePath -ErrorAction SilentlyContinue)) {
        throw
    }
}

if ($RecreateQueue -and (Get-Printer -Name $queueName -ErrorAction SilentlyContinue)) {
    Remove-Printer -Name $queueName
}
if (Get-Printer -Name $queueName -ErrorAction SilentlyContinue) {
    Set-Printer -Name $queueName -DriverName $driverName -PortName $capturePath
} else {
    Add-Printer -Name $queueName -DriverName $driverName -PortName $capturePath
}

$action = New-ScheduledTaskAction -Execute $installedExecutable -Argument '--watch-capture' `
    -WorkingDirectory $installDirectory
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'Converts Canon LBP-810 PWG jobs to CAPT and sends them over USB.' -Force | Out-Null
Start-ScheduledTask -TaskName $taskName

Get-Printer -Name $queueName | Select-Object Name,DriverName,PortName,PrinterStatus
Get-PrinterDriver -Name $driverName | Select-Object Name,MajorVersion,DriverVersion
Write-Output "Bridge installed to $installedExecutable; log: $(Join-Path $captureDirectory 'bridge.log')"
