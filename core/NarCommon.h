#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Constants and tiny helpers shared by the .nar reader (NarArchive) and the
// writer (NarWriter). Format notes live in NarArchive.h.
namespace cso_pak::nar
{
	constexpr uint32_t kSignature = 0x0052414Eu;  // "NAR\0" little-endian
	constexpr uint32_t kVersion = 0x01000000u;
	constexpr uint32_t kHeaderSizeXor = 0x4076551Fu;
	constexpr uint8_t kHeaderXor[16] = {
		0x19, 0x5B, 0x7B, 0x2C, 0x65, 0x5E, 0x79, 0x25,
		0x6E, 0x4B, 0x07, 0x21, 0x62, 0x7F, 0x00, 0x29
	};
	constexpr size_t kDictMax = 8192;  // LZ window == max match distance

	// Python string hash (h = h*1000003 ^ b, then h ^= length), wrapping
	// mod 2^32 -- the seed of the per-entry XOR key.
	inline uint32_t PythonHash(const uint8_t *data, size_t size)
	{
		uint32_t hash = 0;
		for (size_t i = 0; i < size; ++i)
			hash = hash * 1000003u ^ data[i];
		return hash ^ static_cast<uint32_t>(size);
	}

	// 16-byte XOR keystream derived from the entry path bytes (the same
	// bytes that are stored in the directory, i.e. EUC-KR, starting with '/').
	inline void MakeEntryKey(const uint8_t *pathBytes, size_t pathSize, uint8_t key[16])
	{
		uint32_t seed = PythonHash(pathBytes, pathSize);
		for (int i = 0; i < 16; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			key[i] = static_cast<uint8_t>(seed & 255u);
		}
	}

	// Standard CRC-32 (IEEE, reflected, poly 0xEDB88320) -- what the .nar
	// directory stores per entry, computed over the STORED bytes.
	class Crc32
	{
	public:
		void Update(const uint8_t *data, size_t size)
		{
			static const std::array<uint32_t, 256> table = [] {
				std::array<uint32_t, 256> t{};
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t c = i;
					for (int k = 0; k < 8; ++k)
						c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					t[i] = c;
				}
				return t;
			}();
			uint32_t crc = crc_;
			for (size_t i = 0; i < size; ++i)
				crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
			crc_ = crc;
		}
		uint32_t Value() const { return crc_ ^ 0xFFFFFFFFu; }

		static uint32_t Of(const uint8_t *data, size_t size)
		{
			Crc32 c;
			c.Update(data, size);
			return c.Value();
		}

	private:
		uint32_t crc_ = 0xFFFFFFFFu;
	};
}
