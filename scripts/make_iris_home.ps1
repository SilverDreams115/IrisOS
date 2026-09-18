<# NOTE: this file is deliberately pure ASCII.  Windows PowerShell 5.1
   reads a .ps1 without a BOM as ANSI, so a UTF-8 em-dash inside a string
   arrives as three garbage bytes and the parser stops on the line after
   it.  That happened; this is the fix. #>
<#
    make_iris_home.ps1 -- give IRIS a partition of its own on a Windows disk.

    WHAT THIS DOES, IN ORDER
      1. Refuses unless everything it expects is true (see the checks below).
      2. Shrinks the chosen volume to make room.  This does NOT delete files:
         NTFS gives back free space at the tail and relocates movable data.
      3. Creates a partition in the freed space and types it IRISFS-PARTITION.
      4. Writes the format-permission token into its first sector.
      5. Reads everything back and prints what IRIS will see.

    WHAT IT WILL NOT DO
      It never writes inside the existing volume, never formats it, and never
      touches a disk other than the one named.  Every step is guarded, and the
      guards refuse rather than repair: a disk that does not look the way this
      script expects is a disk somebody should look at by hand.

    Re-running it is safe.  If the IRIS partition already exists it verifies it
    and changes nothing.

    RUN IT ELEVATED:
        powershell -ExecutionPolicy Bypass -File make_iris_home.ps1 -DriveLetter E

    Add -WhatIf to see every decision without performing any of them.
#>

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    # The volume to take space FROM.  Its contents are preserved.
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]$')]
    [string]$DriveLetter,

    # How much to give IRIS.  Its filesystem needs kilobytes; this is round.
    [ValidateRange(64MB, 64GB)]
    [long]$SizeBytes = 1GB
)

$ErrorActionPreference = 'Stop'

# A transcript, because this script is meant to be run in a window that closes.
# Whoever asked for it needs to be able to read what happened afterwards, and
# "it scrolled past" is not a record of a disk operation.
$LogPath = Join-Path $env:TEMP 'iris-home.log'
try { Start-Transcript -Path $LogPath -Force | Out-Null } catch { }

# The type that makes a partition IRIS's, and the token that says the partition
# may be formatted.  Both are contracts with the kernel side:
#   kernel/include/iris/blk_ep_proto.h  BLK_PART_TYPE  "IRISFS-PARTITION"
#   kernel/include/iris/fs_ep_proto.h   FS_SCRATCH_*   "S-FMT-OK" at offset 496
# The GUID below is those sixteen ASCII bytes in GPT's mixed-endian order; it
# was verified against a real image with sgdisk rather than derived on paper.
$IRIS_GPT_TYPE   = '{53495249-5346-502D-4152-544954494F4E}'
$IRIS_TOKEN      = [byte[]][char[]]'S-FMT-OK'
$IRIS_TOKEN_OFF  = 496

function Fail([string]$m) {
    Write-Host "[iris-home] REFUSING: $m" -ForegroundColor Red
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}
function Say ([string]$m) { Write-Host "[iris-home] $m" }

# -- 0. Elevation ------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal(
                 [Security.Principal.WindowsIdentity]::GetCurrent())
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    if (-not $WhatIfPreference) {
        Fail 'this needs an elevated PowerShell (Run as administrator).'
    }
    # -WhatIf is allowed unprivileged ON PURPOSE: the point of a dry run is to
    # be readable before anyone decides to elevate.  Some queries below need
    # admin and will simply be reported as unavailable.
    Say 'not elevated: this is a dry run, and some checks cannot be made.'
}

# -- 1. Find the volume, and the disk under it -------------------------------
$letter = $DriveLetter.ToUpper()
$part = Get-Partition | Where-Object { $_.DriveLetter -eq $letter }
if (-not $part) { Fail "no partition carries drive letter ${letter}:." }
if ($part.Count -gt 1) { Fail "drive ${letter}: resolves to more than one partition." }

$disk = Get-Disk -Number $part.DiskNumber
Say ("volume {0}: is partition {1} of disk {2} ({3}, {4:N1} GB, {5})" -f `
     $letter, $part.PartitionNumber, $disk.Number, $disk.FriendlyName,
     ($disk.Size / 1GB), $disk.BusType)

# -- 2. Refuse anything this script was not written for ----------------------
if ($disk.PartitionStyle -ne 'GPT') {
    Fail "disk $($disk.Number) is $($disk.PartitionStyle), and IRIS finds its partition through a GPT."
}
if ($disk.IsBoot -or $disk.IsSystem) {
    Fail "disk $($disk.Number) is the boot/system disk.  Not this one."
}
if ($disk.BusType -ne 'SATA') {
    # `blk` claims an AHCI controller by class code.  An NVMe or USB disk is
    # not something this system can drive, so a partition there would be a
    # partition IRIS can never open.
    Fail "disk $($disk.Number) is $($disk.BusType); IRIS only has an AHCI/SATA driver."
}

$vol = Get-Volume -DriveLetter $letter
Say ("{0}: is {1}, {2:N1} GB with {3:N1} GB free" -f `
     $letter, $vol.FileSystem, ($vol.Size / 1GB), ($vol.SizeRemaining / 1GB))

# -- 3. Already done?  Then verify and stop. ---------------------------------
$existing = Get-Partition -DiskNumber $disk.Number |
            Where-Object { $_.GptType -eq $IRIS_GPT_TYPE }
if ($existing) {
    Say ("an IRIS partition already exists: number {0}, offset {1:N0}, {2:N0} bytes" -f `
         $existing.PartitionNumber, $existing.Offset, $existing.Size)
    Say 'nothing to do.  Re-running this script never creates a second one.'
    exit 0
}

# -- 4. Room -----------------------------------------------------------------
if ($vol.SizeRemaining -lt ($SizeBytes + 1GB)) {
    Fail ("{0}: has only {1:N1} GB free; leave at least 1 GB of headroom." -f `
          $letter, ($vol.SizeRemaining / 1GB))
}

$target = $part.Size - $SizeBytes
try {
    $supported = Get-PartitionSupportedSize -DiskNumber $disk.Number `
                                            -PartitionNumber $part.PartitionNumber
    if ($target -lt $supported.SizeMin) {
        Fail ("{0}: cannot shrink that far -- immovable files put its floor at {1:N1} GB." -f `
              $letter, ($supported.SizeMin / 1GB))
    }
    Say ("shrink floor for {0}: is {1:N1} GB, so {2:N1} GB is allowed" -f `
         $letter, ($supported.SizeMin / 1GB), ($target / 1GB))
} catch {
    if ($elevated) { throw }
    Say 'cannot read the shrink floor without elevation; the real run will check it.'
}

Say ("plan: shrink {0}: from {1:N0} to {2:N0} bytes, then take {3:N0} bytes for IRIS" -f `
     $letter, $part.Size, $target, $SizeBytes)

if (-not $PSCmdlet.ShouldProcess("disk $($disk.Number), volume ${letter}:",
                                 "shrink by $SizeBytes bytes and create an IRIS partition")) {
    Say 'stopped before changing anything (-WhatIf).'
    exit 0
}

# -- 5. Shrink, then create --------------------------------------------------
Say 'shrinking...'
Resize-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber -Size $target
Say 'creating the IRIS partition...'
# Typed AT CREATION where the platform supports it.  A partition that exists as
# "basic data" for even a moment is one Windows may offer to format, and the
# offer is made to whoever is sitting at the machine -- which is a dialog box
# nobody should be answering about this disk.
try {
    $new = New-Partition -DiskNumber $disk.Number -Size $SizeBytes -GptType $IRIS_GPT_TYPE
} catch {
    Say 'this Windows cannot type a partition at creation; typing it immediately after'
    $new = New-Partition -DiskNumber $disk.Number -Size $SizeBytes
    Set-Partition -DiskNumber $disk.Number -PartitionNumber $new.PartitionNumber `
                  -GptType $IRIS_GPT_TYPE
}
# No drive letter and no filesystem: Windows must not mount this, and IRIS is
# the only thing that should ever write in it.
$new = Get-Partition -DiskNumber $disk.Number -PartitionNumber $new.PartitionNumber
Say ("created: partition {0}, offset {1:N0}, {2:N0} bytes" -f `
     $new.PartitionNumber, $new.Offset, $new.Size)

# -- 6. The token that says this partition may be formatted ------------------
# Written through the PARTITION device, not the disk, so the offset cannot be
# mistaken for one inside the volume next to it.
$devPath = "\\.\Harddisk$($disk.Number)Partition$($new.PartitionNumber)"
Say "writing the format-permission token to $devPath"
$fs = $null
try {
    $fs = New-Object System.IO.FileStream($devPath, 'Open', 'ReadWrite', 'ReadWrite')
    $sector = New-Object byte[] 512
    [void]$fs.Read($sector, 0, 512)
    [Array]::Clear($sector, 0, 512)          # a fresh superblock area
    [Array]::Copy($IRIS_TOKEN, 0, $sector, $IRIS_TOKEN_OFF, $IRIS_TOKEN.Length)
    $fs.Position = 0
    $fs.Write($sector, 0, 512)
    $fs.Flush()
} finally {
    if ($fs) { $fs.Dispose() }
}

# -- 7. Read it back.  A write nobody checked is a claim, not a result. ------
$fs = $null
try {
    $fs = New-Object System.IO.FileStream($devPath, 'Open', 'Read', 'ReadWrite')
    $check = New-Object byte[] 512
    [void]$fs.Read($check, 0, 512)
} finally {
    if ($fs) { $fs.Dispose() }
}
$readBack = [System.Text.Encoding]::ASCII.GetString($check, $IRIS_TOKEN_OFF, 8)
if ($readBack -ne 'S-FMT-OK') {
    Fail "the token did not stick -- sector 0 offset $IRIS_TOKEN_OFF reads '$readBack'."
}

Say ''
Say 'done.  What IRIS will see on this machine:'
Say ("  a SATA disk with an IRISFS-PARTITION of {0:N0} sectors" -f ($new.Size / 512))
Say ('  a format token in its first sector, so the first boot will format it')
Say ('  and no way to address a single byte outside it')
Say ''
Say ("{0}: keeps every file it had, {1:N1} GB smaller." -f $letter, ($SizeBytes / 1GB))

try { Stop-Transcript | Out-Null } catch { }
