#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cso_pak
{
	// Bits of Entry::type. Kept here (instead of only inside PakArchive.cpp)
	// so callers such as the GUI can interpret an entry without duplicating
	// the format constants.
	enum EntryTypeFlags : uint32_t
	{
		EntryTypeCompressed = 0x1,
		EntryTypeEncrypted = 0x2,
		EntryTypeEncryptedAgain = 0x4,
	};

	struct PackStats
	{
		size_t totalEntries = 0;
		size_t packedEntries = 0;
	};

	struct PatchStats
	{
		size_t totalEntries = 0;
		size_t replacedEntries = 0;
		size_t preservedEntries = 0;
	};

	struct UnpackStats
	{
		size_t totalEntries = 0;
		size_t writtenEntries = 0;
	};

	class PakArchive
	{
	public:
		struct Entry
		{
			std::u16string path;
			// Stored entry checksum:
			// fileOffset + realSize + packedSize + type + sum(BaseKey bytes).
			uint32_t entryChecksum = 0;
			uint32_t type = 0;
			uint32_t fileOffset = 0;
			uint32_t realSize = 0;
			uint32_t packedSize = 0;
			std::array<uint32_t, 4> baseKey = {};
		};

		static PakArchive Load(const std::filesystem::path &pakPath);

		static PackStats PackDirectory(const std::filesystem::path &inputRoot,
			const std::filesystem::path &outputPakPath);

		UnpackStats UnpackToDirectory(const std::filesystem::path &outputRoot) const;

		PatchStats PatchFromDirectory(const std::filesystem::path &replacementRoot,
			const std::filesystem::path &outputPakPath) const;

		// Read-only access for tooling (e.g. the GUI browser) that wants to
		// list entries and preview individual files without unpacking the
		// whole archive to disk.
		const std::filesystem::path &SourcePath() const { return sourcePath_; }
		const std::vector<Entry> &Entries() const { return entries_; }

		// Decrypts (and, for entries that are not compressed, trims) a single
		// entry's data straight from the in-memory archive buffer. Throws
		// std::runtime_error for entries whose type has the Compressed flag
		// set, since compressed payloads are not supported yet.
		std::vector<uint8_t> ExtractEntry(const Entry &entry) const;

	private:
		std::filesystem::path sourcePath_;
		std::u16string sourceFilename_;
		std::vector<uint8_t> buffer_;
		std::vector<Entry> entries_;
		size_t dataStartOffset_ = 0;

		PakArchive() = default;
	};
}
