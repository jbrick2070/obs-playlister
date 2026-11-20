#!/usr/bin/env bash
# obs-playlister
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves that run-harness.sh's verdict (harness-verdict.sh) fails a run for each reason it must, and passes a good
# one, from made-up OBS logs: no OBS is started. Run by ctest where there is a bash (tests/CMakeLists.txt), or by hand:
#   bash tests/obs/test-harness-verdict.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/obs/harness-verdict.sh
source "${here}/harness-verdict.sh"

work="$(mktemp -d "${TMPDIR:-/tmp}/playlister-verdict-test-XXXXXX")"
trap 'rm -rf "${work}"' EXIT
unset EXPECTED_CHECKS

# A log as OBS writes one for playlister-harness.lua: passes, fails, the count the script expects (empty: a script
# that logs none), the leak line ("" for none, as after a kill)
make_log() { # file passes fails expected leak_line
	local file="$1" passes="$2" fails="$3" expected="$4" leak_line="$5"
	{
		echo "10:00:00.000: [obs-playlister] plugin loaded successfully (version 0.0.0)"
		for ((i = 0; i < passes; i++)); do
			echo "10:00:01.000: [playlister-harness.lua] HARNESS PASS check ${i} (detail)"
		done
		for ((i = 0; i < fails; i++)); do
			echo "10:00:01.500: [playlister-harness.lua] HARNESS FAIL check ${i} (detail)"
		done
		for ((i = 0; i < 3; i++)); do
			echo "10:00:02.000: [obs-playlister] playing /tmp/x/playlister-harness-corrupt.mp4"
		done
		echo "10:00:02.100: [obs-playlister] playing /tmp/x/playlister-harness-corrupt.jpg"
		echo "10:00:02.200: [obs-playlister] select_index: row 9 is out of range (3 rows); ignored"
		echo "10:00:02.300: [obs-playlister] select_index: media_index -1 is out of range; ignored"
		if [[ -n "${expected}" ]]; then
			echo "10:00:03.000: [playlister-harness.lua] HARNESS DONE checks=$((passes + fails)) expected=${expected}"
		else
			echo "10:00:03.000: [playlister-harness.lua] HARNESS DONE"
		fi
		if [[ -n "${leak_line}" ]]; then
			echo "10:00:04.000: ${leak_line}"
		fi
	} > "${file}"
}

failures=0
expect() { # what, wanted exit code (0 pass, 1 fail), text the output must hold ("" for any), then the verdict's arguments
	local what="$1" wanted="$2" text="$3"
	shift 3
	local output code=0
	output="$(harness_verdict "$@")" || code=$?
	if [[ "${code}" -ne "${wanted}" ]]; then
		echo "FAIL ${what}: the verdict returned ${code}, not ${wanted}"
		echo "${output}" | sed 's/^/    /'
		failures=$((failures + 1))
	elif [[ -n "${text}" ]] && ! grep -qF "${text}" <<< "${output}"; then
		echo "FAIL ${what}: the verdict does not say \"${text}\""
		echo "${output}" | sed 's/^/    /'
		failures=$((failures + 1))
	else
		echo "ok   ${what}"
	fi
}

log="${work}/log.txt"
script=playlister-harness.lua

make_log "${log}" 5 0 5 "Number of memory leaks: 0"
expect "a clean run passes" 0 "summary: 5 passed, 0 failed" "${log}" "${script}" true false

expect "OBS had to be ended on exit" 1 "did not exit within 30 s" "${log}" "${script}" true true

make_log "${log}" 5 0 5 "Number of memory leaks: 3"
expect "a leak is reported" 1 "did not report 0 memory leaks (10:00:04.000: Number of memory leaks: 3)" \
	"${log}" "${script}" true false

make_log "${log}" 5 0 5 "Number of memory leaks: 10"
expect "ten leaks are not zero leaks" 1 "did not report 0 memory leaks" "${log}" "${script}" true false

make_log "${log}" 5 0 5 ""
expect "no leak count at all (not a clean exit)" 1 "no leak count in the log" "${log}" "${script}" true false

make_log "${log}" 4 0 5 "Number of memory leaks: 0"
expect "fewer checks ran than expected" 1 "4 checks passed, 5 were expected" "${log}" "${script}" true false

make_log "${log}" 4 1 5 "Number of memory leaks: 0"
expect "a check failed" 1 "summary: 4 passed, 1 failed" "${log}" "${script}" true false

make_log "${log}" 5 0 5 "Number of memory leaks: 0"
expect "the script never reached its end" 1 "did not reach its end" "${log}" "${script}" false false

# a script that does not say how many checks it has: any number above none passes, none fails
make_log "${log}" 2 0 "" "Number of memory leaks: 0"
expect "a script without a count passes with some checks" 0 "does not say how many checks" \
	"${log}" phase2-acceptance.lua true false
make_log "${log}" 0 0 "" "Number of memory leaks: 0"
expect "a script without a count fails with no checks" 1 "no check passed" "${log}" phase2-acceptance.lua true false

# EXPECTED_CHECKS overrides the script's number
make_log "${log}" 5 0 5 "Number of memory leaks: 0"
EXPECTED_CHECKS=6 expect "EXPECTED_CHECKS overrides the script's count" 1 "5 checks passed, 6 were expected" \
	"${log}" "${script}" true false

# the log checks the verdict took over from run-harness.sh still work
make_log "${log}" 5 0 5 "Number of memory leaks: 0"
echo "10:00:02.500: [obs-playlister] playing /tmp/x/playlister-harness-empty-123" >> "${log}"
expect "a folder row loaded as a file" 1 "a folder row was loaded as a file" "${log}" "${script}" true false

if [[ "${failures}" -gt 0 ]]; then
	echo "test-harness-verdict: ${failures} failed"
	exit 1
fi
echo "test-harness-verdict: all passed"
