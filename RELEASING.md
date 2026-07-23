# Releasing

Releases are tag driven. Pushing a tag `vX.Y.Z` runs `.github/workflows/release.yml`, which builds every platform through the regular build workflow, packages the artifacts into install ready archives, and creates a draft GitHub release. Nothing goes public until the draft is published by hand, which matches the policy that tagged releases are fully tested.

## Steps

1. Bump the version in `CMakeLists.txt` (the `project(obs-irl-source VERSION X.Y.Z ...)` line). The release workflow fails early if the tag and this version disagree.

2. Commit the bump:

   ```
   git commit -am "chore(obs-irl-source): bump version to X.Y.Z"
   ```

3. Tag and push:

   ```
   git tag vX.Y.Z
   git push origin master vX.Y.Z
   ```

4. Wait for the Release workflow to finish. It creates a draft release containing:

   * `obs-irl-source-X.Y.Z-linux-x64.tar.gz` (extract into `~/.config/obs-studio/plugins/`)
   * `obs-irl-source-X.Y.Z-windows-x64.zip` (extract into the OBS install folder)
   * `obs-irl-source-X.Y.Z-macos-arm64.zip` (extract into `~/Library/Application Support/obs-studio/plugins/`)
   * `sha256sums.txt`

   The release body starts with install instructions (from `.github/release-notes-header.md`) followed by notes generated from the commit history.

5. Download and test every artifact on a real OBS install before publishing. At minimum: the plugin loads, a source connects, audio and video play.

6. Edit the generated notes into a proper changelog, then publish the draft.

## Fixing a bad tag

If the version check fails or an artifact is broken, delete the draft release in the GitHub UI, fix the problem, then move the tag:

```
git tag -f vX.Y.Z
git push -f origin vX.Y.Z
```

## Supported OBS lines

One archive per platform covers every supported OBS line. The plugin bundles its own media stack, so the only version sensitive link left is libobs, and libobs gates a plugin on `(major, minor) <= host`. Building against the oldest supported line therefore produces a binary that also loads on every newer one.

`OBS_VERSION` at the top of `.github/workflows/build.yml` pins that oldest line. Raise it only to drop support for older OBS releases, never to chase a newer one. See the CI section in `CLAUDE.md`.

## Not automated (yet)

* macOS signing and notarization. The zip ships unsigned, and the release notes tell users to clear the quarantine attribute.
* Windows code signing and installers. The zip layout extracts into the OBS install folder.
