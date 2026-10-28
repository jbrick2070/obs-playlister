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

param(
	[int]$Port = 8765
)

$ErrorActionPreference = 'Stop'
$fixtures = Join-Path $PSScriptRoot '..\fixtures\playlists'
$cap = 16 * 1024 * 1024

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
		while (-not $pending.AsyncWaitHandle.WaitOne(250)) { }
		$context = $pending.GetAwaiter().GetResult()
		$path = $context.Request.Url.AbsolutePath
		$response = $context.Response
		Write-Host ("{0:HH:mm:ss} {1}" -f (Get-Date), $path)
		try {
			if ($path -like '/fixtures/*') {
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
		}
	}
} finally {
	$listener.Stop()
	$listener.Close()
}
