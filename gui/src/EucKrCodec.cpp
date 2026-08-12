#define NOMINMAX
#include "EucKrCodec.h"

#include <windows.h>

#include <algorithm>

namespace cso_gui
{
	namespace
	{
		constexpr UINT KoreanCodepage = 949;

		bool ContainsHangul(std::wstring_view text)
		{
			for (const wchar_t ch : text)
			{
				const unsigned codePoint = static_cast<unsigned>(ch);
				// Hangul syllables, jamo, and compatibility jamo.
				if ((codePoint >= 0xAC00 && codePoint <= 0xD7A3) ||
					(codePoint >= 0x1100 && codePoint <= 0x11FF) ||
					(codePoint >= 0x3130 && codePoint <= 0x318F))
					return true;
			}
			return false;
		}

		bool DecodeToWide(std::string_view input, std::wstring &output)
		{
			if (input.empty())
			{
				output.clear();
				return true;
			}

			const int required = MultiByteToWideChar(
				KoreanCodepage, MB_ERR_INVALID_CHARS, input.data(),
				static_cast<int>(input.size()), nullptr, 0);
			if (required <= 0)
				return false;

			output.resize(static_cast<size_t>(required));
			MultiByteToWideChar(KoreanCodepage, MB_ERR_INVALID_CHARS, input.data(),
				static_cast<int>(input.size()), output.data(), required);
			return true;
		}

		std::string WideToUtf8(std::wstring_view input)
		{
			if (input.empty())
				return {};

			const int required = WideCharToMultiByte(
				CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
				nullptr, 0, nullptr, nullptr);
			if (required <= 0)
				return {};

			std::string output(static_cast<size_t>(required), '\0');
			WideCharToMultiByte(CP_UTF8, 0, input.data(),
				static_cast<int>(input.size()), output.data(), required,
				nullptr, nullptr);
			return output;
		}
	}

	std::string DecodeEucKrToUtf8(const std::vector<uint8_t> &data)
	{
		if (data.empty())
			return {};

		std::string_view input(reinterpret_cast<const char *>(data.data()),
			data.size());

		std::wstring wide;
		if (!DecodeToWide(input, wide))
			return {};

		if (!ContainsHangul(wide))
			return {};

		return WideToUtf8(wide);
	}

	bool LooksLikeEucKr(const std::vector<uint8_t> &data)
	{
		return !DecodeEucKrToUtf8(data).empty();
	}
}