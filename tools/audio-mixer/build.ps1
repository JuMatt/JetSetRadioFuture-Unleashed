param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'dist'))
$ErrorActionPreference='Stop'
$src=$PSScriptRoot
$out=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path "$out\Assets" -Force | Out-Null
Copy-Item -Path "$src\Assets\*" -Destination "$out\Assets" -Force
$csc="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$icon="/win32icon:$src\Assets\jsrf-mixer.ico"
& $csc /nologo /target:winexe "/out:$out\MixerControl.exe" $icon /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll "$src\MixerControl.cs" "$src\MixerSlider.cs" "$src\MixerTheme.cs" "$src\MixerConfigStore.cs" "$src\MixerStatusReader.cs"
if($LASTEXITCODE){throw 'Mixer UI compilation failed'}
& $csc /nologo /target:winexe "/out:$out\JSRF-Audio-Mixer.exe" $icon /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll "$src\LaunchMixer.cs"
if($LASTEXITCODE){throw 'Game launcher compilation failed'}
& $csc /nologo /target:winexe "/out:$out\JSRF-Mixer.exe" $icon /r:System.Windows.Forms.dll "$src\OpenMixer.cs"
if($LASTEXITCODE){throw 'Mixer opener compilation failed'}
