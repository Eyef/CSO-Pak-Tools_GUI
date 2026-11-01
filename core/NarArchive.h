#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cso_pak
{
	// Reader for Nexon .nar archives (Counter-Strike Online's Data/*.nar).
	// Layout (see NARTools' NARToolLib, used here as a format reference):
	//   [8 bytes: "NAR\0" + version] [file blobs...] [XORed+bzip2 directory]
	//   [4 bytes: directory size XOR 0x4076551F] [4 bytes: "NAR\0"].
	// The directory lists path (EUC-KR bytes), storage type (0 = raw,
	// 1 = XOR-encoded, 2 = XOR-encoded + custom LZSS-compressed), offsets
	// and sizes. The per-file XOR keystream derives from the entry path
	// (Python-string-hash LCG, 16-byte cycle).
	//
	// Load() parses the directory only; ExtractEntry() reads/decodes on
	// demand, so multi-gigabyte archives never sit fully in RAM. Throws
	// std::runtime_error on malformed input.
	class NarArchive
	{
	public:
		struct Entry
		{
			// Raw path bytes as stored (EUC-KR; plain ASCII for the game's
			// model/texture paths, which is all the viewer matches on).
			std::string path;
			uint32_t offset = 0;
			uint32_t storedSize = 0;
			uint32_t extractedSize = 0;
			uint32_t type = 0;  // 0 = raw, 1 = encoded, 2 = encoded+compressed
			uint32_t checksum = 0;

			// ASCII-widened path for display/matching (non-ASCII -> '?').
			std::u16string generic_u16string() const;
		};

		static NarArchive Load(const std::filesystem::path &narPath);

		NarArchive() = default;
		~NarArchive();
		NarArchive(const NarArchive &) = delete;
		NarArchive &operator=(const NarArchive &) = delete;
		NarArchive(NarArchive &&other) noexcept;
		NarArchive &operator=(NarArchive &&other) noexcept;

		const std::vector<Entry> &Entries() const { return entries_; }
		const std::filesystem::path &SourcePath() const { return sourcePath_; }

		// Raw file name (last path component) for lookups.
		static std::string FileNameOf(const std::string &path);

		std::vector<uint8_t> ExtractEntry(const Entry &entry) const;

	private:
		std::vector<Entry> entries_;
		std::filesystem::path sourcePath_;
		uint64_t fileSize_ = 0;
		mutable void *stream_ = nullptr;  // opened binary ifstream, lazy
	};
}
