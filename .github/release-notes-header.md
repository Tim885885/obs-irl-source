## Installation

Each Windows and macOS archive is built for one OBS release line, shown in the file name (for example `obs32.1`). Download the one matching your installed OBS version. A build for one line will not load on the other because they bundle different FFmpeg majors.

### Windows

1. Close OBS.
2. Extract the zip into your OBS Studio install folder (usually `C:\Program Files\obs-studio`). The DLLs land in `obs-plugins\64bit`.
3. Start OBS.

### macOS (Apple Silicon)

1. Close OBS.
2. Extract the zip into `~/Library/Application Support/obs-studio/plugins/`.
3. The binary is not signed or notarized, so clear the quarantine flag once:

   ```
   xattr -dr com.apple.quarantine "$HOME/Library/Application Support/obs-studio/plugins/obs-irl-source"
   ```

4. Start OBS.

### Linux

The binary is built on Ubuntu against the distribution libobs and FFmpeg packages. On other distributions, or if OBS does not load it, build from source instead (see the README).

1. Close OBS.
2. Extract the tarball into `~/.config/obs-studio/plugins/`.
3. Start OBS.

Verify downloads against `sha256sums.txt`.

---

