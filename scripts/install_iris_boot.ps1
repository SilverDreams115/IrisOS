<# NOTE: pure ASCII on purpose.  Windows PowerShell 5.1 reads a .ps1 without a
   BOM as ANSI, so a UTF-8 dash inside a string arrives as garbage bytes and
   the parser stops on the line after it.  That already happened once. #>
<#
    install_iris_boot.ps1 -- put IRIS where the firmware can find it.

    THIS IS NOT THE PARTITION make_iris_home.ps1 MADE.  That one is IRIS's
    DATA home: where its filesystem lives.  Firmware does not boot from it.
    Booting needs an EFI System Partition carrying a loader, and this creates
    one -- on the SAME disk, so nothing about the Windows boot disk changes.

    WHAT IT DOES
      1. Refuses unless everything it expects is true.
      2. Shrinks the chosen volume a little more to make room.  No file is
         deleted; NTFS gives back free space at the tail.
      3. Creates an EFI System Partition, formats it FAT32, and copies the
         loader and the kernel into it.
      4. Reads them back and compares sizes.

    WHAT IT DOES NOT DO
      It does not touch the Windows boot disk, does not edit the BCD, and does
      not make IRIS the default boot.  Choosing to boot it stays a decision
      somebody makes at the firmware's boot menu, deliberately, each time.

    RUN IT ELEVATED:
        powershell -ExecutionPolicy Bypass -File install_iris_boot.ps1 `
                   -DriveLetter E -Payload C:\path\to\efi_root

    Add -WhatIf to see every decision without performing any of them.
#>

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]$')]
    [string]$DriveLetter,

    # A directory containing EFI\BOOT\BOOTX64.EFI and EFI\IRIS\KERNEL.ELF.
    [Parameter(Mandatory = $true)]
    [string]$Payload,

    # 512 MB is far more than the 1.2 MB IRIS needs.  It is the smallest size
    # worth carving: Windows wants an ESP of at least 100 MB to format FAT32,
    # and a boot partition that cannot hold a second kernel is one that has to
    # be rebuilt to test anything.
    [ValidateRange(128MB, 4GB)]
    [long]$SizeBytes = 512MB
)

$ErrorActionPreference = 'Stop'

$LogPath = Join-Path $env:TEMP 'iris-boot.log'
try { Start-Transcript -Path $LogPath -Force | Out-Null } catch { }

$ESP_GPT_TYPE = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'

function Fail([string]$m) {
    Write-Host "[iris-boot] REFUSING: $m" -ForegroundColor Red
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}
function Say ([string]$m) { Write-Host "[iris-boot] $m" }

# ---- 0. Elevation -----------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal(
                 [Security.Principal.WindowsIdentity]::GetCurrent())
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    if (-not $WhatIfPreference) { Fail 'this needs an elevated PowerShell.' }
    Say 'not elevated: dry run, and some checks cannot be made.'
}

# ---- 1. The payload ---------------------------------------------------------
$loader = Join-Path $Payload 'EFI\BOOT\BOOTX64.EFI'
$kernel = Join-Path $Payload 'EFI\IRIS\KERNEL.ELF'
if (-not (Test-Path $loader)) { Fail "no loader at $loader" }
if (-not (Test-Path $kernel)) { Fail "no kernel at $kernel" }
$loaderSize = (Get-Item $loader).Length
$kernelSize = (Get-Item $kernel).Length
Say ("payload: BOOTX64.EFI {0:N0} bytes, KERNEL.ELF {1:N0} bytes" -f $loaderSize, $kernelSize)

# ---- 2. The disk ------------------------------------------------------------
$letter = $DriveLetter.ToUpper()
$part = Get-Partition | Where-Object { $_.DriveLetter -eq $letter }
if (-not $part) { Fail "no partition carries drive letter ${letter}:." }
if ($part.Count -gt 1) { Fail "drive ${letter}: resolves to more than one partition." }
$disk = Get-Disk -Number $part.DiskNumber

Say ("volume {0}: is on disk {1} ({2}, {3})" -f $letter, $disk.Number, $disk.FriendlyName, $disk.BusType)
if ($disk.PartitionStyle -ne 'GPT') { Fail "disk $($disk.Number) is not GPT." }
if ($disk.IsBoot -or $disk.IsSystem) {
    Fail "disk $($disk.Number) is the boot/system disk.  This script will not put an ESP there."
}

# IRIS's data home must already exist on this disk.  Installing a loader onto a
# disk that has no home for it would produce a machine that boots and then has
# nowhere to write, which is a worse outcome than refusing.
$home_part = Get-Partition -DiskNumber $disk.Number |
             Where-Object { $_.GptType -eq '{53495249-5346-502D-4152-544954494F4E}' }
if (-not $home_part) {
    Fail "disk $($disk.Number) has no IRISFS-PARTITION.  Run make_iris_home.ps1 first."
}
Say ("IRIS's data home is partition {0} ({1:N0} bytes)" -f $home_part.PartitionNumber, $home_part.Size)

# ---- 3. Already installed? --------------------------------------------------
$esp = Get-Partition -DiskNumber $disk.Number |
       Where-Object { $_.GptType -eq $ESP_GPT_TYPE }
if ($esp) {
    Say ("an EFI System Partition already exists here: partition {0}" -f $esp.PartitionNumber)
    Say 'refreshing the payload in place rather than making a second one.'
} else {
    $vol = Get-Volume -DriveLetter $letter
    if ($vol.SizeRemaining -lt ($SizeBytes + 1GB)) {
        Fail ("{0}: has only {1:N1} GB free." -f $letter, ($vol.SizeRemaining / 1GB))
    }
    $target = $part.Size - $SizeBytes
    if ($elevated) {
        $supported = Get-PartitionSupportedSize -DiskNumber $disk.Number `
                                                -PartitionNumber $part.PartitionNumber
        if ($target -lt $supported.SizeMin) {
            Fail ("{0}: cannot shrink that far; its floor is {1:N1} GB." -f $letter, ($supported.SizeMin / 1GB))
        }
    }
    Say ("plan: shrink {0}: by {1:N0} bytes, then make an ESP of that size" -f $letter, $SizeBytes)

    if (-not $PSCmdlet.ShouldProcess("disk $($disk.Number), volume ${letter}:",
                                     "shrink and create an EFI System Partition")) {
        Say 'stopped before changing anything (-WhatIf).'
        try { Stop-Transcript | Out-Null } catch { }
        exit 0
    }

    Say 'shrinking...'
    Resize-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber -Size $target
    Say 'creating the EFI System Partition...'
    try {
        $esp = New-Partition -DiskNumber $disk.Number -Size $SizeBytes -GptType $ESP_GPT_TYPE
    } catch {
        $esp = New-Partition -DiskNumber $disk.Number -Size $SizeBytes
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $esp.PartitionNumber -GptType $ESP_GPT_TYPE
    }
    $esp = Get-Partition -DiskNumber $disk.Number -PartitionNumber $esp.PartitionNumber
    Say ("created: partition {0}, {1:N0} bytes" -f $esp.PartitionNumber, $esp.Size)
    Say 'formatting FAT32 (what UEFI reads)...'
    Format-Volume -Partition $esp -FileSystem FAT32 -NewFileSystemLabel 'IRISBOOT' -Confirm:$false | Out-Null
}

# ---- 4. Mount it somewhere we can write ------------------------------------
# A temporary letter, removed afterwards: an ESP left mounted is an ESP
# something else can write into.
$used  = (Get-Volume | Where-Object { $_.DriveLetter }).DriveLetter
$spare = @('S','T','U','V','W','Y','Z') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
if (-not $spare) { Fail 'no free drive letter to mount the ESP on.' }

Add-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber $esp.PartitionNumber `
                        -AccessPath "${spare}:"
try {
    Say "mounted the ESP at ${spare}: -- copying"
    New-Item -ItemType Directory -Force -Path "${spare}:\EFI\BOOT"  | Out-Null
    New-Item -ItemType Directory -Force -Path "${spare}:\EFI\IRIS"  | Out-Null
    Copy-Item $loader "${spare}:\EFI\BOOT\BOOTX64.EFI" -Force
    Copy-Item $kernel "${spare}:\EFI\IRIS\KERNEL.ELF"  -Force

    # Read back.  A copy nobody checked is a claim, not a result.
    $l2 = (Get-Item "${spare}:\EFI\BOOT\BOOTX64.EFI").Length
    $k2 = (Get-Item "${spare}:\EFI\IRIS\KERNEL.ELF").Length
    if ($l2 -ne $loaderSize) { Fail "BOOTX64.EFI copied as $l2 bytes, not $loaderSize." }
    if ($k2 -ne $kernelSize) { Fail "KERNEL.ELF copied as $k2 bytes, not $kernelSize." }
    Say ("verified on the ESP: BOOTX64.EFI {0:N0}, KERNEL.ELF {1:N0}" -f $l2, $k2)
} finally {
    Remove-PartitionAccessPath -DiskNumber $disk.Number `
                               -PartitionNumber $esp.PartitionNumber `
                               -AccessPath "${spare}:" -ErrorAction SilentlyContinue
    Say "unmounted ${spare}:"
}

Say ''
Say 'installed.  Nothing about how this machine boots today has changed:'
Say '  the Windows boot disk was not touched and the BCD was not edited.'
Say '  To run IRIS, pick this disk from the firmware boot menu at power-on.'
Say ''
Say 'What IRIS will do when it starts:'
Say '  paint its kernel log on the screen (there is no serial port here),'
Say '  find its partition on this disk by type, and format it on first boot.'

try { Stop-Transcript | Out-Null } catch { }
