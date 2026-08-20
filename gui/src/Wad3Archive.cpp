#include "Wad3Archive.h"

#include <stdexcept>

#include "ByteCursor.h"

namespace cso_gui
{
namespace
{
	constexpr uint32_t kDirEntrySize = 32;       // bytes per directory entry, see Wad3Archive.h
	constexpr uint32_t kMaxLumps = 100000;       // guard against a corrupt lump count causing a huge allocation
	constexpr uint32_t kMaxMiptexDimension = 4096;

	// Parses a single 32-byte directory entry starting at the cursor's
	// current position and advances it past the entry.
	WadEntry ParseDirEntry(ByteCursor &dir)
	{
		const int32_t offset = dir.I32();
		const int32_t diskSize = dir.I32();
		const int32_t memSize = dir.I32();
		const uint8_t type = dir.U8();
		const uint8_t compressedFlag = dir.U8();
		dir.Bytes(2); // unused padding field
		const uint8_t *rawName = dir.Bytes(16);

		if (offset < 0 || diskSize < 0 || memSize < 0)
			throw std::runtime_error("WAD directory entry has a negative offset or size");

		WadEntry entry;
		entry.offset = static_cast<uint32_t>(offset);
		entry.diskSize = static_cast<uint32_t>(diskSize);
		entry.memSize = static_cast<uint32_t>(memSize);
		entry.type = type;
		entry.compressed = compressedFlag != 0;

		// The 16-byte name field is NUL-padded but may not be NUL-terminated
		// if the name fills the whole field, so find the terminator manually
		// instead of assuming one exists.
		size_t len = 0;
		while (len < 16 && rawName[len] != 0)
			++len;
		entry.name.assign(reinterpret_cast<const char *>(rawName), len);

		return entry;
	}
}

Wad3Archive Wad3Archive::Load(std::vector<uint8_t> data)
{
	if (data.size() < 12)
		throw std::runtime_error("file is too small to be a WAD2/WAD3 archive");

	ByteCursor header(data);

	const uint8_t *magic = header.Bytes(4);
	const bool looksLikeWad2 = magic[0] == 'W' && magic[1] == 'A' && magic[2] == 'D' && magic[3] == '2';
	const bool looksLikeWad3 = magic[0] == 'W' && magic[1] == 'A' && magic[2] == 'D' && magic[3] == '3';
	if (!looksLikeWad2 && !looksLikeWad3)
		throw std::runtime_error("not a WAD2/WAD3 archive (bad \"WAD2\"/\"WAD3\" magic)");

	const uint32_t numLumps = header.U32();
	const uint32_t dirOffset = header.U32();

	if (numLumps > kMaxLumps)
		throw std::runtime_error("WAD archive reports an implausible number of entries");

	const uint64_t dirEnd = static_cast<uint64_t>(dirOffset) + static_cast<uint64_t>(numLumps) * kDirEntrySize;
	if (dirOffset > data.size() || dirEnd > data.size())
		throw std::runtime_error("WAD directory is out of range (truncated or corrupt file)");

	Wad3Archive archive;
	archive.isWad3_ = looksLikeWad3;
	archive.buffer_ = std::move(data);
	archive.entries_.reserve(numLumps);

	ByteCursor dir(archive.buffer_);
	dir.Seek(dirOffset);

	for (uint32_t i = 0; i < numLumps; ++i)
	{
		WadEntry entry = ParseDirEntry(dir);

		const uint64_t entryEnd = static_cast<uint64_t>(entry.offset) + entry.diskSize;
		if (entryEnd > archive.buffer_.size())
			throw std::runtime_error("WAD entry '" + entry.name + "' data is out of range");

		archive.entries_.push_back(std::move(entry));
	}

	return archive;
}

std::vector<uint8_t> Wad3Archive::ExtractEntry(const WadEntry &entry) const
{
	if (entry.compressed)
		throw std::runtime_error("this WAD entry is compressed, which isn't supported");

	const uint64_t entryEnd = static_cast<uint64_t>(entry.offset) + entry.diskSize;
	if (entryEnd > buffer_.size())
		throw std::runtime_error("WAD entry data is out of range");

	const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(entry.offset);
	return std::vector<uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(entry.diskSize));
}

QImage LoadWadMiptex(const std::vector<uint8_t> &lump, const std::string &entryName)
{
	ByteCursor cursor(lump);

	cursor.Bytes(16); // embedded texture name, redundant with the directory entry's own name; not re-validated
	const uint32_t width = cursor.U32();
	const uint32_t height = cursor.U32();

	uint32_t mipOffset[4];
	for (uint32_t &offset : mipOffset)
		offset = cursor.U32();

	if (width == 0 || height == 0 || width > kMaxMiptexDimension || height > kMaxMiptexDimension)
		throw std::runtime_error("not a miptex (implausible dimensions)");
	if ((width % 8) != 0 || (height % 8) != 0)
		throw std::runtime_error("not a miptex (dimensions aren't multiples of 8)");
	if (mipOffset[0] != 40)
		throw std::runtime_error("not a miptex (mip level 0 doesn't start right after the header)");

	const size_t mip0Size = static_cast<size_t>(width) * height;
	if (static_cast<uint64_t>(mipOffset[0]) + mip0Size > lump.size())
		throw std::runtime_error("miptex lump is truncated (mip level 0)");

	// The palette immediately follows mip level 3's pixel data.
	const uint32_t mip3Width = width >> 3;
	const uint32_t mip3Height = height >> 3;
	const uint64_t paletteOffset = static_cast<uint64_t>(mipOffset[3]) +
		static_cast<uint64_t>(mip3Width) * mip3Height;
	if (paletteOffset + 2 > lump.size())
		throw std::runtime_error("miptex lump is truncated (no palette)");

	const uint16_t paletteCount = static_cast<uint16_t>(lump[paletteOffset] | (lump[paletteOffset + 1] << 8));
	if (paletteCount == 0 || paletteCount > 256)
		throw std::runtime_error("miptex lump has an invalid palette size");

	const uint64_t paletteDataOffset = paletteOffset + 2;
	if (paletteDataOffset + static_cast<uint64_t>(paletteCount) * 3 > lump.size())
		throw std::runtime_error("miptex lump is truncated (palette data)");

	// Classic GoldSource convention: a texture named with a leading '{' uses
	// palette index 255 as a transparency cutout key, same as studiomdl.
	const bool masked = !entryName.empty() && entryName.front() == '{';

	QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
	for (uint32_t y = 0; y < height; ++y)
	{
		auto *destLine = reinterpret_cast<QRgb *>(image.scanLine(static_cast<int>(y)));
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint8_t index = lump[mipOffset[0] + static_cast<size_t>(y) * width + x];

			if (masked && index == 255)
			{
				destLine[x] = qRgba(0, 0, 0, 0);
				continue;
			}

			if (index >= paletteCount)
			{
				destLine[x] = qRgb(0, 0, 0); // out-of-range index in a malformed file: fall back to black
				continue;
			}

			const size_t p = static_cast<size_t>(paletteDataOffset) + static_cast<size_t>(index) * 3;
			destLine[x] = qRgb(lump[p], lump[p + 1], lump[p + 2]);
		}
	}

	return image;
}
}
