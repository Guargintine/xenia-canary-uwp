# Independently maintained build driver for the pinned FFmpeg source archive.
param(
  [string]$BuildRoot = 'D:/xenia-builds/ffmpeg-6-frontend',
  [string]$VisualStudio = 'C:/Program Files/Microsoft Visual Studio/2022/Community',
  [string]$Bash = 'C:/msys64/usr/bin/bash.exe',
  [ValidateRange(1, 64)][int]$Jobs = 4,
  [switch]$Incremental
)
$ErrorActionPreference = 'Stop'
$commit = 'ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2'
$archive = Join-Path $PSScriptRoot "source/ffmpeg-$commit.tar.gz"
if ((Get-FileHash -LiteralPath $archive).Hash -ne 'AACCE24D5BB6C67FBF1E3343BC15A6977AD700CDB4F43C8551F6544FA54AB7EC') {
  throw 'Pinned FFmpeg source archive hash mismatch.'
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$source = Join-Path $BuildRoot "ffmpeg-$commit"
$build = Join-Path $BuildRoot 'build'
$install = Join-Path $BuildRoot 'install'
foreach ($directory in @($BuildRoot, $build, $install, (Join-Path $BuildRoot 'temp'))) {
  New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$env:TEMP = Join-Path $BuildRoot 'temp'
$env:TMP = $env:TEMP
if (-not (Test-Path -LiteralPath $source)) {
  & tar -xzf $archive -C $BuildRoot
  if ($LASTEXITCODE) { throw 'Source extraction failed.' }
  & git -C $source apply (Join-Path $PSScriptRoot 'ffmpeg-winrt.patch')
  if ($LASTEXITCODE) { throw 'WinRT patch failed.' }
}
if (-not (Select-String -LiteralPath (Join-Path $source 'libavutil/cpu.c') -SimpleMatch '#elif HAVE_GETPROCESSAFFINITYMASK && !HAVE_WINRT')) {
  throw 'Extracted source is missing the required WinRT patch.'
}
$encoderPatch = Join-Path $PSScriptRoot 'ffmpeg-uwp-mf-encoder.patch'
& git -C $source apply --check $encoderPatch 2>$null
if ($LASTEXITCODE -eq 0) {
  & git -C $source apply $encoderPatch
  if ($LASTEXITCODE) { throw 'UWP hardware encoder patch failed.' }
} else {
  & git -C $source apply --reverse --check $encoderPatch 2>$null
  if ($LASTEXITCODE) { throw 'Source differs from the expected UWP encoder patch.' }
}
Import-Module (Join-Path $VisualStudio 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VisualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$assembler = (Get-Command nasm.exe -ErrorAction Stop).Source
$store = Join-Path $env:VCToolsInstallDir 'lib/x64/store'
if (-not (Test-Path -LiteralPath (Join-Path $store 'vcruntime.lib'))) { throw 'Store runtime import library missing.' }
$env:LIB = "$store;$env:LIB"
function MsysPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path).Replace('\', '/')
  if ($full -notmatch '^([A-Za-z]):/(.*)$') { throw 'A local drive path is required.' }
  return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2]
}
$env:XENIA_FFMPEG_SOURCE = MsysPath $source
$env:XENIA_FFMPEG_BUILD = MsysPath $build
$env:XENIA_FFMPEG_INSTALL = MsysPath $install
$env:XENIA_FFMPEG_COMPILER = MsysPath (Split-Path $compiler)
$env:XENIA_FFMPEG_ASSEMBLER = MsysPath (Split-Path $assembler)
$env:XENIA_FFMPEG_JOBS = [string]$Jobs
$env:XENIA_FFMPEG_SKIP_CONFIGURE = if ($Incremental) { '1' } else { '0' }
if ($Incremental -and -not (Test-Path -LiteralPath (Join-Path $build 'ffbuild/config.mak'))) {
  throw 'Incremental build requires a previous complete configuration in this build directory.'
}
$driver = @'
set -euo pipefail
export PATH="$XENIA_FFMPEG_COMPILER:$XENIA_FFMPEG_ASSEMBLER:/usr/bin:/bin:$PATH"
cd "$XENIA_FFMPEG_BUILD"
if [ "$XENIA_FFMPEG_SKIP_CONFIGURE" != "1" ]; then
"$XENIA_FFMPEG_SOURCE/configure" --prefix="$XENIA_FFMPEG_INSTALL" \
  --toolchain=msvc --arch=x86_64 --target-os=win32 --enable-cross-compile \
  --enable-shared --disable-static --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --enable-network \
  --disable-decoders --enable-decoder=h264,mpeg4,vp8,vp9,rawvideo,ffv1 \
  --disable-demuxers --enable-demuxer=mov,matroska,avi \
  --disable-parsers --enable-parser=h264,mpeg4video,vp8,vp9 \
  --disable-bsfs --disable-hwaccels \
  --disable-protocols --enable-protocol=file,udp \
  --disable-muxers --enable-muxer=mp4,matroska,avi,mpegts \
  --disable-encoders --enable-encoder=h264_mf,mpeg4,rawvideo,ffv1,aac,flac,pcm_s16le \
  --disable-filters --enable-filter=buffer,buffersink,fps,format,scale \
  --disable-dxva2 --disable-d3d11va --disable-gpl --disable-version3 \
  --extra-cflags="-MD -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WIN32_WINNT=0x0A00 -wd4267 -wd4133 -wd4334 -wd4101" \
  --extra-ldflags="-APPCONTAINER WindowsApp.lib"
fi
make -j"$XENIA_FFMPEG_JOBS"
make install
'@
$driverPath = Join-Path $build 'xenia-build.sh'
[IO.File]::WriteAllText($driverPath, $driver.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
& $Bash (MsysPath $driverPath)
if ($LASTEXITCODE) { throw 'FFmpeg build failed.' }
$validation = foreach ($name in @('avcodec-60.dll','avformat-60.dll','avutil-58.dll','swresample-4.dll','swscale-7.dll')) {
  $dll = Join-Path $install "bin/$name"
  $headers = (& dumpbin /headers $dll) -join "`n"
  if ($LASTEXITCODE -or $headers -notmatch 'machine \(x64\)' -or $headers -notmatch 'App Container') {
    throw "Invalid AppContainer architecture: $name"
  }
  $imports = (& dumpbin /dependents $dll) -join "`n"
  if ($LASTEXITCODE -or $imports -match '(?im)^\s*(USER32|GDI32|SHELL32|WINMM|VCRUNTIME140|VCRUNTIME140_1|api-ms-win-core-processtopology-obsolete-l1-1-0)\.dll\s*$') {
    throw "Forbidden desktop import: $name"
  }
  [PSCustomObject]@{Name=$name; SHA256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant(); Imports=$imports}
}
$validation | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $BuildRoot 'binary-validation.json')
Write-Output "Validated isolated runtime: $install"
Write-Output 'Run the video and streamer regression suites before copying these files into bin/.'
