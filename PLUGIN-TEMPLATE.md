This project is based on https://github.com/obsproject/obs-plugintemplate

Last updated to c7bc4cc3cf274b5f4a965730099b9cbba2229464

To update to a newer template commit:

```
git fetch plugintemplate master
git cherry-pick <last-commit>..<new_commit>
```

This will not include <last-commit>.

Skipped 941b0f26949a2850e38c4630fce44eac4d49f054

## Deliberate divergences from the template (obs-playlister)

Keep these when syncing a newer template commit; the template (master, 2025-12-09) still targets OBS 31
and the legacy install layout.

- OBS 33 plugin layout: `cmake/windows/helpers.cmake` installs `<name>/<name>.dll` (and `.pdb`) next to
  `<name>/data/`; no `bin/64bit` (OBS 33 flags that as Legacy). The rundir mirrors the same layout.
- Module metadata: `cmake/common/manifest.json.in` is configured and installed as `data/manifest.json` from
  `set_target_properties_plugin`; `bootstrap.cmake` also parses `displayName` (`PLUGIN_DISPLAY_NAME`).
- Nested OBS configure (`cmake/common/buildspec_common.cmake`): arguments are a CMake list, one element per
  argument (the platform lists sit in a `# gersemi: off` block so `-G`/`-A` stay next to their values);
  `-DENABLE_FRONTEND:BOOL=OFF` and `-DENABLE_SCRIPTING:BOOL=OFF`, passed on every platform, replace the ignored
  `ENABLE_UI` and the misspelled `CMAKE_ENABLE_SCRIPTING`; `CMAKE_PREFIX_PATH` is passed without literal quotes. The
  macOS branch was converted the same way but is untested (macOS is not a target).
- `cmake/common/bootstrap.cmake` registers `buildspec.json` as a configure dependency, so a version bump reaches
  `data/manifest.json` and the version resource without a manual reconfigure.
- Default install prefix (`cmake/windows/defaults.cmake`): `%ALLUSERSPROFILE%/obs-studio/plugins`, as in template
  commit 013ffca.
- `CMakePresets.json`: Windows SDK `10.0.26100.0`.
- `buildspec.json`: OBS 33.0.0 and obs-deps/Qt6 2026-10-23, Windows hashes only.
- CI: macOS and Ubuntu jobs disabled with `if: false` and removed from the release job; Windows runner
  `windows-2025`; ctest runs after the build; portable zip uses the OBS 33 `plugins/<name>/` layout.
- `cmake/windows/resources/resource.rc.in`: the version resource keeps the original author's copyright.
- Tests: `tests/` (CTest) is added by the root `CMakeLists.txt` behind `OBS_PLAYLISTER_BUILD_TESTS`.
