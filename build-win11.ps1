# Builds TaskbarX with Windows 11 support (64-bit) and collects everything in dist\TaskbarX.
#   TaskbarX.exe              - x64 build (Windows 11 support needs a 64-bit process)
#   TaskbarX11Hook.dll        - loaded into explorer.exe to move the Windows 11 taskbar's XAML buttons
#   TaskbarX Configurator.exe - settings UI
param(
    [string]$MSBuild = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1)
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

& "$root\TaskbarX11Hook\restore-packages.ps1"

& $MSBuild "$root\TaskbarX11Hook\TaskbarX11Hook.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE) { throw 'TaskbarX11Hook build failed' }

& $MSBuild "$root\TaskbarX\TaskbarX\TaskbarX.vbproj" /p:Configuration=Release /p:Platform=x64 /v:m /nologo
if ($LASTEXITCODE) { throw 'TaskbarX build failed' }

$configurator = "$root\TaskbarX Configurator\TaskbarX Configurator.sln"
& $MSBuild $configurator -t:restore -p:RestorePackagesConfig=true /v:m /nologo
& $MSBuild $configurator /p:Configuration=Release "/p:Platform=Any CPU" /v:m /nologo
if ($LASTEXITCODE) { throw 'TaskbarX Configurator build failed' }

$dist = "$root\dist\TaskbarX"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item "$root\APPX\TaskbarX\Release\*" $dist -Recurse -Force -Exclude *.pdb, *.xml
Copy-Item "$root\APPX\TaskbarX64\Release\*" $dist -Recurse -Force -Exclude *.pdb, *.xml
# One-click on/off: switches between TaskbarX and the default Windows taskbar.
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut("$dist\TaskbarX On-Off.lnk")
$shortcut.TargetPath = "$dist\TaskbarX.exe"
$shortcut.Arguments = '-toggle'
$shortcut.WorkingDirectory = $dist
$shortcut.Description = 'Turn TaskbarX on or off'
$shortcut.Save()

Write-Host "Done: $dist"
