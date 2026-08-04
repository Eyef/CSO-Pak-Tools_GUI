# CSO Pak Browser

An Explorer-style GUI on top of the original `cso_pak` console tool: folder
tree on the left, live preview (text / image / audio+video / CSV table / raw
properties) on the right, plus pack/patch from the same File menu.

```
project/
├── CMakeLists.txt
├── core/                      ← your original files; PakArchive.h/.cpp got a small API addition
│   ├── Main.cpp                 (unchanged console tool, still builds as cso-pak-cli)
│   ├── PakArchive.h/.cpp
│   ├── SnowCipher.h/.cpp        (unchanged)
│   └── CsoDecoder.h/.cpp        second-layer TEA decryption for .cso table entries
└── gui/
    ├── resources/
    │   ├── app.rc                Windows resource script: exe icon + version/author info
    │   └── cpb.ico                ← put your icon here (not included in this zip)
    └── src/
        ├── main.cpp              Qt application entry point
        ├── MainWindow.h/.cpp     window layout, tree wiring, preview dispatch, extraction, pack/patch
        ├── PakTreeModel.h/.cpp   QAbstractItemModel that turns entry paths into a folder tree
        ├── TgaImage.h/.cpp       small standalone .tga decoder (Qt has no built-in TGA support)
        └── EucKrCodec.h/.cpp     Windows-only EUC-KR/CP949 (Korean) to UTF-8 decoding for .cso text
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
   have too).
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
- Click a file to preview it on the right:
  - `.tga` → decoded and shown as an image (uncompressed and RLE, 8/16/24/32bpp,
    truecolor/grayscale/color-mapped — covers the vast majority of game TGAs).
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
    in the same CSV table view. On Windows, the decrypted bytes are checked
    for EUC-KR/CP949 (Korean) content first and converted to UTF-8 if so —
    these table files are the game's own data, and mixed Korean comments
    are common in them.
  - `.txt` and other recognized text extensions, or anything that "looks like
    text" by a byte-content heuristic → shown in a monospace text view.
    Decoding is BOM-aware (UTF-8/UTF-16LE/UTF-16BE/UTF-32 are all detected
    from the file's byte-order mark and decoded correctly — several of the
    game's own locale `.txt` files are UTF-16LE); falls back to Latin-1 if
    there's no BOM and the bytes aren't valid UTF-8.
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
