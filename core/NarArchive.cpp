// Nexon .nar archive reader. Own implementation; NARTools' NARToolLib was
// used as the format reference (entry layout, XOR keystream, custom LZSS,
// bzip2 directory, EUC-KR paths).

#include "NarArchive.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "NarBZip2.h"

namespace cso_pak
{
	namespace
	{
		constexpr uint32_t kSignature = 0x0052414Eu;  // "NAR\0" little-endian
		constexpr uint32_t kVersion = 0x01000000u;
		constexpr uint32_t kHeaderSizeXor = 0x4076551Fu;
		constexpr uint8_t kHeaderXor[16] = {
			0x19, 0x5B, 0x7B, 0x2C, 0x65, 0x5E, 0x79, 0x25,
			0x6E, 0x4B, 0x07, 0x21, 0x62, 0x7F, 0x00, 0x29
		};
		constexpr size_t kDictMax = 8192;

		uint32_t ReadU32LE(const uint8_t *p)
		{
			uint32_t v = 0;
			std::memcpy(&v, p, 4);
			return v;
		}

		uint16_t ReadU16LE(const uint8_t *p)
		{
			uint16_t v = 0;
			std::memcpy(&v, p, 2);
			return v;
		}

		// Python string hash (as in NARTools' decoder): h = h*1000003 ^ b,
		// then h ^= length. All arithmetic wraps mod 2^32.
		uint32_t PythonHash(const uint8_t *data, size_t size)
		{
			uint32_t hash = 0;
			for (size_t i = 0; i < size; ++i)
				hash = hash * 1000003u ^ data[i];
			return hash ^ static_cast<uint32_t>(size);
		}

		// 16-byte XOR keystream from the entry path bytes.
		void MakeEntryKey(const uint8_t *pathBytes, size_t pathSize, uint8_t key[16])
		{
			uint32_t seed = PythonHash(pathBytes, pathSize);
			for (int i = 0; i < 16; ++i)
			{
				seed = seed * 1103515245u + 12345u;
				key[i] = static_cast<uint8_t>(seed & 255u);
			}
		}

		// Custom LZSS used by .nar (dictionary 8192, commands: 0lllll =
		// literal run of lllll+1 bytes, otherwise match of length op+2 with
		// op==7 extended by one more byte, distance ((len<<8)|next)+1).
		std::vector<uint8_t> DecompressNarLz(const uint8_t *src, size_t srcSize,
			size_t extractedSize)
		{
			std::vector<uint8_t> out;
			out.reserve(extractedSize);
			std::array<uint8_t, kDictMax> dict{};
			size_t dictTotal = 0;  // bytes ever written (window = last 8192)
			size_t pos = 0;
			const auto readByte = [&]() -> uint8_t
			{
				if (pos >= srcSize)
					throw std::runtime_error("nar: truncated lz stream");
				return src[pos++];
			};
			const auto emit = [&](uint8_t b)
			{
				out.push_back(b);
				dict[dictTotal % kDictMax] = b;
				++dictTotal;
				if (out.size() > extractedSize)
					throw std::runtime_error("nar: lz overrun");
			};
			while (out.size() < extractedSize)
			{
				if (pos >= srcSize)
					throw std::runtime_error("nar: truncated lz commands");
				const uint8_t cmd = src[pos++];
				const int op = cmd >> 5;
				const int ln = cmd & 31;
				if (op == 0)
				{
					int run = ln + 1;
					while (run-- > 0)
						emit(readByte());
				}
				else
				{
					int length = op;
					if (length == 7)
						length += readByte();
					length += 2;
					const size_t dist = (static_cast<size_t>(ln) << 8) |
						readByte();
					const size_t distance = dist + 1;
					const size_t windowed =
						dictTotal < kDictMax ? dictTotal : kDictMax;
					if (distance == 0 || distance > windowed)
						throw std::runtime_error("nar: bad lz distance");
					for (int i = 0; i < length; ++i)
					{
						const size_t back = dictTotal - distance;
						emit(dict[back % kDictMax]);
					}
				}
			}
			return out;
		}
	}

	std::u16string NarArchive::Entry::generic_u16string() const
	{
		std::u16string out;
		out.reserve(path.size());
		for (const unsigned char c : path)
			out.push_back(c < 128 ? static_cast<char16_t>(c) : u'?');
		return out;
	}

	NarArchive::~NarArchive()
	{
		delete static_cast<std::ifstream *>(stream_);
	}

	NarArchive::NarArchive(NarArchive &&other) noexcept
		: entries_(std::move(other.entries_)),
		sourcePath_(std::move(other.sourcePath_)),
		fileSize_(other.fileSize_),
		stream_(other.stream_)
	{
		other.stream_ = nullptr;
		other.fileSize_ = 0;
	}

	NarArchive &NarArchive::operator=(NarArchive &&other) noexcept
	{
		if (this != &other)
		{
			delete static_cast<std::ifstream *>(stream_);
			entries_ = std::move(other.entries_);
			sourcePath_ = std::move(other.sourcePath_);
			fileSize_ = other.fileSize_;
			stream_ = other.stream_;
			other.stream_ = nullptr;
			other.fileSize_ = 0;
		}
		return *this;
	}

	std::string NarArchive::FileNameOf(const std::string &path)
	{
		const size_t p = path.find_last_of("/\\");
		return p == std::string::npos ? path : path.substr(p + 1);
	}

	NarArchive NarArchive::Load(const std::filesystem::path &narPath)
	{
		std::ifstream file(narPath, std::ios::binary | std::ios::ate);
		if (!file)
			throw std::runtime_error("nar: cannot open file");
		const int64_t fileSize = static_cast<int64_t>(file.tellg());
		if (fileSize < 16)
			throw std::runtime_error("nar: file too small");
		file.seekg(0, std::ios::beg);
		uint8_t head[8];
		if (!file.read(reinterpret_cast<char *>(head), 8))
			throw std::runtime_error("nar: cannot read header");
		if (ReadU32LE(head) != kSignature || ReadU32LE(head + 4) != kVersion)
			throw std::runtime_error("nar: bad signature/version");

		// Footer: [dirSize ^ kHeaderSizeXor][signature].
		file.seekg(fileSize - 8, std::ios::beg);
		uint8_t foot[8];
		if (!file.read(reinterpret_cast<char *>(foot), 8))
			throw std::runtime_error("nar: cannot read footer");
		if (ReadU32LE(foot + 4) != kSignature)
			throw std::runtime_error("nar: bad end signature");
		const uint32_t dirSize = ReadU32LE(foot) ^ kHeaderSizeXor;
		if (dirSize > static_cast<uint64_t>(fileSize) - 16)
			throw std::runtime_error("nar: directory outside file");

		// XORed + bzip2 directory just before the footer.
		std::vector<uint8_t> dirBlob(dirSize);
		file.seekg(fileSize - 8 - dirSize, std::ios::beg);
		if (!file.read(reinterpret_cast<char *>(dirBlob.data()), dirSize))
			throw std::runtime_error("nar: cannot read directory");
		for (size_t i = 0; i < dirBlob.size(); ++i)
			dirBlob[i] ^= kHeaderXor[i & 15];
		const std::vector<uint8_t> dir = DecompressBZip2(dirBlob.data(), dirBlob.size());

		if (dir.size() < 20)
			throw std::runtime_error("nar: directory too small");
		const int32_t dirVersion = static_cast<int32_t>(ReadU32LE(dir.data()));
		if (dirVersion != 1 && dirVersion != 2)
			throw std::runtime_error("nar: unsupported directory version");
		const int32_t dirCount = static_cast<int32_t>(ReadU32LE(dir.data() + 16));
		if (dirCount < 0 || dirCount > 10000000)
			throw std::runtime_error("nar: implausible entry count");

		NarArchive archive;
		archive.sourcePath_ = narPath;
		archive.fileSize_ = static_cast<uint64_t>(fileSize);
		archive.entries_.reserve(static_cast<size_t>(dirCount));
		size_t off = 20;
		for (int32_t i = 0; i < dirCount; ++i)
		{
			if (off + 2 > dir.size())
				throw std::runtime_error("nar: truncated entry");
			const uint16_t pathSize = ReadU16LE(dir.data() + off);
			if (off + 2 + pathSize + 24 > dir.size())
				throw std::runtime_error("nar: truncated entry");
			Entry entry;
			entry.path.assign(
				reinterpret_cast<const char *>(dir.data() + off + 2), pathSize);
			const uint8_t *f = dir.data() + off + 2 + pathSize;
			entry.type = ReadU32LE(f);
			entry.offset = ReadU32LE(f + 4);
			entry.storedSize = static_cast<uint32_t>(static_cast<int32_t>(ReadU32LE(f + 8)));
			entry.extractedSize = static_cast<uint32_t>(static_cast<int32_t>(ReadU32LE(f + 12)));
			entry.checksum = ReadU32LE(f + 20);
			if (entry.type > 2)
				throw std::runtime_error("nar: unsupported entry type");
			const uint64_t end =
				static_cast<uint64_t>(entry.offset) + entry.storedSize;
			if (end > static_cast<uint64_t>(fileSize) - 8 - dirSize &&
				entry.extractedSize != 0)
				throw std::runtime_error("nar: entry outside data region");
			archive.entries_.push_back(std::move(entry));
			off += 2 + pathSize + 24;
		}
		return archive;
	}

	std::vector<uint8_t> NarArchive::ExtractEntry(const Entry &entry) const
	{
		if (entry.extractedSize == 0)
			return {};
		if (stream_ == nullptr)
		{
			auto *file = new std::ifstream(
				sourcePath_, std::ios::binary);
			if (!*file)
			{
				delete file;
				throw std::runtime_error("nar: cannot reopen file");
			}
			stream_ = file;
		}
		auto *file = static_cast<std::ifstream *>(stream_);
		file->clear();
		file->seekg(static_cast<int64_t>(entry.offset), std::ios::beg);
		std::vector<uint8_t> stored(entry.storedSize);
		if (entry.storedSize > 0 &&
			!file->read(reinterpret_cast<char *>(stored.data()), entry.storedSize))
			throw std::runtime_error("nar: cannot read entry data");

		if (entry.type == 0)
		{
			if (stored.size() != entry.extractedSize)
				throw std::runtime_error("nar: raw size mismatch");
			return stored;
		}

		// XOR-decode with the path-derived keystream.
		uint8_t key[16];
		MakeEntryKey(reinterpret_cast<const uint8_t *>(entry.path.data()),
			entry.path.size(), key);
		for (size_t i = 0; i < stored.size(); ++i)
			stored[i] ^= key[i & 15];

		if (entry.type == 1)
		{
			if (stored.size() != entry.extractedSize)
				throw std::runtime_error("nar: decoded size mismatch");
			return stored;
		}

		// type 2: custom LZSS on top of the XOR layer.
		std::vector<uint8_t> out =
			DecompressNarLz(stored.data(), stored.size(), entry.extractedSize);
		if (out.size() != entry.extractedSize)
			throw std::runtime_error("nar: decompressed size mismatch");
		return out;
	}
}
