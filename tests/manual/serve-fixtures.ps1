# obs-playlister: serves the playlist fixtures and the awkward responses TESTING.md needs (R-REM-5), on
# http://localhost:<Port>/ with .NET's HttpListener. Windows PowerShell 5.1; no admin rights (a localhost prefix needs
# no URL reservation). Not run by ctest. Stop it with Ctrl+C.
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Routes:
#   /fixtures/<name>        a file from tests/fixtures/playlists, with a Content-Type from its extension
#   /extensionless-m3u      relative.m3u served as audio/x-mpegurl at a URL without an extension
#   /extensionless-pls      radio.pls served as audio/x-scpls at a URL without an extension
#   /sniff-m3u              vlc.m3u served as text/plain (classification by the first 64 KB)
#   /html                   an HTML page that is not a directory index
#   /redirect/<n>           <n> redirects, then the vlc.m3u fixture (/redirect/0 is the list itself)
#   /status/<code>          an empty response with that status (429, 503, 404, ...)
#   /hang                   accepts, then never answers (the 60 s deadline; removing the source meanwhile)
#   /hang-body              headers and 10 bytes of 1000, then nothing
#   /big                    a 17 MB list body (over the 16 MB cap)
#   /gzip-at-cap            a gzip body that inflates to exactly 16 MB
#   /gzip-over-cap          a gzip body that inflates to 16 MB plus one byte
#   /slow                   the vlc.m3u fixture, one byte every 100 ms
#   /switchable/<name>      a file like /fixtures/<name>, or an empty response with the status that <name>.status
#                           holds while that file exists (a list that starts failing after a good fetch)
#   /media/<n>              the n-th -Media file (from 1) as video/mp4, at a URL without an extension; byte ranges are
#                           answered, and the body is sent in the background, so a player's second connection is served
#                           while the first is still open
#
# The Phase 3a acceptance (tests/obs/phase3a-acceptance.lua) starts it with -Root (a copy of the fixtures, where the
# script writes its own lists), -Media and -Log (one line per request when it starts, and one more when a media body
# ends: time, path, user agent, bytes read for the client, outcome).

param(
	[int]$Port = 8765,
	[string]$Root = '',
	[string[]]$Media = @(),
	[string]$Log = ''
)

$ErrorActionPreference = 'Stop'
# powershell -File passes an array as one string, so the files may also come as one, separated by |
$Media = @($Media | ForEach-Object { $_ -split '\|' } | Where-Object { $_ })
$fixtures = if ($Root) { $Root } else { Join-Path $PSScriptRoot '..\fixtures\playlists' }
$cap = 16 * 1024 * 1024
# the media bodies being sent: @{ Task; File; Response; Start; Path; Agent }
$streams = New-Object System.Collections.ArrayList

# bytes: for a media body, what was read from the file for the client (at most one 64 KB buffer more than it took);
# 0 for every other route, logged as "started" before it is answered
function Write-Request([string]$path, [string]$agent, [long]$bytes, [string]$outcome) {
	if ($Log) {
		$line = '{0:HH:mm:ss.fff}	{1}	{2}	{3}	{4}' -f (Get-Date), $path, $agent, $bytes, $outcome
		[System.IO.File]::AppendAllText($Log, $line + "`n")
	}
}

# Closes the media bodies that finished or whose client went away, and logs how much of each was sent
function Complete-Streams {
	for ($i = $streams.Count - 1; $i -ge 0; $i--) {
		$s = $streams[$i]
		if (-not $s.Task.IsCompleted) { continue }
		# one stream's trouble (a log the antivirus holds, say) must not end the server
		try {
			$read = $s.File.Position - $s.Start
			$outcome = if ($s.Task.Status -eq 'RanToCompletion') { 'complete' } else { 'closed by the client' }
			try {
				if ($outcome -eq 'complete') { $s.Response.Close() } else { $s.Response.Abort() }
			} catch { }
			Write-Request $s.Path $s.Agent $read $outcome
		} catch {
			Write-Host "  $($_.Exception.Message)"
		} finally {
			$s.File.Dispose()
			$streams.RemoveAt($i)
		}
	}
}

function Get-Gzip([long]$size) {
	$memory = New-Object System.IO.MemoryStream
	$gzip = New-Object System.IO.Compression.GZipStream($memory, [System.IO.Compression.CompressionMode]::Compress)
	# a 64 KB block of "#a" comment lines written again and again: a few hundred writes, not millions
	$block = [System.Text.Encoding]::ASCII.GetBytes(("#a`n" * 21846).Substring(0, 65536))
	$written = 0L
	while ($written + $block.Length -le $size) {
		$gzip.Write($block, 0, $block.Length)
		$written += $block.Length
	}
	$rest = $size - $written
	if ($rest -gt 0) {
		$gzip.Write($block, 0, [int]$rest)
	}
	$gzip.Close()
	return $memory.ToArray()
}

function Send-Bytes($response, [byte[]]$bytes, [string]$type) {
	if ($type) { $response.ContentType = $type }
	$response.ContentLength64 = $bytes.Length
	$response.OutputStream.Write($bytes, 0, $bytes.Length)
	$response.OutputStream.Close()
}

function Get-Type([string]$name) {
	switch ([System.IO.Path]::GetExtension($name).ToLowerInvariant()) {
		'.m3u' { 'audio/x-mpegurl' }
		'.m3u8' { 'application/vnd.apple.mpegurl' }
		'.pls' { 'audio/x-scpls' }
		'.rss' { 'application/rss+xml' }
		default { 'application/octet-stream' }
	}
}

Write-Host 'Building the gzip bodies...'
$gzipAtCap = Get-Gzip $cap
$gzipOverCap = Get-Gzip ($cap + 1)

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "Serving on http://localhost:$Port/ (Ctrl+C to stop)"

try {
	while ($listener.IsListening) {
		# polled, as a blocking GetContext() would not let Ctrl+C through until a request came
		$pending = $listener.GetContextAsync()
		while (-not $pending.AsyncWaitHandle.WaitOne(250)) { try { Complete-Streams } catch { } }
		try { Complete-Streams } catch { }
		$context = $pending.GetAwaiter().GetResult()
		$path = $context.Request.Url.AbsolutePath
		$agent = [string]$context.Request.UserAgent
		$response = $context.Response
		Write-Host ("{0:HH:mm:ss} {1}" -f (Get-Date), $path)
		$mediaFile = $null
		try {
			Write-Request $path $agent 0 'started'
			if ($path -match '^/media/(\d+)$' -and [int]$Matches[1] -ge 1 -and [int]$Matches[1] -le $Media.Count) {
				$mediaFile = [System.IO.File]::Open($Media[[int]$Matches[1] - 1], 'Open', 'Read', 'ReadWrite, Delete')
				$length = $mediaFile.Length
				$start = 0L
				# bytes=N- (what FFmpeg asks for); a bounded range is answered to the end of the file, which FFmpeg accepts
				if ($context.Request.Headers['Range'] -match '^bytes=(\d+)-') {
					$start = [long]$Matches[1]
				}
				$response.AddHeader('Accept-Ranges', 'bytes')
				$response.ContentType = 'video/mp4'
				if ($start -ge $length -and $length -gt 0) {
					$mediaFile.Dispose()
					$mediaFile = $null
					$response.StatusCode = 416
					$response.AddHeader('Content-Range', "bytes */$length")
					$response.Close()
					Write-Request $path $agent 0 '416'
				} else {
					if ($start -gt 0) {
						$response.StatusCode = 206
						$response.AddHeader('Content-Range', "bytes $start-$($length - 1)/$length")
					}
					$response.ContentLength64 = $length - $start
					[void]$mediaFile.Seek($start, 'Begin')
					$task = $mediaFile.CopyToAsync($response.OutputStream, 65536)
					[void]$streams.Add(@{ Task = $task; File = $mediaFile; Response = $response; Start = $start; Path = $path; Agent = $agent })
					# Complete-Streams closes it from here on
					$mediaFile = $null
				}
				continue
			}
			if ($path -match '^/switchable/(.+)$') {
				$name = [System.IO.Path]::GetFileName($Matches[1])
				$statusFile = Join-Path $fixtures "$name.status"
				$file = Join-Path $fixtures $name
				if (Test-Path -LiteralPath $statusFile) {
					$response.StatusCode = [int]([System.IO.File]::ReadAllText($statusFile).Trim())
					$response.Close()
				} elseif (Test-Path -LiteralPath $file) {
					Send-Bytes $response ([System.IO.File]::ReadAllBytes($file)) (Get-Type $name)
				} else {
					$response.StatusCode = 404
					$response.Close()
				}
			} elseif ($path -like '/fixtures/*') {
				$name = [System.IO.Path]::GetFileName($path.Substring(10))
				$file = Join-Path $fixtures $name
				if (Test-Path -LiteralPath $file) {
					Send-Bytes $response ([System.IO.File]::ReadAllBytes($file)) (Get-Type $name)
				} else {
					$response.StatusCode = 404
					$response.Close()
				}
			} elseif ($path -eq '/extensionless-m3u') {
				Send-Bytes $response ([System.IO.File]::ReadAllBytes((Join-Path $fixtures 'relative.m3u'))) 'audio/x-mpegurl'
			} elseif ($path -eq '/extensionless-pls') {
				Send-Bytes $response ([System.IO.File]::ReadAllBytes((Join-Path $fixtures 'radio.pls'))) 'audio/x-scpls'
			} elseif ($path -eq '/sniff-m3u') {
				Send-Bytes $response ([System.IO.File]::ReadAllBytes((Join-Path $fixtures 'vlc.m3u'))) 'text/plain'
			} elseif ($path -eq '/html') {
				$page = '<!doctype html><html><head><title>Not an index</title></head><body><p>Hello.</p></body></html>'
				Send-Bytes $response ([System.Text.Encoding]::UTF8.GetBytes($page)) 'text/html; charset=utf-8'
			} elseif ($path -match '^/redirect/(\d+)$') {
				$n = [int]$Matches[1]
				if ($n -gt 0) {
					$response.StatusCode = 302
					$response.RedirectLocation = "/redirect/$($n - 1)"
					$response.Close()
				} else {
					# /redirect/0 is the list itself, so /redirect/<n> takes exactly <n> redirects
					Send-Bytes $response ([System.IO.File]::ReadAllBytes((Join-Path $fixtures 'vlc.m3u'))) 'audio/x-mpegurl'
				}
			} elseif ($path -match '^/status/(\d+)$') {
				$response.StatusCode = [int]$Matches[1]
				$response.Close()
			} elseif ($path -eq '/hang') {
				# never answered; the next request is served only after this one is dropped
				Start-Sleep -Seconds 120
				$response.Abort()
			} elseif ($path -eq '/hang-body') {
				$response.ContentLength64 = 1000
				$bytes = [System.Text.Encoding]::ASCII.GetBytes('0123456789')
				$response.OutputStream.Write($bytes, 0, $bytes.Length)
				$response.OutputStream.Flush()
				Start-Sleep -Seconds 120
				$response.Abort()
			} elseif ($path -eq '/big') {
				$bytes = New-Object byte[] (17 * 1024 * 1024)
				Send-Bytes $response $bytes 'audio/x-mpegurl'
			} elseif ($path -eq '/gzip-at-cap' -or $path -eq '/gzip-over-cap') {
				$response.AddHeader('Content-Encoding', 'gzip')
				$body = if ($path -eq '/gzip-at-cap') { $gzipAtCap } else { $gzipOverCap }
				Send-Bytes $response $body 'audio/x-mpegurl'
			} elseif ($path -eq '/slow') {
				$bytes = [System.IO.File]::ReadAllBytes((Join-Path $fixtures 'vlc.m3u'))
				$response.ContentType = 'audio/x-mpegurl'
				$response.ContentLength64 = $bytes.Length
				foreach ($b in $bytes) {
					$response.OutputStream.WriteByte($b)
					$response.OutputStream.Flush()
					Start-Sleep -Milliseconds 100
				}
				$response.OutputStream.Close()
			} else {
				$response.StatusCode = 404
				$response.Close()
			}
		} catch {
			Write-Host "  $($_.Exception.Message)"
			try { $response.Abort() } catch { }
			if ($mediaFile) { $mediaFile.Dispose() }
		}
	}
} finally {
	foreach ($s in $streams) {
		try { $s.Response.Abort() } catch { }
		$s.File.Dispose()
	}
	$listener.Stop()
	$listener.Close()
}
