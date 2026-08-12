# CSO Pak Browser

An Explorer-style GUI on top of the original `cso_pak` console tool: folder
tree on the left, live preview (text / image / audio+video / CSV table / raw
properties) on the right, plus pack/patch from the same File menu.

```
project/
├── CMakeLists.txt
├── core/                      ← original files; PakArchive.h/.cpp got a small API addition
│   ├── Main.cpp                 (unchanged console tool, still builds as cso-pak-cli)
│   ├── PakArchive.h/.cpp
│   ├── SnowCipher.h/.cpp        (unchanged)
│   └── CsoDecoder.h/.cpp        second-layer TEA decryption for .cso table entries
└── gui/
    ├── resources/
    │   ├── app.rc                Windows resource script: exe icon + version/author info
    │   └── cpb.ico                ← exe file icon
    └── src/
        ├── main.cpp              Qt application entry point
        ├── MainWindow.h/.cpp     window layout, tree wiring, preview dispatch, extraction, pack/patch
        ├── PakTreeModel.h/.cpp   QAbstractItemModel that turns entry paths into a folder tree
        ├── TgaImage.h/.cpp       small standalone .tga decoder (Qt has no built-in TGA support)
        ├── EucKrCodec.h/.cpp     Windows-only EUC-KR/CP949 (Korean) to UTF-8 decoding for .cso text
        ├── StudioModel.h/.cpp    parser for GoldSource/CSO studiomdl (.mdl) files: bones, meshes,
        │                          textures, skins, and sequence (animation) data
        ├── ModelViewWidget.h/.cpp  QOpenGLWidget: renders a StudioModel with orbit/pan/zoom camera,
        │                            bodygroup/skin selection, wireframe toggle, and sequence playback
        ├── SpriteImage.h/.cpp    decoder for GoldSource/CSO .spr sprites (v2 palette-indexed,
        │                          v3 embedded-DDS DXT5/DXT1/A8), flattened to a per-frame image list
        ├── ByteCursor.h          shared bounds-checked little-endian reader (.spr, .dds)
        ├── DxtDecompress.h/.cpp  shared DXT1/DXT3/DXT5 block decompression (.spr v3, .dds)
        └── DdsImage.h/.cpp       standalone .dds decoder: DXT1/DXT3/DXT5 plus common
                                    uncompressed RGB/RGBA/luminance layouts, mip level 0 only
```

## What changed in the core files

`PakArchive` originally only exposed "unpack everything to disk". The GUI
needs to list entries and pull a single file's bytes into memory for
preview, so it gained, without touching any existing behavior:

```cpp
const std::vector<Entry> &Entries() const;
std::vector<uint8_t> ExtractEntry(const Entry &entry) const;
```

plus a public `EntryTypeFlags` enum (`EntryTypeCompressed` / `EntryTypeEncrypted`
/ `EntryTypeEncryptedAgain`) so callers outside `PakArchive.cpp` can read
`entry.type` without redefining the bit values. `ExtractEntry` reuses the
same decrypt path `UnpackToDirectory` already used per-entry, so it
inherits the existing limitation: **entries with the Compressed flag throw**
(`"compressed entry is not supported yet"`), same as before. The GUI catches
that and shows it as a note in the properties panel instead of crashing.
`PackDirectory` and `PatchFromDirectory` are untouched — the GUI's Pack/Patch
menu items just call them directly, same as the console tool's `pack`/`patch`
commands.

## Building with Visual Studio 2022 + Qt6

1. Install Qt6 via the Qt Online Installer, picking the **MSVC 2022 64-bit**
   component, and make sure **Qt Multimedia** is also checked (needed for the
   audio/video preview — `Qt Images`/image formats are optional but nice to
   have too). OpenGL/OpenGLWidgets (used by the `.mdl` 3D viewer) ship as
   part of the base Qt install, so there's nothing extra to check for that.
2. In Visual Studio 2022, make sure the **"Desktop development with C++"**
   workload is installed (gives you the built-in CMake support).
3. `File → Open → Folder...` and pick the `project/` folder (the one with
   `CMakeLists.txt`).
4. Point CMake at your Qt install with a `CMakeUserPresets.json` next to
   `CMakeLists.txt`:

   ```json
   {
     "version": 4,
     "configurePresets": [
       {
         "name": "default",
         "generator": "Ninja",
         "binaryDir": "${sourceDir}/out/build/${presetName}",
         "cacheVariables": {
           "CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/msvc2022_64",
           "CMAKE_BUILD_TYPE": "Release"
         }
       }
     ]
   }
   ```

   Adjust the Qt version/path to whatever you installed (forward slashes,
   no `inherits` — VS doesn't ship a `windows-default` preset for this
   project to inherit from). Switch `CMAKE_BUILD_TYPE` to `Debug` if you
   want debuggable binaries instead of a clean, deployable Release build.
5. Build. Two targets come out: `cso-pak-cli.exe` (the original console
   tool, unchanged) and `cso-pak-gui.exe` (the browser).

If you'd rather use qmake/Qt Creator instead of CMake, the same source files
under `gui/src/` plus `core/` drop straight into a `.pro` file with
`QT += widgets multimedia multimediawidgets` and `CONFIG += c++20`.

## Getting a clean, deployable build (`install`)

Plain **Build All** in VS only produces the `out/build/default` folder,
which is full of CMake/Ninja intermediates alongside the actual exe/DLLs —
normal for a build directory, not meant for handing to someone else.

The bottom of `CMakeLists.txt` has an `install()` block (only active when
`CMAKE_BUILD_TYPE` is `Release`) that copies just `cso-pak-cli.exe`,
`cso-pak-gui.exe`, the DLLs `windeployqt` dropped next to them, and the Qt
plugin subfolders (`platforms`, `styles`, `imageformats`, `multimedia`, etc.)
into a separate, clean `out/install/` folder. **This does not run
automatically on Build All** — in VS, switch Solution Explorer to the
**CMake Targets View**, find the `install` target, and Build it (or use
**Build → Install cso_pak_tool** from the menu). `out/install/` is what you
should actually zip up and hand to someone else.

Each `install(DIRECTORY ...)` entry has `OPTIONAL` on it, so if a particular
Qt plugin folder doesn't exist for your setup (e.g. no `tls/` because your
Qt build has no SSL support), install just skips it instead of aborting the
whole step.

## Using the browser

- **File → Open Pak...** loads a `.pak` and populates the tree.
- **File → Recent Files** lists the last 10 successfully opened `.pak`
  paths (newest first), persisted across restarts via `QSettings` — handy
  with a large collection of pak files where you keep going back to a
  handful of specific ones (character models in one, effects in another,
  data tables in a third...). A path that's been moved/deleted since shows
  a "file no longer exists" message instead of silently doing nothing.
  **Clear Recent Files** at the bottom empties the list.
- Click a file to preview it on the right:
  - `.tga` → decoded and shown as an image (uncompressed and RLE, 8/16/24/32bpp,
    truecolor/grayscale/color-mapped — covers the vast majority of game TGAs).
  - `.dds` → decoded and shown as an image: DXT1/DXT3/DXT5 compressed, plus
    common uncompressed RGB/RGBA/luminance layouts (whatever channel order
    the file's own bitmasks describe). Only mip level 0 is decoded — plenty
    for a preview. BC4-7/ATI2 and DX10-extended-header DDS files aren't
    supported and fall back to the properties panel with the reason why.
  - `.png/.jpg/.bmp/.gif/.ppm` → shown natively via Qt in case any turn up.
  - `.wav` and `.webm` → played back with transport controls (play/pause,
    seek slider, elapsed/total time). Both share one `QMediaPlayer`; the
    video area only appears for files that actually have a video track, so
    `.wav` still looks like a plain audio player. Playback goes through a
    temporary file (`QTemporaryFile`, auto-deleted) since `QMediaPlayer`
    wants a file/URL rather than a raw in-memory buffer.
  - `.csv` → parsed into a table (simple `,`/`;` split, no quoted-field
    escaping — good enough for a preview, not a full CSV parser).
  - `.cso` → decrypted with a second TEA-style layer (`CsoDecoder`, on top
    of the normal pak decryption `ExtractEntry` already does), then shown
    in the same CSV table view. Trailing zero-padding bytes (the cipher
    works in 8-byte blocks, so a file not already a multiple of 8 gets
    padded before encryption, and those padding bytes decrypt back to real
    `0x00`s) are trimmed before the text check — a handful of legitimate
    padding bytes at the end used to veto the whole table as "not text",
    even when it was a perfectly readable, purely numeric table. On
    Windows, the decrypted bytes are checked for EUC-KR/CP949 (Korean)
    content first and converted to UTF-8 if so — these table files are the
    game's own data, and mixed Korean comments are common in them.
  - `.txt` and other recognized text extensions, or anything that "looks like
    text" by a byte-content heuristic → shown in a monospace text view.
    Decoding is BOM-aware (UTF-8/UTF-16LE/UTF-16BE/UTF-32 are all detected
    from the file's byte-order mark and decoded correctly — several of the
    game's own locale `.txt` files are UTF-16LE); on Windows, un-BOM'd text
    that isn't valid UTF-8 is also checked for EUC-KR/CP949 (Korean) before
    falling back to Latin-1 as the last resort — the same check `.cso`
    tables get, since plain `.csv`/`.txt` files can have Korean text in
    them too, not just decrypted `.cso` ones.
  - `.mdl` → parsed with `StudioModel` and shown in an interactive 3D viewer
    (`ModelViewWidget`, OpenGL): orbit with left-drag, pan with right-drag,
    zoom with the wheel. The side panel lets you pick a submodel per
    bodypart, switch skin families, toggle wireframe, and play back
    sequences (animations) at the sequence's own fps with a scrub slider.
    Lighting is two camera-attached lights (a key light from the upper
    right, plus a dimmer fill from the lower left purely *added* on top —
    it never subtracts from the key light's contribution, so nothing that
    already looked right gets dimmer); a persistent **Lighting** box above
    the model controls (Yaw/Pitch sliders + Reset) lets you rotate both by
    hand for any model that still looks uneven — it's a viewer preference,
    not saved per file, so it carries over as you switch between models
    until you change or reset it. Note: if a model looks lit correctly on
    the body but wrong on the head (or vice versa) no matter the angle,
    that was actually a bone-skinning bug (vertex normals for bones far
    from the origin — like a head — were getting the bone's *position*
    added into the normal direction, not just rotated by it), not a
    lighting-angle problem; it's fixed in `StudioModel::ApplyFrame` and the
    rest-pose bake, not something the light sliders can work around.
    A **Texture** section lists every texture embedded in the .mdl with a
    thumbnail and its GoldSource `STUDIO_NF_*` flags. `masked` textures
    render properly transparent at their cutout index (alpha discarded in
    the shader). Additive rendering (fire, glow, particle FX — a solid
    black plane on a weapon or boss is almost always one of these) is
    detected two ways: the file's own `additive` flag, or the texture name
    starting with `$0a_`/`$0b_` — an undocumented CSO naming convention
    confirmed by hand across ~20 models where the flag isn't set but the
    black background is still meant to be transparent. Either way, it gets
    a real second render pass: drawn blended (`GL_ONE`, so it adds light
    instead of alpha-compositing), unlit (its own texture color/brightness,
    not modulated by the scene lighting above — self-illumination isn't
    "lit" by anything), and without writing depth so it layers like light
    rather than occluding or z-fighting with geometry behind it. For
    anything that still looks wrong — an effect texture that's neither
    flagged nor prefixed, or a rare false positive — a **Treat as additive
    (transparent)** checkbox next to the texture info shows the *current*
    effective state (flag, name prefix, or a manual choice, whichever
    applies) and lets you force either direction by hand per texture. It's
    per-model (cleared when you load a different .mdl, since texture
    indices aren't comparable across files).
    Some models only *name* a texture (CSO's own convention: a `#`-prefixed
    name, or a tiny placeholder image like 4×1) and ship the real pixels as
    a separate loose file elsewhere in the same pak — the tool searches the
    open archive for a same-named file (any common image extension, not
    just the exact one named) and swaps it in automatically before the
    model is shown. When that search comes up empty, the panel lists
    exactly which textures are still missing and a **Load textures from
    folder...** button lets you point at a folder (e.g. an extracted/loose
    texture dump) to resolve them by hand instead.
    A parse failure (unsupported version, corrupt file, etc.) falls back to
    the properties panel with the error instead of crashing.
  - `.spr` → decoded with `SpriteImage` and played back frame by frame
    (auto-plays for multi-frame sprites, like a GIF; a single-frame sprite
    just shows as a static image with playback disabled). Each frame has
    its own display duration read from the file, so playback speed isn't a
    fixed fps — a frame with a short interval flips faster than one with a
    long one. `Fit to window` defaults to *off* here (unlike the plain
    image preview) since sprites are usually small icons that look bad
    stretched up to fill the window.
  - Everything else (compressed entries, unrecognized binary formats) → a
    properties panel with path, sizes, type flags, base key, checksum.
- **File → Extract All...** — same as the console tool's `unpack` command.
- **File → Extract Selected...** / right-click a selection — extracts just
  the selected files/folders (recursing into folders) to a folder you pick.
- **File → Extract Selected (Decode .cso to CSV)...** — same selection
  logic, but any `.cso` entries in the selection get the same TEA-decrypt +
  Korean-detection treatment as the preview, and are written out as `.csv`
  (not `.cso`) with the decoded, readable content. Everything else in the
  selection is extracted as-is, same as the plain Extract Selected.
- Double-click a file to save just that one file via a Save As dialog.
- **File → Pack Directory into New Pak...** — same as the console tool's
  `pack` command: pick a folder, pick where to save the new `.pak`.
- **File → Patch Archive with Replacements...** — same as `patch`: pick a
  source `.pak`, a folder of replacement files, and an output path.
  Independent of whatever's currently open in the tree. Offers to open the
  result once it's done, for both Pack and Patch.

## Known limitations

- Compressed entries (`type & Compressed`) can't be previewed or extracted —
  that was already true of `UnpackToDirectory`; `ExtractEntry` just surfaces
  it as a message instead of a crash.
- Text without a BOM falls back to UTF-8, then Latin-1, if neither decodes
  cleanly. If some of your archives turn out to be Windows-1251 (or another
  8-bit code page) *without* a BOM, add the optional **Qt6 Core5Compat**
  module (installable via the Qt Maintenance Tool) and decode with
  `QTextCodec::codecForName("Windows-1251")` in `MainWindow::DecodeText` —
  there's a comment marking exactly where.
- CSV preview doesn't handle quoted fields containing the delimiter; it's a
  preview, not a spreadsheet import.
- `.dds` support covers DXT1/DXT3/DXT5 and common uncompressed layouts —
  the formats actually seen in this game's own pak files (and what the
  `.spr` v3 decoder already needed). Newer compression (BC4-7/ATI2) and the
  DX10-extended-header variant of DDS aren't implemented; both throw a
  clear "unsupported" message rather than showing garbage. Only mip level 0
  is read, so a texture with 8 mip levels decodes just as fast as one
  without any — the rest of the file is simply never touched.
- EUC-KR/CP949 detection for `.cso` files uses the Win32 codepage API
  directly, so it's Windows-only by design (guarded with `#ifdef _WIN32`).
  On other platforms `.cso` content still gets TEA-decrypted and shown as
  CSV, just without the Korean-specific decoding step — fine for tables
  that are plain ASCII/English, garbled for ones with Korean text.
- `.webm` playback depends on the codecs your Qt Multimedia backend
  supports. Official Qt 6.4+ Windows builds bundle FFmpeg by default, which
  decodes the usual VP8/VP9 + Opus/Vorbis combination found in `.webm` files
  without extra codec packs — but if Qt was built against Windows Media
  Foundation instead, some `.webm` variants may not play.
- On Windows, the media backend can briefly hold the temp playback file open
  after `stop()`; if you close the app mid-playback, that one temp file can
  occasionally survive in the OS temp folder instead of being cleaned up
  immediately. Harmless, just a stray file.
- `ModelViewWidget::paintGL` explicitly resets `GL_BLEND`/blend func/depth
  mask at the very top, before anything else runs. This isn't defensive
  paranoia: `QOpenGLWidget`'s own internal compositing (blitting its
  rendered frame into the rest of the window) does its own GL work between
  your `paintGL` calls and can leave blend state on afterward — confirmed
  directly in this environment (forcing a second frame of the same model
  showed `GL_BLEND` already enabled on entry, before the reset ran).