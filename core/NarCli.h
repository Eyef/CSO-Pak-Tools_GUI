#pragma once

#include <string_view>

namespace cso_pak
{
	// Prints the nar-* part of the usage text.
	void PrintNarUsage(const char *programName);

	// Handles nar-list / nar-unpack / nar-pack / nar-patch. Returns -1 if
	// `command` isn't one of them, otherwise the process exit code
	// (2 = bad arguments, usage should be shown).
	int RunNarCommand(std::string_view command, int argc, char **argv);
}
