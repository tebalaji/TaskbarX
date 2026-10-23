# Downloads the NuGet packages TaskbarX11Hook needs to build (Windows SDK headers/libs and C++/WinRT),
# so the project builds on machines that only have the MSVC toolset and no installed Windows SDK.
param(
    [string]$PackagesDir = (Join-Path $PSScriptRoot '..\packages')
)

$ErrorActionPreference = 'Stop'

$packages = @(
    @{ Id = 'Microsoft.Windows.SDK.CPP';     Version = '10.0.26100.7705' },
    @{ Id = 'Microsoft.Windows.SDK.CPP.x64'; Version = '10.0.26100.7705' },
    @{ Id = 'Microsoft.Windows.CppWinRT';    Version = '2.0.250303.1' }
)

New-Item -ItemType Directory -Force $PackagesDir | Out-Null

foreach ($p in $packages) {
    $target = Join-Path $PackagesDir "$($p.Id).$($p.Version)"
    if (Test-Path $target) { continue }

    $lower = $p.Id.ToLowerInvariant()
    $url = "https://api.nuget.org/v3-flatcontainer/$lower/$($p.Version)/$lower.$($p.Version).nupkg"
    $zip = Join-Path $PackagesDir "$($p.Id).$($p.Version).zip"

    Write-Host "Downloading $($p.Id) $($p.Version)"
    Invoke-WebRequest $url -OutFile $zip
    Expand-Archive $zip -DestinationPath $target
    Remove-Item $zip
}
