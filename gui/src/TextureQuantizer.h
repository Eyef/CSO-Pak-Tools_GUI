#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace cso_gui
{
	// An 8-bit paletted picture as stored in a classic GoldSrc .mdl texture.
	struct IndexedTexture
	{
		int width = 0;
		int height = 0;
		std::vector<std::uint8_t> indices;          // width * height palette indices
		std::array<std::uint8_t, 768> palette{};    // 256 * RGB
		double transparentFraction = 0.0;           // masked textures only
	};

	// Reduces an RGBA picture (bytes in R,G,B,A order, tightly packed) to 256 colours.
	//
	// The palette is built with median cut over the distinct colours and then
	// refined with a few frequency-weighted k-means (Lloyd) iterations, like the
	// reference Python converter.  With `masked` set, texels whose alpha is below
	// `alphaThreshold` become index 255, which is palette colour (0, 0, 255) -
	// the GoldSrc convention for "transparent" - and only 255 colours are used
	// for the picture.  `dither` enables Floyd-Steinberg error diffusion.
	IndexedTexture QuantizeRgba(const std::uint8_t *rgba, int width, int height,
		bool masked, int alphaThreshold, bool dither, int refineIterations);

	// Resizes an RGBA picture (R,G,B,A byte order) with an area-weighted
	// (box / bilinear-for-upscale) filter.  Used when a texture is too large for
	// the classic format.
	std::vector<std::uint8_t> ResizeRgba(const std::uint8_t *rgba, int width, int height,
		int newWidth, int newHeight);
}
