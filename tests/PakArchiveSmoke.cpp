#include "PakArchive.h"
#include "SnowCipher.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
	constexpr std::u16string_view PakKey = u"CqeLFV@*0IfewH(";
	constexpr std::u16string_view FixtureEntryPath = u"custom/content/message.txt";
	constexpr uint8_t PakVersion = 2;
	constexpr uint32_t PakTypeEncrypted = 0x2;
	constexpr uint32_t PakTypeEncryptedAgain = 0x4;
	constexpr size_t HeaderSize = 0xC;
	constexpr size_t DataBlockSize = 0x400;
	constexpr size_t BaseKeyWordCount = 4;
	constexpr size_t BaseKeyByteCount = BaseKeyWordCount * sizeof(uint32_t);

	void Expect(bool condition, std::string_view message)
	{
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

	uint8_t ByteAt(uint32_t value, size_t bytePos)
	{
		return static_cast<uint8_t>(value >> (bytePos * 8));
	}

	uint32_t ToU32(size_t value, const char *name)
	{
		if (value > std::numeric_limits<uint32_t>::max())
			throw std::runtime_error(std::string(name) + " is too large");

		return static_cast<uint32_t>(value);
	}

	size_t AlignUp(size_t value, size_t alignment)
	{
		return alignment * ((value + alignment - 1) / alignment);
	}

	template <typename CharT>
	size_t SumChars(std::basic_string_view<CharT> value)
	{
		size_t result = 0;
		for (const auto ch : value)
			result += ch;

		return result;
	}

	template <typename CharT>
	size_t SpecialSumChars(std::basic_string_view<CharT> value)
	{
		size_t result = 0;
		for (const auto ch : value)
			result += ch + ch * 2;

		return result;
	}

	void WriteU32Le(std::vector<uint8_t> &data, uint32_t value)
	{
		data.push_back(ByteAt(value, 0));
		data.push_back(ByteAt(value, 1));
		data.push_back(ByteAt(value, 2));
		data.push_back(ByteAt(value, 3));
	}

	void WriteU32LeAt(std::vector<uint8_t> &data, size_t offset, uint32_t value)
	{
		data[offset] = ByteAt(value, 0);
		data[offset + 1] = ByteAt(value, 1);
		data[offset + 2] = ByteAt(value, 2);
		data[offset + 3] = ByteAt(value, 3);
	}

	size_t HeaderOffset(std::u16string_view filename)
	{
		return SumChars<char16_t>(filename) % 312 + 30;
	}

	size_t EntriesOffset(std::u16string_view filename, size_t headerOffset)
	{
		return headerOffset + 42 + (SpecialSumChars<char16_t>(filename) % 212);
	}

	uint8_t BaseKeyByte(std::span<const uint32_t, BaseKeyWordCount> baseKey,
		size_t byteIndex)
	{
		return ByteAt(baseKey[byteIndex / sizeof(uint32_t)],
			byteIndex % sizeof(uint32_t));
	}

	std::array<uint8_t, 128> GenerateHeaderKey(std::u16string_view filename)
	{
		std::array<uint8_t, 128> result = {};
		std::u16string fullKey(filename);
		fullKey += PakKey;

		for (size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<uint8_t>(i + fullKey[i % fullKey.size()]);

		return result;
	}

	std::array<uint8_t, 128> GenerateEntriesKey(std::u16string_view filename)
	{
		std::array<uint8_t, 128> result = {};
		std::u16string fullKey(filename);
		fullKey += PakKey;
		const auto keyLen = fullKey.length();

		for (size_t i = 0; i < result.size(); ++i)
		{
			const size_t cycleOffset = (i % 3) + 2;
			const char16_t keyChar = fullKey[keyLen - i % keyLen - 1];
			result[i] = static_cast<uint8_t>(i + cycleOffset * keyChar);
		}

		return result;
	}

	std::array<uint8_t, 128> GenerateDataKey(std::u16string_view filename,
		std::span<const uint32_t, BaseKeyWordCount> baseKey)
	{
		std::array<uint8_t, 128> result = {};

		for (size_t i = 0; i < result.size(); ++i)
		{
			const char16_t filenameChar = filename[i % filename.size()];
			const uint8_t baseKeyByte = BaseKeyByte(baseKey, i % BaseKeyByteCount);
			const size_t cycleOffset = (i % 5) + 2;
			result[i] = static_cast<uint8_t>(
				i + filenameChar * (baseKeyByte + cycleOffset));
		}

		return result;
	}

	void EncryptAligned(std::span<uint8_t> data, std::span<const uint8_t> key)
	{
		Expect((data.size() % sizeof(uint32_t)) == 0, "test pak data is not aligned");

		SnowCipher cipher;
		cipher.SetKey(key.data());
		cipher.EncryptBuffer(data.data(), data.data(), ToU32(data.size(), "test encrypt length"));
	}

	struct FixtureEntry
	{
		std::u16string path = std::u16string(FixtureEntryPath);
		uint32_t entryChecksum = 0;
		uint32_t type = PakTypeEncrypted | PakTypeEncryptedAgain;
		uint32_t fileOffset = 0;
		uint32_t realSize = 0;
		uint32_t packedSize = 0;
		std::array<uint32_t, BaseKeyWordCount> baseKey = {
			0x11223344,
			0x55667788,
			0x99AABBCC,
			0xDDEEFF00
		};
	};

	uint32_t CalculateEntryChecksum(const FixtureEntry &entry)
	{
		uint32_t baseKeySum = 0;
		for (size_t i = 0; i < BaseKeyByteCount; ++i)
			baseKeySum += BaseKeyByte(entry.baseKey, i);

		return entry.fileOffset + entry.realSize + entry.packedSize +
			entry.type + baseKeySum;
	}

	std::vector<uint8_t> Bytes(std::string_view value)
	{
		std::vector<uint8_t> result;
		result.reserve(value.size());
		for (const char ch : value)
			result.push_back(static_cast<uint8_t>(ch));

		return result;
	}

	std::vector<uint8_t> ReadFile(const std::filesystem::path &path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
			throw std::runtime_error("failed to open test file: " + path.string());

		const auto size = input.tellg();
		if (size < 0)
			throw std::runtime_error("failed to read test file size: " + path.string());

		std::vector<uint8_t> data(static_cast<size_t>(size));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char *>(data.data()),
			static_cast<std::streamsize>(data.size()));
		if (!input && !data.empty())
			throw std::runtime_error("failed to read test file: " + path.string());

		return data;
	}

	void WriteFile(const std::filesystem::path &path, std::span<const uint8_t> data)
	{
		std::filesystem::create_directories(path.parent_path());

		std::ofstream output(path, std::ios::binary);
		if (!output)
			throw std::runtime_error("failed to open test file: " + path.string());

		output.write(reinterpret_cast<const char *>(data.data()),
			static_cast<std::streamsize>(data.size()));
		if (!output)
			throw std::runtime_error("failed to write test file: " + path.string());
	}

	std::vector<uint8_t> EncryptEntryData(const FixtureEntry &entry,
		std::vector<uint8_t> storedData)
	{
		storedData.resize(entry.packedSize, 0);
		const auto dataKey = GenerateDataKey(entry.path, entry.baseKey);

		// Match the production packer: top block first, then the whole payload.
		const size_t topBytes = std::min({
			storedData.size(),
			DataBlockSize
		});
		const size_t alignedTopBytes = topBytes - (topBytes % sizeof(uint32_t));
		if (alignedTopBytes != 0)
			EncryptAligned({ storedData.data(), alignedTopBytes }, dataKey);

		EncryptAligned(storedData, dataKey);
		return storedData;
	}

	std::vector<uint8_t> BuildHeader(size_t entryCount)
	{
		std::vector<uint8_t> header(HeaderSize, 0);
		const uint32_t count = ToU32(entryCount, "fixture entry count");
		WriteU32LeAt(header, 0, PakVersion + count);
		header[4] = PakVersion;
		WriteU32LeAt(header, 5, count);
		return header;
	}

	std::vector<uint8_t> BuildEntriesPlain(const FixtureEntry &entry)
	{
		std::vector<uint8_t> data;
		WriteU32Le(data, ToU32(entry.path.size(), "fixture path length"));
		for (const char16_t ch : entry.path)
		{
			data.push_back(ByteAt(ch, 0));
			data.push_back(ByteAt(ch, 1));
		}

		WriteU32Le(data, entry.entryChecksum);
		WriteU32Le(data, entry.type);
		WriteU32Le(data, entry.fileOffset);
		WriteU32Le(data, entry.realSize);
		WriteU32Le(data, entry.packedSize);
		for (const uint32_t word : entry.baseKey)
			WriteU32Le(data, word);

		data.resize(AlignUp(data.size(), sizeof(uint32_t)), 0);
		return data;
	}

	void WriteCustomPak(const std::filesystem::path &pakPath,
		std::span<const uint8_t> plain)
	{
		FixtureEntry entry;

		std::vector<uint8_t> storedData(plain.begin(), plain.end());
		entry.realSize = ToU32(plain.size(), "fixture file size");
		entry.packedSize = ToU32(AlignUp(storedData.size(), sizeof(uint32_t)),
			"fixture packed size");
		entry.entryChecksum = CalculateEntryChecksum(entry);

		const auto filename = pakPath.filename().generic_u16string();
		const size_t headerOffset = HeaderOffset(filename);
		const size_t entriesOffset = EntriesOffset(filename, headerOffset);

		auto header = BuildHeader(1);
		EncryptAligned(header, GenerateHeaderKey(filename));

		auto entriesPlain = BuildEntriesPlain(entry);
		EncryptAligned(entriesPlain, GenerateEntriesKey(filename));

		auto encryptedData = EncryptEntryData(entry, std::move(storedData));
		const size_t dataStartOffset =
			AlignUp(entriesOffset + entriesPlain.size(), DataBlockSize);
		std::vector<uint8_t> pak(dataStartOffset + DataBlockSize, 0);

		std::ranges::copy(header,
			pak.begin() + static_cast<std::ptrdiff_t>(headerOffset));
		std::ranges::copy(entriesPlain,
			pak.begin() + static_cast<std::ptrdiff_t>(entriesOffset));
		std::ranges::copy(encryptedData,
			pak.begin() + static_cast<std::ptrdiff_t>(dataStartOffset));

		WriteFile(pakPath, pak);
	}

	void WriteCustomPak(const std::filesystem::path &pakPath)
	{
		const auto plain = Bytes("custom pak fixture\n");
		WriteCustomPak(pakPath, plain);
	}

	void CheckStats(const cso_pak::PackStats &stats, size_t total, size_t packed)
	{
		Expect(stats.totalEntries == total, "unexpected total pack entry count");
		Expect(stats.packedEntries == packed, "unexpected packed entry count");
	}

	void CheckStats(const cso_pak::PatchStats &stats, size_t total, size_t replaced,
		size_t preserved)
	{
		Expect(stats.totalEntries == total, "unexpected total entry count");
		Expect(stats.replacedEntries == replaced, "unexpected replaced entry count");
		Expect(stats.preservedEntries == preserved, "unexpected preserved entry count");
	}

	void CheckStats(const cso_pak::UnpackStats &stats, size_t total, size_t written)
	{
		Expect(stats.totalEntries == total, "unexpected total unpack entry count");
		Expect(stats.writtenEntries == written, "unexpected written entry count");
	}

	void ExpectFileBytes(const std::filesystem::path &path,
		std::span<const uint8_t> expected)
	{
		const auto actual = ReadFile(path);
		Expect(actual == std::vector<uint8_t>(expected.begin(), expected.end()),
			"unexpected unpacked file content");
	}
}

int main(int argc, char **argv)
{
	try
	{
		if (argc != 2)
			throw std::runtime_error("usage: pakarchive_smoke <work_dir>");

		const std::filesystem::path workDir = argv[1];

		std::filesystem::remove_all(workDir);
		std::filesystem::create_directories(workDir);

		const auto sourcePak = workDir / "custom_source.pak";
		WriteCustomPak(sourcePak);

		const auto original = cso_pak::PakArchive::Load(sourcePak);
		const auto sourceUnpackRoot = workDir / "unpacked_source";
		CheckStats(original.UnpackToDirectory(sourceUnpackRoot), 1, 1);
		const auto sourceExpected = Bytes("custom pak fixture\n");
		ExpectFileBytes(sourceUnpackRoot / "custom" / "content" / "message.txt",
			sourceExpected);

		const auto emptyReplacementRoot = workDir / "empty";
		std::filesystem::create_directories(emptyReplacementRoot);
		const auto emptyOutput = workDir / "custom_empty_patch.pak";
		CheckStats(original.PatchFromDirectory(emptyReplacementRoot, emptyOutput), 1, 0, 1);
		(void)cso_pak::PakArchive::Load(emptyOutput);

		const auto replacementRoot = workDir / "replacement";
		const auto replacementPath = replacementRoot / "custom" / "content" / "message.txt";
		WriteFile(replacementPath, Bytes("replacement pak fixture\n"));

		const auto replacementOutput = workDir / "custom_replacement_patch.pak";
		CheckStats(original.PatchFromDirectory(replacementRoot, replacementOutput), 1, 1, 0);
		const auto replacementArchive = cso_pak::PakArchive::Load(replacementOutput);
		const auto replacementUnpackRoot = workDir / "unpacked_replacement";
		CheckStats(replacementArchive.UnpackToDirectory(replacementUnpackRoot), 1, 1);
		ExpectFileBytes(replacementUnpackRoot / "custom" / "content" / "message.txt",
			Bytes("replacement pak fixture\n"));

		const auto packInputRoot = workDir / "pack_input";
		const std::vector<uint8_t> extraContent = {
			0x00, 0x01, 0x02, 'e', 'x', 't', 'r', 'a', ' ', 'd', 'a', 't', 'a'
		};
		WriteFile(packInputRoot / "custom" / "content" / "message.txt",
			Bytes("packed pak fixture\n"));
		WriteFile(packInputRoot / "custom" / "content" / "extra.bin",
			extraContent);

		const auto packedOutput = workDir / "custom_packed.pak";
		CheckStats(cso_pak::PakArchive::PackDirectory(packInputRoot, packedOutput), 2, 2);
		const auto packedArchive = cso_pak::PakArchive::Load(packedOutput);
		const auto packedUnpackRoot = workDir / "unpacked_packed";
		CheckStats(packedArchive.UnpackToDirectory(packedUnpackRoot), 2, 2);
		ExpectFileBytes(packedUnpackRoot / "custom" / "content" / "message.txt",
			Bytes("packed pak fixture\n"));
		ExpectFileBytes(packedUnpackRoot / "custom" / "content" / "extra.bin",
			extraContent);
	}
	catch (const std::exception &ex)
	{
		std::cerr << ex.what() << '\n';
		return 1;
	}

	return 0;
}
