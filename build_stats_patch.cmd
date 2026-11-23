@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title Weasel input statistics patch build

set "PROJECT_ROOT=%~dp0"
pushd "%PROJECT_ROOT%" >nul
if errorlevel 1 (
  echo [ERROR] Cannot enter project directory: %PROJECT_ROOT%
  pause
  exit /b 1
)

echo.
echo Weasel input statistics patch build
echo ============================
echo.

if not exist "%PROJECT_ROOT%weasel.sln" (
  echo [ERROR] The current directory is not the Weasel source root.
  goto :failed
)
if not exist "%PROJECT_ROOT%WeaselStats\fetch_dependencies.ps1" (
  echo [ERROR] The statistics dependency script is missing.
  goto :failed
)
if not exist "%PROJECT_ROOT%deploy_stats_patch.cmd" (
  echo [ERROR] The deployment script is missing: deploy_stats_patch.cmd
  goto :failed
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Windows PowerShell was not found.
  goto :failed
)

set "MSBUILD="
set "VSINSTALL="
set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "delims=" %%I in ('"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul') do if not defined VSINSTALL set "VSINSTALL=%%I"
if defined VSINSTALL if exist "%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if defined VSINSTALL if exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not defined MSBUILD if not exist "%VSWHERE%" if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not defined VSDEVCMD if not exist "%VSWHERE%" if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not defined MSBUILD (
  echo [ERROR] A complete Visual Studio 2022 C++ build environment was not found.
  call :show_cpp_install_help
  goto :failed
)
if not defined VSDEVCMD (
  echo [ERROR] The Visual Studio 2022 C++ environment script VsDevCmd.bat was not found.
  call :show_cpp_install_help
  goto :failed
)

echo MSBuild: %MSBUILD%
echo VS environment: %VSDEVCMD%
echo.

echo [1/9] Preparing Boost, WebView2, and ECharts dependencies...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%WeaselStats\fetch_dependencies.ps1" -DestinationRoot "%PROJECT_ROOT%deps"
if errorlevel 1 (
  echo [ERROR] Failed to prepare build dependencies.
  goto :failed
)

set "BOOST_X64=%PROJECT_ROOT%deps\boost_1_84_0"
set "BOOST_X86=%PROJECT_ROOT%deps\boost_1_84_0"
if defined WEASEL_PATCH_BOOST_X64 set "BOOST_X64=%WEASEL_PATCH_BOOST_X64%"
if defined WEASEL_PATCH_BOOST_X86 set "BOOST_X86=%WEASEL_PATCH_BOOST_X86%"
if not exist "%BOOST_X64%\boost\version.hpp" (
  echo [ERROR] x64 Boost was not found. Set WEASEL_PATCH_BOOST_X64 to override the path.
  goto :failed
)
if not exist "%BOOST_X86%\boost\version.hpp" (
  echo [ERROR] x86 Boost was not found. Set WEASEL_PATCH_BOOST_X86 to override the path.
  goto :failed
)

echo Boost x64: %BOOST_X64%
echo Boost x86: %BOOST_X86%
echo.

echo [2/9] Building Boost static libraries ^(Release x86/x64^)...
call :build_boost_arch "%BOOST_X86%" 32
if errorlevel 1 (
  echo [ERROR] Failed to build the Boost x86 static libraries.
  goto :failed
)
call :build_boost_arch "%BOOST_X64%" 64
if errorlevel 1 (
  echo [ERROR] Failed to build the Boost x64 static libraries.
  goto :failed
)
dir /b "%BOOST_X86%\stage\lib\libboost_wserialization-vc*-mt-s-x32-*.lib" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] The required Boost x86 wserialization Release library is missing.
  goto :failed
)
dir /b "%BOOST_X64%\stage\lib\libboost_wserialization-vc*-mt-s-x64-*.lib" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] The required Boost x64 wserialization Release library is missing.
  goto :failed
)

echo.
echo [3/9] Preparing librime x86/x64 development files...
set "NEED_RIME=0"
if not exist "%PROJECT_ROOT%include\rime_api.h" set "NEED_RIME=1"
if not exist "%PROJECT_ROOT%lib\rime.lib" set "NEED_RIME=1"
if not exist "%PROJECT_ROOT%lib64\rime.lib" set "NEED_RIME=1"
if not exist "%PROJECT_ROOT%output\Win32\rime.dll" set "NEED_RIME=1"
if not exist "%PROJECT_ROOT%output\rime.dll" set "NEED_RIME=1"
if "%NEED_RIME%"=="1" (
  set "PATH=%PROJECT_ROOT%output;%PATH%"
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%get-rime.ps1" -use dev -skip_server_stop
)
if not exist "%PROJECT_ROOT%include\rime_api.h" (
  echo [ERROR] Failed to prepare the librime headers.
  goto :failed
)
if not exist "%PROJECT_ROOT%lib\rime.lib" (
  echo [ERROR] Failed to prepare the librime x86 import library.
  goto :failed
)
if not exist "%PROJECT_ROOT%lib64\rime.lib" (
  echo [ERROR] Failed to prepare the librime x64 import library.
  goto :failed
)

echo.
echo [4/9] Generating project version properties...
if not defined VERSION_MAJOR for /f "tokens=2 delims==" %%V in ('findstr /b /c:"if not defined VERSION_MAJOR set VERSION_MAJOR=" "%PROJECT_ROOT%build.bat"') do set "VERSION_MAJOR=%%V"
if not defined VERSION_MINOR for /f "tokens=2 delims==" %%V in ('findstr /b /c:"if not defined VERSION_MINOR set VERSION_MINOR=" "%PROJECT_ROOT%build.bat"') do set "VERSION_MINOR=%%V"
if not defined VERSION_PATCH for /f "tokens=2 delims==" %%V in ('findstr /b /c:"if not defined VERSION_PATCH set VERSION_PATCH=" "%PROJECT_ROOT%build.bat"') do set "VERSION_PATCH=%%V"
if not defined VERSION_MAJOR (
  echo [ERROR] Cannot read the major version from build.bat.
  goto :failed
)
if not defined VERSION_MINOR (
  echo [ERROR] Cannot read the minor version from build.bat.
  goto :failed
)
if not defined VERSION_PATCH (
  echo [ERROR] Cannot read the patch version from build.bat.
  goto :failed
)
if not defined WEASEL_BUILD set "WEASEL_BUILD=0"
if not defined PLATFORM_TOOLSET set "PLATFORM_TOOLSET=v143"
if not defined PRODUCT_VERSION set "PRODUCT_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH%.%WEASEL_BUILD%"
if not defined FILE_VERSION set "FILE_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH%.%WEASEL_BUILD%"
set "BOOST_ROOT=%BOOST_X64%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; try { $templatePath = Join-Path $env:PROJECT_ROOT 'weasel.props.template'; $outputPath = Join-Path $env:PROJECT_ROOT 'weasel.props'; $content = [IO.File]::ReadAllText($templatePath); foreach ($name in @('BOOST_ROOT', 'PLATFORM_TOOLSET', 'VERSION_MAJOR', 'VERSION_MINOR', 'VERSION_PATCH', 'PRODUCT_VERSION', 'FILE_VERSION')) { $value = [Environment]::GetEnvironmentVariable($name); if ([string]::IsNullOrEmpty($value)) { throw ('Missing build property: {0}' -f $name) }; $content = $content.Replace('$' + $name, [Security.SecurityElement]::Escape($value)) }; [IO.File]::WriteAllText($outputPath, $content, [Text.UTF8Encoding]::new($false)); exit 0 } catch { Write-Host ('[ERROR] {0}' -f $_.Exception.Message); exit 1 }"
if errorlevel 1 (
  echo [ERROR] Failed to generate weasel.props.
  goto :failed
)

echo.
echo [5/9] Building WeaselStats.exe ^(Release x64^)...
"%MSBUILD%" "%PROJECT_ROOT%weasel.sln" /m /t:WeaselStats /p:Configuration=Release /p:Platform=x64 "/p:BOOST_ROOT=%BOOST_X64%" /v:minimal
if errorlevel 1 (
  echo [ERROR] Failed to build WeaselStats.exe.
  goto :failed
)

echo.
echo [6/9] Building WeaselServer.exe ^(Release x64^)...
"%MSBUILD%" "%PROJECT_ROOT%weasel.sln" /m /t:WeaselServer /p:Configuration=Release /p:Platform=x64 "/p:BOOST_ROOT=%BOOST_X64%" /v:minimal
if errorlevel 1 (
  echo [ERROR] Failed to build WeaselServer.exe.
  goto :failed
)

echo.
echo [7/9] Building weaselx64.dll ^(Release x64^)...
"%MSBUILD%" "%PROJECT_ROOT%weasel.sln" /m /t:WeaselTSF /p:Configuration=Release /p:Platform=x64 "/p:BOOST_ROOT=%BOOST_X64%" /v:minimal
if errorlevel 1 (
  echo [ERROR] Failed to build weaselx64.dll.
  goto :failed
)

echo.
echo [8/9] Building weasel.dll ^(Release Win32^)...
"%MSBUILD%" "%PROJECT_ROOT%weasel.sln" /m /t:WeaselTSF /p:Configuration=Release /p:Platform=Win32 "/p:BOOST_ROOT=%BOOST_X86%" /v:minimal
if errorlevel 1 (
  echo [ERROR] Failed to build weasel.dll.
  goto :failed
)

set "BUILD_STATS=%PROJECT_ROOT%output\WeaselStats.exe"
set "BUILD_SERVER=%PROJECT_ROOT%output\WeaselServer.exe"
set "BUILD_TSF32=%PROJECT_ROOT%output\weasel.dll"
set "BUILD_TSF64=%PROJECT_ROOT%output\weaselx64.dll"
if not exist "%BUILD_STATS%" (
  echo [ERROR] WeaselStats.exe is missing after the build.
  goto :failed
)
if not exist "%BUILD_SERVER%" (
  echo [ERROR] WeaselServer.exe is missing after the build.
  goto :failed
)
if not exist "%BUILD_TSF32%" (
  echo [ERROR] weasel.dll is missing after the build.
  goto :failed
)
if not exist "%BUILD_TSF64%" (
  echo [ERROR] weaselx64.dll is missing after the build.
  goto :failed
)

echo.
echo [9/9] Validating versions and creating the manifest and ZIP...
set "PACKAGE_MANIFEST=%PROJECT_ROOT%output\weasel-stats-patch-manifest.json"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; function Get-Version([string] $path) { $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path); $version = [Version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart); if ($version.ToString() -eq '0.0.0.0') { throw ('File has no valid version: {0}' -f $path) }; return $version }; function Get-Sha256Hex([string] $path) { $stream = [IO.File]::OpenRead($path); $sha = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $sha.Dispose(); $stream.Dispose() } }; function Get-Crc32([string] $value) { [uint32]$crc = [uint32]::MaxValue; foreach ($byte in [Text.Encoding]::UTF8.GetBytes($value)) { $crc = [uint32]($crc -bxor [uint32]$byte); for ($bit = 0; $bit -lt 8; ++$bit) { if (($crc -band 1) -ne 0) { $crc = [uint32](($crc -shr 1) -bxor 3988292384L) } else { $crc = [uint32]($crc -shr 1) } } }; return ('{0:X8}' -f ([uint32]($crc -bxor [uint32]::MaxValue))) }; try { $files = @($env:BUILD_STATS, $env:BUILD_SERVER, $env:BUILD_TSF32, $env:BUILD_TSF64); $expected = Get-Version $files[0]; $hashes = [ordered]@{}; foreach ($file in $files) { $actual = Get-Version $file; if ($actual -ne $expected) { throw ('Build artifact version mismatch: {0} is {1}, expected {2}' -f $file, $actual, $expected) }; $hashes[[IO.Path]::GetFileName($file)] = Get-Sha256Hex $file }; $buildMinute = (Get-Date).ToString('yyyyMMddHHmm'); $buildVersion = Get-Crc32 $buildMinute; $manifest = [ordered]@{ schemaVersion = 1; buildMinute = $buildMinute; buildVersion = $buildVersion; baseVersion = $expected.ToString(); files = $hashes }; $json = $manifest | ConvertTo-Json -Depth 3; [IO.File]::WriteAllText($env:PACKAGE_MANIFEST, $json, [Text.UTF8Encoding]::new($false)); Write-Host ('All four build artifact versions match: {0}; patch build version: {1}' -f $expected, $buildVersion); exit 0 } catch { Write-Host ('[ERROR] {0}' -f $_.Exception.Message); exit 1 }"
if errorlevel 1 goto :failed

set "PATCH_VERSION="
for /f "delims=" %%V in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($env:BUILD_STATS); '{0}.{1}.{2}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart"') do set "PATCH_VERSION=%%V"
if not defined PATCH_VERSION (
  echo [ERROR] Cannot read the patch version.
  goto :failed
)
set "PATCH_BUILD_VERSION="
for /f "delims=" %%V in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$manifest = Get-Content -LiteralPath $env:PACKAGE_MANIFEST -Raw | ConvertFrom-Json; [string]$manifest.buildVersion"') do set "PATCH_BUILD_VERSION=%%V"
if not defined PATCH_BUILD_VERSION (
  echo [ERROR] Cannot read the 8-character patch build version.
  goto :failed
)

set "PACKAGE_DEPLOY=%PROJECT_ROOT%deploy_stats_patch.cmd"
set "PACKAGE_ZIP=%PROJECT_ROOT%output\weasel-input-statistics-patch-%PATCH_VERSION%-%PATCH_BUILD_VERSION%.zip"
set "PACKAGE_TEMP=%PROJECT_ROOT%output\weasel-input-statistics-patch-%PATCH_VERSION%-%PATCH_BUILD_VERSION%.tmp-%RANDOM%-%RANDOM%.zip"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; try { $files = @($env:BUILD_STATS, $env:BUILD_SERVER, $env:BUILD_TSF32, $env:BUILD_TSF64, $env:PACKAGE_MANIFEST, $env:PACKAGE_DEPLOY); Compress-Archive -LiteralPath $files -DestinationPath $env:PACKAGE_TEMP -CompressionLevel Optimal -Force; Add-Type -AssemblyName System.IO.Compression.FileSystem; $archive = [System.IO.Compression.ZipFile]::OpenRead($env:PACKAGE_TEMP); try { $expected = @('WeaselStats.exe', 'WeaselServer.exe', 'weasel.dll', 'weaselx64.dll', 'weasel-stats-patch-manifest.json', 'deploy_stats_patch.cmd'); if ($archive.Entries.Count -ne $expected.Count) { throw ('Incorrect ZIP entry count: {0}' -f $archive.Entries.Count) }; foreach ($name in $expected) { $found = $false; foreach ($entry in $archive.Entries) { if ($entry.FullName -eq $name) { $found = $true; break } }; if (-not $found) { throw ('ZIP is missing file: {0}' -f $name) } } } finally { $archive.Dispose() }; exit 0 } catch { Write-Host ('[ERROR] Package validation failed: {0}' -f $_.Exception.Message); exit 1 }"
if errorlevel 1 (
  if exist "%PACKAGE_TEMP%" del /f /q "%PACKAGE_TEMP%" >nul 2>&1
  goto :failed
)

move /y "%PACKAGE_TEMP%" "%PACKAGE_ZIP%" >nul
if errorlevel 1 (
  echo [ERROR] Cannot write the final ZIP: %PACKAGE_ZIP%
  if exist "%PACKAGE_TEMP%" del /f /q "%PACKAGE_TEMP%" >nul 2>&1
  goto :failed
)

echo.
echo [SUCCESS] Built four artifacts and created the manifest and ZIP.
echo Output: %PACKAGE_ZIP%
echo ZIP contents:
echo   WeaselStats.exe
echo   WeaselServer.exe
echo   weasel.dll
echo   weaselx64.dll
echo   weasel-stats-patch-manifest.json
echo   deploy_stats_patch.cmd
echo.
popd >nul
pause
exit /b 0

:failed
echo.
echo Build or packaging did not complete.
echo.
popd >nul
pause
exit /b 1

:show_cpp_install_help
echo.
echo Open Visual Studio Installer and install or modify Visual Studio 2022 / Build Tools 2022:
echo   1. Select the Desktop development with C++ workload.
echo   2. Include MSVC v143 - VS 2022 C++ x64/x86 build tools.
echo   3. Include a Windows 10 SDK or Windows 11 SDK.
echo Microsoft installation guide:
echo https://learn.microsoft.com/zh-cn/cpp/build/vscpp-step-0-installation?view=msvc-170
echo Close this window and rerun build_stats_patch.cmd after installation.
echo.
exit /b 0

:build_boost_arch
setlocal
set "BOOST_BUILD_ROOT=%~1"
set "BOOST_ADDRESS_MODEL=%~2"
set "BOOST_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined BOOST_JOBS set "BOOST_JOBS=1"
set "BOOST_VS_ARCH=x64"
if "%BOOST_ADDRESS_MODEL%"=="32" set "BOOST_VS_ARCH=x86"
call "%VSDEVCMD%" -no_logo -arch=%BOOST_VS_ARCH% -host_arch=x64 >nul
if errorlevel 1 (
  echo [ERROR] Failed to initialize the Visual Studio C++ %BOOST_VS_ARCH% environment.
  call :show_cpp_install_help
  endlocal
  exit /b 1
)
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] The Visual Studio C++ %BOOST_VS_ARCH% compiler cl.exe is unavailable.
  call :show_cpp_install_help
  endlocal
  exit /b 1
)
where rc.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] The Windows SDK resource compiler rc.exe is unavailable.
  call :show_cpp_install_help
  endlocal
  exit /b 1
)
pushd "%BOOST_BUILD_ROOT%" >nul
if errorlevel 1 (
  endlocal
  exit /b 1
)
if not exist "b2.exe" call bootstrap.bat
if errorlevel 1 (
  popd >nul
  endlocal
  exit /b 1
)
"%BOOST_BUILD_ROOT%\b2.exe" -j%BOOST_JOBS% ^
  --reconfigure ^
  --with-filesystem ^
  --with-json ^
  --with-locale ^
  --with-regex ^
  --with-serialization ^
  --with-system ^
  --with-thread ^
  define=BOOST_USE_WINAPI_VERSION=0x0603 ^
  toolset=msvc-14.3 ^
  link=static ^
  runtime-link=static ^
  --build-type=complete ^
  architecture=x86 ^
  address-model=%BOOST_ADDRESS_MODEL% ^
  stage
if errorlevel 1 (
  popd >nul
  endlocal
  exit /b 1
)
popd >nul
endlocal
exit /b 0
