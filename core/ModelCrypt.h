#pragma once

#include <cstdint>
#include <vector>

namespace cso_pak
{
	// Counter-Strike Online model (.mdl) encryption.
	//
	// Encrypted CSO models keep the normal "IDST" studio header, but their
	// version field is 20 or 21 (instead of a plain studio version) and two
	// kinds of data blocks are ICE-encrypted (see IceCipher):
	//   * every texture's pixels + 768-byte palette  (width*height + 768 bytes)
	//   * every sub-model's vertex array              (numverts * 12 bytes)
	// Version 20 and 21 select one of two fixed 32-byte keys. Decrypting
	// rewrites the version field to 9, which is what the reference tools
	// (NARTools ModelDecryptor / CSNZ-Nar) mark "plain" with; the layout of
	// the remaining header is the usual GoldSrc studio layout.
	//
	// ICE works on 8-byte blocks and the game applies it in chunks of up to
	// 1024 bytes. If the final chunk of a region isn't a multiple of 8 bytes
	// it is left unencrypted (verified on v_famas_l85a2.mdl: a 508x507
	// texture has a 276-byte plain tail = the end of its palette).
	namespace ModelCrypt
	{
		enum class Result
		{
			Success,
			NotModel,         // no "IDST" signature / too small
			NotEncrypted,     // Decrypt(): version isn't 20/21
			AlreadyEncrypted, // Encrypt(): version is already 20/21
			Invalid,          // header fields don't make sense
		};

		constexpr int32_t kPlainVersion = 9;  // written by Decrypt()

		// 20 or 21 if `data` is an encrypted CSO model, 0 otherwise
		// (including "not a model at all").
		int EncryptedVersion(const uint8_t *data, size_t size);
		inline int EncryptedVersion(const std::vector<uint8_t> &data)
		{
			return EncryptedVersion(data.data(), data.size());
		}

		// In-place. On success `originalVersion` (optional) receives 20/21.
		Result Decrypt(std::vector<uint8_t> &data, int *originalVersion = nullptr);

		// In-place. `version` must be 20 or 21. The model must not already
		// be encrypted (any other studio version is accepted as plaintext).
		Result Encrypt(std::vector<uint8_t> &data, int version);
	}
}
