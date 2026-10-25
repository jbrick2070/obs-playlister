# obs-playlister
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs playlister-harness.lua in a portable OBS 33 (TESTING.md, "Runtime harness"). It builds and installs the
# current build (build_x64, RelWithDebInfo) into <ObsRoot>\plugins\obs-playlister, registers the harness in the
# scene collection for this run only, creates an empty folder for the empty-folder checks, starts OBS, waits for
# "HARNESS DONE", closes OBS, restores the scene collection, removes the folder and prints the results with the
# plugin's log lines. Exit code 0 only when every check passed.
#
# The three files need different durations, and the second one must be longer than about 35 s (one step seeks to
# 30 s). -SceneCollection must name the collection OBS opens, or the harness never loads.
#
# Example:
#   .\tests\obs\run-harness.ps1 -ObsRoot C:\OBS-Portable -Files a.mp4, b.mp4, c.mp4
param(
	[Parameter(Mandatory = $true)][string]$ObsRoot,
	[Parameter(Mandatory = $true)][string[]]$Files,
	[string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
	[string]$SceneCollection = 'Untitled',
	[int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
if ($Files.Count -ne 3) { throw 'pass exactly three media files, each of a different duration' }
if (-not (Test-Path (Join-Path $ObsRoot 'portable_mode.txt'))) { throw "$ObsRoot is not a portable OBS (no portable_mode.txt)" }
# another OBS (an installed one) may stay open: the portable copy keeps its own config, logs and plugins, and starts
# with --multi
$portableExe = (Resolve-Path (Join-Path $ObsRoot 'bin\64bit\obs64.exe')).Path
if (Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $portableExe }) {
	throw 'the portable OBS is already running'
}

$collection = Join-Path $ObsRoot "config\obs-studio\basic\scenes\$SceneCollection.json"
$backup = "$collection.harness-backup"
if (-not (Test-Path $collection)) { throw "scene collection not found: $collection" }
# a backup left by an interrupted run may be the only good copy; restore it by hand before running again
if (Test-Path $backup) { throw "a backup from an earlier run exists: $backup" }

Push-Location $Repo
try {
	cmake --build --preset windows-x64 --config RelWithDebInfo | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'the build failed' }
	Remove-Item -Recurse -Force 'release\RelWithDebInfo' -ErrorAction SilentlyContinue
	cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo | Out-Null
	$target = Join-Path $ObsRoot 'plugins\obs-playlister'
	Remove-Item -Recurse -Force $target -ErrorAction SilentlyContinue
	New-Item -ItemType Directory -Force (Join-Path $ObsRoot 'plugins') | Out-Null
	Copy-Item -Recurse 'release\RelWithDebInfo\obs-playlister' $target
}
finally {
	Pop-Location
}

Copy-Item $collection $backup
$failed = $true
$emptyFolder = $null
try {
	$emptyFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-empty-' + [guid]::NewGuid().ToString('N'))
	New-Item -ItemType Directory $emptyFolder | Out-Null
	$env:PLAYLISTER_HARNESS_EMPTY_FOLDER = $emptyFolder -replace '\\', '/'
	$script = (Join-Path $PSScriptRoot 'playlister-harness.lua') -replace '\\', '/'
	$json = Get-Content $collection -Raw | ConvertFrom-Json
	$json.modules.'scripts-tool' = @([pscustomobject]@{ path = $script; settings = [pscustomobject]@{} })
	[IO.File]::WriteAllText($collection, ($json | ConvertTo-Json -Depth 64), (New-Object Text.UTF8Encoding $false))

	$env:PLAYLISTER_HARNESS_FILES = (($Files | ForEach-Object { (Resolve-Path $_).Path -replace '\\', '/' }) -join '|')
	$bin = Join-Path $ObsRoot 'bin\64bit'
	$obs = Start-Process -FilePath (Join-Path $bin 'obs64.exe') -WorkingDirectory $bin -ArgumentList '--portable', '--disable-updater', '--multi' -PassThru
	$logDir = Join-Path $ObsRoot 'config\obs-studio\logs'
	$log = $null
	$done = $false
	for ($i = 0; $i -lt $TimeoutSeconds -and -not $done; $i++) {
		Start-Sleep -Seconds 1
		if ($obs.HasExited) { break }
		$log = Get-ChildItem $logDir -Filter *.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
		if ($log -and $log.LastWriteTime -gt $obs.StartTime) {
			$done = [bool](Select-String -Path $log.FullName -Pattern 'HARNESS DONE' -Quiet)
		}
	}
	if (-not $obs.HasExited) {
		[void]$obs.CloseMainWindow()
		if (-not $obs.WaitForExit(30000)) {
			# OBS writes the scene collection when it exits, so it must be gone before the restore below
			'OBS did not exit within 30 s; closing it'
			Stop-Process -Id $obs.Id -Force
			$obs.WaitForExit(10000) | Out-Null
		}
	}
	"harness finished: $done; OBS exit code: $($obs.ExitCode); log: $($log.FullName)"
	Select-String -Path $log.FullName -Pattern 'HARNESS|\[obs-playlister\]|memory leaks|Crash|crash' |
		ForEach-Object { '  ' + $_.Line.Trim() }
	$passes = @(Select-String -Path $log.FullName -Pattern 'HARNESS PASS').Count
	$fails = @(Select-String -Path $log.FullName -Pattern 'HARNESS FAIL').Count
	# the player logs every load; an empty folder row is never loaded, so a load of it means it was taken for a file
	$folderLoads = @(Select-String -Path $log.FullName -Pattern 'playing .*playlister-harness-empty-').Count
	if ($folderLoads -gt 0) { "FAIL the empty folder row was loaded as a file ($folderLoads loads)" }
	"summary: $passes passed, $fails failed"
	$failed = (-not $done) -or $fails -gt 0 -or $folderLoads -gt 0
}
finally {
	Remove-Item Env:\PLAYLISTER_HARNESS_FILES -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_EMPTY_FOLDER -ErrorAction SilentlyContinue
	Copy-Item $backup $collection -Force
	Remove-Item $backup -Force
	# created by this run and still empty; Directory.Delete refuses a folder that is not empty, so anything put into
	# it is kept and reported, never deleted
	if ($emptyFolder -and (Test-Path $emptyFolder)) {
		try { [IO.Directory]::Delete($emptyFolder) } catch { "warning: could not remove ${emptyFolder}: $_" }
	}
}
if ($failed) { exit 1 }
