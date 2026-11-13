#pragma once

#include <cstdint>

namespace cso_gui
{
	// Decompresses one BC7 (BPTC, DXGI_FORMAT_BC7_UNORM) block image into
	// tightly packed RGBA bytes (4 bytes/pixel, row-major). Same contract as
	// DecompressDXT1/3/5: `input` holds ceil(w/4)*ceil(h/4) 16-byte blocks,
	// `output` has room for width*height*4 bytes.
	void DecompressBC7(const uint8_t *input, int width, int height, uint8_t *output);
}
