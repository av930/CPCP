#Requires -Version 5.1
<#
.SYNOPSIS
Installs GitHub CLI (gh) when it is missing.

.DESCRIPTION
Uses winget when it is available. Otherwise downloads the latest official
windows_amd64 ZIP from github.com/cli/cli and extracts it to
%LOCALAPPDATA%\Programs\GitHub CLI (no administrator rights needed). The
extracted bin folder is added to the user PATH.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-Gh {
	$command = Get-Command gh.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($null -ne $command) { return $command.Source }
	foreach ($directory in @((Join-Path $env:ProgramFiles 'GitHub CLI'), (Join-Path $env:LOCALAPPDATA 'Programs\GitHub CLI\bin'))) {
		$candidate = Join-Path $directory 'gh.exe'
		if (Test-Path -LiteralPath $candidate) { return $candidate }
	}
	return $null
}

try {
	$existing = Find-Gh
	if ($null -ne $existing) {
		Write-Host "[OKAY] GitHub CLI is already installed: $existing" -ForegroundColor Green
		exit 0
	}

	$winget = Get-Command winget.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($null -ne $winget) {
		Write-Host '==> Installing GitHub CLI with winget' -ForegroundColor Cyan
		& $winget.Source install --id GitHub.cli --exact --source winget --accept-package-agreements --accept-source-agreements
		if ($LASTEXITCODE -eq 0 -and $null -ne (Find-Gh)) {
			Write-Host '[OKAY] GitHub CLI installed with winget' -ForegroundColor Green
			exit 0
		}
		Write-Host "winget did not install GitHub CLI (exit code $LASTEXITCODE). Falling back to the ZIP download." -ForegroundColor Yellow
	}

	Write-Host '==> Downloading the latest GitHub CLI release' -ForegroundColor Cyan
	[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
	$ProgressPreference = 'SilentlyContinue'
	$release = Invoke-RestMethod -UseBasicParsing -Uri 'https://api.github.com/repos/cli/cli/releases/latest' -Headers @{ 'User-Agent' = 'ClipMaster-release' }
	$asset = @($release.assets | Where-Object { $_.name -match '^gh_.+_windows_amd64\.zip$' }) | Select-Object -First 1
	if ($null -eq $asset) { throw 'The windows_amd64 ZIP was not found in the latest GitHub CLI release.' }

	$temporary = Join-Path ([IO.Path]::GetTempPath()) ('gh-install-' + [Guid]::NewGuid().ToString('N'))
	New-Item -ItemType Directory -Path $temporary | Out-Null
	try {
		$archive = Join-Path $temporary $asset.name
		Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $archive
		$target = Join-Path $env:LOCALAPPDATA 'Programs\GitHub CLI'
		if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
		Expand-Archive -LiteralPath $archive -DestinationPath $target -Force
	} finally {
		Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
	}

	$binary = Join-Path $env:LOCALAPPDATA 'Programs\GitHub CLI\bin'
	if (-not (Test-Path -LiteralPath (Join-Path $binary 'gh.exe'))) { throw "gh.exe was not found after extracting to $binary" }
	$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
	if (@($userPath -split ';') -notcontains $binary) {
		[Environment]::SetEnvironmentVariable('Path', (($userPath.TrimEnd(';'), $binary) -join ';').TrimStart(';'), 'User')
	}
	& (Join-Path $binary 'gh.exe') --version
	Write-Host "[OKAY] GitHub CLI installed to $binary" -ForegroundColor Green
} catch {
	Write-Host "[FAIL] $($_.Exception.Message)" -ForegroundColor Red
	exit 1
}
