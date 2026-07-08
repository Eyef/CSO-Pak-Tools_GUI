# CSO Pak Tool

`cso-pak-tool` is a command-line tool for extracting and rebuilding
Counter-Strike: Online PAK archives. It can unpack decrypted files to a
directory, pack a directory into a new `.pak`, or patch an existing `.pak` with
selected replacement files.

## Features

- Parses encrypted CSO PAK headers and entry tables.
- Unpacks decrypted entry files to a normal directory tree.
- Packs a normal directory tree into a new encrypted PAK archive.
- Preserves original entries when no replacement file is provided.
- Re-encrypts replaced files with the entry-specific data key.
- Recomputes entry checksums when offsets or sizes change.
- Rebuilds archive data on `0x400`-byte block boundaries.

## Requirements

- CMake 3.16 or newer.
- A C++20 compiler.

## Build

```sh
cmake -S . -B build
cmake --build build
```

The main executable is:

```sh
build/cso-pak-tool
```

## Usage

```sh
cso-pak-tool unpack <source.pak> [output_root]
cso-pak-tool unpack <source_dir> [output_root]
cso-pak-tool pack <input_dir> <output.pak>
cso-pak-tool patch <source.pak> <replacement_dir> <output.pak>
```

Unpack example:

```sh
cso-pak-tool unpack source.pak extracted
```

Files are always written under `output_root` using the paths stored inside the
PAK archive. If `output_root` is omitted, the archive is extracted into the
source PAK's directory:

```sh
cso-pak-tool unpack source.pak
```

Batch unpack example:

```sh
cso-pak-tool unpack pak_dir extracted
```

When `source_dir` is used, all `.pak` files under that directory are unpacked
recursively. Each archive writes its internal paths under `output_root`. If
`output_root` is omitted, each archive is extracted into its own source
directory. Files without a `.pak` extension are ignored.

Pack example:

```sh
cso-pak-tool pack extracted output.pak
```

`pack` creates a new archive from every regular file under `input_dir`; it does
not require a source PAK.

Patch example:

```sh
cso-pak-tool patch source.pak replacement output.pak
```

Replacement paths must match the paths stored inside the PAK archive. For
example, if the archive contains:

```text
lstrike/common/maps/zm_oasis_detail.txt
```

then the replacement file must be placed at:

```text
replacement/lstrike/common/maps/zm_oasis_detail.txt
```

Entries missing from `replacement_dir` are copied from the source PAK unchanged.
When a source entry is compressed, `patch` can preserve the original encrypted
blob if no replacement file is provided, but it refuses to replace that entry
until the compression algorithm is known. `pack` currently creates encrypted,
uncompressed entries by default.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

## Credits

Parts of the PAK parsing, key generation, encryption, and archive layout logic
were studied from and rewritten based on these projects:

- `cso-pak`: https://git.sr.ht/~leite/cso-pak
