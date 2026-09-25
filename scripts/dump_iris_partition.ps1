<# NOTE: pure ASCII on purpose -- Windows PowerShell 5.1 reads a BOM-less .ps1
   as ANSI, and a UTF-8 dash inside a string stops the parser. #>
<#
    dump_iris_partition.ps1 -- copy the head of IRIS's partition to a file.

    IRIS writes a report into its own filesystem when it boots.  Reading that
    back needs the raw partition, and there is no Python on a stock Windows --
    so this copies the part that matters (the superblock, the directory and the
    file area) into an ordinary file, which anything can then parse.

    It only READS.  Nothing here writes to the disk.

        powershell -ExecutionPolicy Bypass -File dump_iris_partition.ps1 `
                   -Out $env:USERPROFILE\iris-partition.bin

    Then, from anywhere with Python:  readrep.py iris-partition.bin --partition-offset 0
#>
[CmdletBinding()]
param(
    [int]$DiskNumber = 0,
    [string]$Out = "$env:USERPROFILE\iris-partition.bin",
    # 16 sectors of metadata plus 16 of file data, with room to spare.
    [int]$Sectors = 64
)

$ErrorActionPreference = 'Stop'
$IRIS_TYPE = '{53495249-5346-502D-4152-544954494F4E}'

$part = Get-Partition -DiskNumber $DiskNumber |
        Where-Object { $_.GptType -eq $IRIS_TYPE }
if (-not $part) {
    Write-Host "[dump] disk $DiskNumber has no IRISFS-PARTITION." -ForegroundColor Red
    exit 1
}
Write-Host ("[dump] partition {0}, offset {1:N0}, {2:N0} bytes" -f `
            $part.PartitionNumber, $part.Offset, $part.Size)

$dev = "\\.\Harddisk$DiskNumber" + "Partition" + $part.PartitionNumber
$fs = $null
try {
    $fs = New-Object System.IO.FileStream($dev, 'Open', 'Read', 'ReadWrite')
    $buf = New-Object byte[] ($Sectors * 512)
    $got = $fs.Read($buf, 0, $buf.Length)
    [System.IO.File]::WriteAllBytes($Out, $buf[0..($got - 1)])
    Write-Host ("[dump] {0:N0} bytes -> {1}" -f $got, $Out)
} finally {
    if ($fs) { $fs.Dispose() }
}
