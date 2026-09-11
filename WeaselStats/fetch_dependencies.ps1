param(
  [string]$DestinationRoot = (Join-Path $PSScriptRoot "..\deps")
)

$ErrorActionPreference = "Stop"

function Get-VerifiedFile {
  param(
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Sha256
  )

  if (Test-Path -LiteralPath $Path) {
    $existingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    if ($existingHash -ieq $Sha256) {
      return
    }
    throw "Dependency exists with an unexpected hash: $Path"
  }

  $temporary = Join-Path ([System.IO.Path]::GetTempPath()) (
    "weasel-stats-" + [guid]::NewGuid().ToString("N") + ".download")
  Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $temporary
  $downloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
  if ($downloadHash -ine $Sha256) {
    throw "Dependency hash verification failed: $Uri"
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) |
    Out-Null
  Move-Item -LiteralPath $temporary -Destination $Path
}

$webViewVersion = "1.0.4191.47"
$webViewRoot = Join-Path $DestinationRoot "Microsoft.Web.WebView2.$webViewVersion"
$webViewHeader = Join-Path $webViewRoot "build\native\include\WebView2.h"
$webViewX86Lib = Join-Path $webViewRoot "build\native\x86\WebView2LoaderStatic.lib"
$webViewX64Lib = Join-Path $webViewRoot "build\native\x64\WebView2LoaderStatic.lib"
if (!(Test-Path -LiteralPath $webViewHeader) -or
    !(Test-Path -LiteralPath $webViewX86Lib) -or
    !(Test-Path -LiteralPath $webViewX64Lib)) {
  if (Test-Path -LiteralPath $webViewRoot) {
    throw "Incomplete WebView2 dependency directory: $webViewRoot"
  }
  $package = Join-Path ([System.IO.Path]::GetTempPath()) (
    "Microsoft.Web.WebView2.$webViewVersion." +
    [guid]::NewGuid().ToString("N") + ".nupkg")
  Get-VerifiedFile `
    -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$webViewVersion/microsoft.web.webview2.$webViewVersion.nupkg" `
    -Path $package `
    -Sha256 "F492BBF547D0DA329553B6727435B677579B1E9F91CC9E4A1AD029366D5F23D0"
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  New-Item -ItemType Directory -Force -Path $webViewRoot | Out-Null
  [System.IO.Compression.ZipFile]::ExtractToDirectory($package, $webViewRoot)
}

$echartsVersion = "6.1.0"
$echartsPath = Join-Path $DestinationRoot "echarts\$echartsVersion\echarts.min.js"
Get-VerifiedFile `
  -Uri "https://raw.githubusercontent.com/apache/echarts/$echartsVersion/dist/echarts.min.js" `
  -Path $echartsPath `
  -Sha256 "B66B25AEB4DF84E33199DC21694014D336D222CBD9DEB0E5A7C14BD6AA0D0FD0"

Write-Output "WeaselStats dependencies are ready in $DestinationRoot"
