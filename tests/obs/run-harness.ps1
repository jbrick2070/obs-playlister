# obs-playlister
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs playlister-harness.lua in a portable OBS 33 (TESTING.md, "Runtime harness"). It builds and installs the
# current build (build_x64, RelWithDebInfo) into <ObsRoot>\plugins\obs-playlister, registers the harness in the
# scene collection for this run only, creates an empty folder for the empty-folder checks (and names one that is
# never created, for a saved folder row whose folder is absent), starts OBS, waits for
# "HARNESS DONE", closes OBS, restores the scene collection and user.ini (Studio Mode is off for the run), removes the folder and prints the results with the
# plugin's log lines. Exit code 0 only when every check passed.
#
# The three files need different durations, and the second one must be longer than about 35 s (one step seeks to
# 30 s). -SceneCollection must name the collection OBS opens, or the harness never loads.
#
# Example:
#   .\tests\obs\run-harness.ps1 -ObsRoot C:\OBS-Portable -Files a.mp4, b.mp4, c.mp4
#
# The Phase 2 acceptance (phase2-acceptance.lua) also needs -Otr, a folder of renders on the same volume as the temp
# folder: the run makes a temporary folder of hard links to its files (never touching the files themselves), a
# folder of two links that the script moves away and back, an empty folder the current user may not read (a deny
# entry on that folder alone, not inherited, removed afterwards), and checks whether this machine reaches its own
# admin share for a UNC path:
#   .\tests\obs\run-harness.ps1 -ObsRoot C:\OBS-Portable -Files a.mp4, b.mp4, c.mp4 -Script phase2-acceptance.lua `
#     -Otr C:\renders -TimeoutSeconds 600
param(
	[Parameter(Mandatory = $true)][string]$ObsRoot,
	[Parameter(Mandatory = $true)][string[]]$Files,
	[string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
	[string]$SceneCollection = 'Untitled',
	[int]$TimeoutSeconds = 180,
	# another script in this folder, run the same way (spike-child-signals.lua records the child's behavior)
	[string]$Script = 'playlister-harness.lua',
	# a folder of renders for phase2-acceptance.lua
	[string]$Otr = ''
)
$ErrorActionPreference = 'Stop'
if ($Files.Count -ne 3) { throw 'pass exactly three media files, each of a different duration' }
if ($Script -eq 'phase2-acceptance.lua' -and -not $Otr) { throw 'phase2-acceptance.lua needs -Otr <render folder>' }
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
# Studio Mode leaves the harness sources in a preview scene that never plays (another project left it on here once,
# and every playback check failed), so it is off for the run and user.ini is restored afterwards
$userIni = Join-Path $ObsRoot 'config\obs-studio\user.ini'
$userIniBackup = "$userIni.harness-backup"
if (Test-Path $userIniBackup) { throw "a backup from an earlier run exists: $userIniBackup" }

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
$obs = $null
$failed = $true
$emptyFolder = $null
$mediaFolder = $null
$mediaJunction = $null
$stillFile = $null
$mediaLinks = @()
$otrFolder = $null
$otrLinks = @()
$otrOrder = $null
$moveFolder = $null
$moveLinks = @()
$deniedFolder = $null
try {
	if (Test-Path $userIni) {
		Copy-Item $userIni $userIniBackup
		$ini = [IO.File]::ReadAllText($userIni) -replace '(?m)^PreviewProgramMode=true', 'PreviewProgramMode=false'
		[IO.File]::WriteAllText($userIni, $ini, (New-Object Text.UTF8Encoding $false))
	}
	$emptyFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-empty-' + [guid]::NewGuid().ToString('N'))
	New-Item -ItemType Directory $emptyFolder | Out-Null
	$env:PLAYLISTER_HARNESS_EMPTY_FOLDER = $emptyFolder -replace '\\', '/'
	# never created: a row saved as a folder whose folder is gone (R-LIST-1)
	$missingFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-gone-' + [guid]::NewGuid().ToString('N'))
	$env:PLAYLISTER_HARNESS_MISSING_FOLDER = $missingFolder -replace '\\', '/'
	# a folder row of the three files, as hard links named 1.mp4 to 3.mp4 (no copies; the files must be on the same
	# volume as the temp folder)
	$mediaFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-media-' + [guid]::NewGuid().ToString('N'))
	New-Item -ItemType Directory $mediaFolder | Out-Null
	for ($i = 0; $i -lt 3; $i++) {
		$link = Join-Path $mediaFolder ('{0}.mp4' -f ($i + 1))
		New-Item -ItemType HardLink -Path $link -Target (Resolve-Path $Files[$i]).Path | Out-Null
		$mediaLinks += $link
	}
	$env:PLAYLISTER_HARNESS_MEDIA_FOLDER = $mediaFolder -replace '\\', '/'
	# the same folder through a junction whose name ends in a media extension: only a saved folder kind keeps such a
	# row a folder (R-LIST-1)
	$mediaJunction = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-linked-' + [guid]::NewGuid().ToString('N') + '.mp4')
	New-Item -ItemType Junction -Path $mediaJunction -Target $mediaFolder | Out-Null
	$env:PLAYLISTER_HARNESS_MEDIA_JUNCTION = $mediaJunction -replace '\\', '/'
	# a small valid still image for the still hold (R-DIR-9)
	Add-Type -AssemblyName System.Drawing
	$stillFile = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-still-' + [guid]::NewGuid().ToString('N') + '.jpg')
	$bitmap = New-Object System.Drawing.Bitmap 320, 180
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	$graphics.Clear([System.Drawing.Color]::SteelBlue)
	$graphics.Dispose()
	$bitmap.Save($stillFile, [System.Drawing.Imaging.ImageFormat]::Jpeg)
	$bitmap.Dispose()
	$env:PLAYLISTER_HARNESS_STILL = $stillFile -replace '\\', '/'
	if ($Otr) {
		# hard links to every render, named as the renders, and their names in date order (R-DIR-12: date modified,
		# ascending); a link shares its file's dates
		# the default extension filter's files (R-DIR-5), as the folder row lists them
		$extensions = 'mp4 m4v mov mkv webm avi flv ts mpg mxf gif mp3 m4a mka aac ogg opus wav flac jpg jpeg png webp bmp' -split ' '
		$renders = @(Get-ChildItem -LiteralPath $Otr -File | Where-Object { $extensions -contains $_.Extension.TrimStart('.').ToLower() } |
			Sort-Object LastWriteTimeUtc, Name)
		if ($renders.Count -lt 10) { throw "-Otr needs at least ten media files: $Otr" }
		# the script writes files named zz-...; a link of that name would let it write into a render
		if ($renders | Where-Object { $_.Name -like 'zz-*' }) { throw "-Otr holds a file named zz-..., which the script uses: $Otr" }
		$otrFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-otr-' + [guid]::NewGuid().ToString('N'))
		New-Item -ItemType Directory $otrFolder | Out-Null
		foreach ($render in $renders) {
			$link = Join-Path $otrFolder $render.Name
			New-Item -ItemType HardLink -Path $link -Target $render.FullName | Out-Null
			$otrLinks += $link
		}
		$otrOrder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-otr-order-' + [guid]::NewGuid().ToString('N') + '.txt')
		[IO.File]::WriteAllLines($otrOrder, [string[]]($renders | ForEach-Object { $_.Name }), (New-Object Text.UTF8Encoding $false))
		$env:PLAYLISTER_HARNESS_OTR_FOLDER = $otrFolder -replace '\\', '/'
		$env:PLAYLISTER_HARNESS_OTR_ORDER = $otrOrder -replace '\\', '/'
		# two links in a folder the script moves away and back
		$moveFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-moving-' + [guid]::NewGuid().ToString('N'))
		New-Item -ItemType Directory $moveFolder | Out-Null
		for ($i = 0; $i -lt 2; $i++) {
			$link = Join-Path $moveFolder ('{0}.mp4' -f [char](97 + $i))
			New-Item -ItemType HardLink -Path $link -Target (Resolve-Path $Files[$i]).Path | Out-Null
			$moveLinks += Split-Path -Leaf $link
		}
		$env:PLAYLISTER_HARNESS_MOVE_FOLDER = $moveFolder -replace '\\', '/'
		# empty, so the deny entry (on the folder alone, without inheritance) reaches no file
		$deniedFolder = Join-Path ([IO.Path]::GetTempPath()) ('playlister-harness-denied-' + [guid]::NewGuid().ToString('N'))
		New-Item -ItemType Directory $deniedFolder | Out-Null
		icacls $deniedFolder /deny "${env:USERDOMAIN}\${env:USERNAME}:(RX)" | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "icacls could not deny reading $deniedFolder" }
		$env:PLAYLISTER_HARNESS_DENIED_FOLDER = $deniedFolder -replace '\\', '/'
		# the media folder through this machine's admin share, when it is reachable
		$unc = '\\localhost\' + $mediaFolder.Substring(0, 1) + '$' + $mediaFolder.Substring(2)
		if (Test-Path -LiteralPath (Join-Path $unc '1.mp4')) {
			$env:PLAYLISTER_HARNESS_UNC_FOLDER = $unc
		}
		else {
			Remove-Item Env:\PLAYLISTER_HARNESS_UNC_FOLDER -ErrorAction SilentlyContinue
		}
	}
	# PowerShell names are case-insensitive: a $script here would overwrite the -Script parameter
	$scriptPath = (Join-Path $PSScriptRoot $Script) -replace '\\', '/'
	$json = Get-Content $collection -Raw | ConvertFrom-Json
	$json.modules.'scripts-tool' = @([pscustomobject]@{ path = $scriptPath; settings = [pscustomobject]@{} })
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
	# the player logs every load; the empty folder, the absent one, the denied one and the junction are never loaded
	# themselves (a file inside the junction is), so such a load means a folder row was taken for a file
	# loads only: the player's own "playing <path>" line (a skip message names its path after "playing" too)
	$folderLoads = @(Select-String -Path $log.FullName -Pattern '\[obs-playlister\] playing .*playlister-harness-(empty|gone|denied)-|\[obs-playlister\] playing .*playlister-harness-linked-[0-9a-f]+\.mp4$').Count
	if ($folderLoads -gt 0) { "FAIL a folder row was loaded as a file ($folderLoads loads)" }
	# R-BEH-5: the corrupt file is loaded once by each failing source and once more by Play, the corrupt still once; a
	# list that cycles through its failures loads them again and again, and a Play that did nothing loads one less
	$loadCountsWrong = $false
	if ($Script -eq 'playlister-harness.lua') {
		$corruptLoads = @(Select-String -Path $log.FullName -Pattern '\[obs-playlister\] playing .*playlister-harness-corrupt\.mp4').Count
		$stillLoads = @(Select-String -Path $log.FullName -Pattern '\[obs-playlister\] playing .*playlister-harness-corrupt\.jpg').Count
		if ($corruptLoads -ne 3) { "FAIL the corrupt file was loaded $corruptLoads times, not 3"; $loadCountsWrong = $true }
		if ($stillLoads -ne 1) { "FAIL the corrupt still was loaded $stillLoads times, not once"; $loadCountsWrong = $true }
	}
	$logLineMissing = $false
	if ($Script -eq 'phase2-acceptance.lua') {
		# R-PLAT-10: the denied folder's log line names the Win32 code
		if (-not (Select-String -Path $log.FullName -Pattern 'access denied \(permissions or security software\); Win32 error 5\)' -Quiet)) {
			'FAIL the denied folder has no log line naming Win32 error 5'
			$logLineMissing = $true
		}
		$overflows = @(Select-String -Path $log.FullName -Pattern 'more changes than the buffer holds').Count
		"overflows logged: $overflows"
	}
	"summary: $passes passed, $fails failed"
	$failed = (-not $done) -or $fails -gt 0 -or $folderLoads -gt 0 -or $loadCountsWrong -or $logLineMissing
}
finally {
	# OBS writes the scene collection and user.ini when it exits, so after Ctrl+C or an error it must be gone before
	# the restores below
	if ($obs -and -not $obs.HasExited) {
		Stop-Process -Id $obs.Id -Force -ErrorAction SilentlyContinue
		$obs.WaitForExit(10000) | Out-Null
	}
	# user.ini first, so a failed collection restore cannot leave Studio Mode off
	if (Test-Path $userIniBackup) {
		try {
			Copy-Item $userIniBackup $userIni -Force
			Remove-Item $userIniBackup -Force
		}
		catch { "warning: could not restore ${userIni} from ${userIniBackup}: $_" }
	}
	Remove-Item Env:\PLAYLISTER_HARNESS_FILES -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_EMPTY_FOLDER -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_MEDIA_FOLDER -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_MISSING_FOLDER -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_MEDIA_JUNCTION -ErrorAction SilentlyContinue
	Remove-Item Env:\PLAYLISTER_HARNESS_STILL -ErrorAction SilentlyContinue
	foreach ($name in 'OTR_FOLDER', 'OTR_ORDER', 'MOVE_FOLDER', 'DENIED_FOLDER', 'UNC_FOLDER') {
		Remove-Item "Env:\PLAYLISTER_HARNESS_$name" -ErrorAction SilentlyContinue
	}
	# the acceptance folders: only what this run made is removed (links, never the files they name); a file the
	# script left behind keeps its folder and is reported
	foreach ($link in $otrLinks) {
		if (Test-Path -LiteralPath $link) {
			try { [IO.File]::Delete($link) } catch { "warning: could not remove ${link}: $_" }
		}
	}
	if ($otrFolder -and (Test-Path $otrFolder)) {
		try { [IO.Directory]::Delete($otrFolder) } catch { "warning: could not remove ${otrFolder}: $_" }
	}
	if ($otrOrder -and (Test-Path $otrOrder)) {
		try { [IO.File]::Delete($otrOrder) } catch { "warning: could not remove ${otrOrder}: $_" }
	}
	if ($moveFolder) {
		# the script moves the folder back; if it stopped half way, the folder is still away
		foreach ($folder in $moveFolder, "$moveFolder-away") {
			if (Test-Path $folder) {
				foreach ($name in $moveLinks) {
					$link = Join-Path $folder $name
					if (Test-Path -LiteralPath $link) {
						try { [IO.File]::Delete($link) } catch { "warning: could not remove ${link}: $_" }
					}
				}
				try { [IO.Directory]::Delete($folder) } catch { "warning: could not remove ${folder}: $_" }
			}
		}
	}
	if ($deniedFolder -and (Test-Path $deniedFolder)) {
		icacls $deniedFolder /remove:d "${env:USERDOMAIN}\${env:USERNAME}" | Out-Null
		try { [IO.Directory]::Delete($deniedFolder) } catch { "warning: could not remove ${deniedFolder}: $_" }
	}
	if ($stillFile -and (Test-Path $stillFile)) {
		try { [IO.File]::Delete($stillFile) } catch { "warning: could not remove ${stillFile}: $_" }
	}
	Copy-Item $backup $collection -Force
	Remove-Item $backup -Force
	# created by this run and still empty; Directory.Delete refuses a folder that is not empty, so anything put into
	# it is kept and reported, never deleted
	if ($emptyFolder -and (Test-Path $emptyFolder)) {
		try { [IO.Directory]::Delete($emptyFolder) } catch { "warning: could not remove ${emptyFolder}: $_" }
	}
	# the junction first: removing it removes the link only, never the folder it points to
	if ($mediaJunction -and (Test-Path $mediaJunction)) {
		try { [IO.Directory]::Delete($mediaJunction) } catch { "warning: could not remove ${mediaJunction}: $_" }
	}
	# only the links this run made are deleted (removing a hard link leaves the file itself), then their folder
	foreach ($link in $mediaLinks) {
		try { [IO.File]::Delete($link) } catch { "warning: could not remove ${link}: $_" }
	}
	if ($mediaFolder -and (Test-Path $mediaFolder)) {
		try { [IO.Directory]::Delete($mediaFolder) } catch { "warning: could not remove ${mediaFolder}: $_" }
	}
}
if ($failed) { exit 1 }
