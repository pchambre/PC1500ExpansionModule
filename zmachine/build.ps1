# build.ps1 -- assembles zip.asm into ZIP, an M file for SDLOAD M "ZIP".
#
# Same toolchain as the ROM (rom/build.ps1): sdaslh5801 -> sdld -> makebin.
# The M file is the 4-byte header keywords.c's SDLOAD M reads (load
# address, then CALL address, both high byte first), then the code from
# ZIPORG to ZIPEND. The CALL address is ZIPORG: its first instruction
# jumps to ENTRY.

$ErrorActionPreference = "Stop"

$sdas    = "C:\Users\paulc\sdcc-pc1500\sdcc\bin_vc\sdaslh5801.exe"
$sdld    = "C:\Users\paulc\sdcc-pc1500\sdcc\bin_vc\sdld.exe"
$makebin = "C:\Users\paulc\sdcc-pc1500\sdcc\bin_vc\makebin.exe"

Push-Location $PSScriptRoot
try {
    Remove-Item -ErrorAction SilentlyContinue zip.rel, zip.lst, zip.ihx, zip.bin, zip.lnk, zip.map, zip.rst, zip.sym, ZIP

    & $sdas -l -o zip.rel zip.asm
    if ($LASTEXITCODE -ne 0) { throw "sdaslh5801 failed (exit $LASTEXITCODE)" }
    Set-Content -NoNewline -Path zip.lnk -Value "-muwx`n-i zip`nzip.rel`n`n-e`n"
    & $sdld -nf zip
    if ($LASTEXITCODE -ne 0) { throw "sdld failed (exit $LASTEXITCODE)" }

    $src = Get-Content zip.asm -Raw
    if ($src -notmatch '(?m)^ZIPORG\s*=\s*0x([0-9A-Fa-f]+)') { throw "no ZIPORG in zip.asm" }
    $org = [Convert]::ToInt32($Matches[1], 16)

    # The end: the highest address any data record reaches.
    $end = $org
    foreach ($line in Get-Content zip.ihx) {
        if (-not $line.StartsWith(":") -or $line.Substring(7, 2) -ne "00") { continue }
        $n = [Convert]::ToInt32($line.Substring(1, 2), 16)
        $a = [Convert]::ToInt32($line.Substring(3, 4), 16)
        if ($a + $n -gt $end) { $end = $a + $n }
    }
    if ($end -gt 0x6800) { throw ("the code ends at {0:X4}H, past the variables at 6800H" -f $end) }

    & $makebin -p -s 0x10000 -o $org zip.ihx zip.bin
    if ($LASTEXITCODE -ne 0) { throw "makebin failed (exit $LASTEXITCODE)" }
    $code = [IO.File]::ReadAllBytes("$PSScriptRoot\zip.bin")[0..($end - $org - 1)]
    $hdr = [byte[]]@(($org -shr 8), ($org -band 0xFF), ($org -shr 8), ($org -band 0xFF))
    [IO.File]::WriteAllBytes("$PSScriptRoot\ZIP", $hdr + $code)
    Write-Host ("ZIP: {0:X4}H-{1:X4}H, {2} bytes; swap pages end at {0:X4}H" -f $org, ($end - 1), ($end - $org))
} finally {
    Pop-Location
}
