# shellcheck shell=bash
# obs-playlister
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Whether a harness run passed, decided from OBS's log. Sourced by run-harness.sh; test-harness-verdict.sh runs it
# against made-up logs, so each way of failing is known to be caught without starting OBS.
#
#   harness_verdict <log> <script> <finished: true|false> <killed: true|false>
#
# prints what it found and returns 0 only when all of this holds:
# - the script reached its end ("HARNESS DONE" in the log);
# - no check failed, and as many passed as were expected: EXPECTED_CHECKS when set, else the number the script itself
#   logs with DONE ("HARNESS DONE checks=<run> expected=<n>"; playlister-harness.lua counts the checks declared in
#   its own text, so the number cannot go stale). A script that logs no number must pass at least one check. Without
#   this a step that stops scheduling its checks still passed;
# - OBS exited by itself when asked to. killed is true when it had to be ended: a hang on exit is the plugin's worst
#   failure (a worker or player thread that is never joined), and it used to pass;
# - OBS reported "Number of memory leaks: 0", for every script. A missing line counts as a leak: OBS writes it only
#   on a clean exit;
# - the script's own log checks (folder rows never loaded as files, the corrupt files loaded as often as they should
#   be, out-of-range selections ignored).

harness_verdict() {
	local log="$1" script="$2" finished="$3" killed="$4"
	local failed=false
	local passes fails
	passes="$(grep -c "HARNESS PASS" "${log}" || true)"
	fails="$(grep -c "HARNESS FAIL" "${log}" || true)"

	if [[ "${finished}" != true ]]; then
		echo "FAIL the harness did not reach its end (no HARNESS DONE in the log)"
		failed=true
	fi
	[[ "${fails}" -eq 0 ]] || failed=true

	local expected="${EXPECTED_CHECKS:-}"
	if [[ -z "${expected}" ]]; then
		expected="$(grep -oE "HARNESS DONE checks=[0-9]+ expected=[0-9]+" "${log}" | head -n 1 |
			sed -E 's/.*expected=//' || true)"
	fi
	if [[ -n "${expected}" ]]; then
		if [[ "${passes}" -ne "${expected}" ]]; then
			echo "FAIL ${passes} checks passed, ${expected} were expected"
			failed=true
		fi
	elif [[ "${passes}" -eq 0 ]]; then
		echo "FAIL no check passed"
		failed=true
	else
		echo "note: ${script} does not say how many checks it has; ${passes} passed"
	fi

	# the empty, absent and linked folders are never loaded themselves (a file inside the link is), so such a load
	# means a folder row was taken for a file
	local folder_pattern="\[obs-playlister\] playing .*playlister-harness-(empty|gone)-"
	folder_pattern+="|\[obs-playlister\] playing .*playlister-harness-linked-[0-9]+\.mp4$"
	local folder_loads
	folder_loads="$(grep -cE "${folder_pattern}" "${log}" || true)"
	if [[ "${folder_loads}" -gt 0 ]]; then
		echo "FAIL a folder row was loaded as a file (${folder_loads} loads)"
		failed=true
	fi
	if [[ "${script}" == playlister-harness.lua ]]; then
		# R-BEH-5: the corrupt file is loaded once by each failing source and once more by Play, the corrupt still
		# once
		local corrupt_loads still_loads ignored_selects
		corrupt_loads="$(grep -c "\[obs-playlister\] playing .*playlister-harness-corrupt\.mp4" "${log}" || true)"
		still_loads="$(grep -c "\[obs-playlister\] playing .*playlister-harness-corrupt\.jpg" "${log}" || true)"
		if [[ "${corrupt_loads}" -ne 3 ]]; then
			echo "FAIL the corrupt file was loaded ${corrupt_loads} times, not 3"
			failed=true
		fi
		if [[ "${still_loads}" -ne 1 ]]; then
			echo "FAIL the corrupt still was loaded ${still_loads} times, not once"
			failed=true
		fi
		# R-UP-5: select_index with a row past the end and with -1 is ignored, and each says so in the log
		ignored_selects="$(grep -cE "select_index: .*out of range.*ignored" "${log}" || true)"
		if [[ "${ignored_selects}" -lt 2 ]]; then
			echo "FAIL select_index out of range was logged ${ignored_selects} times, not at least twice"
			failed=true
		fi
	fi

	if [[ "${killed}" == true ]]; then
		echo "FAIL OBS did not exit within 30 s of being asked to, and was ended (a hang on exit)"
		failed=true
	fi
	if ! grep -qE "Number of memory leaks: 0[[:space:]]*$" "${log}"; then
		local leak_line
		leak_line="$(grep -E "Number of memory leaks" "${log}" | tail -n 1 || true)"
		echo "FAIL OBS did not report 0 memory leaks (${leak_line:-no leak count in the log: not a clean exit})"
		failed=true
	fi

	echo "summary: ${passes} passed, ${fails} failed"
	[[ "${failed}" == false ]]
}
