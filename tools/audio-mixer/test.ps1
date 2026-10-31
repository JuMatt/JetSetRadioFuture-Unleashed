param([string]$EvidenceDirectory=(Join-Path $PSScriptRoot 'test-results'))
$ErrorActionPreference='Stop'
$src=$PSScriptRoot
$out=[IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path "$out\Assets" -Force | Out-Null
Copy-Item -LiteralPath "$src\Assets\jsrf-mixer.ico" -Destination "$out\Assets\jsrf-mixer.ico" -Force
$csc="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
& $csc /nologo "/out:$out\TestMixerConfigStore.exe" "$src\MixerConfigStore.cs" "$src\TestMixerConfigStore.cs"
if($LASTEXITCODE){throw 'Config test compilation failed'}
& "$out\TestMixerConfigStore.exe" $out | Tee-Object "$out\save-fix-tests.log"
if($LASTEXITCODE){throw 'Config tests failed'}
& $csc /nologo /main:TestMixerUi "/out:$out\TestMixerUi.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll "$src\TestMixerUi.cs" "$src\MixerControl.cs" "$src\MixerSlider.cs" "$src\MixerTheme.cs" "$src\MixerConfigStore.cs" "$src\MixerStatusReader.cs"
if($LASTEXITCODE){throw 'UI test compilation failed'}
& "$out\TestMixerUi.exe" $out | Tee-Object "$out\ui-tests.log"
if($LASTEXITCODE){throw 'UI tests failed'}
& $csc /nologo "/out:$out\TestMixerStatusReader.exe" /r:System.Web.Extensions.dll "$src\TestMixerStatusReader.cs" "$src\MixerStatusReader.cs" "$src\MixerConfigStore.cs"
if($LASTEXITCODE){throw 'Status reader test compilation failed'}
& "$out\TestMixerStatusReader.exe" $out | Tee-Object "$out\status-reader-tests.log"
if($LASTEXITCODE){throw 'Status reader tests failed'}
