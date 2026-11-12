#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cso_pak
{
	// ICE block cipher (Matthew Kwan's "Information Concealment Engine"),
	// ECB, no padding. Counter-Strike Online encrypts parts of its .mdl
	// models with the "level 4" variant: 64-bit blocks, 64 rounds, 32-byte
	// key. Ported from NARTools' NARToolLib (IceCryptoTransform) and
	// cross-checked against CSNZ-Nar's Ice.cs.
	class IceCipher
	{
	public:
		static constexpr size_t kBlockSize = 8;
		static constexpr size_t kLevel = 4;
		static constexpr size_t kKeySize = kLevel * 8;  // 32 bytes
		static constexpr size_t kRounds = kLevel * 16;  // 64 rounds

		// `key` must point at kKeySize bytes.
		explicit IceCipher(const uint8_t *key);

		// In-place. `size` is rounded DOWN to a multiple of 8; trailing
		// bytes that don't form a full block are left untouched.
		// Returns the number of bytes actually transformed.
		size_t Decrypt(uint8_t *data, size_t size) const;
		size_t Encrypt(uint8_t *data, size_t size) const;

		void DecryptBlock(uint8_t block[kBlockSize]) const;
		void EncryptBlock(uint8_t block[kBlockSize]) const;

	private:
		uint32_t Transform(uint32_t value, size_t subKey) const;
		void BuildSchedule(uint16_t keyBuilder[4], size_t base, size_t rotationOffset);

		std::array<std::array<uint32_t, 3>, kRounds> schedule_{};
	};
}
