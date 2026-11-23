@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title Weasel input statistics patch deployment

set "PATCH_SCRIPT=%~f0"
set "PATCH_DIR=%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent()); if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 0 } else { exit 1 }"
if not errorlevel 1 goto :main

echo Requesting administrator privileges...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$arguments = '/d /c ""{0}""' -f $env:PATCH_SCRIPT; try { Start-Process -FilePath $env:ComSpec -ArgumentList $arguments -WorkingDirectory $env:PATCH_DIR -Verb RunAs ^| Out-Null; exit 0 } catch { exit 1 }"
if errorlevel 1 (
  echo [ERROR] Administrator privileges were not granted. The patch was not deployed.
  pause
)
exit /b

:main
echo.
echo Weasel input statistics patch deployment
echo ========================
echo.

call :require_file "%PATCH_DIR%WeaselStats.exe"
if errorlevel 1 goto :failed_before_stop
call :require_file "%PATCH_DIR%WeaselServer.exe"
if errorlevel 1 goto :failed_before_stop
call :require_file "%PATCH_DIR%weasel.dll"
if errorlevel 1 goto :failed_before_stop
call :require_file "%PATCH_DIR%weaselx64.dll"
if errorlevel 1 goto :failed_before_stop
call :require_file "%PATCH_DIR%weasel-stats-patch-manifest.json"
if errorlevel 1 goto :failed_before_stop

set "WEASEL_ROOT="
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Rime\Weasel" /v WeaselRoot 2^>nul ^| findstr /i /c:"WeaselRoot"') do set "WEASEL_ROOT=%%B"
if defined WEASEL_ROOT goto :root_found
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Rime\Weasel" /v WeaselRoot 2^>nul ^| findstr /i /c:"WeaselRoot"') do set "WEASEL_ROOT=%%B"
if defined WEASEL_ROOT goto :root_found
for /f "delims=" %%D in ('dir /b /ad /o-d "%ProgramFiles%\Rime\weasel-*" 2^>nul') do if not defined WEASEL_ROOT set "WEASEL_ROOT=%ProgramFiles%\Rime\%%D"

:root_found
if not defined WEASEL_ROOT (
  echo [ERROR] The Weasel installation directory was not found.
  goto :failed_before_stop
)
if not exist "%WEASEL_ROOT%\WeaselServer.exe" (
  echo [ERROR] Invalid installation directory: %WEASEL_ROOT%
  goto :failed_before_stop
)
call :require_installed_file "%WEASEL_ROOT%\WeaselStats.exe"
if errorlevel 1 goto :failed_before_stop
call :require_installed_file "%WEASEL_ROOT%\weasel.dll"
if errorlevel 1 goto :failed_before_stop
call :require_installed_file "%WEASEL_ROOT%\weaselx64.dll"
if errorlevel 1 goto :failed_before_stop

set "TSF_CLSID={A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}"
set "TSF32_TARGET="
set "TSF64_TARGET="
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Classes\CLSID\%TSF_CLSID%\InprocServer32" /ve /reg:64 2^>nul ^| findstr /i /c:"REG_SZ"') do set "TSF64_TARGET=%%B"
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Classes\CLSID\%TSF_CLSID%\InprocServer32" /ve /reg:32 2^>nul ^| findstr /i /c:"REG_SZ"') do set "TSF32_TARGET=%%B"
if not defined TSF64_TARGET (
  echo [ERROR] The registered 64-bit TSF path was not found.
  goto :failed_before_stop
)
if not defined TSF32_TARGET (
  echo [ERROR] The registered 32-bit TSF path was not found.
  goto :failed_before_stop
)
if /i "%TSF64_TARGET%"=="%TSF32_TARGET%" (
  echo [ERROR] The registered 32-bit and 64-bit TSF paths are unexpectedly identical: %TSF64_TARGET%
  goto :failed_before_stop
)
call :require_registered_file "%TSF64_TARGET%"
if errorlevel 1 goto :failed_before_stop
call :require_registered_file "%TSF32_TARGET%"
if errorlevel 1 goto :failed_before_stop

set "VERSION_BASE=%WEASEL_ROOT%\WeaselServer.exe"
set "VERSION_INSTALLED_STATS=%WEASEL_ROOT%\WeaselStats.exe"
set "VERSION_INSTALLED_TSF32=%WEASEL_ROOT%\weasel.dll"
set "VERSION_INSTALLED_TSF64=%WEASEL_ROOT%\weaselx64.dll"
set "VERSION_PATCH_STATS=%PATCH_DIR%WeaselStats.exe"
set "VERSION_PATCH_SERVER=%PATCH_DIR%WeaselServer.exe"
set "VERSION_PATCH_TSF32=%PATCH_DIR%weasel.dll"
set "VERSION_PATCH_TSF64=%PATCH_DIR%weaselx64.dll"
set "PATCH_MANIFEST=%PATCH_DIR%weasel-stats-patch-manifest.json"
echo Validating patch version consistency...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; function Get-NumericVersion([string] $path) { $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path); $version = [Version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart); if ($version.ToString() -eq '0.0.0.0') { throw ('File has no valid version information: {0}' -f $path) }; return $version }; function Get-Sha256Hex([string] $path) { $stream = [IO.File]::OpenRead($path); $sha = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $sha.Dispose(); $stream.Dispose() } }; function Get-Crc32([string] $value) { [uint32]$crc = [uint32]::MaxValue; foreach ($byte in [Text.Encoding]::UTF8.GetBytes($value)) { $crc = [uint32]($crc -bxor [uint32]$byte); for ($bit = 0; $bit -lt 8; ++$bit) { if (($crc -band 1) -ne 0) { $crc = [uint32](($crc -shr 1) -bxor 3988292384L) } else { $crc = [uint32]($crc -shr 1) } } }; return ('{0:X8}' -f ([uint32]($crc -bxor [uint32]::MaxValue))) }; try { $manifest = Get-Content -LiteralPath $env:PATCH_MANIFEST -Raw | ConvertFrom-Json; if ($manifest.schemaVersion -ne 1) { throw ('Unsupported manifest schema version: {0}' -f $manifest.schemaVersion) }; $buildMinute = [string]$manifest.buildMinute; $buildVersion = [string]$manifest.buildVersion; if ($buildMinute -notmatch '^\d{12}$' -or $buildVersion -notmatch '^[0-9A-F]{8}$') { throw 'Invalid build minute or patch build version format' }; $calculatedVersion = Get-Crc32 $buildMinute; if ($calculatedVersion -cne $buildVersion) { throw ('Patch build version validation failed: manifest {0}, calculated {1}' -f $buildVersion, $calculatedVersion) }; $base = Get-NumericVersion $env:VERSION_BASE; if ([string]$manifest.baseVersion -ne $base.ToString()) { throw ('Manifest base version mismatch: {0}; installed version: {1}' -f $manifest.baseVersion, $base) }; $installedFiles = @($env:VERSION_INSTALLED_STATS, $env:VERSION_INSTALLED_TSF32, $env:VERSION_INSTALLED_TSF64); foreach ($path in $installedFiles) { $version = Get-NumericVersion $path; if ($version -ne $base) { throw ('Installed file version mismatch: {0} is {1}; base version is {2}' -f $path, $version, $base) } }; $patchFiles = @(@{ Path = $env:VERSION_PATCH_STATS; Name = 'WeaselStats.exe' }, @{ Path = $env:VERSION_PATCH_SERVER; Name = 'WeaselServer.exe' }, @{ Path = $env:VERSION_PATCH_TSF32; Name = 'weasel.dll' }, @{ Path = $env:VERSION_PATCH_TSF64; Name = 'weaselx64.dll' }); foreach ($item in $patchFiles) { $version = Get-NumericVersion $item.Path; if ($version -ne $base) { throw ('Patch version mismatch: {0} is {1}; base version is {2}' -f $item.Path, $version, $base) }; $property = $manifest.files.PSObject.Properties[$item.Name]; if (!$property) { throw ('Manifest is missing file: {0}' -f $item.Name) }; $actualHash = Get-Sha256Hex $item.Path; if ($actualHash -cne [string]$property.Value) { throw ('Patch file does not belong to build {0}: {1}' -f $buildVersion, $item.Name) } }; Write-Host ('Version and build validation passed: base {0}, build {1}' -f $base, $buildVersion); exit 0 } catch { Write-Host ('[ERROR] {0}' -f $_.Exception.Message); exit 1 }"
if errorlevel 1 goto :failed_before_stop

echo Installation directory: %WEASEL_ROOT%
echo 64-bit TSF: %TSF64_TARGET%
echo 32-bit TSF: %TSF32_TARGET%
echo Stopping the Weasel service cleanly...
set "WEASEL_SERVER=%WEASEL_ROOT%\WeaselServer.exe"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; $helper = $null; try { $session = [Diagnostics.Process]::GetCurrentProcess().SessionId; $serverPath = [IO.Path]::GetFullPath($env:WEASEL_SERVER); $serverIds = @(Get-Process -Name 'WeaselServer' -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $session -and $_.Path -ieq $serverPath } | ForEach-Object { $_.Id }); if (!$serverIds.Count) { exit 0 }; $helper = Start-Process -FilePath $serverPath -ArgumentList '/q' -WindowStyle Hidden -PassThru; $deadline = [DateTime]::UtcNow.AddSeconds(10); do { $running = @($serverIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue }); if (!$running.Count) { exit 0 }; Start-Sleep -Milliseconds 100 } while ([DateTime]::UtcNow -lt $deadline); exit 1 } catch { exit 1 } finally { if ($helper -and !$helper.HasExited) { $helper.Kill(); $helper.WaitForExit() } }"
if errorlevel 1 (
  echo [ERROR] The running Weasel service did not stop within 10 seconds. The patch was not deployed.
  goto :restart_after_failure
)
timeout /t 1 /nobreak >nul
tasklist /fi "IMAGENAME eq WeaselServer.exe" /nh 2>nul | findstr /i /c:"WeaselServer.exe" >nul
if not errorlevel 1 (
  echo [ERROR] The Weasel service is still running. The patch was not deployed.
  goto :failed_before_stop
)
tasklist /fi "IMAGENAME eq WeaselStats.exe" /nh 2>nul | findstr /i /c:"WeaselStats.exe" >nul
if not errorlevel 1 (
  echo [ERROR] WeaselStats is still running. The patch was not deployed.
  goto :restart_after_failure
)

set "BACKUP_DIR=%TEMP%\weasel-stats-patch-%RANDOM%-%RANDOM%"
md "%BACKUP_DIR%" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Cannot create the temporary backup directory. The patch was not deployed.
  goto :restart_after_failure
)

echo Backing up the installed files...
copy /y "%WEASEL_ROOT%\WeaselServer.exe" "%BACKUP_DIR%\WeaselServer.exe" >nul
if errorlevel 1 goto :backup_failed
copy /y "%WEASEL_ROOT%\WeaselStats.exe" "%BACKUP_DIR%\WeaselStats.exe" >nul
if errorlevel 1 goto :backup_failed
copy /y "%WEASEL_ROOT%\weasel.dll" "%BACKUP_DIR%\weasel.dll" >nul
if errorlevel 1 goto :backup_failed
copy /y "%WEASEL_ROOT%\weaselx64.dll" "%BACKUP_DIR%\weaselx64.dll" >nul
if errorlevel 1 goto :backup_failed
copy /y "%TSF32_TARGET%" "%BACKUP_DIR%\registered-weasel32.dll" >nul
if errorlevel 1 goto :backup_failed
copy /y "%TSF64_TARGET%" "%BACKUP_DIR%\registered-weasel64.dll" >nul
if errorlevel 1 goto :backup_failed

echo Deploying four patch files to the installation and registered TSF locations...
call :replace_file "%PATCH_DIR%WeaselServer.exe" "%WEASEL_ROOT%\WeaselServer.exe"
if errorlevel 1 goto :deploy_failed
call :replace_file "%PATCH_DIR%WeaselStats.exe" "%WEASEL_ROOT%\WeaselStats.exe"
if errorlevel 1 goto :deploy_failed
call :replace_file "%PATCH_DIR%weasel.dll" "%WEASEL_ROOT%\weasel.dll"
if errorlevel 1 goto :deploy_failed
call :replace_file "%PATCH_DIR%weaselx64.dll" "%WEASEL_ROOT%\weaselx64.dll"
if errorlevel 1 goto :deploy_failed
call :replace_file "%PATCH_DIR%weasel.dll" "%TSF32_TARGET%"
if errorlevel 1 goto :deploy_failed
call :replace_file "%PATCH_DIR%weaselx64.dll" "%TSF64_TARGET%"
if errorlevel 1 goto :deploy_failed

echo Verifying deployed files...
fc /b "%PATCH_DIR%WeaselServer.exe" "%WEASEL_ROOT%\WeaselServer.exe" >nul
if errorlevel 1 goto :verify_failed
fc /b "%PATCH_DIR%WeaselStats.exe" "%WEASEL_ROOT%\WeaselStats.exe" >nul
if errorlevel 1 goto :verify_failed
fc /b "%PATCH_DIR%weasel.dll" "%WEASEL_ROOT%\weasel.dll" >nul
if errorlevel 1 goto :verify_failed
fc /b "%PATCH_DIR%weaselx64.dll" "%WEASEL_ROOT%\weaselx64.dll" >nul
if errorlevel 1 goto :verify_failed
fc /b "%PATCH_DIR%weasel.dll" "%TSF32_TARGET%" >nul
if errorlevel 1 goto :verify_failed
fc /b "%PATCH_DIR%weaselx64.dll" "%TSF64_TARGET%" >nul
if errorlevel 1 goto :verify_failed

call :start_server
if errorlevel 1 (
  echo [ERROR] The patched Weasel service did not start. Rolling back...
  goto :rollback
)
rmdir /s /q "%BACKUP_DIR%" >nul 2>&1

echo.
echo [SUCCESS] Four statistics patch files were deployed and verified.
echo The Weasel service has been restarted.
echo Restart running applications, or sign out and sign in again.
echo Applications will load the new TSF DLL after they restart.
echo.
pause
exit /b 0

:backup_failed
echo [ERROR] The installed files could not be fully backed up. The patch was not deployed.
rmdir /s /q "%BACKUP_DIR%" >nul 2>&1
goto :restart_after_failure

:deploy_failed
echo [ERROR] Cannot write a patch file. Rolling back...
goto :rollback

:verify_failed
echo [ERROR] Deployed file verification failed. Rolling back...

:rollback
call :replace_file "%BACKUP_DIR%\WeaselServer.exe" "%WEASEL_ROOT%\WeaselServer.exe" >nul 2>&1
call :replace_file "%BACKUP_DIR%\WeaselStats.exe" "%WEASEL_ROOT%\WeaselStats.exe" >nul 2>&1
call :replace_file "%BACKUP_DIR%\weasel.dll" "%WEASEL_ROOT%\weasel.dll" >nul 2>&1
call :replace_file "%BACKUP_DIR%\weaselx64.dll" "%WEASEL_ROOT%\weaselx64.dll" >nul 2>&1
call :replace_file "%BACKUP_DIR%\registered-weasel32.dll" "%TSF32_TARGET%" >nul 2>&1
call :replace_file "%BACKUP_DIR%\registered-weasel64.dll" "%TSF64_TARGET%" >nul 2>&1
fc /b "%BACKUP_DIR%\WeaselServer.exe" "%WEASEL_ROOT%\WeaselServer.exe" >nul
if errorlevel 1 goto :rollback_failed
fc /b "%BACKUP_DIR%\WeaselStats.exe" "%WEASEL_ROOT%\WeaselStats.exe" >nul
if errorlevel 1 goto :rollback_failed
fc /b "%BACKUP_DIR%\weasel.dll" "%WEASEL_ROOT%\weasel.dll" >nul
if errorlevel 1 goto :rollback_failed
fc /b "%BACKUP_DIR%\weaselx64.dll" "%WEASEL_ROOT%\weaselx64.dll" >nul
if errorlevel 1 goto :rollback_failed
fc /b "%BACKUP_DIR%\registered-weasel32.dll" "%TSF32_TARGET%" >nul
if errorlevel 1 goto :rollback_failed
fc /b "%BACKUP_DIR%\registered-weasel64.dll" "%TSF64_TARGET%" >nul
if errorlevel 1 goto :rollback_failed
rmdir /s /q "%BACKUP_DIR%" >nul 2>&1
echo The original files were restored.
goto :restart_after_failure

:rollback_failed
echo [CRITICAL] The original files could not be fully restored.
echo Backup retained at: %BACKUP_DIR%

:restart_after_failure
call :start_server

:failed_before_stop
echo.
echo Deployment did not complete.
echo.
pause
exit /b 1

:require_file
if exist "%~1" exit /b 0
echo [ERROR] Missing patch file: %~1
exit /b 1

:require_installed_file
if exist "%~1" exit /b 0
echo [ERROR] Missing target file in the installation directory: %~1
exit /b 1

:require_registered_file
if exist "%~1" exit /b 0
echo [ERROR] Missing target file at the registered TSF path: %~1
exit /b 1

:replace_file
copy /y "%~1" "%~2" >nul 2>&1
if not errorlevel 1 exit /b 0
for /l %%I in (0,1,9) do (
  if not exist "%~2.old.%%I" (
    move /y "%~2" "%~2.old.%%I" >nul
    if not errorlevel 1 (
      copy /y "%~1" "%~2" >nul
      if errorlevel 1 (
        move /y "%~2.old.%%I" "%~2" >nul
        exit /b 1
      )
      del /f /q "%~2.old.%%I" >nul 2>&1
      if errorlevel 1 (
        set "STALE_TSF_FILE=%~2.old.%%I"
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
          "$ErrorActionPreference = 'Stop'; Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class NativeMethods { [DllImport(\"kernel32.dll\", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool MoveFileEx(string existingFile, string newFile, int flags); }'; if (![NativeMethods]::MoveFileEx($env:STALE_TSF_FILE, $null, 4)) { exit 1 }"
        if errorlevel 1 exit /b 1
      )
      exit /b 0
    )
  )
)
exit /b 1

:start_server
tasklist /fi "IMAGENAME eq WeaselServer.exe" /nh 2>nul | findstr /i /c:"WeaselServer.exe" >nul
if not errorlevel 1 exit /b 0
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; $shell = New-Object -ComObject Shell.Application; $shell.ShellExecute($env:WEASEL_SERVER, '', $env:WEASEL_ROOT, 'open', 0)"
if errorlevel 1 exit /b 1
timeout /t 2 /nobreak >nul
tasklist /fi "IMAGENAME eq WeaselServer.exe" /nh 2>nul | findstr /i /c:"WeaselServer.exe" >nul
if errorlevel 1 exit /b 1
exit /b 0
