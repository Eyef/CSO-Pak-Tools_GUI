// Minimal bzip2 decompressor for Nexon .nar directory blobs.
// Own implementation from the public bzip2 format (stages: file RLE ->
// Burrows-Wheeler -> move-to-front with RUNA/RUNB run coding -> Huffman
// with 2-6 tables and MTF selectors -> stream of blocks framed by 48-bit
// magic values). Verified against real .nar data via per-block CRC32.

#include "NarBZip2.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace cso_pak
{
	namespace
	{
		constexpr uint64_t kBlockMagic = 0x314159265359ULL;  // "1AY&SY"
		constexpr uint64_t kEndMagic = 0x177245385090ULL;    // "17rE8P90"
		constexpr int kMaxAlphabet = 258;  // 256 symbols + RUNA + RUNB
		constexpr int kMaxCodeLen = 20;
		constexpr int kGroupSize = 50;

		uint32_t CrcTable[256];
		bool CrcTableReady = false;

		void EnsureCrcTable()
		{
			if (CrcTableReady)
				return;
			for (uint32_t i = 0; i < 256; ++i)
			{
				uint32_t c = i << 24;
				for (int k = 0; k < 8; ++k)
					c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
				CrcTable[i] = c;
			}
			CrcTableReady = true;
		}

		class BitReader
		{
		public:
			BitReader(const uint8_t *data, size_t size)
				: data_(data), size_(size)
			{
			}

			uint32_t GetBits(int count)
			{
				if (count < 0 || count > 32)
					throw std::runtime_error("bzip2: bad bit count");
				uint32_t value = 0;
				while (count > 0)
				{
					if (bytePos_ >= size_)
						throw std::runtime_error("bzip2: truncated stream");
					const int avail = 8 - bitPos_;
					const int take = count < avail ? count : avail;
					const uint8_t byte = data_[bytePos_];
					const int shift = avail - take;
					value = (value << take) |
						((byte >> shift) & (0xFFu >> (8 - take)));
					bitPos_ += take;
					if (bitPos_ == 8)
					{
						bitPos_ = 0;
						++bytePos_;
					}
					count -= take;
				}
				return value;
			}

			bool GetBit() { return GetBits(1) != 0; }

		private:
			const uint8_t *data_ = nullptr;
			size_t size_ = 0;
			size_t bytePos_ = 0;
			int bitPos_ = 0;  // next bit index inside the byte (0 = MSB)
		};

		struct HuffmanTable
		{
			int minLen = 0;
			int maxLen = 0;
			// base/limit indexed by length, perm maps code-minBase -> symbol.
			std::array<int, kMaxCodeLen + 2> base{};
			std::array<int, kMaxCodeLen + 2> limit{};
			std::array<int, kMaxAlphabet> perm{};
		};

		void BuildHuffman(const int *lengths, int alphaSize, HuffmanTable &table)
		{
			std::array<int, kMaxCodeLen + 1> count{};
			int minLen = kMaxCodeLen, maxLen = 0;
			for (int i = 0; i < alphaSize; ++i)
			{
				const int len = lengths[i];
				if (len < 0 || len > kMaxCodeLen)
					throw std::runtime_error("bzip2: bad code length");
				if (len == 0)
					continue;
				++count[len];
				if (len < minLen)
					minLen = len;
				if (len > maxLen)
					maxLen = len;
			}
			if (minLen > maxLen)
				throw std::runtime_error("bzip2: empty huffman table");
			table.minLen = minLen;
			table.maxLen = maxLen;
			// Canonical codes: base[len] is biased by the perm start so
			// that perm[code - base[len]] is the symbol (base[len] =
			// firstCodeValue - firstPermIndex for that length).
			int code = 0;
			int permPos = 0;
			for (int len = minLen; len <= maxLen; ++len)
			{
				table.base[len] = code - permPos;
				code += count[len];
				table.limit[len] = code - 1;
				code <<= 1;
				permPos += count[len];
			}
			int pos = 0;
			for (int len = minLen; len <= maxLen; ++len)
			{
				for (int i = 0; i < alphaSize; ++i)
				{
					if (lengths[i] == len)
						table.perm[pos++] = i;
				}
			}
		}

		int DecodeSymbol(BitReader &br, const HuffmanTable &table)
		{
			int len = table.minLen;
			uint32_t code = br.GetBits(len);
			while (code > static_cast<uint32_t>(table.limit[len]))
			{
				++len;
				if (len > table.maxLen)
					throw std::runtime_error("bzip2: bad huffman code");
				code = (code << 1) | (br.GetBit() ? 1u : 0u);
			}
			return table.perm[code - table.base[len]];
		}

		// Decompresses one block; appends the final (post-RLE1) bytes to
		// out and folds them into crc (running block CRC, init 0xFFFFFFFF).
		void DecodeBlock(BitReader &br, int blockSize100k,
			std::vector<uint8_t> &out, uint32_t &crc)
		{
			const size_t maxBlock = static_cast<size_t>(blockSize100k) * 100000;
			const uint32_t storedCrc = br.GetBits(32);
			if (br.GetBit())
				throw std::runtime_error("bzip2: randomised blocks unsupported");
			const uint32_t origPtr = br.GetBits(24);

			// Symbol mapping (in-use table).
			std::array<uint8_t, 256> seqToUnseq{};
			int nInUse = 0;
			{
				const uint32_t inUse16 = br.GetBits(16);
				for (int i = 0; i < 16; ++i)
				{
					if (!(inUse16 & (0x8000u >> i)))
						continue;
					const uint32_t inUseBits = br.GetBits(16);
					for (int k = 0; k < 16; ++k)
					{
						if (inUseBits & (0x8000u >> k))
							seqToUnseq[nInUse++] = static_cast<uint8_t>(i * 16 + k);
					}
				}
			}
			if (nInUse == 0)
				throw std::runtime_error("bzip2: empty alphabet");
			// Indices: 0 = RUNA, 1 = RUNB, 2..nInUse+1 = MTF values,
			// nInUse+1 = end-of-block.
			const int alphaSize = nInUse + 2;

			// Selectors (MTF over 2..6 tables).
			const int nGroups = static_cast<int>(br.GetBits(3));
			if (nGroups < 2 || nGroups > 6)
				throw std::runtime_error("bzip2: bad group count");
			const int nSelectors = static_cast<int>(br.GetBits(15));
			if (nSelectors < 1 || nSelectors > 18002)
				throw std::runtime_error("bzip2: bad selector count");
			std::vector<int> selectors(static_cast<size_t>(nSelectors));
			{
				std::array<int, 6> mtf{};
				for (int i = 0; i < nGroups; ++i)
					mtf[i] = i;
				for (int i = 0; i < nSelectors; ++i)
				{
					int j = 0;
					while (br.GetBit())
					{
						++j;
						if (j >= nGroups)
							throw std::runtime_error("bzip2: bad selector");
					}
					const int uc = mtf[j];
					for (; j > 0; --j)
						mtf[j] = mtf[j - 1];
					mtf[0] = uc;
					selectors[static_cast<size_t>(i)] = uc;
				}
			}

			// Code lengths per group: 5-bit start length, then per symbol a
			// 1-bit marker loop -- while the marker is 1, one more bit
			// adjusts the length (0 = len+1, 1 = len-1); a 0 marker stores
			// the current length for the symbol. Every one of the alphaSize
			// symbols always has an entry.
			std::array<std::array<int, kMaxAlphabet>, 6> lengths{};
			for (int g = 0; g < nGroups; ++g)
			{
				int len = static_cast<int>(br.GetBits(5));
				if (len < 1 || len > kMaxCodeLen)
					throw std::runtime_error("bzip2: bad start length");
				for (int i = 0; i < alphaSize; ++i)
				{
					while (br.GetBit())
					{
						if (br.GetBit())
							--len;
						else
							++len;
						if (len < 1 || len > kMaxCodeLen)
							throw std::runtime_error("bzip2: bad code length");
					}
					lengths[g][i] = len;
				}
			}

			std::array<HuffmanTable, 6> tables{};
			for (int g = 0; g < nGroups; ++g)
				BuildHuffman(lengths[g].data(), alphaSize, tables[g]);

			// MTF + RUNA/RUNB stream -> BWT last column.
			std::vector<uint8_t> last;
			last.reserve(maxBlock);
			{
				std::array<uint8_t, 256> yy{};
				for (int i = 0; i < nInUse; ++i)
					yy[i] = seqToUnseq[i];
				int groupNo = -1;
				int groupPos = 0;
				const HuffmanTable *table = nullptr;
				while (true)
				{
					if (groupPos == 0)
					{
						++groupNo;
						if (groupNo >= nSelectors)
							throw std::runtime_error("bzip2: too many groups");
						groupPos = kGroupSize;
						table = &tables[selectors[static_cast<size_t>(groupNo)]];
					}
					--groupPos;
					const int sym = DecodeSymbol(br, *table);
					if (sym == alphaSize - 1)
						break;  // end of block
					if (sym == 0 || sym == 1)
					{
						// RUNA/RUNB run.
						int es = -1;
						int n = 1;
						int s = sym;
						for (;;)
						{
							if (s == 0)
								es += n;
							else
								es += 2 * n;
							n *= 2;
							if (groupPos == 0)
							{
								++groupNo;
								if (groupNo >= nSelectors)
									throw std::runtime_error("bzip2: too many groups");
								groupPos = kGroupSize;
								table = &tables[selectors[static_cast<size_t>(groupNo)]];
							}
							--groupPos;
							s = DecodeSymbol(br, *table);
							if (s >= 2)
								break;
						}
						const int run = es + 1;
						if (run < 1)
							throw std::runtime_error("bzip2: bad run");
						if (last.size() + static_cast<size_t>(run) > maxBlock)
							throw std::runtime_error("bzip2: block overrun");
						last.insert(last.end(), static_cast<size_t>(run), yy[0]);
						// s now holds the first non-run symbol: fall through
						// to the MTF handling below with sym = s.
						if (s == alphaSize - 1)
							break;
						const int nn = s - 1;
						if (nn < 0 || nn >= nInUse)
							throw std::runtime_error("bzip2: bad mtf symbol");
						const uint8_t uc = yy[nn];
						for (int j = nn; j > 0; --j)
							yy[j] = yy[j - 1];
						yy[0] = uc;
						if (last.size() + 1 > maxBlock)
							throw std::runtime_error("bzip2: block overrun");
						last.push_back(uc);
						continue;
					}
					// Plain MTF literal.
					const int nn = sym - 1;
					if (nn < 0 || nn >= nInUse)
						throw std::runtime_error("bzip2: bad mtf symbol");
					const uint8_t uc = yy[nn];
					for (int j = nn; j > 0; --j)
						yy[j] = yy[j - 1];
					yy[0] = uc;
					if (last.size() + 1 > maxBlock)
						throw std::runtime_error("bzip2: block overrun");
					last.push_back(uc);
				}
			}

			if (origPtr >= last.size())
				throw std::runtime_error("bzip2: bad origPtr");

			// Inverse Burrows-Wheeler transform.
			std::vector<uint8_t> bwt(last.size());
			{
				std::array<uint32_t, 256> freq{};
				for (uint8_t c : last)
					++freq[c];
				std::array<uint32_t, 256> start{};
				uint32_t sum = 0;
				for (int c = 0; c < 256; ++c)
				{
					start[c] = sum;
					sum += freq[c];
				}
				std::vector<uint32_t> trans(last.size());
				for (size_t i = 0; i < last.size(); ++i)
					trans[start[last[i]]++] = static_cast<uint32_t>(i);
				uint32_t j = origPtr;
				for (size_t i = 0; i < last.size(); ++i)
				{
					j = trans[j];
					bwt[i] = last[j];
				}
			}

			// File-level RLE: 4 identical bytes + 1 count byte (extra copies
			// beyond the 4). Fold output bytes into the block CRC.
			{
				size_t i = 0;
				const size_t n = bwt.size();
				while (i < n)
				{
					const uint8_t c = bwt[i++];
					out.push_back(c);
					crc = (crc << 8) ^ CrcTable[((crc >> 24) ^ c) & 0xFFu];
					size_t run = 1;
					while (i < n && bwt[i] == c && run < 4)
					{
						out.push_back(c);
						crc = (crc << 8) ^ CrcTable[((crc >> 24) ^ c) & 0xFFu];
						++i;
						++run;
					}
					if (run == 4)
					{
						if (i >= n)
							throw std::runtime_error("bzip2: truncated run");
						const size_t extra = bwt[i++];
						for (size_t k = 0; k < extra; ++k)
						{
							out.push_back(c);
							crc = (crc << 8) ^ CrcTable[((crc >> 24) ^ c) & 0xFFu];
						}
					}
				}
			}

			// Block CRC is the non-reflected CRC-32 (init 0xFFFFFFFF) of the
			// final bytes with the standard final xor applied.
			if ((crc ^ 0xFFFFFFFFu) != storedCrc)
				throw std::runtime_error("bzip2: block CRC mismatch");
		}
	}

	std::vector<uint8_t> DecompressBZip2(const uint8_t *data, size_t size)
	{
		EnsureCrcTable();
		if (size < 4)
			throw std::runtime_error("bzip2: too small");
		BitReader br(data, size);
		if (br.GetBits(8) != 'B' || br.GetBits(8) != 'Z' || br.GetBits(8) != 'h')
			throw std::runtime_error("bzip2: bad 'BZh' magic");
		// 'B','Z','h' checked above bit by bit (keeps the reader simple).
		const int blockSize100k = static_cast<int>(br.GetBits(8)) - '0';
		if (blockSize100k < 1 || blockSize100k > 9)
			throw std::runtime_error("bzip2: bad block size");

		std::vector<uint8_t> out;
		while (true)
		{
			uint64_t magic = 0;
			for (int i = 0; i < 48; ++i)
				magic = (magic << 1) | (br.GetBit() ? 1u : 0u);
			if (magic == kEndMagic)
			{
				(void)br.GetBits(32);  // stream CRC, informational
				break;
			}
			if (magic != kBlockMagic)
				throw std::runtime_error("bzip2: bad block magic");
			uint32_t crc = 0xFFFFFFFFu;
			DecodeBlock(br, blockSize100k, out, crc);
		}
		return out;
	}
}
