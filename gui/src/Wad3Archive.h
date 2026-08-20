#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// One directory entry from a WAD2 (Quake) / WAD3 (Half-Life / GoldSource)
	// archive.
	//
	// Format reference: the WAD2 directory layout was published by id
	// Software as part of the Quake source release and is documented in the
	// classic community spec (id Software's Quake source release; see e.g.
	// the QuakeSpec/qfiles.h "wadinfo_t"/"lumpinfo_t" description). WAD3, used
	// by the Half-Life / GoldSource engine (and, in this app's case, its
	// Counter-Strike Online derivative), reuses the exact same 32-byte
	// directory entry layout and only changes one byte of the 4-byte magic
	// ("WAD2" -> "WAD3"). On-disk layout, little-endian:
	//
	//   offset 0:  int32  lump data offset in the file
	//   offset 4:  int32  lump size on disk ("dsize")
	//   offset 8:  int32  lump size once decompressed ("size")
	//   offset 12: int8   lump type
	//   offset 13: int8   compression flag (0 = stored, non-zero = compressed)
	//   offset 14: int16  padding, unused
	//   offset 16: char[16] lump name, NUL-padded, not necessarily
	//                        NUL-terminated if it fills all 16 bytes
	//
	// (32 bytes total per entry.)
	struct WadEntry
	{
		std::string name;
		uint32_t offset = 0;
		uint32_t diskSize = 0;  // on-disk size
		uint32_t memSize = 0;   // decompressed size (only differs from diskSize when compressed)
		uint8_t type = 0;       // Raw format type byte; shown as-is, not decoded --
		                        // see LoadWadMiptex for why entries are identified
		                        // by structural validation instead.
		bool compressed = false;
	};

	// A parsed WAD2/WAD3 directory. Mirrors PakArchive's Load()/ExtractEntry()
	// split: Load() parses the directory and keeps its own copy of the raw
	// bytes, ExtractEntry() slices a lump's bytes back out of that buffer.
	class Wad3Archive
	{
	public:
		// Throws std::runtime_error on a malformed or truncated file.
		static Wad3Archive Load(std::vector<uint8_t> data);

		const std::vector<WadEntry> &Entries() const { return entries_; }
		// false = WAD2 (Quake), true = WAD3 (Half-Life/GoldSource). CSO's own
		// files are WAD3, same as the rest of its GoldSource-derived engine.
		bool IsWad3() const { return isWad3_; }

		// Throws for a compressed lump (unsupported -- real GoldSource WAD3
		// files essentially never use this despite the format allowing it)
		// or an out-of-range entry.
		std::vector<uint8_t> ExtractEntry(const WadEntry &entry) const;

	private:
		Wad3Archive() = default;

		std::vector<uint8_t> buffer_;
		std::vector<WadEntry> entries_;
		bool isWad3_ = false;
	};

	// Decodes a GoldSource "miptex" lump (mip level 0 only) to an image.
	// miptex is the standard WAD3 texture format: a 40-byte header (name,
	// width, height, 4 mip-level offsets) followed by all 4 mip levels'
	// palette-index pixel data, then one shared 256-color RGB palette.
	//
	// There's no reliable type-byte value to gate this on (WAD2/WAD3 has
	// several lump types, and getting the exact numeric constant wrong would
	// silently misclassify real textures), so this instead validates the
	// lump's own internal structure -- plausible dimensions, mip0 starting
	// exactly after the 40-byte header, enough bytes for all 4 mips plus a
	// trailing palette -- and throws std::runtime_error if it doesn't look
	// like a real miptex. Same "try it, throw on a bad shape" approach as
	// the TGA/DDS decoders elsewhere in this app.
	//
	// `entryName` is used only for the classic GoldSource convention where a
	// texture named with a leading '{' treats palette index 255 as a
	// transparency cutout (matching the equivalent studiomdl texture logic).
	QImage LoadWadMiptex(const std::vector<uint8_t> &lumpData, const std::string &entryName);
}
