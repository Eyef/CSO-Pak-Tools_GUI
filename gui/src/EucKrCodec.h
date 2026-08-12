#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cso_gui
{
	// Decodes a raw byte buffer as EUC-KR / CP949 (the Korean encoding used by
	// Counter-Strike: Online text tables) into UTF-8.
	//
	// Returns the decoded text, or an empty string when:
	//  - the buffer is not valid CP949, or
	//  - the decoded output contains no Hangul characters (so the guess is that
	//    the data is not actually Korean).
	//
	// This avoids mis-decoding plain DOS/ANSI text that happens to be valid
	// CP949 sequences.
	std::string DecodeEucKrToUtf8(const std::vector<uint8_t> &data);

	// True when `data` decodes as CP949 and contains at least one Hangul
	// syllable.
	bool LooksLikeEucKr(const std::vector<uint8_t> &data);
}