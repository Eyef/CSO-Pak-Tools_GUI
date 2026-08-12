#pragma once

#include <cstdint>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// Decodes a standalone top-level (mip 0 only -- any further mipmaps in
	// the file are ignored, we only need one image for preview) .dds file.
	//
	// Supports the block-compressed formats DXT1/DXT3/DXT5, and uncompressed
	// RGB layouts (16/24/32 bits per pixel, whatever channel order the file's
	// bitmasks describe -- covers the common BGRA8888/BGR888/RGB565 etc.
	// variants). Anything else (BC4-7/ATI2, DX10 extended header, YUV, plain
	// paletted, ...) throws std::runtime_error, same as an unsupported .cso
	// or .mdl does elsewhere in this app.
	QImage LoadDdsImage(const std::vector<uint8_t> &data);
}
