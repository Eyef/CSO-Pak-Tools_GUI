#include "IceCipher.h"

#include <array>

namespace cso_pak
{
	namespace
	{
		constexpr int kSMod[4][4] = {
			{ 333, 313, 505, 369 },
			{ 379, 375, 319, 391 },
			{ 361, 445, 451, 397 },
			{ 397, 425, 395, 505 },
		};
		constexpr int kSXor[4][4] = {
			{ 0x83, 0x85, 0x9b, 0xcd },
			{ 0xcc, 0xa7, 0xad, 0x41 },
			{ 0x4b, 0x2e, 0xd4, 0x33 },
			{ 0xea, 0xcb, 0x2e, 0x04 },
		};
		constexpr uint32_t kPBox[32] = {
			0x00000001, 0x00000080, 0x00000400, 0x00002000,
			0x00080000, 0x00200000, 0x01000000, 0x40000000,
			0x00000008, 0x00000020, 0x00000100, 0x00004000,
			0x00010000, 0x00800000, 0x04000000, 0x20000000,
			0x00000004, 0x00000010, 0x00000200, 0x00008000,
			0x00020000, 0x00400000, 0x08000000, 0x10000000,
			0x00000002, 0x00000040, 0x00000800, 0x00001000,
			0x00040000, 0x00100000, 0x02000000, 0x80000000,
		};
		constexpr size_t kKeyRotation[16] = {
			0, 1, 2, 3, 2, 1, 3, 0, 1, 3, 2, 0, 3, 1, 0, 2
		};

		uint32_t GfMultiply(uint32_t a, uint32_t b, uint32_t m)
		{
			uint32_t result = 0;
			while (b != 0)
			{
				if (b & 1)
					result ^= a;
				a <<= 1;
				b >>= 1;
				if (a >= 256)
					a ^= m;
			}
			return result;
		}

		uint32_t GfExp7(uint32_t b, uint32_t m)
		{
			if (b == 0)
				return 0;
			uint32_t x = GfMultiply(b, b, m);
			x = GfMultiply(b, x, m);
			x = GfMultiply(x, x, m);
			return GfMultiply(b, x, m);
		}

		uint32_t Perm32(uint32_t x)
		{
			uint32_t result = 0;
			size_t bit = 0;
			while (x != 0)
			{
				if (x & 1)
					result |= kPBox[bit];
				++bit;
				x >>= 1;
			}
			return result;
		}

		using SBoxTable = std::array<std::array<uint32_t, 1024>, 4>;

		const SBoxTable &SBox()
		{
			// Built once on first use (function-local static: thread-safe).
			static const SBoxTable table = []
			{
				SBoxTable t{};
				for (uint32_t i = 0; i < 1024; ++i)
				{
					const uint32_t col = (i >> 1) & 0xff;
					const uint32_t row = (i & 0x1) | ((i & 0x200) >> 8);
					t[0][i] = Perm32(GfExp7(col ^ kSXor[0][row], kSMod[0][row]) << 24);
					t[1][i] = Perm32(GfExp7(col ^ kSXor[1][row], kSMod[1][row]) << 16);
					t[2][i] = Perm32(GfExp7(col ^ kSXor[2][row], kSMod[2][row]) << 8);
					t[3][i] = Perm32(GfExp7(col ^ kSXor[3][row], kSMod[3][row]));
				}
				return t;
			}();
			return table;
		}

		uint32_t LoadBE(const uint8_t *p)
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
				(uint32_t(p[2]) << 8) | uint32_t(p[3]);
		}

		void StoreBE(uint8_t *p, uint32_t v)
		{
			p[0] = static_cast<uint8_t>(v >> 24);
			p[1] = static_cast<uint8_t>(v >> 16);
			p[2] = static_cast<uint8_t>(v >> 8);
			p[3] = static_cast<uint8_t>(v);
		}
	}

	void IceCipher::BuildSchedule(uint16_t keyBuilder[4], size_t base, size_t rotationOffset)
	{
		for (size_t i = 0; i < 8; ++i)
		{
			const size_t rotation = kKeyRotation[rotationOffset + i];
			auto &row = schedule_[base + i];
			row = { 0, 0, 0 };
			for (size_t j = 0; j < 15; ++j)
			{
				for (size_t k = 0; k < 4; ++k)
				{
					const uint16_t current = keyBuilder[(rotation + k) & 3];
					const uint16_t bit = static_cast<uint16_t>(current & 1);
					row[j % 3] = (row[j % 3] << 1) | bit;
					keyBuilder[(rotation + k) & 3] = static_cast<uint16_t>(
						(current >> 1) | ((bit ^ 1) << 15));
				}
			}
		}
	}

	IceCipher::IceCipher(const uint8_t *key)
	{
		uint16_t builder[4];
		for (size_t block = 0; block < kLevel; ++block)
		{
			const size_t pos = block << 3;
			for (size_t j = 0; j < 4; ++j)
				builder[3 - j] = static_cast<uint16_t>(
					(key[pos + (j << 1)] << 8) | key[pos + (j << 1) + 1]);
			BuildSchedule(builder, pos, 0);
			BuildSchedule(builder, kRounds - 8 - pos, 8);
		}
	}

	uint32_t IceCipher::Transform(uint32_t value, size_t subKey) const
	{
		const auto &ks = schedule_[subKey];
		const uint32_t tl = ((value >> 16) & 0x3ff) | (((value >> 14) | (value << 18)) & 0xffc00);
		const uint32_t tr = (value & 0x3ff) | ((value << 2) & 0xffc00);
		uint32_t al = ks[2] & (tl ^ tr);
		uint32_t ar = al ^ tr;
		al ^= tl;
		al ^= ks[0];
		ar ^= ks[1];
		const SBoxTable &s = SBox();
		return s[0][al >> 10] | s[1][al & 0x3ff] | s[2][ar >> 10] | s[3][ar & 0x3ff];
	}

	void IceCipher::EncryptBlock(uint8_t block[kBlockSize]) const
	{
		uint32_t l = LoadBE(block);
		uint32_t r = LoadBE(block + 4);
		for (size_t i = 0; i < kRounds; i += 2)
		{
			l ^= Transform(r, i);
			r ^= Transform(l, i + 1);
		}
		StoreBE(block, r);
		StoreBE(block + 4, l);
	}

	void IceCipher::DecryptBlock(uint8_t block[kBlockSize]) const
	{
		uint32_t l = LoadBE(block);
		uint32_t r = LoadBE(block + 4);
		// Rounds run backwards: 63,62, 61,60, ... 1,0 (counter is unsigned,
		// so count down by pairs from kRounds instead of testing i > 0).
		for (size_t pair = kRounds; pair > 0; pair -= 2)
		{
			l ^= Transform(r, pair - 1);
			r ^= Transform(l, pair - 2);
		}
		StoreBE(block, r);
		StoreBE(block + 4, l);
	}

	size_t IceCipher::Decrypt(uint8_t *data, size_t size) const
	{
		const size_t aligned = size & ~static_cast<size_t>(7);
		for (size_t i = 0; i < aligned; i += kBlockSize)
			DecryptBlock(data + i);
		return aligned;
	}

	size_t IceCipher::Encrypt(uint8_t *data, size_t size) const
	{
		const size_t aligned = size & ~static_cast<size_t>(7);
		for (size_t i = 0; i < aligned; i += kBlockSize)
			EncryptBlock(data + i);
		return aligned;
	}
}
