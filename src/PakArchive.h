#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cso_pak
{
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

	private:
		std::filesystem::path sourcePath_;
		std::u16string sourceFilename_;
		std::vector<uint8_t> buffer_;
		std::vector<Entry> entries_;
		size_t dataStartOffset_ = 0;

		PakArchive() = default;
	};
}
