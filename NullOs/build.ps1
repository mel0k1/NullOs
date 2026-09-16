# ==================================================================
# build.ps1 — Windows build wrapper for NullOs
#
# The kernel must be linked as ELF (multiboot2), which requires a
# Linux-target toolchain. On Windows this script delegates the whole
# build to WSL (Ubuntu) and the canonical Makefile:
#
#   .\build.ps1            -> build kernel.bin (default: all)
#   .\build.ps1 run        -> run in QEMU
#   .\build.ps1 debug      -> QEMU with GDB stub (-s -S)
#   .\build.ps1 iso        -> create bootable ISO
#   .\build.ps1 clean      -> remove build directory
#   .\build.ps1 rebuild    -> clean + build
#   .\build.ps1 disasm     -> disassemble kernel.bin
#   .\build.ps1 help       -> show targets
#
# One-time WSL setup (inside Ubuntu):
#   sudo apt update && sudo apt install -y build-essential grub-pc-bin xorriso mtools
# ==================================================================

param(
    [Parameter(Position = 0)]
    [string]$Target = "all"
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

function Get-WslPath([string]$p) {
    # C:\Users\admin\Downloads\NullOs -> /mnt/c/Users/admin/Downloads/NullOs
    $p = $p -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') {
        return "/mnt/$($Matches[1].ToLower())$($Matches[2])"
    }
    return $null
}

function Test-Wsl {
    try {
        wsl --status *> $null
        if ($LASTEXITCODE -ne 0) { return $false }
    } catch { return $false }

    & wsl bash -c "command -v make >/dev/null && command -v gcc >/dev/null && command -v ld >/dev/null"
    return ($LASTEXITCODE -eq 0)
}

$wslRoot = Get-WslPath $Root
if (-not $wslRoot) {
    Write-Host "[BUILD] ERROR: project must live on a drive letter path" -ForegroundColor Red
    exit 1
}

if (-not (Test-Wsl)) {
    Write-Host @"
[BUILD] ERROR: WSL with build tools not found.

The kernel needs an ELF toolchain (multiboot2); MSYS2 gcc/ld produce
PE/COFF only. Install WSL Ubuntu, then:

    wsl --install -d Ubuntu
    wsl bash -c "sudo apt update && sudo apt install -y build-essential"

After that rerun:  .\build.ps1
"@ -ForegroundColor Red
    exit 1
}

Write-Host "[BUILD] target: $Target  (via WSL: $wslRoot)" -ForegroundColor Cyan

& wsl bash -c "cd $wslRoot && make -s $(if ($Target -eq 'all') { 'all' } else { $Target.ToLower() })"
exit $LASTEXITCODE
