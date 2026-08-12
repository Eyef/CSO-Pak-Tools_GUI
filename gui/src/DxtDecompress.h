#pragma once

#include <cstdint>
#include <QImage>

namespace cso_gui
{
	// Allocates an RGBA8888 QImage, throwing if the dimensions are too large
	// to allocate (shared error handling for every decoder that builds one).
	QImage MakeRgbaImage(int width, int height);

	// Each decompresses one block-compressed image into tightly packed RGBA
	// bytes (4 bytes/pixel, row-major). `output` must have room for
	// width*height*4 bytes -- typically QImage::bits() from MakeRgbaImage.
	void DecompressDXT1(const uint8_t *input, int width, int height, uint8_t *output);
	void DecompressDXT3(const uint8_t *input, int width, int height, uint8_t *output);
	void DecompressDXT5(const uint8_t *input, int width, int height, uint8_t *output);
}
