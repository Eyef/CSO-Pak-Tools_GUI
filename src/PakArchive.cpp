#include "PakArchive.h"

#include "SnowCipher.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>

namespace cso_pak
{
namespace
{
	// Archive-wide constants recovered from the original PAK layout.
	constexpr std::u16string_view PakKey = u"CqeLFV@*0IfewH(";
	constexpr uint8_t PakVersion = 2;
	// The compressed flag is recognized only to reject unsupported entries for now.
	constexpr uint32_t PakTypeCompressed = 0x1;
	constexpr uint32_t PakTypeEncrypted = 0x2;
	constexpr uint32_t PakTypeEncryptedAgain = 0x4;
	constexpr uint32_t MaxPathChars = 16384;
	constexpr size_t HeaderSize = 0xC;
	constexpr size_t DataBlockSize = 0x400;
	constexpr size_t BaseKeyWordCount = 4;
	constexpr size_t BaseKeyByteCount = BaseKeyWordCount * sizeof(uint32_t);

	uint32_t ReadU32Le(const uint8_t *data)
	{
		// PAK metadata is always little-endian, independent of host endianness.
		return static_cast<uint32_t>(data[0]) |
			(static_cast<uint32_t>(data[1]) << 8) |
			(static_cast<uint32_t>(data[2]) << 16) |
			(static_cast<uint32_t>(data[3]) << 24);
	}

	uint8_t ByteAt(uint32_t value, size_t bytePos)
	{
		return static_cast<uint8_t>(value >> (bytePos * 8));
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

	size_t AlignUp(size_t value, size_t alignment)
	{
		return alignment * ((value + alignment - 1) / alignment);
	}

	uint32_t ToU32(size_t value, const char *name)
	{
		if (value > std::numeric_limits<uint32_t>::max())
			throw std::runtime_error(std::string(name) + " is too large");

		return static_cast<uint32_t>(value);
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

	// The PAK layout intentionally depends on the physical file name.
	// Renaming a pak changes where its encrypted header and entry table live.
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
		// BaseKey is stored as four little-endian words but used byte-by-byte.
		return ByteAt(baseKey[byteIndex / sizeof(uint32_t)],
			byteIndex % sizeof(uint32_t));
	}

	uint32_t SumBaseKeyBytes(std::span<const uint32_t, BaseKeyWordCount> baseKey)
	{
		uint32_t result = 0;
		for (size_t i = 0; i < BaseKeyByteCount; ++i)
			result += BaseKeyByte(baseKey, i);

		return result;
	}

	// This field was named "unk" in CSO-Paker. Across the available 31007
	// entries it matches this metadata checksum exactly; it is not a file-data
	// checksum.
	uint32_t CalculateEntryChecksum(const PakArchive::Entry &entry)
	{
		return entry.fileOffset + entry.realSize + entry.packedSize +
			entry.type + SumBaseKeyBytes(entry.baseKey);
	}

	// These key schedules mirror CSO-Paker's decompiled formulas. Keep the
	// uint8_t truncation behavior; the overflow is part of the file format.
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

	// File data encryption uses the entry path plus the entry-local BaseKey,
	// so BaseKey must be preserved when patching existing archives.
	std::array<uint8_t, 128> GenerateDataKey(std::u16string_view filename,
		std::span<const uint32_t, BaseKeyWordCount> baseKey)
	{
		std::array<uint8_t, 128> result = {};
		const auto *filenamePtr = filename.data();
		const auto filenameLen = filename.size();

		for (size_t i = 0; i < result.size(); ++i)
		{
			const char16_t filenameChar = filenamePtr[i % filenameLen];
			const uint8_t baseKeyByte = BaseKeyByte(baseKey, i % BaseKeyByteCount);
			const size_t cycleOffset = (i % 5) + 2;
			result[i] = static_cast<uint8_t>(
				i + filenameChar * (baseKeyByte + cycleOffset));
		}

		return result;
	}

	std::string Utf16ToUtf8(std::u16string_view value)
	{
		// Avoid deprecated codecvt and make invalid surrogate pairs fail early.
		std::string result;
		result.reserve(value.size());

		const auto appendCodePoint = [&result](uint32_t codePoint)
		{
			if (codePoint <= 0x7F)
			{
				result.push_back(static_cast<char>(codePoint));
				return;
			}

			if (codePoint <= 0x7FF)
			{
				result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
				result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
				return;
			}

			if (codePoint <= 0xFFFF)
			{
				result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
				result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
				result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
				return;
			}

			result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
			result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		};

		for (size_t i = 0; i < value.size(); ++i)
		{
			uint32_t codePoint = value[i];
			if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
			{
				// Combine a valid UTF-16 surrogate pair into one Unicode scalar.
				if (i + 1 == value.size())
					throw std::runtime_error("invalid UTF-16 path");

				const uint32_t lowSurrogate = value[++i];
				if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF)
					throw std::runtime_error("invalid UTF-16 path");

				codePoint = 0x10000 + ((codePoint - 0xD800) << 10) +
					(lowSurrogate - 0xDC00);
			}
			else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
			{
				throw std::runtime_error("invalid UTF-16 path");
			}

			appendCodePoint(codePoint);
		}

		return result;
	}

	std::vector<uint8_t> ReadFile(const std::filesystem::path &path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
			throw std::runtime_error("failed to open input file: " + path.string());

		const auto size = input.tellg();
		if (size < 0)
			throw std::runtime_error("failed to read input file size: " + path.string());

		std::vector<uint8_t> data(static_cast<size_t>(size));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
		if (!input && !data.empty())
			throw std::runtime_error("failed to read input file: " + path.string());

		return data;
	}

	void WriteFile(const std::filesystem::path &path, std::span<const uint8_t> data)
	{
		if (!path.parent_path().empty())
			std::filesystem::create_directories(path.parent_path());

		std::ofstream output(path, std::ios::binary);
		if (!output)
			throw std::runtime_error("failed to open output file: " + path.string());

		output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
		if (!output)
			throw std::runtime_error("failed to write output file: " + path.string());
	}

	// SnowCipher works on 32-bit words. All callers pad table/data buffers
	// before encryption, so treating unaligned lengths as corruption is safer.
	void DecryptAligned(std::span<uint8_t> data, std::span<const uint8_t> key)
	{
		if ((data.size() % sizeof(uint32_t)) != 0)
			throw std::runtime_error("decrypt length is not 4-byte aligned");

		SnowCipher cipher;
		cipher.SetKey(key.data());
		cipher.DecryptBuffer(data.data(), data.data(), ToU32(data.size(), "decrypt length"));
	}

	void EncryptAligned(std::span<uint8_t> data, std::span<const uint8_t> key)
	{
		if ((data.size() % sizeof(uint32_t)) != 0)
			throw std::runtime_error("encrypt length is not 4-byte aligned");

		SnowCipher cipher;
		cipher.SetKey(key.data());
		cipher.EncryptBuffer(data.data(), data.data(), ToU32(data.size(), "encrypt length"));
	}

	// The entry table is one continuous encrypted byte stream, not a sequence
	// of independently encrypted fields. This reader decrypts 4-byte chunks and
	// keeps leftover bytes for UTF-16 strings and later fields.
	class EntryReader
	{
	public:
		EntryReader(std::span<const uint8_t> encrypted, std::span<const uint8_t> key)
			: encrypted_(encrypted)
		{
			cipher_.SetKey(key.data());
		}

		uint32_t ReadU32()
		{
			std::array<uint8_t, sizeof(uint32_t)> data = {};
			ReadPlain(data);
			return ReadU32Le(data.data());
		}

		std::u16string ReadString(size_t chars)
		{
			std::u16string result;
			result.resize(chars);

			for (size_t i = 0; i < chars; ++i)
			{
				std::array<uint8_t, sizeof(char16_t)> bytes = {};
				ReadPlain(bytes);
				result[i] = static_cast<char16_t>(bytes[0] |
					(static_cast<uint16_t>(bytes[1]) << 8));
			}

			return result;
		}

		size_t EncryptedOffset() const
		{
			return encryptedOffset_;
		}

	private:
		void ReadPlain(std::span<uint8_t> target)
		{
			// Reads can cross SnowCipher's 4-byte block boundary, so keep leftovers.
			size_t written = 0;
			while (written < target.size())
			{
				if (availableCount_ == 0)
					DecryptNextBlock();

				const size_t copySize = std::min(target.size() - written, availableCount_);
				std::ranges::copy_n(
					available_.begin() + static_cast<std::ptrdiff_t>(availableOffset_),
					copySize,
					target.begin() + static_cast<std::ptrdiff_t>(written));
				availableOffset_ += copySize;
				availableCount_ -= copySize;
				written += copySize;
			}
		}

		void DecryptNextBlock()
		{
			if (encryptedOffset_ + available_.size() > encrypted_.size())
				throw std::runtime_error("entry table is truncated");

			const auto encryptedBlock = encrypted_.subspan(encryptedOffset_, available_.size());
			cipher_.DecryptBuffer(available_.data(), encryptedBlock.data(),
				static_cast<uint32_t>(available_.size()));
			encryptedOffset_ += available_.size();
			availableOffset_ = 0;
			availableCount_ = available_.size();
		}

		std::span<const uint8_t> encrypted_;
		SnowCipher cipher_;
		std::array<uint8_t, 4> available_ = {};
		size_t encryptedOffset_ = 0;
		size_t availableOffset_ = 0;
		size_t availableCount_ = 0;
	};

	class EntryWriter
	{
	public:
		void WriteEntry(const PakArchive::Entry &entry)
		{
			WriteU32(ToU32(entry.path.size(), "entry path length"));
			WriteString(entry.path);
			WriteU32(entry.entryChecksum);
			WriteU32(entry.type);
			WriteU32(entry.fileOffset);
			WriteU32(entry.realSize);
			WriteU32(entry.packedSize);
			for (const auto word : entry.baseKey)
				WriteU32(word);
		}

		std::vector<uint8_t> FinishPlainTable()
		{
			// The table is encrypted in 32-bit blocks; pad only the serialized tail.
			data_.resize(AlignUp(data_.size(), sizeof(uint32_t)), 0);
			return std::move(data_);
		}

	private:
		void WriteU32(uint32_t value)
		{
			WriteU32Le(data_, value);
		}

		void WriteString(std::u16string_view value)
		{
			for (const auto ch : value)
			{
				data_.push_back(ByteAt(ch, 0));
				data_.push_back(ByteAt(ch, 1));
			}
		}

		std::vector<uint8_t> data_;
	};

	// Entry paths come from the archive. Normalize them as relative paths before
	// joining with replacementRoot to avoid reading outside that tree.
	std::filesystem::path SafeRelativePath(std::u16string_view entryPath)
	{
		std::filesystem::path relative(Utf16ToUtf8(entryPath));

		if (relative.empty() || relative.is_absolute())
			throw std::runtime_error("invalid entry path: " + Utf16ToUtf8(entryPath));

		for (const auto &part : relative)
		{
			if (part == "..")
				throw std::runtime_error("entry path escapes root: " + Utf16ToUtf8(entryPath));
		}

		return relative;
	}

	// Build the plaintext entry table first, then encrypt it as a single stream
	// so the reader's carry-over bytes line up with the original format.
	std::vector<uint8_t> BuildEntriesPlain(const std::vector<PakArchive::Entry> &entries)
	{
		EntryWriter writer;

		for (const auto &entry : entries)
			writer.WriteEntry(entry);

		return writer.FinishPlainTable();
	}

	std::vector<uint8_t> BuildHeader(size_t entryCount)
	{
		// Header checksum is deliberately simple: version + entry count.
		std::vector<uint8_t> header(HeaderSize, 0);
		const auto count = ToU32(entryCount, "entry count");
		WriteU32LeAt(header, 0, PakVersion + count);
		header[4] = PakVersion;
		WriteU32LeAt(header, 5, count);
		return header;
	}

	// FileOffset is a 0x400-byte block index from the data section, not an
	// absolute file offset. PackedSize is the exact encrypted payload length.
	std::vector<uint8_t> CopyOriginalBlob(const std::vector<uint8_t> &source,
		size_t dataStartOffset, const PakArchive::Entry &entry)
	{
		const size_t startOffset = dataStartOffset + (static_cast<size_t>(entry.fileOffset) << 10);
		const size_t endOffset = startOffset + entry.packedSize;
		if (startOffset > source.size() || endOffset > source.size())
			throw std::runtime_error("entry data is outside source pak: " + Utf16ToUtf8(entry.path));

		return { source.begin() + static_cast<std::ptrdiff_t>(startOffset),
			source.begin() + static_cast<std::ptrdiff_t>(endOffset) };
	}

	void EncryptEntryData(const PakArchive::Entry &entry, std::vector<uint8_t> &data)
	{
		const auto dataKey = GenerateDataKey(entry.path, entry.baseKey);

		// Decryption applies ENCRYPTED_AGAIN first, then ENCRYPTED on the top
		// bytes. Packing must apply the inverse operations in the opposite order.
		if ((entry.type & PakTypeEncrypted) != 0)
		{
			const size_t topBytes = std::min({
				static_cast<size_t>(entry.realSize),
				data.size(),
				DataBlockSize
			});
			const size_t alignedTopBytes = topBytes - (topBytes % sizeof(uint32_t));
			if (alignedTopBytes != 0)
				EncryptAligned({ data.data(), alignedTopBytes }, dataKey);
		}

		if ((entry.type & PakTypeEncryptedAgain) != 0 && !data.empty())
			EncryptAligned(data, dataKey);
	}

	std::vector<uint8_t> DecryptEntryData(const PakArchive::Entry &entry,
		std::vector<uint8_t> encryptedData)
	{
		if ((entry.type & PakTypeCompressed) == 0 && entry.packedSize < entry.realSize)
			throw std::runtime_error("entry packed size is smaller than real size: " +
				Utf16ToUtf8(entry.path));

		const auto dataKey = GenerateDataKey(entry.path, entry.baseKey);

		// Decryption reverses packing order: whole payload first, then top bytes.
		if ((entry.type & PakTypeEncryptedAgain) != 0 && !encryptedData.empty())
			DecryptAligned(encryptedData, dataKey);

		if ((entry.type & PakTypeEncrypted) != 0)
		{
			const size_t topBytes = std::min({
				static_cast<size_t>(entry.realSize),
				encryptedData.size(),
				DataBlockSize
			});
			const size_t alignedTopBytes = topBytes - (topBytes % sizeof(uint32_t));
			if (alignedTopBytes != 0)
				DecryptAligned({ encryptedData.data(), alignedTopBytes }, dataKey);
		}

		if ((entry.type & PakTypeCompressed) != 0)
			throw std::runtime_error("compressed entry is not supported yet: " +
				Utf16ToUtf8(entry.path));

		encryptedData.resize(entry.realSize);
		return encryptedData;
	}

	struct PackedEntry
	{
		PakArchive::Entry entry;
		std::vector<uint8_t> encryptedData;
		bool replaced = false;
	};

	void AssignEntryFileOffset(PakArchive::Entry &entry, size_t nextDataBlock)
	{
		entry.fileOffset = ToU32(nextDataBlock, "file offset");
	}

	void RefreshEntryChecksum(PakArchive::Entry &entry)
	{
		entry.entryChecksum = CalculateEntryChecksum(entry);
	}

	uint32_t MixHash(uint32_t hash, uint32_t value)
	{
		hash ^= value;
		hash *= 16777619u;
		return hash;
	}

	std::array<uint32_t, BaseKeyWordCount> GenerateBaseKey(std::u16string_view entryPath)
	{
		std::array<uint32_t, BaseKeyWordCount> result = {};
		for (size_t wordIndex = 0; wordIndex < result.size(); ++wordIndex)
		{
			uint32_t hash = 2166136261u ^ static_cast<uint32_t>(wordIndex * 0x9E3779B9u);
			for (const char16_t ch : entryPath)
				hash = MixHash(hash, ch);

			hash = MixHash(hash, static_cast<uint32_t>(entryPath.size()));
			result[wordIndex] = hash;
		}

		return result;
	}

	bool IsSameFile(const std::filesystem::path &left, const std::filesystem::path &right)
	{
		std::error_code error;
		const bool same = std::filesystem::equivalent(left, right, error);
		return !error && same;
	}

	std::vector<std::filesystem::path> CollectInputFiles(const std::filesystem::path &inputRoot,
		const std::filesystem::path &outputPakPath)
	{
		std::vector<std::filesystem::path> files;
		for (const auto &entry : std::filesystem::recursive_directory_iterator(inputRoot))
		{
			if (!entry.is_regular_file())
				continue;

			if (IsSameFile(entry.path(), outputPakPath))
				continue;

			files.push_back(entry.path());
		}

		std::ranges::sort(files,
			[&inputRoot](const auto &left, const auto &right)
			{
				return std::filesystem::relative(left, inputRoot).generic_string() <
					std::filesystem::relative(right, inputRoot).generic_string();
			});

		return files;
	}

	std::u16string EntryPathFromFile(const std::filesystem::path &inputRoot,
		const std::filesystem::path &filePath)
	{
		const auto relative = std::filesystem::relative(filePath, inputRoot);
		if (relative.empty() || relative.is_absolute())
			throw std::runtime_error("invalid input file path: " + filePath.string());

		return relative.generic_u16string();
	}

	std::vector<uint8_t> EncryptReplacementData(PakArchive::Entry &entry,
		const std::filesystem::path &replacementPath)
	{
		if ((entry.type & PakTypeCompressed) != 0)
			throw std::runtime_error("repacking compressed entry is not supported yet: " +
				Utf16ToUtf8(entry.path));

		auto plain = ReadFile(replacementPath);
		entry.realSize = ToU32(plain.size(), "replacement file size");

		std::vector<uint8_t> storedData = std::move(plain);
		entry.packedSize = ToU32(AlignUp(storedData.size(), sizeof(uint32_t)),
			"packed file size");
		storedData.resize(entry.packedSize, 0);
		EncryptEntryData(entry, storedData);
		return storedData;
	}

	PackedEntry BuildPackedEntryFromFile(const std::filesystem::path &inputRoot,
		const std::filesystem::path &filePath, size_t nextDataBlock)
	{
		PackedEntry packed;
		packed.entry.path = EntryPathFromFile(inputRoot, filePath);
		packed.entry.type = PakTypeEncrypted | PakTypeEncryptedAgain;
		packed.entry.fileOffset = ToU32(nextDataBlock, "file offset");
		packed.entry.baseKey = GenerateBaseKey(packed.entry.path);
		packed.encryptedData = EncryptReplacementData(packed.entry, filePath);
		RefreshEntryChecksum(packed.entry);
		return packed;
	}

	PackedEntry BuildPackedEntry(const PakArchive::Entry &sourceEntry,
		const std::vector<uint8_t> &sourceBuffer, size_t dataStartOffset,
		const std::filesystem::path &replacementRoot, size_t nextDataBlock)
	{
		PackedEntry packed;
		packed.entry = sourceEntry;
		// Rebuilt archives pack data densely, so every entry receives a new block index.
		AssignEntryFileOffset(packed.entry, nextDataBlock);

		const auto relativePath = SafeRelativePath(sourceEntry.path);
		const auto replacementPath = replacementRoot / relativePath;

		if (!std::filesystem::exists(replacementPath))
		{
			packed.encryptedData = CopyOriginalBlob(sourceBuffer, dataStartOffset, sourceEntry);
			// Preserved blobs still need a refreshed checksum because FileOffset changed.
			RefreshEntryChecksum(packed.entry);
			return packed;
		}

		if (!std::filesystem::is_regular_file(replacementPath))
			throw std::runtime_error("replacement path is not a file: " + replacementPath.string());

		packed.encryptedData = EncryptReplacementData(packed.entry, replacementPath);
		packed.replaced = true;
		RefreshEntryChecksum(packed.entry);
		return packed;
	}

	size_t PackedDataBlockCount(std::span<const uint8_t> encryptedData)
	{
		// Data blobs start on 0x400-byte boundaries; trailing padding is implicit.
		return AlignUp(encryptedData.size(), DataBlockSize) / DataBlockSize;
	}

	std::vector<PakArchive::Entry> BuildOutputEntries(
		const std::vector<PackedEntry> &packedEntries)
	{
		std::vector<PakArchive::Entry> outputEntries;
		outputEntries.reserve(packedEntries.size());
		std::ranges::transform(packedEntries, std::back_inserter(outputEntries),
			[](const PackedEntry &packed)
			{
				return packed.entry;
			});

		return outputEntries;
	}

	std::vector<uint8_t> BuildOutputPak(std::u16string_view outputFilename,
		const std::vector<PackedEntry> &packedEntries, size_t dataBlockCount)
	{
		const auto outputEntries = BuildOutputEntries(packedEntries);

		// Header and entry table offsets are recomputed from the output file name,
		// so the output pak must be read back using the same base name.
		const size_t outputHeaderOffset = HeaderOffset(outputFilename);
		const size_t outputEntriesOffset = EntriesOffset(outputFilename, outputHeaderOffset);

		auto entriesPlain = BuildEntriesPlain(outputEntries);
		const size_t outputDataStartOffset =
			AlignUp(outputEntriesOffset + entriesPlain.size(), DataBlockSize);
		const size_t outputSize = outputDataStartOffset + dataBlockCount * DataBlockSize;
		std::vector<uint8_t> output(outputSize, 0);

		auto header = BuildHeader(outputEntries.size());
		auto headerKey = GenerateHeaderKey(outputFilename);
		EncryptAligned(header, headerKey);
		std::ranges::copy(header,
			output.begin() + static_cast<std::ptrdiff_t>(outputHeaderOffset));

		auto entriesKey = GenerateEntriesKey(outputFilename);
		EncryptAligned(entriesPlain, entriesKey);
		std::ranges::copy(entriesPlain,
			output.begin() + static_cast<std::ptrdiff_t>(outputEntriesOffset));

		for (const auto &packed : packedEntries)
		{
			const size_t targetOffset = outputDataStartOffset +
				(static_cast<size_t>(packed.entry.fileOffset) << 10);
			std::ranges::copy(packed.encryptedData,
				output.begin() + static_cast<std::ptrdiff_t>(targetOffset));
		}

		return output;
	}
}

PakArchive PakArchive::Load(const std::filesystem::path &pakPath)
{
	PakArchive archive;
	archive.sourcePath_ = pakPath;
	archive.sourceFilename_ = pakPath.filename().generic_u16string();
	archive.buffer_ = ReadFile(pakPath);

	const size_t headerOffset = HeaderOffset(archive.sourceFilename_);
	if (headerOffset + HeaderSize > archive.buffer_.size())
		throw std::runtime_error("pak is too small for header");

	std::array<uint8_t, HeaderSize> header = {};
	std::ranges::copy_n(
		archive.buffer_.begin() + static_cast<std::ptrdiff_t>(headerOffset),
		HeaderSize,
		header.begin());
	DecryptAligned(header, GenerateHeaderKey(archive.sourceFilename_));

	const uint32_t headerChecksum = ReadU32Le(header.data());
	const uint8_t version = header[4];
	const uint32_t entryCount = ReadU32Le(header.data() + 5);
	if (version != PakVersion || headerChecksum != version + entryCount)
		throw std::runtime_error("invalid pak header or wrong source filename");

	const size_t entriesOffset = EntriesOffset(archive.sourceFilename_, headerOffset);
	if (entriesOffset >= archive.buffer_.size())
		throw std::runtime_error("entry table offset is outside pak");

	EntryReader reader({ archive.buffer_.data() + entriesOffset,
		archive.buffer_.size() - entriesOffset }, GenerateEntriesKey(archive.sourceFilename_));
	archive.entries_.reserve(entryCount);

	for (uint32_t i = 0; i < entryCount; ++i)
	{
		// Entry records are variable-length because the UTF-16 path is inline.
		const uint32_t pathLength = reader.ReadU32();
		if (pathLength > MaxPathChars)
			throw std::runtime_error("entry path is too long");

		Entry entry;
		entry.path = reader.ReadString(pathLength);
		entry.entryChecksum = reader.ReadU32();
		entry.type = reader.ReadU32();
		entry.fileOffset = reader.ReadU32();
		entry.realSize = reader.ReadU32();
		entry.packedSize = reader.ReadU32();
		for (auto &word : entry.baseKey)
			word = reader.ReadU32();

		if (entry.entryChecksum != CalculateEntryChecksum(entry))
			throw std::runtime_error("invalid entry checksum: " + Utf16ToUtf8(entry.path));

		archive.entries_.push_back(entry);
	}

	// Data starts after the encrypted entry stream and is aligned to 0x400 bytes.
	archive.dataStartOffset_ = AlignUp(entriesOffset + reader.EncryptedOffset(), DataBlockSize);
	if (archive.dataStartOffset_ > archive.buffer_.size())
		throw std::runtime_error("data start offset is outside pak");

	return archive;
}

UnpackStats PakArchive::UnpackToDirectory(const std::filesystem::path &outputRoot) const
{
	std::filesystem::create_directories(outputRoot);
	if (!std::filesystem::is_directory(outputRoot))
		throw std::runtime_error("output root is not a directory: " + outputRoot.string());

	UnpackStats stats{ .totalEntries = entries_.size() };
	for (const auto &entry : entries_)
	{
		const auto relativePath = SafeRelativePath(entry.path);
		auto encryptedData = CopyOriginalBlob(buffer_, dataStartOffset_, entry);
		auto plain = DecryptEntryData(entry, std::move(encryptedData));
		WriteFile(outputRoot / relativePath, plain);
		++stats.writtenEntries;
	}

	return stats;
}

PackStats PakArchive::PackDirectory(const std::filesystem::path &inputRoot,
	const std::filesystem::path &outputPakPath)
{
	if (!std::filesystem::is_directory(inputRoot))
		throw std::runtime_error("input root is not a directory: " + inputRoot.string());

	const auto inputFiles = CollectInputFiles(inputRoot, outputPakPath);
	std::vector<PackedEntry> packedEntries;
	packedEntries.reserve(inputFiles.size());

	PackStats stats{ .totalEntries = inputFiles.size() };

	size_t dataBlockCount = 0;
	for (const auto &inputFile : inputFiles)
	{
		// This counter is the next FileOffset value, measured in 0x400-byte blocks.
		auto packed = BuildPackedEntryFromFile(inputRoot, inputFile, dataBlockCount);
		dataBlockCount += PackedDataBlockCount(packed.encryptedData);
		packedEntries.push_back(std::move(packed));
		++stats.packedEntries;
	}

	const auto outputFilename = outputPakPath.filename().generic_u16string();
	auto output = BuildOutputPak(outputFilename, packedEntries, dataBlockCount);
	WriteFile(outputPakPath, output);
	return stats;
}

PatchStats PakArchive::PatchFromDirectory(const std::filesystem::path &replacementRoot,
	const std::filesystem::path &outputPakPath) const
{
	if (!std::filesystem::is_directory(replacementRoot))
		throw std::runtime_error("replacement root is not a directory: " + replacementRoot.string());

	std::vector<PackedEntry> packedEntries;
	packedEntries.reserve(entries_.size());

	PatchStats stats{ .totalEntries = entries_.size() };

	size_t dataBlockCount = 0;
	for (const auto &sourceEntry : entries_)
	{
		// This counter is the next FileOffset value, measured in 0x400-byte blocks.
		auto packed = BuildPackedEntry(sourceEntry, buffer_, dataStartOffset_,
			replacementRoot, dataBlockCount);

		if (packed.replaced)
			++stats.replacedEntries;
		else
			++stats.preservedEntries;

		dataBlockCount += PackedDataBlockCount(packed.encryptedData);
		packedEntries.push_back(std::move(packed));
	}

	const auto outputFilename = outputPakPath.filename().generic_u16string();
	auto output = BuildOutputPak(outputFilename, packedEntries, dataBlockCount);
	WriteFile(outputPakPath, output);
	return stats;
}
}
