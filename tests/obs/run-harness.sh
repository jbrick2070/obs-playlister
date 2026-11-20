#!/usr/bin/env bash
# obs-playlister
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The Linux twin of run-harness.ps1 (PLAN Linux port step 6): runs playlister-harness.lua inside an installed OBS 33
# with the plugin installed, and prints the results with the plugin's log lines. Exit code 0 only when every check
# passed, as many checks ran as the script expects, OBS exited by itself when asked to, and it reported no memory
# leaks (harness-verdict.sh; test-harness-verdict.sh proves each of those failures is caught).
#
# Everything lives in one temporary folder: OBS's whole configuration (XDG_CONFIG_HOME, so nothing of the user's is
# read or written), three test videos made with ffmpeg (different durations, the second over 35 s, as the harness
# needs), the folders the harness watches, and the log. Without a display it starts Xvfb, so it runs in CI; OBS then
# renders with Mesa's software OpenGL.
#
# Usage:
#   tests/obs/run-harness.sh                 # after installing the plugin (sudo cmake --install build)
#   TIMEOUT_SECONDS=600 tests/obs/run-harness.sh
#   HARNESS_LOG_COPY=/path/log.txt tests/obs/run-harness.sh   # keeps OBS's log (the run's folder is removed)
#   EXPECTED_CHECKS=<n> tests/obs/run-harness.sh              # instead of the number the script logs with DONE
# Needs: obs (33 or later, with Lua scripting), ffmpeg, and Xvfb when there is no DISPLAY.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/obs/harness-verdict.sh
source "${repo}/tests/obs/harness-verdict.sh"
script="${1:-playlister-harness.lua}"
timeout_seconds="${TIMEOUT_SECONDS:-300}"
obs_bin="${OBS_BIN:-obs}"

command -v "${obs_bin}" > /dev/null || { echo "OBS not found (${obs_bin})"; exit 2; }
command -v ffmpeg > /dev/null || { echo "ffmpeg not found"; exit 2; }

tmp_root="${TMPDIR:-/tmp}"
tmp_root="${tmp_root%/}" # a trailing "/" would double in the paths the harness compares as text
work="$(mktemp -d "${tmp_root:-/tmp}/playlister-harness-run-XXXXXX")"
obs_pid=""
xvfb_pid=""
cleanup() {
	if [[ -n "${obs_pid}" ]] && kill -0 "${obs_pid}" 2> /dev/null; then
		kill -KILL "${obs_pid}" 2> /dev/null || true
	fi
	if [[ -n "${xvfb_pid}" ]]; then
		kill "${xvfb_pid}" 2> /dev/null || true
	fi
	rm -rf "${work}"
}
trap cleanup EXIT

# ---- test media ----

mkdir -p "${work}/files" "${work}/temp"
make_video() { # name, seconds
	ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=30:duration=$2" \
		-f lavfi -i "sine=frequency=440:duration=$2" -shortest -c:v libx264 -preset ultrafast -crf 30 -pix_fmt yuv420p \
		-c:a aac "${work}/files/$1"
}
make_video a.mp4 30
make_video b.mp4 45
make_video c.mp4 20
files="${work}/files/a.mp4|${work}/files/b.mp4|${work}/files/c.mp4"

export TEMP="${work}/temp"
export PLAYLISTER_HARNESS_FILES="${files}"
export PLAYLISTER_HARNESS_EMPTY_FOLDER="${work}/playlister-harness-empty-$$"
mkdir -p "${PLAYLISTER_HARNESS_EMPTY_FOLDER}"
# never created: a row saved as a folder whose folder is gone (R-LIST-1)
export PLAYLISTER_HARNESS_MISSING_FOLDER="${work}/playlister-harness-gone-$$"
# a folder row of the three files, as hard links named 1.mp4 to 3.mp4
export PLAYLISTER_HARNESS_MEDIA_FOLDER="${work}/playlister-harness-media-$$"
mkdir -p "${PLAYLISTER_HARNESS_MEDIA_FOLDER}"
ln "${work}/files/a.mp4" "${PLAYLISTER_HARNESS_MEDIA_FOLDER}/1.mp4"
ln "${work}/files/b.mp4" "${PLAYLISTER_HARNESS_MEDIA_FOLDER}/2.mp4"
ln "${work}/files/c.mp4" "${PLAYLISTER_HARNESS_MEDIA_FOLDER}/3.mp4"
# the same folder through a symbolic link whose name ends in a media extension (the junction on Windows): only a
# saved folder kind keeps such a row a folder (R-LIST-1)
export PLAYLISTER_HARNESS_MEDIA_JUNCTION="${work}/playlister-harness-linked-$$.mp4"
ln -s "${PLAYLISTER_HARNESS_MEDIA_FOLDER}" "${PLAYLISTER_HARNESS_MEDIA_JUNCTION}"
# a small valid still image for the still hold (R-DIR-9)
export PLAYLISTER_HARNESS_STILL="${work}/playlister-harness-still-$$.jpg"
ffmpeg -loglevel error -y -f lavfi -i "color=c=steelblue:size=320x180" -frames:v 1 "${PLAYLISTER_HARNESS_STILL}"

# ---- OBS's configuration, for this run only ----

export XDG_CONFIG_HOME="${work}/config"
config="${XDG_CONFIG_HOME}/obs-studio"
mkdir -p "${config}/basic/scenes" "${config}/basic/profiles/Harness" "${config}/logs"
# FirstRun skips the auto-configuration wizard; Studio Mode stays off, as a preview scene never plays
for ini in global.ini user.ini; do
	cat > "${config}/${ini}" << EOF
[General]
FirstRun=true
LastVersion=553648128

[Basic]
Profile=Harness
ProfileDir=Harness
SceneCollection=Harness
SceneCollectionFile=Harness

[BasicWindow]
PreviewProgramMode=false
EOF
done
cat > "${config}/basic/profiles/Harness/basic.ini" << EOF
[General]
Name=Harness

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=30

[Audio]
SampleRate=48000
ChannelSetup=Stereo
EOF
cat > "${config}/basic/scenes/Harness.json" << EOF
{
	"name": "Harness",
	"version": 2,
	"current_scene": "Scene",
	"current_program_scene": "Scene",
	"scene_order": [{"name": "Scene"}],
	"sources": [
		{"id": "scene", "versioned_id": "scene", "name": "Scene", "settings": {"items": [], "id_counter": 0}}
	],
	"modules": {"scripts-tool": [{"path": "${repo}/tests/obs/${script}", "settings": {}}]}
}
EOF

# ---- run ----

if [[ -z "${DISPLAY:-}" ]]; then
	command -v Xvfb > /dev/null || { echo "no DISPLAY and no Xvfb"; exit 2; }
	export DISPLAY=":97"
	Xvfb "${DISPLAY}" -screen 0 1920x1080x24 -nolisten tcp > "${work}/xvfb.log" 2>&1 &
	xvfb_pid=$!
	for ((i = 0; i < 20; i++)); do
		[[ -S /tmp/.X11-unix/X97 ]] && break
		kill -0 "${xvfb_pid}" 2> /dev/null || { echo "Xvfb did not start:"; cat "${work}/xvfb.log"; exit 1; }
		sleep 0.5
	done
fi
# no GPU in CI or on a server: Mesa's software OpenGL
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

# --multi also turns off the unclean-shutdown check
"${obs_bin}" --multi > "${work}/obs-stdout.log" 2>&1 &
obs_pid=$!

log=""
done=false
for ((i = 0; i < timeout_seconds; i++)); do
	sleep 1
	kill -0 "${obs_pid}" 2> /dev/null || break
	log="$(ls -t "${config}/logs/"*.txt 2> /dev/null | head -n 1 || true)"
	if [[ -n "${log}" ]] && grep -q "HARNESS DONE" "${log}"; then
		done=true
		break
	fi
done

# a clean exit writes "Number of memory leaks"; OBS quits on SIGINT. An OBS that must be ended instead hangs on exit,
# which fails the run (harness-verdict.sh)
killed=false
if kill -0 "${obs_pid}" 2> /dev/null; then
	kill -INT "${obs_pid}" 2> /dev/null || true
	for ((i = 0; i < 30; i++)); do
		kill -0 "${obs_pid}" 2> /dev/null || break
		sleep 1
	done
	if kill -0 "${obs_pid}" 2> /dev/null; then
		echo "OBS did not exit within 30 s; ending it"
		kill -KILL "${obs_pid}" 2> /dev/null || true
		killed=true
	fi
fi
wait "${obs_pid}" 2> /dev/null || true
obs_pid=""

log="$(ls -t "${config}/logs/"*.txt 2> /dev/null | head -n 1 || true)"
if [[ -z "${log}" ]]; then
	echo "OBS wrote no log; its output:"
	cat "${work}/obs-stdout.log"
	exit 1
fi
echo "harness finished: ${done}; log: ${log}"
if [[ -n "${HARNESS_LOG_COPY:-}" ]]; then
	cp "${log}" "${HARNESS_LOG_COPY}"
	cp "${work}/obs-stdout.log" "${HARNESS_LOG_COPY%.txt}-stdout.txt" 2> /dev/null || true
fi
grep -E "HARNESS|\[obs-playlister\]|memory leaks|[Cc]rash" "${log}" | sed 's/^/  /' || true

harness_verdict "${log}" "${script}" "${done}" "${killed}"
