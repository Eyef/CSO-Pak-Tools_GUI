# CSO Pak Browser

An Explorer-style GUI on top of the original `cso_pak` console tool: folder
tree on the left, live preview on the right, plus pack/patch for both `.pak`
and Nexon `.nar` from the same File menu. Besides `.pak` it reads Nexon `.nar`
archives (where CSO keeps most models), and it can write `.nar` archives back
(raw/XOR/LZ storage, optional ICE model encryption), so repacking and patching
retail CSO data needs no external tools. It previews text, images
(TGA/DDS/WAD textures and `.ico` icons), audio/video, CSV tables, `.mdl`
models and `.spr` sprites in 3D/animated viewers, and whole `.bsp` maps with
baked lighting, placed models/sprites and an optional skybox.

```
project/
├── CMakeLists.txt
├── CMakeUserPresets.json         points CMake at the local Qt install (see "Building")
├── core/                         shared library `cso_pak_core` + console tool (23 files)
│   ├── Main.cpp                    console tool (unpack/pack/patch), builds as cso-pak-cli
│   ├── PakArchive.h/.cpp           .pak reader/writer; small API addition for the GUI
│   ├── SnowCipher.h/.cpp           Snow cipher used by the pak format (unchanged)
│   ├── CsoDecoder.h/.cpp           second-layer TEA decryption for .cso table entries
│   ├── NarArchive.h/.cpp           Nexon .nar archive reader (directory + on-demand extraction)
│   ├── NarBZip2.h/.cpp             minimal bzip2 decoder for the .nar directory blob
│   ├── NarBZip2Enc.cpp             bzip2 encoder used to write the .nar directory
│   ├── NarCommon.h                 signatures, keys, CRC-32 and LZ constants shared by reader/writer
│   ├── NarWriter.h/.cpp            .nar writer: raw/XOR/LZ storage, directory, temp file + rename
│   ├── NarTools.h/.cpp             unpack/pack/patch + `.nar-info.json` manifest
│   ├── NarCli.h/.cpp               `nar-list`/`nar-unpack`/`nar-extract`/`nar-pack`/`nar-patch`
│   ├── IceCipher.h/.cpp            ICE block cipher (CSO model crypto)
│   └── ModelCrypt.h/.cpp           .mdl v20/21 <-> plain (v9) texture/vertex ICE encryption
└── gui/
    ├── resources/
    │   ├── app.rc                  Windows resource script: exe icon + version/author info
    │   └── cpb.ico                 exe file icon
    └── src/                      Qt GUI (27 files)
        ├── main.cpp                Qt application entry point
        ├── MainWindow.h/.cpp       window layout, tree wiring, preview dispatch, extraction,
        │                            pak pack/patch, and everything that feeds the viewers:
        │                            texture/model resolution across archives, .wad lookup, BSP
        │                            map assembly (placed models, sprites, skybox)
        ├── MainWindowNar.cpp       NAR pack/patch/unpack/verify actions (delegates to core/NarTools)
        ├── PakTreeModel.h/.cpp     QAbstractItemModel that turns entry paths of every loaded
        │                            .pak/.nar (and the lumps of .wad files) into one folder tree
        ├── TgaImage.h/.cpp         small standalone .tga decoder (Qt has no built-in TGA support)
        ├── EucKrCodec.h/.cpp       Windows-only EUC-KR/CP949 (Korean) to UTF-8 decoding for .cso text
        ├── ByteCursor.h            shared bounds-checked little-endian reader (.spr, .dds, .wad, .bsp)
        ├── DxtDecompress.h/.cpp    shared DXT1/DXT3/DXT5 block decompression (.spr v3, .dds)
        ├── DdsImage.h/.cpp         standalone .dds decoder: DXT1/DXT3/DXT5 plus common
        │                            uncompressed RGB/RGBA/luminance layouts, mip level 0 only
        ├── Wad3Archive.h/.cpp      WAD2 (Quake)/WAD3 (Half-Life/GoldSource) archive parser plus a
        │                            miptex texture decoder, for .wad files (inside archives or loose)
        ├── SpriteImage.h/.cpp      decoder for GoldSource/CSO .spr sprites (v2 palette-indexed,
        │                            v3 embedded-DDS DXT5/DXT1/A8), per-frame images, orientation
        │                            type and frame origins (for placing sprites on maps)
        ├── StudioModel.h/.cpp      parser for GoldSource/CSO studiomdl (.mdl) files: bones, meshes,
        │                            textures, skins, bodyparts (with the `base` divisor used by the
        │                            map "body" key) and sequence (animation) data
        ├── ModelViewWidget.h/.cpp  QOpenGLWidget: renders a StudioModel with orbit / first-person
        │                            camera, bodygroup/skin selection, wireframe, sequence playback
        ├── BspMap.h/.cpp           GoldSrc BSP v30 parser (all lumps, textures, entities), brush-entity
        │                            and placed-entity parsing, lightmap atlas builder, mesh builder
        │                            (no OpenGL, usable headless) and the light-probe used for models
        └── BspViewWidget.h/.cpp    QOpenGLWidget: renders a BSP map (lightmapped brushes, translucent /
                                     additive passes, placed models, sprites, skybox) with orbit and
                                     fly cameras
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

`Load` no longer keeps the whole file in memory: after parsing the header
and entry table the buffer is released, and `ExtractEntry` /
`UnpackToDirectory` / `PatchFromDirectory` read each entry's byte range
straight from disk on demand. The public API is unchanged, but this is what
lets the GUI keep hundreds of archives open at once without holding
gigabytes of payload data in RAM.

`NarArchive` / `NarBZip2` are the read side: Counter-Strike Online ships most
models, sprites and textures in Nexon `.nar` archives (`Data/*.nar`) rather
than in the `.pak` files. The write side (`NarWriter`, `NarBZip2Enc`,
`NarTools`, `NarCli`, `IceCipher`, `ModelCrypt`) is described under
[".nar write support and ICE models"](#nar-write-support-and-ice-models)
below. A `.nar` ends with an XOR-obfuscated, bzip2-compressed
directory; `NarArchive::Load` decodes only that directory (via the built-in
`NarBZip2` decoder, which verifies block CRCs) and `ExtractEntry` reads and
decodes a single file on demand (raw, XOR-encoded, or XOR-encoded + LZSS
compressed storage), so multi-gigabyte archives are never held in memory.
Nar entries use the same `FoundEntry` abstraction in the GUI as pak entries,
so every lookup (model textures, BSP-placed models, sprites, `.wad` files)
searches both kinds.

## Building with Visual Studio 2022 + Qt6

1. Install Qt6 via the Qt Online Installer, picking the **MSVC 2022 64-bit**
   component, and make sure **Qt Multimedia** is also checked (needed for the
   audio/video preview — `Qt Images`/image formats are optional but nice to
   have too). OpenGL/OpenGLWidgets (used by the `.mdl` and `.bsp` 3D viewers)
   ship as part of the base Qt install, so there's nothing extra to check
   for that. The viewers need an OpenGL 3.3 core-capable driver.
2. In Visual Studio 2022, make sure the **"Desktop development with C++"**
   workload is installed (gives you the built-in CMake support).
3. `File → Open → Folder...` and pick the `project/` folder (the one with
   `CMakeLists.txt`).
4. Point CMake at your Qt install with a `CMakeUserPresets.json` next to
   `CMakeLists.txt` (the snippet shows one preset; the project's file also
   defines `relwithdebinfo` and `debug`, which are identical apart from
   `CMAKE_BUILD_TYPE` and the output folder):

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
   project to inherit from). The presets are:

   | preset | build type | output |
   |---|---|---|
   | `default` | Release | `out/build/default/` |
   | `relwithdebinfo` | optimized + full PDB | `out/build/relwithdebinfo/` |
   | `debug` | unoptimized, checks on | `out/build/debug/` |

   Each preset has its own build folder, so they never overwrite each
   other and you can switch back and forth freely.
5. Build. Two targets come out: `cso-pak-cli.exe` (the original console
   tool, unchanged) and `cso-pak-gui.exe` (the browser).

From a terminal (with the MSVC environment loaded, e.g. from an
"x64 Native Tools" prompt, or after `call vcvars64.bat`):

```bat
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
```

In Visual Studio, pick the preset from the configuration dropdown in the
toolbar, then **Build → Build All**. To debug, select `cso-pak-gui.exe` as
the startup item and press F5.

<details>

### Debug vs RelWithDebInfo

- **RelWithDebInfo** = the Release build plus debug info (`/Zi`, linker
  `/DEBUG`, plus `/Zo` for better stepping through optimized code). Speed
  and behavior are almost identical to Release, so it is the best choice
  for reproducing a crash that only happens in real use. The function name
  and source line show up in the Visual Studio call stack as long as
  `cso-pak-gui.pdb` sits next to `cso-pak-gui.exe` (build a matching pair
  after every change — never mix an old PDB with a new EXE).
- **Debug** = `/Od /RTC1 /MDd`. Besides no optimization, this switches on
  the MSVC debug runtime, which gives you the "clear message" checks:
  `_ITERATOR_DEBUG_LEVEL=2` (e.g. *"vector subscript out of range"*,
  *"cannot dereference end vector iterator"*, *"iterator + offset out of
  range"*), `assert()` failures, uninitialized-variable runtime checks
  (`/RTC1`) and the debug heap (catches some double frees / use-after-free
  patterns). Debug builds link against the debug Qt DLLs
  (`Qt6Cored.dll`, ...), which this Qt installation ships, so they run
  out of the box. Note that a Debug build changes timing and heap layout,
  so a race-condition crash may not reproduce there — test both
  configurations.

### Catching the crash in the debugger

- Start the app with **F5** from Visual Studio rather than double-clicking
  the EXE; then the debugger is already attached when the fault happens.
- **Debug → Windows → Exception Settings → Win32 Exceptions**: tick
  *Access violation* (and *Stack overflow*). Without this, the process may
  die before the debugger gets a chance to show the call stack.
- If the crash comes from the standard library (`std::out_of_range`,
  `std::length_error`, "vector subscript out of range"), a Debug build
  stops right at the check with the message in the Output window. In
  RelWithDebInfo those checks are disabled, but an access violation still
  lands on the correct source line.
- The `.pdb` must match the `.exe` exactly. Copy them together, and after
  rebuilding always use the PDB from the same build folder.
- If it only crashes outside the IDE, capture a dump (`procdump -e -ma
  -w cso-pak-gui.exe`) and open the `.dmp` in Visual Studio; with the
  matching EXE/PDB you get the same call stack and line numbers.

Optional, for heap corruption that the debug CRT misses: MSVC has
AddressSanitizer. Add it to a copy of the debug preset
(`"CMAKE_CXX_FLAGS_DEBUG": "/Zi /Ob0 /Od /fsanitize=address"`), because
ASan is incompatible with `/RTC1`; it prints a full allocation/free stack
on the first bad access. It roughly halves performance and is only meant
for hunting a specific bug.

If you'd rather use qmake/Qt Creator instead of CMake, the same source files
under `gui/src/` plus `core/` drop straight into a `.pro` file with
`QT += widgets multimedia multimediawidgets opengl openglwidgets` and
`CONFIG += c++20` (plus the system OpenGL library).

</details>

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

- **File → Open Archive...** loads a single `.pak` or `.nar` file and shows
  exactly its contents in the tree. Exception: opening a single `.pak` also
  silently pulls in the neighbouring `.nar` archives (same folder and its
  parent — CSO keeps most models there, next to `Packer/`), because model
  textures and BSP-placed models usually live in `.nar`, not in the `.pak`
  itself. Those auto-loaded `.nar` files stay hidden from the tree (see
  `View` below) but are fully used for resolution — a single `.nar` on its
  own loads exactly, with no additions.
- **File → Open Multiple Archives...** loads several `.pak` and/or `.nar`
  files as one merged tree — free choice, exactly what was selected, no
  silent additions. Picking only `.pak` files here shows only their contents
  (models whose files live in `.nar` will then report missing files).
- **File → Open Paks + All Nars...** — pick `.pak` files, and every
  neighbouring `.nar` is pulled in automatically **and shown**: the one-click
  setup for BSP maps, whose placed models (cars, trees, fences, …) almost
  all live in `.nar`.
- **File → Open Archives Folder...** loads every `.pak`/`.nar` in a folder
  (e.g. the game's whole `Data/Packer` directory — several hundred archives
  work fine), plus the parent folder's `.nar` files when the folder itself
  has none.
- **View → Show auto-loaded NAR archives** (checkbox, remembered across
  restarts): shows the silently pulled-in `.nar` files in the tree, or hides
  them again. Hiding only affects the tree — texture/model/WAD resolution
  always searches every loaded archive either way, so opening one `.pak`
  keeps resolving its models' textures from the hidden `.nar` files.
- **View → Find File...** (Ctrl+F, or the search box in the toolbar) searches
  the full internal path of every entry in every loaded `.pak` **and** `.nar`
  archive — including auto-loaded `.nar` files that the option above hides
  from the tree. Matching is case-insensitive; `*` and `?` work as wildcards.
  Results appear in a dock at the bottom with **Name / Path / Archive / Type**
  columns, so the same file name found in several archives is shown once per
  archive (with the archive that holds it). Selecting a row previews that
  concrete file; double-clicking reveals it in the archive tree when the
  archive is shown there. This is the quick way to answer "which archive
  contains `v_xxx.mdl`?" without expanding hundreds of folders by hand.
- All loaded archives appear as **one merged tree**: folders with
  the same virtual path are combined, and files with the same virtual path
  from different archives are all shown side by side — no duplicate is
  hidden and no "newest pak wins" rule is applied, since game updates
  scatter newer copies of files across archives unpredictably. Use the
  tooltip (hover) or the status bar (click a file) to see which concrete
  archive a given entry came from: `lstrike/.../v_usp.mdl — common_00137.pak`.
  Loading shows a progress dialog with Cancel; archives that fail to parse
  are reported in a list at the end instead of aborting the whole batch.
  Only each archive's entry table is kept in memory — file bytes are read
  from disk on demand when you preview or extract something — so opening
  hundreds of archives (several GB on disk) doesn't balloon RAM usage.
- **File → Recent Files** lists the last 10 successfully opened archive
  paths (`.pak`/`.nar`, newest first), persisted across restarts via
  `QSettings` — handy with a large collection of pak files where you keep
  going back to a handful of specific ones (character models in one, effects
  in another, data tables in a third...). A path that's been moved/deleted since shows
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
  - `.png/.jpg/.bmp/.gif/.ppm/.ico` → shown natively via Qt in case any turn
    up. A multi-size `.ico` (the ones in `cstrike.nar` carry up to six
    variants) shows its largest frame instead of the first, which is usually
    the sharpest and most colourful one.
  - `.wad` (WAD2/Quake or WAD3/Half-Life-GoldSource — CSO's own are WAD3) is
    "an archive inside the archive": it shows up in the tree with a
    distinct icon and an expand arrow, like a folder, with its own lumps as
    children — no separate extract-then-reopen step needed. Clicking the
    `.wad` itself shows a summary (format, entry count); clicking a lump
    tries decoding it as a `miptex` texture (the standard WAD3 format —
    palette-indexed, with the classic `{`-prefixed-name convention for a
    transparent cutout color, same idea as studiomdl's masked textures) and
    falls back to properties (name, size, raw type byte) for anything that
    isn't. A `.wad` that fails to parse just stays a normal, non-expandable
    file instead of breaking the rest of the tree.
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
  - `.txt` and other recognized text extensions (`txt ini cfg log xml json lst
    def vdf inf`), or anything that "looks like
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
    zoom with the wheel. Right-drag pan moves the actual orbit pivot using
    the camera's *current* right/up directions (recomputed from the live
    yaw/pitch every drag) rather than a fixed offset that was only correct
    at the default angle — panning after rotating the camera used to send
    the pivot somewhere unrelated to what was on screen, which made models
    with far-apart pieces (a boss standing well away from its own
    attack-effect meshes, for instance) hard to frame. Zooming in is no
    longer capped by the *whole* model's bounding radius either, so you can
    get close to one small part of a spread-out model instead of stopping
    short because something else in the file is far away. A **Reset View
    (camera)** button next to Lighting re-frames from the model's geometry
    from scratch if you still get the camera lost. The side panel lets you
    pick a submodel per bodypart, switch skin families, toggle wireframe,
    and play back
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
    starting with `$0a_`/`$0b_`/`ef_` — undocumented CSO naming conventions
    confirmed by hand across many models where the flag isn't set but the
    black background is still meant to be transparent (`ef_` = effect
    textures such as `ef_sewerhole*`). Either way, it gets
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
    The **Camera** box offers *Orbit* and *First person* (view-model
    framing with an adjustable FOV, for weapon `v_` models). Right-click
    a texture entry → **Export texture...** saves it as BMP/PNG/JPEG.
    Textureless world models (`w_xxx_big.mdl`) automatically take their
    textures from the twin `w_xxx_bigt.mdl` texture file.
    Part of a model's animations can live outside the `.mdl`, in companion
    files named `<model name><N>.seq` (`buffclass21s1ct.mdl` +
    `buffclass21s1ct1.seq` ... `buffclass21s1ct13.seq`). They are found
    automatically and appended to the model's **Sequence** list in ascending
    N order (hover an entry to see its file). The model's own archive is
    searched first, then every other loaded archive and, with **View >
    Search neighbouring archives for external animations (.seq)** on (the
    default), the `.pak`/`.nar` files lying next to the loaded ones, which
    are opened once per session. A line under the sequence list reports
    which files were used and which numbers below the highest one were not
    found. A `.seq` file is `int32 numseq, int32 seqdescOffset`, the
    events/animation data from offset 8, and the `mstudioseqdesc_t` table
    at `seqdescOffset`; the pointers inside it are addresses of the original
    unsplit model, so `file offset = pointer - (lowest pointer - 8)`
    (`StudioModel::AppendExternalSequences`). A `.seq` that does not fit the
    model's skeleton is rejected. Models placed on a BSP map do not load
    `.seq` files.
    Some models only *name* a texture (CSO's own convention: a `#`-prefixed
    name, or a tiny placeholder image like 4×1) and ship the real pixels as
    a separate loose file elsewhere in the same pak — the tool searches
    every loaded archive for a same-named file (the `#` marker is ignored
    while matching, and any common image extension matches, not just the
    exact one named — the model's own archive is tried first) and swaps it
    in automatically before the
    model is shown. When that search comes up empty, the panel lists
    exactly which textures are still missing and a **Load textures from
    folder...** button lets you point at a folder (e.g. an extracted/loose
    texture dump) to resolve them by hand instead. The picked folder is
    remembered (across restarts too) and applied automatically to every
    subsequently opened model, so you don't re-pick it per weapon. As a
    last fallback, a `textures/` folder placed next to the exe is also
    searched automatically.
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
  - `.bsp` (GoldSrc BSP v30 maps) → an interactive 3D map viewer
    (`BspViewWidget`). Left-drag looks/orbits, right-drag pans (orbit mode),
    wheel zooms. The side panel has a camera selector — **Orbit** or **Fly
    (WASD)** (WASD/Q/E/C/Space/Shift work on any keyboard layout, because
    they are matched by scan code) — plus *Reset View*, *Map brightness*,
    *Model light boost*, *FOV*, *Wireframe* and *Pulse (renderfx)* (pulsing
    brightness of glowing signs, off by default), and a summary of faces,
    models, entities, texture sources, used/missing `.wad` files and placed
    models/sprites. Several viewer options live in the **View** menu (all
    apply to the BSP viewer only):
    - **Show skybox in BSP viewer** — sky on/off (off by default: the sky
      faces enclose parts of the level and hide it when viewed from outside).
    - **Hide map parts and models outside the PVS (as in game)** — off by
      default. Draws world faces, placed models, sprites and brush entities
      only when the game would send them to a player standing at the camera
      (their leaf is in the camera leaf's PVS). Works with the fly camera
      inside the map; with the camera outside everything stays visible. While
      it is on, a permanent status-bar line shows the camera position, its
      leaves and how many entities/sprites are drawn.
    - **Cull back faces in BSP viewer (as in game)** — on by default, as the
      engine draws brush faces from their front side only. Turn it off to see
      both sides (useful for one-sided faces that look wrongly missing).
    - **Apply env_fog in BSP viewer (as in game)** — off by default. Maps with
      an `env_fog` entity fade distant geometry into the fog colour
      (exponential-squared fog, like the engine); no effect on maps without
      one.
    - **Show map state after the scripted switch (cutscene, destroyed props)**
      — off by default. CSO maps swap some models with a trigger
      (`cycler_mdl` `_mdl0` → `_mdl1`, `env_render` "..._on" targets, etc.:
      intact antennas become burnt ones, a hidden boss appears). Off shows the
      map as it is at round start; on shows it after the switch. Toggling it
      reloads the map (the camera resets).
    - *Textures* come from the BSP itself, from the `.wad` files the map
      names (searched in every loaded archive, in the texture folders, and
      in neighbouring `.pak` files; wads that are still missing textures
      are searched for the remaining names), or from loose files. Textures
      starting with `{` are cut-outs (palette index 255 transparent). As in
      the engine, the transparent texels are only cut out on brush entities
      with rendermode Solid/Texture/Glow/Additive and on models and sprites;
      worldspawn faces and Normal/Color-mode brush entities draw them opaque
      black. Their mip levels are rebuilt the way the `.wad` stores them
      (binary alpha), so far-away fences and wires keep their thickness.
      Liquid (`!`) faces are drawn only from the side facing the eye.
      Editor/tool textures (`null`, `clip`, `aaatrigger`, `hint`, `skip`,
      `origin`, CSO's blue `nowallshot` marker, ...) are not drawn.
    - *Transparent edges*: alpha-tested images (`{` textures, model textures
      and `.spr` sprites with the "alpha test" format) keep their blue
      transparency key colour (0,0,255) in the RGB of the masked texels.
      Before upload to the GPU the viewer fills those texels with the colour
      of the nearest opaque ones (`BleedTransparentColors`), so bilinear
      filtering and mipmaps no longer draw a blue fringe around grass
      blades, leaves or fences.
    - *Lighting* uses the map's baked lightmaps (packed into one atlas;
      faces without a lightmap stay fullbright). All stored light styles of
      a face are treated as active and summed (clamped to 255); style
      animation/switching is not simulated. The default **game-fitted** model
      is calibrated against in-game screenshots; **Map brightness** scales
      it and **Model light boost** blends placed models between "lit exactly
      like the map under them" and the game-fitted ambient band. (A second,
      "Plain (like goldsrc-bsp-viewer)" shader mode still exists but its
      switch is hidden — set `kShowLightingModeSwitch` to `true` in
      `MainWindow.cpp` to bring it back; the boost slider reproduces it.)
      Maps lit by a very strong sun (e.g.
      `zs_evacuate3`, `zs_electron2`) store sun-facing surfaces clipped at
      255, so for those maps (detected automatically from the lightmap
      histogram) the brightest values are compressed by a smooth, monotonic
      roll-off — the constants `kKneeStart/kKneeLen/kKneeSlope` in
      `BspViewWidget.cpp` tune it. Placed models are lit like the engine
      does: the lightmap texel under the model supplies the light's hue, the
      brightness stays in a compressed ambient band (`Model light boost`),
      CSO's `renderfx 22` + `rendercolor` light multiplier is honoured, and
      a model with no lit floor beneath it gets only its minimum ambient.
    - *Brush entities* (doors, glass, glow arrows, signs, ...) follow
      their `rendermode`/`renderamt`/`rendercolor`: colour tint, alpha blend
      (translucent faces are sorted back to front like the engine does) and
      additive glow/beam rendering. As in the engine, an entity with a
      non-normal rendermode and no `renderamt` is invisible (used for
      invisible blockers), `func_rotating`-type brushes are positioned from
      their origin, brush-entity faces are drawn from their front side only
      and depth-biased so they don't z-fight with coplanar world faces.
      Some entities are animated: spinning pictograms (`func_rotating` signs
      such as the "E" prompts) turn at the entity's `speed`, and the
      **Pulse (renderfx)** checkbox modulates translucent/additive signs with
      `renderfx` 1-4. CSO's `renderfx 24` coloured glow shell (pushable boxes
      green, breakable boxes red) and crossed-plane marker brushes (drawn as
      camera-facing quads) are handled too. `func_train` brushes are drawn
      where the mapper placed them or at their first `path_corner`, and
      `func_wall_toggle`/`env_render` targets follow the "map state after the
      scripted switch" option above. There is still no other entity logic —
      doors and lifts are drawn where they are stored.
    - *Placed props*: every `.mdl` named by a point entity (`cycler_mdl`,
      supply boxes, weapon boxes via `packmdl`, trees, vehicles, ...) is
      found in the loaded `.pak`/`.nar` archives and baked into the scene
      with the entity's origin/angles/scale, `skin`, `body` (submodel per
      bodypart) and `sequence` (default 0), including chrome textures and
      additive effect textures. `.spr` entities (`env_sprite`, ...) are drawn
      as billboards honouring the sprite's orientation type, frame origin
      and render mode (first frame only). Sprites in Normal/Texture mode (grass,
      bushes) are lit by the lightmap texel under the entity, like placed
      models; additive/glow sprites stay self-luminous. Special cases: `cycler_mdl`
      model switchers (`_mdl0`/`_mdl1`), `models/null.mdl` (= not shown),
      `hostage_entity` (dropped to the floor like the engine does),
      `naval_weapon`, and trains with `model_ex` and no origin (placed at
      their first `path_corner`). Use **File → Open Paks + All Nars...** for maps:
      their models almost all live in `.nar`.
    - *Skybox*: the map's `skyname` is resolved to the six `gfx/env/<sky>*`
      images in the archives, and the map's own `sky` faces show it exactly
      like the engine does (direction from the eye).
  - Everything else (compressed entries, unrecognized binary formats) → a
    properties panel with path, sizes, type flags, base key, checksum.
- **File → Extract All...** — same as the console tool's `unpack` command
  (in multi-pak mode: the whole merged tree; same virtual paths coming
  from different archives are all written, later ones with a
  `__from_<pakname>` suffix instead of overwriting each other). For `.nar`
  entries this writes the files after XOR/LZ decoding only: ICE-encrypted
  models stay encrypted and no `.nar-info.json` is written — use **File →
  Unpack NAR to Directory...** for a full, repack-ready extraction.
- **File → Extract Selected...** / right-click a selection — extracts just
  the selected files/folders (recursing into folders) to a folder you pick
  (same `__from_<pakname>` disambiguation for duplicates).
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
- **File → Pack Directory into New NAR...**, **Patch NAR with
  Replacements...**, **Unpack NAR to Directory...** and **Verify NAR Archive
  Integrity...** — the `.nar` counterparts of the actions above; see
  [".nar write support and ICE models"](#nar-write-support-and-ice-models)
  for their options.

## Known limitations

- BSP viewer: all stored light styles are simply summed, so there are no
  switchable/animated lights and no dynamic lights; there are no animated
  textures and water is a plain (non-waving) texture; PVS culling is
  optional (View menu, off by default) and only the map's leaf-level
  visibility is used, no portals/areas. Entity logic is limited to what is
  listed above — doors, lifts and the like are drawn where they are
  stored. Placed `.mdl` models are static (rest pose or frame 0 of the entity's
  `sequence`) and placed sprites show only their first frame. The light
  calibration is empirical (CSO's exact curve is unknown), so brightness can
  differ a little from the game; the tuning constants are named and
  commented in the shader.
- A map's skybox only shows if its images are found in the loaded archives.

- Compressed entries (`type & Compressed`) in `.pak` files can't be previewed
  or extracted — that was already true of `UnpackToDirectory`; `ExtractEntry`
  just surfaces it as a message instead of a crash. (`.nar` type-2
  “encoded+compressed” entries are fully supported.)
- NAR repacking is semantically lossless, not byte-identical. Every entry's
  decoded content, path, storage type and timestamp survive, but entries
  stored as type 2 are recompressed by this project's own greedy LZ encoder,
  so their stored bytes, sizes, CRCs and offsets can differ from the retail
  packer's. `nar-patch` copies untouched entries byte-for-byte, and raw-only
  archives come out byte-identical in the data region; the bzip2 directory
  stream itself is standards-compliant but may differ from the original
  packer's output.
- `nar-unpack` sets file mtimes only for entries whose directory timestamp is
  non-zero (zero means “no timestamp”). Repacking such a file gives it the
  current time instead. No such entries were found in the retail archives
  checked so far.
- Individual lumps inside a `.wad` can be previewed and saved as images, but
  not extracted as raw lumps: right-click one lump → **Save Texture As
  Image...** (PNG/BMP/JPEG) or several → **Save N Textures As PNG...**
  (to a folder; PNG keeps the transparency of `{` cut-outs). "Extract
  Selected" only knows how to pull a real, top-level pak/nar entry out, and
  a WAD lump isn't one (it's nested inside the `.wad`'s own bytes).
  Extracting the `.wad` file itself works normally and gets you the whole
  thing, lumps included.
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
  directly (forcing a second frame of the same model showed `GL_BLEND`
  already enabled on entry, before the reset ran).

## NAR write support and ICE models

All of this lives in `core/` and has no Qt dependency:

| File | What it does |
|---|---|
| `IceCipher`, `ModelCrypt` | ICE (level 4) and CSO model encryption: `.mdl` version 20/21 <-> plain (version 9). Encrypts texture pixels+palette and vertex arrays. |
| `NarWriter`, `NarBZip2Enc.cpp` | Writes `.nar`: LZ compressor (type 2), XOR encoder (type 1), raw (type 0), bzip2-compressed directory, CRC-32. Temp file + rename, nothing is touched if a step fails. |
| `NarTools` | `ExtractNarToDirectory`, `PackDirectoryToNar`, `PatchNar` + `.nar-info.json` manifest (compatible with CSNZ-Nar's manifest schema). |
| `NarCli` | CLI commands below. |

`NarArchive` additionally exposes `Entry::timestamp`, `ReadStored()` and `VerifyEntry()` (CRC-32 of the stored bytes).
`StudioModel::Load` decrypts ICE models (v20/21) transparently and accepts plain v9/v10/v11.

CLI (`cso-pak-cli`):

    nar-list   <archive.nar> [--verify]
    nar-unpack <archive.nar> [output_dir] [--keep-encrypted] [--verify] [--no-manifest]
    nar-extract <archive.nar> <path_text> [output_dir] [--keep-encrypted] [--verify]   (single entries, fast on huge archives)
    nar-pack   <input_dir> <output.nar> [--type raw|enc|lz] [--root NAME] [--encrypt-models 20|21] [--no-manifest]
    nar-patch  <source.nar> <replacement_dir> <output.nar> [--type ...] [--add-new --root NAME] [--encrypt-models 20|21]

`nar-unpack` decrypts models and writes `.nar-info.json`; `nar-pack` on that folder restores storage types,
model encryption, data-block order and directory order automatically (the manifest is searched in the
input folder and up to two parents, and is never packed itself). Without a manifest the input folder's name
becomes the archive root (`cstrike/` -> `/cstrike/...`), override with `--root` (or `--root ""` for no root);
files not listed in a manifest use `--type`, whose default is `enc` (XOR-encoded, no compression), and plain
`.mdl` files stay unencrypted unless `--encrypt-models 20|21` is given. `nar-patch` copies untouched entries
byte-for-byte (no re-compression) and keeps replaced ICE models encrypted; `--add-new` requires `--root`
because new files have no archive path to inherit.

### NAR in the GUI (File menu)

- **Pack Directory into New NAR...** — same as `nar-pack`: store type for new files (Encoded by default,
  LZ, or Raw), ICE encryption for plain `.mdl` files (v20/v21 or leave as is), optional reuse of
  `.nar-info.json`, and an optional archive root (empty = use the input folder's name; a checkbox for no
  root at all).
- **Patch NAR with Replacements...** — same as `nar-patch`: replace files by full archive path or unique
  path suffix, optionally add new files under a root folder, and choose the store type and ICE version for
  new files. Replaced models that were ICE-encrypted stay encrypted automatically. The output must be a
  different file from the source.
- **Unpack NAR to Directory...** — same as `nar-unpack`: optional ICE decryption, `.nar-info.json`
  manifest, and optional CRC verification of every entry.
- **Verify NAR Archive Integrity...** — recomputes the CRC-32 of every entry in every loaded `.nar` and
  reports mismatches; enabled whenever at least one `.nar` is open.
