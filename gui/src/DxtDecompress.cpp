#include "DxtDecompress.h"

#include <stdexcept>

namespace cso_gui
{
namespace
{
	inline uint8_t Expand5(uint16_t v) { return static_cast<uint8_t>((v & 0x1F) * 255 / 31); }
	inline uint8_t Expand6(uint16_t v) { return static_cast<uint8_t>((v & 0x3F) * 255 / 63); }
}

QImage MakeRgbaImage(int width, int height)
{
	QImage image(width, height, QImage::Format_RGBA8888);
	if (image.isNull())
		throw std::runtime_error("image is too large to decode");
	return image;
}

// Decompress a DXT5 block image to tightly packed RGBA, keeping the
// interpolated alpha ramp.
void DecompressDXT5(const uint8_t *input, int width, int height, uint8_t *output)
{
	const int blocksX = (width + 3) / 4;
	const int blocksY = (height + 3) / 4;

	for (int blockY = 0; blockY < blocksY; ++blockY)
	{
		for (int blockX = 0; blockX < blocksX; ++blockX)
		{
			const uint8_t *block = input + static_cast<size_t>(blockY * blocksX + blockX) * 16;

			// Alpha: two 8-bit references + 3bpp selector ramp, 48-bit word.
			const uint8_t alpha0 = block[0];
			const uint8_t alpha1 = block[1];
			uint8_t alphaTable[8];
			alphaTable[0] = alpha0;
			alphaTable[1] = alpha1;
			if (alpha0 > alpha1)
			{
				for (int i = 1; i <= 6; ++i)
					alphaTable[1 + i] = static_cast<uint8_t>(((7 - i) * alpha0 + i * alpha1) / 7);
			}
			else
			{
				for (int i = 1; i <= 4; ++i)
					alphaTable[1 + i] = static_cast<uint8_t>(((5 - i) * alpha0 + i * alpha1) / 5);
				alphaTable[6] = 0;
				alphaTable[7] = 255;
			}
			uint64_t alphaBits = 0;
			for (int i = 0; i < 6; ++i)
				alphaBits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);

			// Colour: two 16-bit RGB565 references + 2bpp selectors.
			const uint16_t color0 = static_cast<uint16_t>(block[8] | (block[9] << 8));
			const uint16_t color1 = static_cast<uint16_t>(block[10] | (block[11] << 8));
			const uint32_t colorBits = static_cast<uint32_t>(block[12]) |
				(static_cast<uint32_t>(block[13]) << 8) |
				(static_cast<uint32_t>(block[14]) << 16) |
				(static_cast<uint32_t>(block[15]) << 24);

			uint8_t table[4][3];
			table[0][0] = Expand5(color0 >> 11); table[0][1] = Expand6(color0 >> 5); table[0][2] = Expand5(color0);
			table[1][0] = Expand5(color1 >> 11); table[1][1] = Expand6(color1 >> 5); table[1][2] = Expand5(color1);
			if (color0 > color1)
			{
				for (int c = 0; c < 3; ++c)
				{
					table[2][c] = static_cast<uint8_t>((2 * table[0][c] + table[1][c]) / 3);
					table[3][c] = static_cast<uint8_t>((table[0][c] + 2 * table[1][c]) / 3);
				}
			}
			else
			{
				for (int c = 0; c < 3; ++c)
				{
					table[2][c] = static_cast<uint8_t>((table[0][c] + table[1][c]) / 2);
					table[3][c] = 0;
				}
			}

			for (int i = 0; i < 16; ++i)
			{
				const int px = blockX * 4 + (i % 4);
				const int py = blockY * 4 + (i / 4);
				if (px >= width || py >= height) // Padded tail of partial blocks.
					continue;

				uint8_t *dst = output + (static_cast<size_t>(py) * width + px) * 4;
				const int code = static_cast<int>((colorBits >> (2 * i)) & 0x03);
				dst[0] = table[code][0];
				dst[1] = table[code][1];
				dst[2] = table[code][2];
				dst[3] = alphaTable[(alphaBits >> (3 * i)) & 0x07];
			}
		}
	}
}

// Decompress DXT1 to RGBA (1-bit cutout alpha when color0 <= color1).
void DecompressDXT1(const uint8_t *input, int width, int height, uint8_t *output)
{
	const int blocksX = (width + 3) / 4;
	const int blocksY = (height + 3) / 4;

	for (int blockY = 0; blockY < blocksY; ++blockY)
	{
		for (int blockX = 0; blockX < blocksX; ++blockX)
		{
			const uint8_t *block = input + static_cast<size_t>(blockY * blocksX + blockX) * 8;

			const uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
			const uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
			const uint32_t colorBits = static_cast<uint32_t>(block[4]) |
				(static_cast<uint32_t>(block[5]) << 8) |
				(static_cast<uint32_t>(block[6]) << 16) |
				(static_cast<uint32_t>(block[7]) << 24);

			uint8_t table[4][3];
			uint8_t alpha[4] = { 255, 255, 255, 255 };
			table[0][0] = Expand5(color0 >> 11); table[0][1] = Expand6(color0 >> 5); table[0][2] = Expand5(color0);
			table[1][0] = Expand5(color1 >> 11); table[1][1] = Expand6(color1 >> 5); table[1][2] = Expand5(color1);
			if (color0 > color1)
			{
				for (int c = 0; c < 3; ++c)
				{
					table[2][c] = static_cast<uint8_t>((2 * table[0][c] + table[1][c]) / 3);
					table[3][c] = static_cast<uint8_t>((table[0][c] + 2 * table[1][c]) / 3);
				}
			}
			else
			{
				for (int c = 0; c < 3; ++c)
					table[2][c] = static_cast<uint8_t>((table[0][c] + table[1][c]) / 2);
				table[3][0] = table[3][1] = table[3][2] = 0;
				alpha[3] = 0; // Cutout.
			}

			for (int i = 0; i < 16; ++i)
			{
				const int px = blockX * 4 + (i % 4);
				const int py = blockY * 4 + (i / 4);
				if (px >= width || py >= height)
					continue;

				uint8_t *dst = output + (static_cast<size_t>(py) * width + px) * 4;
				const int code = static_cast<int>((colorBits >> (2 * i)) & 0x03);
				dst[0] = table[code][0];
				dst[1] = table[code][1];
				dst[2] = table[code][2];
				dst[3] = alpha[code];
			}
		}
	}
}

// Decompress DXT3 to RGBA: explicit 4-bit-per-texel alpha (first 8 bytes)
// plus a DXT1-shaped color block (last 8 bytes) that's always decoded in
// opaque 4-color mode -- DXT3 never uses DXT1's 1-bit cutout-alpha branch,
// since alpha is stored separately.
void DecompressDXT3(const uint8_t *input, int width, int height, uint8_t *output)
{
	const int blocksX = (width + 3) / 4;
	const int blocksY = (height + 3) / 4;

	for (int blockY = 0; blockY < blocksY; ++blockY)
	{
		for (int blockX = 0; blockX < blocksX; ++blockX)
		{
			const uint8_t *block = input + static_cast<size_t>(blockY * blocksX + blockX) * 16;

			uint64_t alphaBits = 0;
			for (int i = 0; i < 8; ++i)
				alphaBits |= static_cast<uint64_t>(block[i]) << (8 * i);

			const uint8_t *colorBlock = block + 8;
			const uint16_t color0 = static_cast<uint16_t>(colorBlock[0] | (colorBlock[1] << 8));
			const uint16_t color1 = static_cast<uint16_t>(colorBlock[2] | (colorBlock[3] << 8));
			const uint32_t colorBits = static_cast<uint32_t>(colorBlock[4]) |
				(static_cast<uint32_t>(colorBlock[5]) << 8) |
				(static_cast<uint32_t>(colorBlock[6]) << 16) |
				(static_cast<uint32_t>(colorBlock[7]) << 24);

			uint8_t table[4][3];
			table[0][0] = Expand5(color0 >> 11); table[0][1] = Expand6(color0 >> 5); table[0][2] = Expand5(color0);
			table[1][0] = Expand5(color1 >> 11); table[1][1] = Expand6(color1 >> 5); table[1][2] = Expand5(color1);
			for (int c = 0; c < 3; ++c)
			{
				table[2][c] = static_cast<uint8_t>((2 * table[0][c] + table[1][c]) / 3);
				table[3][c] = static_cast<uint8_t>((table[0][c] + 2 * table[1][c]) / 3);
			}

			for (int i = 0; i < 16; ++i)
			{
				const int px = blockX * 4 + (i % 4);
				const int py = blockY * 4 + (i / 4);
				if (px >= width || py >= height)
					continue;

				uint8_t *dst = output + (static_cast<size_t>(py) * width + px) * 4;
				const int code = static_cast<int>((colorBits >> (2 * i)) & 0x03);
				const int alphaNibble = static_cast<int>((alphaBits >> (4 * i)) & 0x0F);
				dst[0] = table[code][0];
				dst[1] = table[code][1];
				dst[2] = table[code][2];
				dst[3] = static_cast<uint8_t>(alphaNibble * 17); // 0..15 -> 0..255
			}
		}
	}
}
}
