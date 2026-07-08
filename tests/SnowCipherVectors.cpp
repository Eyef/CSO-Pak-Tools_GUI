#include "SnowCipher.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
	uint8_t HexNibble(char ch)
	{
		if (ch >= '0' && ch <= '9')
			return static_cast<uint8_t>(ch - '0');
		if (ch >= 'a' && ch <= 'f')
			return static_cast<uint8_t>(ch - 'a' + 10);
		if (ch >= 'A' && ch <= 'F')
			return static_cast<uint8_t>(ch - 'A' + 10);

		throw std::runtime_error("invalid hex digit");
	}

	std::array<uint8_t, 64> Hex64(std::string_view hex)
	{
		if (hex.size() != 128)
			throw std::runtime_error("invalid vector length");

		std::array<uint8_t, 64> result = {};
		for (size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<uint8_t>((HexNibble(hex[i * 2]) << 4) |
				HexNibble(hex[i * 2 + 1]));

		return result;
	}

	std::array<uint8_t, 128> SequentialKey()
	{
		std::array<uint8_t, 128> key = {};
		for (size_t i = 0; i < key.size(); ++i)
			key[i] = static_cast<uint8_t>(i);
		return key;
	}

	std::array<uint8_t, 128> XorKey()
	{
		std::array<uint8_t, 128> key = {};
		for (size_t i = 0; i < key.size(); ++i)
			key[i] = static_cast<uint8_t>(0xA5u ^ (i * 7u));
		return key;
	}

	bool CheckVector(const std::array<uint8_t, 128> &key,
		std::array<uint8_t, 64> plain,
		const std::array<uint8_t, 64> &expectedCipher)
	{
		auto encrypted = plain;
		SnowCipher encryptCipher;
		encryptCipher.SetKey(key.data());
		encryptCipher.EncryptBuffer(encrypted.data(), encrypted.data(),
			static_cast<uint32_t>(encrypted.size()));

		if (encrypted != expectedCipher)
		{
			std::cerr << "encrypted bytes did not match vector\n";
			return false;
		}

		SnowCipher decryptCipher;
		decryptCipher.SetKey(key.data());
		decryptCipher.DecryptBuffer(encrypted.data(), encrypted.data(),
			static_cast<uint32_t>(encrypted.size()));

		if (encrypted != plain)
		{
			std::cerr << "decrypt(encrypt(plain)) did not return plain\n";
			return false;
		}

		return true;
	}
}

int main()
{
	const auto plain1 = Hex64(
		"101316191c1f2225282b2e3134373a3d404346494c4f5255585b5e6164676a6d"
		"707376797c7f8285888b8e9194979a9da0a3a6a9acafb2b5b8bbbec1c4c7cacd");
	const auto cipher1 = Hex64(
		"c2fbe010df23de17789784d5e41ba64e95924393a3aae74768f7bdff16baf94e"
		"ac8a9b323b957684a0f6a34714dae128848ad46a9fcea33bbb6410d80b003953");

	const auto plain2 = Hex64(
		"4144474a4d505356595c5f6265686b6e7174777a7d808386898c8f9295989b9e"
		"a1a4a7aaadb0b3b6b9bcbfc2c5c8cbced1d4d7dadde0e3e6e9eceff2f5f8fbfe");
	const auto cipher2 = Hex64(
		"ce6f52b277d87859ee38c1c453cdc7d40c56696a9cdbaa47cd6f579b03c6d659"
		"cf75e51dc48f7ab70310024aff8ab082fb17e1bb0eaf50d6463e87b6bde63928");

	if (!CheckVector(SequentialKey(), plain1, cipher1))
		return 1;
	if (!CheckVector(XorKey(), plain2, cipher2))
		return 1;

	return 0;
}
