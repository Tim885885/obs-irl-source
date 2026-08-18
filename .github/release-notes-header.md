## Installation

One archive per platform, and it works on OBS 32.1 and newer. The plugin bundles its own FFmpeg and libsrt, so there is no longer a separate download per OBS version.

### Windows

1. Close OBS.
2. Extract the zip into your OBS Studio install folder (usually `C:\Program Files\obs-studio`). The DLLs land in `obs-plugins\64bit`.
3. Start OBS.

### macOS (Apple Silicon)

1. Close OBS.
2. Extract the zip into `~/Library/Application Support/obs-studio/plugins/`. It contains an `obs-irl-source.plugin` bundle; if an `obs-irl-source` folder from an older release is still there, delete it (that flat layout is invisible to OBS on macOS).
3. The binary is not signed or notarized, so clear the quarantine flag once:

   ```
   xattr -dr com.apple.quarantine "$HOME/Library/Application Support/obs-studio/plugins/obs-irl-source.plugin"
   ```

4. Start OBS.

### Linux

1. Close OBS.
2. Extract the tarball into `~/.config/obs-studio/plugins/`.
3. Start OBS.

The build bundles its own media stack but still links your distribution's libobs, and it is built against Ubuntu's. On other distributions, or if OBS does not load it, build from source instead (see the README).

Verify downloads against `sha256sums.txt`.

---

