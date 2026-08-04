#pragma once

#include <cstdint>
#include <vector>

#include <QImage>

// Minimal Truetype/Targa (.tga) decoder.
//
// Qt ships without TGA support out of the box, and the CS Online pak files
// store most of their textures as TGA, so this is a small, self-contained
// decoder rather than a dependency on an external imaging library.
//
// Supported:
//  - Image types 2 (truecolor) and 10 (RLE truecolor), 16/24/32 bpp
//  - Image types 3 (grayscale) and 11 (RLE grayscale), 8 bpp
//  - Image types 1 (color-mapped) and 9 (RLE color-mapped), 8 bpp indices
//    into a 15/16/24/32 bpp color map
//  - Both bottom-up and top-down row order (image descriptor bit 5)
//
// Not supported: right-to-left pixel order (image descriptor bit 4), which
// does not occur in game texture exports in practice.
namespace tga
{
	// Returns a null QImage if `data` is not a TGA file this decoder
	// understands. `errorMessage`, if provided, receives a short explanation.
	QImage Load(const std::vector<uint8_t> &data, QString *errorMessage = nullptr);
}
