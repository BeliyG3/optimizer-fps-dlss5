# Releasing "Optimizer FPS for DLSS5"

This is the release procedure. Local packaging and validation do not publish
anything or install into a live game. Publication of 2026.9.1 and game rollout
follow the separate release acceptance work. `.github/workflows/release.yml`
publishes only when a matching tag is pushed or an existing tag is dispatched.

## The two version numbers

`cmake/Version.cmake` holds both, and they move independently:

| Variable | What it is | When it moves |
|----------|------------|---------------|
| `OFPS_RELEASE_VERSION` | The user-facing release number of the add-on: the year and the month it comes out, plus a third component for a second release inside one month (`2026.9`, then `2026.9.1`). Releases up to `26.29` were numbered after the work stage instead. This is what the tag mirrors: tag `vX.Y[.Z]` ⇔ `OFPS_RELEASE_VERSION "X.Y[.Z]"`. | Every shipped build. |
| `OFPS_SDK_VERSION` | The semantic version of the Optimizer FPS core library — the CMake `project(... VERSION ...)`, the installed package version, the ABI story. | Only when the public API/ABI changes. |

Everything downstream reads `OFPS_RELEASE_VERSION` rather than carrying its own copy:
`tools\Package-Release.ps1` (when `-Version` is omitted), the release workflow's tag check, the
generated `ofps_version.h` and `NOTICE`, and the banner line in the add-on's tab.

> **The add-on's exported NAME is deliberately version-free.** ReShade keys its `DisabledAddons`
> list on the exported NAME, so a version number inside the name would silently re-enable the
> add-on for every user who had turned it off, on every single release. The release number lives in
> DESCRIPTION and in the overlay banner instead. Do not "helpfully" put the version back into NAME
> when bumping — a release must never reset a user's DisabledAddons choice.

## Checklist

### 1. Write the changelog section

Add a `## X.Y[.Z]` section at the top of `CHANGELOG.md` with the notes for the release. This is the
**source of the release body** — `release.yml` copies the section verbatim, from the `## X.Y[.Z]`
heading down to the next `## ` heading, into `release-notes.md`. Write it for users of the mod, not
for the diff.

### 2. Bump `cmake/Version.cmake`

Set `OFPS_RELEASE_VERSION` to `X.Y[.Z]` — the month without a leading zero, so `2026.9`, never `2026.09`, because CMake compares the CHANGELOG heading with it character for character (and `OFPS_SDK_VERSION` too, if the public API/ABI moved).

Order matters: CMake **refuses to configure** when `CHANGELOG.md` has no line reading exactly
`## <OFPS_RELEASE_VERSION>`. So bumping the version before writing the changelog breaks every local
build until the section exists — write the changelog first.

### 3. Push to `master` and wait for `build.yml` to go green

`build.yml` builds five presets (`windows-x64`, `windows-x64-mt`,
`windows-x86`, `windows-x86-remote`, `sdk-only-x64`) on `windows-2022` with
warnings as errors. It inventories and runs three CTest presets (currently
26/13/10 tests), checks ABI and core includes, checks file size and PowerShell
5.1 compatibility, verifies dependencies, packages and validates the zip, and
runs installer self-tests. Do not tag on red.

### 4. Package locally

From the repository root, run the five configure/build pairs and three CTest
presets in [BUILD.md](../BUILD.md) and [CI.md](CI.md). Run the same guards and
installer self-tests as CI before packaging. On a maintainer machine, invoke
CMake and CTest through the VS 2022 `vcvars64.bat` command shown in `CI.md`.

Build a local zip in a fresh temporary directory:

```powershell
$out = Join-Path ([IO.Path]::GetTempPath()) ('ofps-release-' + [Guid]::NewGuid().ToString('N'))
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Package-Release.ps1 -Out $out -Force
$version = ([regex]::Match((Get-Content cmake\Version.cmake -Raw), 'OFPS_RELEASE_VERSION\s+"([^"]+)"')).Groups[1].Value
$stage = Join-Path $out ('Optimizer-FPS-for-DLSS5-' + $version)
$zip = Join-Path $out ('Optimizer-FPS-for-DLSS5-' + $version + '.zip')
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Test-ReleasePackage.ps1 -Zip $zip
```

`-Version` is intentionally omitted: the script reads `OFPS_RELEASE_VERSION` out of
`cmake\Version.cmake`, exactly as CI does, so a local zip and the published one cannot disagree.
The script prints the zip path and its SHA-256 — **write the SHA-256 down**, you will compare it
against the one CI publishes in step 7.

Output: `$zip` (installer `.cmd`/`.ps1`, `Verify-OptimizerFPS.ps1`, all
`installer/*.psm1`, `README.txt` generated from `docs\INSTALL.md` before
`README-END`, notices and licenses, and `payload\` with `VERSION.txt` and
`files.sha256`). The payload includes the x64 core DLL and every built DXBC.

Run the package self-tests with the real ReShade DLL fixtures and the unpacked
payload. The tests install only into temporary copies:

```powershell
# A real previous release for the Update/migration cases (26.28); CI uses the current build instead.
$old = Join-Path $env:TEMP 'ofps-old-26.28'
Expand-Archive -LiteralPath out\background-clean-pre-update26.28.zip -DestinationPath $old -Force
$oldAddon = Get-ChildItem -LiteralPath $old -Recurse -Filter optimizer-fps-dlss5.addon64 | Select-Object -First 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\installer-tests\Run-InstallerTests.ps1 `
    -Payload (Join-Path $stage 'payload') -Zip $zip `
    -ReShade64 tools\bench12\run_addon\dxgi.dll -ReShade32 tools\bench9\run\dxgi.dll `
    -OldAddon64 $oldAddon.FullName
```

Expect 0 failed and 0 skipped. The archive is a local file, not in the repository; SHA-256 of the
archive `D99622728DA268E30E47435ECA7B26B70CF88237DAF9BDD8481AF34837FC345E`, of the 26.28
`optimizer-fps-dlss5.addon64` inside it `8A6DD1FBAAE3ED632D85011DC78736DAD167E4498588E4C19AF952F5BF34D695`.

For a check using a temporary copy of the x64 bench executable and ReShade DLL,
create a test directory, install from the staged zip, then run Verify. Do not
launch the copied executable:

```powershell
$gameCopy = Join-Path $out 'verify-x64'
New-Item -ItemType Directory -Path $gameCopy | Out-Null
$gameExe = Join-Path $gameCopy 'pw_bench12.exe'
Copy-Item tools\bench12\pw_bench12.exe $gameExe
Copy-Item tools\bench12\run_addon\dxgi.dll (Join-Path $gameCopy 'dxgi.dll')
$installer = Join-Path $stage 'Install-OptimizerFPS.ps1'
$verifier = Join-Path $stage 'Verify-OptimizerFPS.ps1'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer -GameExe $gameExe `
    -Payload (Join-Path $stage 'payload') -Mode Install -Yes -NoPause -NoVerify
powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier -GameExe $gameExe `
    -Payload (Join-Path $stage 'payload') -Json
```

Before a new runtime log exists, exit code 10 is expected if static checks pass.
Do not use this command on a live game as part of the package gate.

### 5. Later release acceptance: two games, both architectures

Do this only as part of the separate release acceptance work, after the local
package gate. Unzip the release **somewhere clean** (not the build tree) and run
the installer on approved game installations:

* **one x64 game** — a renodx-style ReShade install, where the add-on goes beside the game's own
  ReShade DLL;
* **one x86 game** — a DLSS5-Feeder game, where the add-on goes into `<game>\host64` and only the
  small remote tab goes beside the 32-bit ReShade DLL. This is the path that the x64 test does not
  cover at all.

For each:

```
Install-OptimizerFPS.cmd "D:\Games\<game>\<game>.exe"
```

then start the game, load into an actual scene (the add-on needs the host to create an NGX feature
before anything is warped), quit, and check:

```powershell
.\Verify-OptimizerFPS.ps1 -GameExe "D:\Games\<game>\<game>.exe"
```

It must report **frames are being warped** (`Frames are being warped: the model runs on WxH, N% of
the native pixels.`) — `No warped frame in the log yet.` means the run did not exercise the hook,
not that it passed. Verify also re-checks the installed files against `payload\files.sha256`.

Then confirm the uninstall is clean:

```powershell
.\Install-OptimizerFPS.ps1 -GameExe "D:\Games\<game>\<game>.exe" -Mode Uninstall
```

The game folder must come back to exactly what it was: the add-on, core DLL,
forwarder, and shaders gone, the installer-owned `[OptimizerFPS]` keys removed,
and nothing the installer did not put
there touched. Compare against a directory listing taken before the install if in doubt.

### 6. Tag and push

```
git tag vX.Y[.Z]
git push origin refs/tags/vX.Y[.Z]
```

The tag must be `v` + `OFPS_RELEASE_VERSION`, character for character. `release.yml` fails the run
with an explicit tag/version mismatch message otherwise, before it builds anything.

`release.yml` then, on `windows-2022`: checks the tag against `cmake/Version.cmake`,
runs the same five builds, three CTest suites, guards, lint, dependency check,
package validation, and installer self-tests as `build.yml`; it extracts the
changelog section and creates a **draft** GitHub Release with the zip and a `.zip.sha256`
sidecar attached. A tag containing a `-` (`v26.27-rc1`) is marked as a **prerelease**. Review the
draft (assets, `payload/VERSION.txt`, the body), then publish it by hand:

```powershell
gh release edit vX.Y[.Z] -R BeliyG3/optimizer-fps-dlss5 --draft=false
```

`workflow_dispatch` with a `tag` input re-runs the same thing against an existing tag — useful when
a run failed on infrastructure rather than on the code.

### 7. Verify what was actually published

Download the zip **from the release page** (not your local one), and:

* compare its SHA-256 with the one in the release body and in the attached `.zip.sha256`, and with
  the hash `Package-Release.ps1` printed locally in step 4 — the builds are not bit-identical in
  general, so treat a mismatch between the local and the CI zip as expected only if you can explain
  it; the release body hash and the sidecar hash must always match the downloaded file;
* unzip it and re-run `Verify-OptimizerFPS.ps1` against a game installed from *that* zip, so the
  published `payload\files.sha256` is the one being checked.

## Yanking a release

If a published release turns out to be broken:

1. Delete the GitHub Release (this does not delete the tag).
2. Delete the tag, locally and on the remote:
   ```
   git tag -d vX.Y[.Z]
   git push origin :refs/tags/vX.Y[.Z]
   ```
3. **Bump to a new version rather than re-using a number.** Fix the bug, add a section for the next
   number to `CHANGELOG.md` saying what was wrong with the withdrawn one, bump `OFPS_RELEASE_VERSION`,
   and run the
   checklist from the top. Re-pushing a tag that people may already have fetched, or re-publishing
   a different zip under the same version, makes `payload\VERSION.txt` and any bug report quoting it
   meaningless.

The zip is also kept as a workflow artifact on the release run (`optimizer-fps-for-dlss5-X.Y[.Z]`), so a
yanked build can still be fetched for post-mortem after the release itself is gone.

Users who already installed the yanked version are covered by the normal uninstall path
(`Install-OptimizerFPS.ps1 -Mode Uninstall`), and by the fact that the add-on's NAME is stable: an
update over the top of a bad release keeps their ReShade `DisabledAddons` choice intact.
