<# NOTE: pure ASCII on purpose -- Windows PowerShell 5.1 reads a BOM-less .ps1
   as ANSI, and a UTF-8 dash inside a string stops the parser. #>
<#
    boot_iris_once.ps1 -- boot IRIS on the NEXT restart, once.

    The firmware already lists IRIS: installing an EFI System Partition with
    \EFI\BOOT\BOOTX64.EFI on it makes most firmwares add an entry called
    "UEFI OS" by themselves.  The problem is catching it.  This machine's
    firmware boot manager has a timeout of ONE SECOND, with Windows first, so
    the boot menu has to be hit in a window that is easy to miss.

    `bootsequence` is the firmware's own answer to that: a ONE-TIME boot order.
    The firmware uses it for the next boot and then discards it.  It does not
    change the default, does not reorder anything permanently, and needs no
    undo -- come back from IRIS and the machine boots Windows as before.

        powershell -ExecutionPolicy Bypass -File boot_iris_once.ps1   (elevated)

    -Undo clears it again, for a change of mind before restarting.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param([switch]$Undo)

$ErrorActionPreference = 'Stop'

# Its own transcript: this runs in a window that closes, and `-Verb RunAs`
# cannot be combined with output redirection, so the caller has no other way to
# find out what happened.
$LogPath = Join-Path $env:TEMP 'iris-once.log'
try { Start-Transcript -Path $LogPath -Force | Out-Null } catch { }

function Say ([string]$m) { Write-Host "[iris-once] $m" }
function Fail([string]$m) {
    Write-Host "[iris-once] REFUSING: $m" -ForegroundColor Red
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}

$principal = New-Object Security.Principal.WindowsPrincipal(
                 [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'this needs an elevated PowerShell.'
}

if ($Undo) {
    & bcdedit /deletevalue '{fwbootmgr}' bootsequence | Out-Null
    Say 'one-time boot cleared; the next restart is Windows as usual.'
    exit 0
}

# Find the firmware entry that points at an ESP on a disk carrying an IRIS
# partition.  By CONTENT, not by name: "UEFI OS" is a label the firmware makes
# up, and a machine can have more than one.
$iris_disk = (Get-Partition |
    Where-Object { $_.GptType -eq '{53495249-5346-502D-4152-544954494F4E}' } |
    Select-Object -First 1).DiskNumber
if ($null -eq $iris_disk) { Fail 'no disk here has an IRISFS-PARTITION.' }
Say "IRIS's partition is on disk $iris_disk"

$enum = (& bcdedit /enum firmware) -join "`n"
$blocks = $enum -split "(?m)^-{3,}\s*$"
$guid = $null
foreach ($b in $blocks) {
    # Capture the GUID BEFORE testing anything else.  `$Matches` is rewritten by
    # every -match, and the second test here has no capture group -- so reading
    # $Matches[1] after it read the wrong thing, in the one block that was
    # right.  Windows' own entry points at BOOTMGFW.EFI, so BOOTX64.EFI
    # identifies the added one without further disambiguation.
    if ($b -notmatch '\{([0-9a-fA-F-]{36})\}') { continue }
    $candidate = $Matches[1]
    if ($b -match 'BOOTX64\.EFI') { $guid = $candidate }
}
if (-not $guid) {
    Fail 'the firmware lists no entry for \EFI\BOOT\BOOTX64.EFI.  Add the disk in the firmware setup, or boot it once from the boot menu so the firmware records it.'
}
Say "firmware entry for \EFI\BOOT\BOOTX64.EFI is {$guid}"

if (-not $PSCmdlet.ShouldProcess('the firmware boot manager', 'set a one-time boot into IRIS')) {
    Say 'stopped before changing anything (-WhatIf).'
    exit 0
}

& bcdedit /set '{fwbootmgr}' bootsequence "{$guid}" | Out-Null
$after = (& bcdedit /enum '{fwbootmgr}') -join "`n"
if ($after -notmatch [regex]::Escape($guid)) {
    Fail 'the setting did not stick.'
}
Say 'done.  The NEXT restart boots IRIS, once.'
Say 'Everything after that is Windows as before; nothing needs undoing.'

try { Stop-Transcript | Out-Null } catch { }
