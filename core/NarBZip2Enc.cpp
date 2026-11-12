// Minimal bzip2 compressor (see NarBZip2.h). Written from the public bzip2
// stream format description, mirroring the stages of the existing decoder in
// NarBZip2.cpp in the opposite order:
//   RLE1 -> BWT -> MTF + RUNA/RUNB (RLE2) -> Huffman -> bit stream.

#include "NarBZip2.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace cso_pak
{
	namespace
	{
		constexpr uint32_t kMaxCodeLen = 17;  // what the reference encoder limits to
		constexpr size_t kGroupSize = 50;     // symbols per selector

		// bzip2's CRC is the MSB-first CRC-32 (poly 0x04C11DB7).
		uint32_t Crc32Msb(uint32_t crc, const uint8_t *data, size_t size)
		{
			static const std::array<uint32_t, 256> table = [] {
				std::array<uint32_t, 256> t{};
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t c = i << 24;
					for (int k = 0; k < 8; ++k)
						c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
					t[i] = c;
				}
				return t;
			}();
			for (size_t i = 0; i < size; ++i)
				crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
			return crc;
		}

		class BitWriter
		{
		public:
			void Put(uint32_t value, int bits)
			{
				for (int i = bits - 1; i >= 0; --i)
				{
					cur_ = static_cast<uint8_t>((cur_ << 1) | ((value >> i) & 1u));
					if (++used_ == 8)
					{
						out_.push_back(cur_);
						cur_ = 0;
						used_ = 0;
					}
				}
			}
			void Flush()
			{
				while (used_ != 0)
					Put(0, 1);
			}
			std::vector<uint8_t> &Bytes() { return out_; }

		private:
			std::vector<uint8_t> out_;
			uint8_t cur_ = 0;
			int used_ = 0;
		};

		// Burrows-Wheeler transform over the cyclic rotations of `block`.
		// Returns the last column and the row of the unrotated string.
		// Prefix doubling; identical rotations (periodic blocks) may land in
		// any order, which is fine -- they decode to the same text.
		void Bwt(const std::vector<uint8_t> &block, std::vector<uint8_t> &last, uint32_t &origPtr)
		{
			const size_t n = block.size();
			last.resize(n);
			if (n == 1)
			{
				last[0] = block[0];
				origPtr = 0;
				return;
			}

			struct Item
			{
				uint64_t key;
				uint32_t idx;
			};
			std::vector<uint32_t> rank(n), nextRank(n);
			std::vector<Item> items(n);

			// Initial rank: first 4 bytes of each rotation (cyclic).
			size_t k = 4;
			for (size_t i = 0; i < n; ++i)
			{
				uint64_t key = 0;
				for (size_t j = 0; j < 4; ++j)
					key = (key << 8) | block[(i + j) % n];
				items[i] = { key, static_cast<uint32_t>(i) };
			}
			const auto byKey = [](const Item &a, const Item &b) {
				return a.key != b.key ? a.key < b.key : a.idx < b.idx;
			};
			std::sort(items.begin(), items.end(), byKey);

			const auto assignRanks = [&] {
				uint32_t r = 0;
				nextRank[items[0].idx] = 0;
				for (size_t i = 1; i < n; ++i)
				{
					if (items[i].key != items[i - 1].key)
						++r;
					nextRank[items[i].idx] = r;
				}
				rank.swap(nextRank);
				return static_cast<size_t>(r) + 1;
			};
			size_t distinct = assignRanks();

			while (distinct < n && k < n)
			{
				for (size_t i = 0; i < n; ++i)
				{
					size_t j = i + k;
					if (j >= n)
						j -= n;
					items[i] = { (static_cast<uint64_t>(rank[i]) << 32) | rank[j],
						static_cast<uint32_t>(i) };
				}
				std::sort(items.begin(), items.end(), byKey);
				distinct = assignRanks();
				k *= 2;
			}

			for (size_t row = 0; row < n; ++row)
			{
				const size_t start = items[row].idx;
				last[row] = block[start == 0 ? n - 1 : start - 1];
				if (start == 0)
					origPtr = static_cast<uint32_t>(row);
			}
		}

		// Length-limited Huffman code lengths for `freq` (all symbols get a
		// code: zero frequencies are treated as 1).
		std::vector<uint8_t> HuffmanLengths(const std::vector<uint32_t> &freq)
		{
			const size_t alpha = freq.size();
			std::vector<uint64_t> weight(alpha);
			for (size_t i = 0; i < alpha; ++i)
				weight[i] = freq[i] == 0 ? 1 : freq[i];

			for (;;)
			{
				// Simple O(n log n) build with a sorted work list.
				struct Node { uint64_t w; int left; int right; };
				std::vector<Node> nodes;
				nodes.reserve(alpha * 2);
				std::vector<int> heap;
				for (size_t i = 0; i < alpha; ++i)
				{
					nodes.push_back({ weight[i], -1, -1 });
					heap.push_back(static_cast<int>(i));
				}
				const auto cmp = [&](int a, int b) {
					return nodes[a].w != nodes[b].w ? nodes[a].w > nodes[b].w : a > b;
				};
				std::make_heap(heap.begin(), heap.end(), cmp);
				while (heap.size() > 1)
				{
					std::pop_heap(heap.begin(), heap.end(), cmp);
					const int a = heap.back();
					heap.pop_back();
					std::pop_heap(heap.begin(), heap.end(), cmp);
					const int b = heap.back();
					heap.pop_back();
					nodes.push_back({ nodes[a].w + nodes[b].w, a, b });
					heap.push_back(static_cast<int>(nodes.size()) - 1);
					std::push_heap(heap.begin(), heap.end(), cmp);
				}

				std::vector<uint8_t> len(alpha, 0);
				uint32_t maxLen = 0;
				// Iterative depth walk from the root (last node).
				std::vector<std::pair<int, uint32_t>> stack;
				stack.push_back({ static_cast<int>(nodes.size()) - 1, 0 });
				while (!stack.empty())
				{
					const auto [id, depth] = stack.back();
					stack.pop_back();
					if (nodes[id].left < 0)
					{
						const uint32_t d = depth == 0 ? 1 : depth;
						len[static_cast<size_t>(id)] = static_cast<uint8_t>(std::min<uint32_t>(d, 255));
						maxLen = std::max(maxLen, d);
					}
					else
					{
						stack.push_back({ nodes[id].left, depth + 1 });
						stack.push_back({ nodes[id].right, depth + 1 });
					}
				}
				if (maxLen <= kMaxCodeLen)
					return len;
				// Too deep: flatten the distribution and retry (what bzip2 does).
				for (auto &w : weight)
					w = 1 + w / 2;
			}
		}

		void AssignCodes(const std::vector<uint8_t> &len, std::vector<uint32_t> &code)
		{
			uint8_t minLen = 255, maxLen = 0;
			for (uint8_t l : len)
			{
				minLen = std::min(minLen, l);
				maxLen = std::max(maxLen, l);
			}
			code.assign(len.size(), 0);
			uint32_t vec = 0;
			for (uint32_t n = minLen; n <= maxLen; ++n)
			{
				for (size_t i = 0; i < len.size(); ++i)
					if (len[i] == n)
						code[i] = vec++;
				vec <<= 1;
			}
		}

		void WriteBlock(BitWriter &bw, const std::vector<uint8_t> &rle1, uint32_t blockCrc)
		{
			std::vector<uint8_t> last;
			uint32_t origPtr = 0;
			Bwt(rle1, last, origPtr);

			// Symbols in use -> compact indexes.
			std::array<bool, 256> inUse{};
			for (uint8_t b : last)
				inUse[b] = true;
			std::array<uint8_t, 256> toIndex{};
			uint32_t nInUse = 0;
			for (int i = 0; i < 256; ++i)
				if (inUse[i])
					toIndex[i] = static_cast<uint8_t>(nInUse++);
			const uint32_t alphaSize = nInUse + 2;
			const uint32_t eob = nInUse + 1;

			// MTF + zero-run coding (RUNA = 0, RUNB = 1, value v>0 -> v+1).
			std::vector<uint16_t> mtfOut;
			mtfOut.reserve(last.size() + 1);
			std::array<uint8_t, 256> mtf{};
			for (uint32_t i = 0; i < nInUse; ++i)
				mtf[i] = static_cast<uint8_t>(i);
			size_t zeroRun = 0;
			const auto flushRun = [&] {
				if (zeroRun == 0)
					return;
				size_t r = zeroRun - 1;
				for (;;)
				{
					mtfOut.push_back((r & 1) ? 1 : 0);
					if (r < 2)
						break;
					r = (r - 2) / 2;
				}
				zeroRun = 0;
			};
			for (uint8_t b : last)
			{
				const uint8_t idx = toIndex[b];
				if (mtf[0] == idx)
				{
					++zeroRun;
					continue;
				}
				flushRun();
				size_t pos = 1;
				while (mtf[pos] != idx)
					++pos;
				std::memmove(&mtf[1], &mtf[0], pos);
				mtf[0] = idx;
				mtfOut.push_back(static_cast<uint16_t>(pos + 1));
			}
			flushRun();
			mtfOut.push_back(static_cast<uint16_t>(eob));

			// One Huffman table, used by two identical table slots (bzip2
			// requires at least two).
			std::vector<uint32_t> freq(alphaSize, 0);
			for (uint16_t s : mtfOut)
				++freq[s];
			const std::vector<uint8_t> lens = HuffmanLengths(freq);
			std::vector<uint32_t> codes;
			AssignCodes(lens, codes);

			// --- block header ---
			bw.Put(0x314159u, 24);
			bw.Put(0x265359u, 24);
			bw.Put(blockCrc, 32);
			bw.Put(0, 1);  // not randomised
			bw.Put(origPtr, 24);

			// Symbol map: 16 groups of 16.
			uint32_t groupMask = 0;
			for (int g = 0; g < 16; ++g)
				for (int j = 0; j < 16; ++j)
					if (inUse[static_cast<size_t>(g * 16 + j)])
						groupMask |= 0x8000u >> g;
			bw.Put(groupMask, 16);
			for (int g = 0; g < 16; ++g)
			{
				if (!(groupMask & (0x8000u >> g)))
					continue;
				uint32_t bits = 0;
				for (int j = 0; j < 16; ++j)
					if (inUse[static_cast<size_t>(g * 16 + j)])
						bits |= 0x8000u >> j;
				bw.Put(bits, 16);
			}

			const size_t nSelectors = (mtfOut.size() + kGroupSize - 1) / kGroupSize;
			bw.Put(2, 3);  // two tables
			bw.Put(static_cast<uint32_t>(nSelectors), 15);
			for (size_t i = 0; i < nSelectors; ++i)
				bw.Put(0, 1);  // selector 0, MTF-coded unary "0"

			for (int t = 0; t < 2; ++t)
			{
				int cur = lens[0];
				bw.Put(static_cast<uint32_t>(cur), 5);
				for (uint32_t i = 0; i < alphaSize; ++i)
				{
					while (cur < lens[i])
					{
						bw.Put(2, 2);  // "10": increment
						++cur;
					}
					while (cur > lens[i])
					{
						bw.Put(3, 2);  // "11": decrement
						--cur;
					}
					bw.Put(0, 1);
				}
			}

			for (uint16_t s : mtfOut)
				bw.Put(codes[s], lens[s]);
		}
	}

	std::vector<uint8_t> CompressBZip2(const uint8_t *data, size_t size, int blockSize100k)
	{
		if (blockSize100k < 1 || blockSize100k > 9)
			throw std::invalid_argument("bzip2: block size must be 1..9");
		const size_t blockMax = static_cast<size_t>(blockSize100k) * 100000 - 19;

		BitWriter bw;
		bw.Put('B', 8);
		bw.Put('Z', 8);
		bw.Put('h', 8);
		bw.Put(static_cast<uint32_t>('0' + blockSize100k), 8);

		uint32_t combinedCrc = 0;
		std::vector<uint8_t> block;
		block.reserve(blockMax + 8);
		uint32_t blockCrc = 0xFFFFFFFFu;

		const auto closeBlock = [&] {
			if (block.empty())
				return;
			const uint32_t crc = ~blockCrc;
			combinedCrc = ((combinedCrc << 1) | (combinedCrc >> 31)) ^ crc;
			WriteBlock(bw, block, crc);
			block.clear();
			blockCrc = 0xFFFFFFFFu;
		};

		size_t i = 0;
		while (i < size)
		{
			// RLE1: runs of 4..255 equal bytes -> 4 bytes + (run - 4).
			size_t run = 1;
			while (i + run < size && run < 255 && data[i + run] == data[i])
				++run;
			if (block.size() + 5 > blockMax)
				closeBlock();
			if (run >= 4)
			{
				for (int j = 0; j < 4; ++j)
					block.push_back(data[i]);
				block.push_back(static_cast<uint8_t>(run - 4));
			}
			else
			{
				for (size_t j = 0; j < run; ++j)
					block.push_back(data[i]);
			}
			// The block CRC covers the ORIGINAL bytes.
			for (size_t j = 0; j < run; ++j)
				blockCrc = Crc32Msb(blockCrc, data + i, 1);
			i += run;
		}
		closeBlock();

		bw.Put(0x177245u, 24);
		bw.Put(0x385090u, 24);
		bw.Put(combinedCrc, 32);
		bw.Flush();
		return std::move(bw.Bytes());
	}
}
