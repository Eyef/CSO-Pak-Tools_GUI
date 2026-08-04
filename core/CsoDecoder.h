#pragma once

#include <cstdint>
#include <vector>

namespace cso_pak
{
	// Decryption for Counter-Strike: Online `.cso` (raw skill/table data) files.
	// The format is a TEA/XTEA-style block cipher with a fixed key, applied in
	// 32-bit word pairs over the whole buffer.
	class CsoDecoder
	{
	public:
		// Decrypts `data` in place and returns it. Non-8-byte trailing bytes are
		// left untouched (they are usually padding).
		static void Decrypt(std::vector<uint8_t> &data);

	private:
		static void DecryptPair(uint32_t &v0, uint32_t &v1);
	};
}