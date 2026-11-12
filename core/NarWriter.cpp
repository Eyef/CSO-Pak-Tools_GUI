#include "NarWriter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "NarBZip2.h"
#include "NarCommon.h"

namespace cso_pak
{
	namespace
	{
		constexpr size_t kMinMatch = 3;
		constexpr size_t kMaxMatch = 264;
		constexpr size_t kMaxLiteralRun = 32;
		constexpr size_t kWindow = nar::kDictMax;       // 8192, also max distance
		constexpr size_t kWindowMask = kWindow - 1;
		constexpr int kMaxChain = 128;
		constexpr size_t kNiceLength = 96;
		constexpr int kHashBits = 16;
		constexpr uint32_t kNone = 0xFFFFFFFFu;

		uint32_t Hash3(const uint8_t *p)
		{
			const uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
			return (v * 2654435761u) >> (32 - kHashBits);
		}

		void PutU32(std::vector<uint8_t> &v, uint32_t x)
		{
			for (int i = 0; i < 4; ++i)
				v.push_back(static_cast<uint8_t>(x >> (8 * i)));
		}
	}

	std::vector<uint8_t> CompressNarLz(const uint8_t *src, size_t n)
	{
		// Segment formats (see the reader): literal run = byte (len-1) then
		// the bytes; match = ((len-2)<<5 | dist-1 high 5 bits) [+ (len-9) if
		// len > 8] then dist-1 low 8 bits.
		std::vector<uint8_t> out;
		out.reserve(n / 2 + 16);
		if (n == 0)
			return out;
		if (n > UINT32_MAX - 1)
			throw std::runtime_error("nar: entry too large to compress");

		std::vector<uint32_t> head(size_t(1) << kHashBits, kNone);
		std::array<uint32_t, kWindow> prev;
		prev.fill(kNone);

		size_t litStart = 0, litCount = 0;
		const auto flushLiterals = [&] {
			while (litCount > 0)
			{
				const size_t run = std::min(litCount, kMaxLiteralRun);
				out.push_back(static_cast<uint8_t>(run - 1));
				out.insert(out.end(), src + litStart, src + litStart + run);
				litStart += run;
				litCount -= run;
			}
		};
		const auto insertPos = [&](size_t p) {
			if (p + kMinMatch <= n)
			{
				const uint32_t h = Hash3(src + p);
				prev[p & kWindowMask] = head[h];
				head[h] = static_cast<uint32_t>(p);
			}
		};

		size_t pos = 0;
		while (pos < n)
		{
			const size_t maxLen = std::min(kMaxMatch, n - pos);
			size_t bestLen = 0, bestDist = 0;

			if (maxLen >= kMinMatch)
			{
				uint32_t cand = head[Hash3(src + pos)];
				int walk = kMaxChain;
				while (cand != kNone && pos - cand <= kWindow && walk-- > 0)
				{
					if (src[cand + bestLen] == src[pos + bestLen] || bestLen == 0)
					{
						size_t len = 0;
						while (len < maxLen && src[cand + len] == src[pos + len])
							++len;
						if (len > bestLen)
						{
							bestLen = len;
							bestDist = pos - cand;
							if (len >= kNiceLength || len == maxLen)
								break;
						}
					}
					const uint32_t next = prev[cand & kWindowMask];
					if (next == kNone || next >= cand)
						break;
					cand = next;
				}
				if (bestLen < kMinMatch)
					bestLen = 0;
			}

			if (bestLen == 0)
			{
				insertPos(pos);
				if (litCount == 0)
					litStart = pos;
				if (++litCount == kMaxLiteralRun)
					flushLiterals();
				++pos;
				continue;
			}

			flushLiterals();
			const size_t value = bestDist - 1;  // 0..8191
			const size_t code = bestLen - 2;    // 1..262
			if (code <= 6)
			{
				out.push_back(static_cast<uint8_t>((code << 5) | ((value >> 8) & 0x1F)));
			}
			else
			{
				out.push_back(static_cast<uint8_t>(0xE0 | ((value >> 8) & 0x1F)));
				out.push_back(static_cast<uint8_t>(bestLen - 9));
			}
			out.push_back(static_cast<uint8_t>(value & 0xFF));

			for (size_t p = pos; p < pos + bestLen; ++p)
				insertPos(p);
			pos += bestLen;
		}
		flushLiterals();
		return out;
	}

	NarWriter::NarWriter(const std::filesystem::path &outputPath)
		: target_(outputPath)
	{
		temp_ = target_;
		temp_ += ".tmp";
		out_.open(temp_, std::ios::binary | std::ios::trunc);
		if (!out_)
			throw std::runtime_error("nar: cannot create output file");
		uint8_t header[8];
		std::memcpy(header, &nar::kSignature, 4);
		std::memcpy(header + 4, &nar::kVersion, 4);
		WriteBytes(header, sizeof(header));
	}

	NarWriter::~NarWriter()
	{
		if (!finished_)
		{
			out_.close();
			std::error_code ec;
			std::filesystem::remove(temp_, ec);
		}
	}

	void NarWriter::WriteBytes(const uint8_t *data, size_t size)
	{
		if (size == 0)
			return;
		out_.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
		if (!out_)
			throw std::runtime_error("nar: write failed (disk full?)");
		position_ += size;
	}

	void NarWriter::Append(const std::string &path, uint32_t type, const uint8_t *stored,
		size_t storedSize, size_t extractedSize, uint32_t timestamp, uint32_t checksum,
		uint64_t order)
	{
		if (finished_)
			throw std::logic_error("nar: writer already finished");
		if (path.empty() || path.size() > 0xFFFF)
			throw std::runtime_error("nar: bad entry path length");
		// The directory stores a 32-bit offset and signed 32-bit sizes.
		if (position_ > 0xFFFFFFFFull || storedSize > 0x7FFFFFFFu || extractedSize > 0x7FFFFFFFu)
			throw std::runtime_error("nar: entry exceeds the limits of the format: " + path);

		Record r;
		r.path = path;
		r.type = type;
		r.offset = static_cast<uint32_t>(position_);
		r.storedSize = static_cast<uint32_t>(storedSize);
		r.extractedSize = static_cast<uint32_t>(extractedSize);
		r.timestamp = timestamp;
		r.checksum = checksum;
		r.seq = records_.size();
		r.order = order;  // UINT64_MAX (default) sorts after explicit orders
		WriteBytes(stored, storedSize);
		records_.push_back(std::move(r));
	}

	void NarWriter::AddFile(const std::string &archivePath, const uint8_t *data, size_t size,
		NarStoreType type, uint32_t timestamp, uint64_t directoryOrder)
	{
		if (type == NarStoreType::Raw)
		{
			Append(archivePath, 0, data, size, size, timestamp,
				nar::Crc32::Of(data, size), directoryOrder);
			return;
		}

		std::vector<uint8_t> stored;
		if (type == NarStoreType::EncodedCompressed)
			stored = CompressNarLz(data, size);
		else if (type == NarStoreType::Encoded)
			stored.assign(data, data + size);
		else
			throw std::invalid_argument("nar: unknown store type");

		uint8_t key[16];
		nar::MakeEntryKey(reinterpret_cast<const uint8_t *>(archivePath.data()),
			archivePath.size(), key);
		for (size_t i = 0; i < stored.size(); ++i)
			stored[i] ^= key[i & 15];

		Append(archivePath, static_cast<uint32_t>(type), stored.data(), stored.size(), size,
			timestamp, nar::Crc32::Of(stored.data(), stored.size()), directoryOrder);
	}

	void NarWriter::AddStored(const std::string &archivePath, const NarArchive::Entry &source,
		const std::vector<uint8_t> &storedBytes, uint64_t directoryOrder)
	{
		Append(archivePath, source.type, storedBytes.data(), storedBytes.size(),
			source.extractedSize, source.timestamp, source.checksum, directoryOrder);
	}

	void NarWriter::Finish()
	{
		if (finished_)
			throw std::logic_error("nar: writer already finished");

		std::vector<const Record *> sorted;
		sorted.reserve(records_.size());
		for (const Record &r : records_)
			sorted.push_back(&r);
		std::stable_sort(sorted.begin(), sorted.end(), [](const Record *a, const Record *b) {
			return a->order != b->order ? a->order < b->order : a->seq < b->seq;
		});

		// Directory: version 1, then the original packer's fields. Field 2
		// (offset 8) is the directory size -- every retail archive stores
		// "entry bytes + 4" there (i.e. the uncompressed table length minus
		// the 16 bytes before the count), and NARTools' writer does the same.
		// The client may use it, so don't hard-code the reference tools'
		// placeholder 4.
		uint64_t entryBytes = 0;
		for (const Record *r : sorted)
			entryBytes += 2 + r->path.size() + 24;
		if (entryBytes + 4 > 0x7FFFFFFFull)
			throw std::runtime_error("nar: directory too large");

		std::vector<uint8_t> dir;
		PutU32(dir, 1);
		PutU32(dir, 0);
		PutU32(dir, static_cast<uint32_t>(entryBytes + 4));
		PutU32(dir, 1);
		PutU32(dir, static_cast<uint32_t>(sorted.size()));
		for (const Record *r : sorted)
		{
			dir.push_back(static_cast<uint8_t>(r->path.size() & 0xFF));
			dir.push_back(static_cast<uint8_t>(r->path.size() >> 8));
			dir.insert(dir.end(), r->path.begin(), r->path.end());
			PutU32(dir, r->type);
			PutU32(dir, r->offset);
			PutU32(dir, r->storedSize);
			PutU32(dir, r->extractedSize);
			PutU32(dir, r->timestamp);
			PutU32(dir, r->checksum);
		}

		std::vector<uint8_t> packed = CompressBZip2(dir.data(), dir.size(), 1);
		for (size_t i = 0; i < packed.size(); ++i)
			packed[i] ^= nar::kHeaderXor[i & 15];
		WriteBytes(packed.data(), packed.size());

		const uint32_t sizeField = static_cast<uint32_t>(packed.size()) ^ nar::kHeaderSizeXor;
		uint8_t footer[8];
		std::memcpy(footer, &sizeField, 4);
		std::memcpy(footer + 4, &nar::kSignature, 4);
		WriteBytes(footer, sizeof(footer));

		out_.flush();
		if (!out_)
			throw std::runtime_error("nar: flush failed");
		out_.close();
		if (!out_)
			throw std::runtime_error("nar: close failed");

		// std::filesystem::rename replaces an existing target (POSIX rename,
		// MoveFileEx on Windows). Never delete the old file ourselves: if the
		// move fails it must survive.
		std::error_code ec;
		std::filesystem::rename(temp_, target_, ec);
		if (ec)
			throw std::runtime_error("nar: cannot move result into place: " + ec.message());
		finished_ = true;
	}
}
