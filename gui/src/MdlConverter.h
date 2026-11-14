#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "StudioModel.h"

namespace cso_gui
{
	// Options for the "2026 -> classic v10" model converter.  The defaults
	// match the reference Python tool (mdl2026_to_v10.py).
	struct MdlConvertSettings
	{
		bool ditherTextures = true;
		int paletteRefineIterations = 4;
		int alphaThreshold = 128;
		int maxTextureSize = 512;
		bool renameDdsToBmp = true;
	};

	// What the conversion did, for the summary shown to the user.
	struct MdlConvertReport
	{
		int bones = 0;
		int textures = 0;
		int bodyParts = 0;
		int models = 0;
		int sourceVertices = 0;
		int vertices = 0;
		int normals = 0;
		int triangles = 0;
		int strips = 0;
		int blendedVertices = 0;      // source vertices weighted to 2+ bones
		int weakBlendVertices = 0;    // ... whose heaviest bone is below 75 %
		std::vector<std::string> warnings;
	};

	// True for a decrypted CSO 2026 studio model (starts with "IDST", version 2026).
	bool IsModel2026(const std::vector<std::uint8_t> &data);

	// Converts a parsed 2026 model into a classic GoldSrc v10 file.
	//
	// `model` must have been loaded from exactly `source` (the raw, already
	// decrypted .mdl bytes): the skeleton/animation front block and the skin
	// table are copied from those bytes, while geometry and textures are
	// rebuilt in the old layout.  External <model><N>.seq files are not part
	// of this function (the caller copies the compatible ones).
	//
	// Throws std::runtime_error with a user-readable message on failure; the
	// produced file is parsed again before returning, so a successful call
	// yields a model this program can load.
	std::vector<std::uint8_t> ConvertModel2026ToV10(const StudioModel &model,
		const std::vector<std::uint8_t> &source, const MdlConvertSettings &settings,
		MdlConvertReport *report);

	// Structural validation of a v10 model (used after conversion, also
	// handy for tests).  Returns an empty string when the file is fine.
	std::string ValidateV10Model(const std::vector<std::uint8_t> &data);
}
