# Releasing "Optimizer FPS for DLSS5"

Cutting a release is: write the changelog, bump one number, push, let CI go green, package and
smoke-test locally, then push the tag. `.github/workflows/release.yml` does the publishing; it
never decides *what* the version is, it only checks that the tag agrees with the repository.

## The two version numbers

`cmake/Version.cmake` holds both, and they move independently:

| Variable | What it is | When it moves |
|----------|------------|---------------|
| `PW_RELEASE_VERSION` | The user-facing release number of the add-on (the `26.x` line the CHANGELOG is written in). This is what the tag mirrors: tag `vX.Y` ⇔ `PW_RELEASE_VERSION "X.Y"`. | Every shipped build. |
| `PW_SDK_VERSION` | The semantic version of the Optimizer FPS core library — the CMake `project(... VERSION ...)`, the installed package version, the ABI story. | Only when the public API/ABI changes. |

Everything downstream reads `PW_RELEASE_VERSION` rather than carrying its own copy:
`tools\Package-Release.ps1` (when `-Version` is omitted), the release workflow's tag check, the
generated `pw_version.h` and `NOTICE`, and the banner line in the add-on's tab.

> **The add-on's exported NAME is deliberately version-free.** ReShade keys its `DisabledAddons`
> list on the exported NAME, so a version number inside the name would silently re-enable the
> add-on for every user who had turned it off, on every single release. The release number lives in
> DESCRIPTION and in the overlay banner instead. Do not "helpfully" put the version back into NAME
> when bumping — a release must never reset a user's DisabledAddons choice.

## Checklist

### 1. Write the changelog section

Add a `## X.Y` section at the top of `CHANGELOG.md` with the notes for the release. This is the
**source of the release body** — `release.yml` copies the section verbatim, from the `## X.Y`
heading down to the next `## ` heading, into `release-notes.md`. Write it for users of the mod, not
for the diff.

### 2. Bump `cmake/Version.cmake`

Set `PW_RELEASE_VERSION` to `X.Y` (and `PW_SDK_VERSION` too, if the public API/ABI moved).

Order matters: CMake **refuses to configure** when `CHANGELOG.md` has no line reading exactly
`## <PW_RELEASE_VERSION>`. So bumping the version before writing the changelog breaks every local
build until the section exists — write the changelog first.

### 3. Push to `master` and wait for `build.yml` to go green

`build.yml` builds all four presets (`windows-x64` + ctest, `windows-x64-mt`, `windows-x86-remote`,
`windows-x86` + ctest) and verifies `external\` against `DEPENDENCIES.lock.json`. Do not tag on red.

### 4. Package locally

```powershell
cmake --preset windows-x64-mt
cmake --build --preset windows-x64-mt-release --parallel
cmake --preset windows-x86-remote
cmake --build --preset windows-x86-remote-release --parallel

powershell.exe -NoProfile -File tools\Package-Release.ps1 `
    -BuildDirX64 out\build\x64-mt -BuildDirX86 out\build\x86-remote -Out dist-release
```

`-Version` is intentionally omitted: the script reads `PW_RELEASE_VERSION` out of
`cmake\Version.cmake`, exactly as CI does, so a local zip and the published one cannot disagree.
The script prints the zip path and its SHA-256 — **write the SHA-256 down**, you will compare it
against the one CI publishes in step 7.

Output: `dist-release\Optimizer-FPS-for-DLSS5-X.Y.zip` (installer `.cmd`/`.ps1`, `Verify-OptimizerFPS.ps1`,
`README.txt` generated from `docs\INSTALL.md`, `LICENSE`, `NOTICE`, `THIRD-PARTY-LICENSES.txt`, and
`payload\` with `VERSION.txt` + `files.sha256`).

### 5. Manual smoke test of the zip — two games, both architectures

CI proves it compiles. It cannot prove it installs. Unzip the release **somewhere clean** (not the
build tree) and run the real installer on:

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

The game folder must come back to exactly what it was: the add-on, the forwarder and the shaders
gone, the `[PeripheralWarp]` block removed from `ReShade.ini`, and nothing the installer did not put
there touched. Compare against a directory listing taken before the install if in doubt.

### 6. Tag and push

```
git tag vX.Y
git push --tags
```

The tag must be `v` + `PW_RELEASE_VERSION`, character for character. `release.yml` fails the run
with an explicit tag/version mismatch message otherwise, before it builds anything.

`release.yml` then, on `windows-latest`: checks the tag against `cmake/Version.cmake`, lints the
PowerShell 5.1 compatibility of the shipped scripts, restores the `external\` cache, builds
`windows-x64` and runs ctest as a gate, builds `windows-x64-mt` and `windows-x86-remote`, verifies
the dependency lock, runs `tools\Package-Release.ps1`, extracts the changelog section, and publishes
a GitHub Release with the zip and a `.zip.sha256` sidecar attached. A tag containing a `-`
(`v26.27-rc1`) is published as a **prerelease**.

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
   git tag -d vX.Y
   git push origin :refs/tags/vX.Y
   ```
3. **Bump to a new version rather than re-using `X.Y`.** Fix the bug, add a `## X.Y+1` section to
   `CHANGELOG.md` that says what was wrong with `X.Y`, bump `PW_RELEASE_VERSION`, and run the
   checklist from the top. Re-pushing a tag that people may already have fetched, or re-publishing
   a different zip under the same version, makes `payload\VERSION.txt` and any bug report quoting it
   meaningless.

The zip is also kept as a workflow artifact on the release run (`optimizer-fps-for-dlss5-X.Y`), so a
yanked build can still be fetched for post-mortem after the release itself is gone.

Users who already installed the yanked version are covered by the normal uninstall path
(`Install-OptimizerFPS.ps1 -Mode Uninstall`), and by the fact that the add-on's NAME is stable: an
update over the top of a bad release keeps their ReShade `DisabledAddons` choice intact.
