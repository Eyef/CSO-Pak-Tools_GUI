#include "CsoDecoder.h"

#include <array>

namespace cso_pak
{
	namespace
	{
		constexpr std::array<uint32_t, 4> CsoKey = {
			0x4C7ADF03,
			0x5F30E75F,
			0x1D149820,
			0x03985ADF,
		};

		constexpr uint32_t CsoDelta = 0x9E3779B9;
		constexpr uint32_t CsoRounds = 32;
	}

	void CsoDecoder::DecryptPair(uint32_t &v0, uint32_t &v1)
	{
		uint32_t sum = CsoDelta * CsoRounds;
		for (uint32_t r = 0; r < CsoRounds; ++r)
		{
			v1 -= ((v0 << 4) - CsoKey[2]) ^ (v0 + sum) ^ ((v0 >> 5) - CsoKey[3]);
			v0 -= ((v1 << 4) - CsoKey[0]) ^ (v1 + sum) ^ ((v1 >> 5) + CsoKey[1]);
			sum -= CsoDelta;
		}
	}

	void CsoDecoder::Decrypt(std::vector<uint8_t> &data)
	{
		const size_t count = data.size() / sizeof(uint32_t);
		for (size_t i = 0; i + 1 < count; i += 2)
		{
			uint32_t v0 = 0;
			uint32_t v1 = 0;
			for (size_t b = 0; b < 4; ++b)
			{
				v0 |= static_cast<uint32_t>(data[i * 4 + b]) << (b * 8);
				v1 |= static_cast<uint32_t>(data[i * 4 + 4 + b]) << (b * 8);
			}
			DecryptPair(v0, v1);
			for (size_t b = 0; b < 4; ++b)
			{
				data[i * 4 + b] = static_cast<uint8_t>(v0 >> (b * 8));
				data[i * 4 + 4 + b] = static_cast<uint8_t>(v1 >> (b * 8));
			}
		}
	}
}