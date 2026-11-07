# fetch_iolibrary.ps1 - downloads the WIZnet ioLibrary sources for the w5500_net component
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst  = Join-Path $here "ioLibrary"
$repo = "https://github.com/Wiznet/ioLibrary_Driver.git"

if ((Test-Path (Join-Path $dst "wizchip_conf.c")) -or (Test-Path (Join-Path $dst "Ethernet"))) {
    Write-Host "ioLibrary already present in $dst"; exit 0
}

$tmp = Join-Path $env:TEMP ("iol_" + [guid]::NewGuid().ToString("N"))
Write-Host "Cloning ioLibrary into $tmp ..."
git clone --depth 1 $repo (Join-Path $tmp "iol")

$src = Join-Path $tmp "iol\Ethernet"
New-Item -ItemType Directory -Force -Path (Join-Path $dst "W5500") | Out-Null
Copy-Item (Join-Path $src "wizchip_conf.c") $dst
Copy-Item (Join-Path $src "wizchip_conf.h") $dst
Copy-Item (Join-Path $src "socket.c")       $dst
Copy-Item (Join-Path $src "socket.h")       $dst
Copy-Item (Join-Path $src "W5500\w5500.c")  (Join-Path $dst "W5500")
Copy-Item (Join-Path $src "W5500\w5500.h")  (Join-Path $dst "W5500")
Remove-Item -Recurse -Force $tmp

Write-Host "Done. Files in $dst :"
Get-ChildItem -Recurse -File $dst | ForEach-Object { "  " + $_.FullName.Substring($dst.Length+1) }