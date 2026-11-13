#include "DdsImage.h"

#include <stdexcept>

#include "ByteCursor.h"
#include "Bc7Decompress.h"
#include "DxtDecompress.h"

// Standard little-endian DDS layout: "DDS " magic + a fixed 124-byte
// DDS_HEADER (dwSize=0x7C) whose embedded 32-byte DDS_PIXELFORMAT
// (dwSize=0x20) says how the pixel data that follows is laid out. This is
// the exact same structure SpriteImage.cpp reads for each .spr v3 frame --
// a .spr v3 frame's payload literally *is* one of these, just embedded
// mid-file instead of being the whole file. We only decode mip level 0;
// any further mip levels stored after it are simply never read.
namespace cso_gui
{
namespace
{
	constexpr uint32_t kFourCcDds = 0x20534444;  // "DDS "
	constexpr uint32_t kFourCcDxt1 = 0x31545844; // "DXT1"
	constexpr uint32_t kFourCcDxt3 = 0x33545844; // "DXT3"
	constexpr uint32_t kFourCcDxt5 = 0x35545844; // "DXT5"
	constexpr uint32_t kFourCcDx10 = 0x30315844; // "DX10" -> 20-byte DDS_HEADER_DXT10 follows

	// DXGI_FORMAT values that can appear in a DX10 extended header.
	constexpr uint32_t kDxgiR8G8B8A8 = 28, kDxgiR8G8B8A8Srgb = 29;
	constexpr uint32_t kDxgiBc1 = 71, kDxgiBc1Srgb = 72;
	constexpr uint32_t kDxgiBc2 = 74, kDxgiBc2Srgb = 75;
	constexpr uint32_t kDxgiBc3 = 77, kDxgiBc3Srgb = 78;
	constexpr uint32_t kDxgiB8G8R8A8 = 87, kDxgiB8G8R8A8Srgb = 91;
	constexpr uint32_t kDxgiBc7 = 98, kDxgiBc7Srgb = 99;

	constexpr uint32_t kPfAlphaPixels = 0x00000001; // DDPF_ALPHAPIXELS
	constexpr uint32_t kPfFourCc = 0x00000004;      // DDPF_FOURCC
	constexpr uint32_t kPfRgb = 0x00000040;         // DDPF_RGB
	constexpr uint32_t kPfLuminance = 0x00020000;   // DDPF_LUMINANCE

	// Real textures top out well below this; guards against a corrupt
	// width/height causing a huge allocation.
	constexpr int kMaxDimension = 16384;

	// Extracts one channel from a packed pixel value using its bitmask,
	// scaling it up to a full 0-255 byte regardless of the mask's width
	// (covers 4/5/6-bit channels from 16-bit formats like RGB565/ARGB4444,
	// not just the common 8-bit-per-channel case).
	uint8_t ExtractChannel(uint32_t pixel, uint32_t mask)
	{
		if (mask == 0)
			return 0;

		int shift = 0;
		uint32_t m = mask;
		while ((m & 1) == 0)
		{
			m >>= 1;
			++shift;
		}
		int bits = 0;
		while (m & 1)
		{
			m >>= 1;
			++bits;
		}
		if (bits > 8)
			bits = 8; // Not expected in practice, but keeps the math in range.

		const uint32_t value = (pixel & mask) >> shift;
		const uint32_t maxValue = (1u << bits) - 1;
		return static_cast<uint8_t>(bits >= 8 ? (value & 0xFF) : (value * 255u) / maxValue);
	}

	void DecodeUncompressed(ByteCursor &cursor, int width, int height, uint32_t bitCount,
		uint32_t rMask, uint32_t gMask, uint32_t bMask, uint32_t aMask,
		bool hasAlpha, bool luminance, uint8_t *output)
	{
		const int bytesPerPixel = static_cast<int>(bitCount / 8);
		if (bytesPerPixel < 1 || bytesPerPixel > 4)
			throw std::runtime_error("unsupported DDS bit depth");

		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const uint8_t *src = cursor.Bytes(static_cast<size_t>(bytesPerPixel));
				uint32_t pixel = 0;
				for (int b = 0; b < bytesPerPixel; ++b)
					pixel |= static_cast<uint32_t>(src[b]) << (8 * b);

				uint8_t *dst = output + (static_cast<size_t>(y) * width + x) * 4;
				if (luminance)
				{
					const uint8_t l = ExtractChannel(pixel, rMask);
					dst[0] = dst[1] = dst[2] = l;
				}
				else
				{
					dst[0] = ExtractChannel(pixel, rMask);
					dst[1] = ExtractChannel(pixel, gMask);
					dst[2] = ExtractChannel(pixel, bMask);
				}
				dst[3] = hasAlpha ? ExtractChannel(pixel, aMask) : 255;
			}
		}
	}
}

QImage LoadDdsImage(const std::vector<uint8_t> &data)
{
	ByteCursor cursor(data);

	if (cursor.U32() != kFourCcDds)
		throw std::runtime_error("not a DDS file (bad \"DDS \" magic)");

	const uint32_t dwSize = cursor.U32();
	if (dwSize != 0x7C)
		throw std::runtime_error("unsupported DDS header size");

	cursor.U32(); // dwFlags
	const uint32_t ddHeight = cursor.U32();
	const uint32_t ddWidth = cursor.U32();
	cursor.U32(); // dwPitchOrLinearSize (recomputed from width instead of trusted)
	cursor.U32(); // dwDepth
	cursor.U32(); // dwMipMapCount (only mip 0 is decoded)
	cursor.Bytes(11 * 4); // dwReserved1

	const uint32_t pfSize = cursor.U32();
	if (pfSize != 0x20)
		throw std::runtime_error("unsupported DDS pixel-format size");
	const uint32_t pfFlags = cursor.U32();
	const uint32_t fourCc = cursor.U32();
	const uint32_t bitCount = cursor.U32();
	const uint32_t rMask = cursor.U32();
	const uint32_t gMask = cursor.U32();
	const uint32_t bMask = cursor.U32();
	const uint32_t aMask = cursor.U32();

	cursor.U32(); // dwCaps
	cursor.U32(); // dwCaps2
	cursor.U32(); // dwCaps3
	cursor.U32(); // dwCaps4
	cursor.U32(); // dwReserved2

	if (ddWidth < 1 || ddWidth > static_cast<uint32_t>(kMaxDimension) ||
		ddHeight < 1 || ddHeight > static_cast<uint32_t>(kMaxDimension))
		throw std::runtime_error("DDS image has an invalid size");

	const int width = static_cast<int>(ddWidth);
	const int height = static_cast<int>(ddHeight);

	QImage image = MakeRgbaImage(width, height);

	if ((pfFlags & kPfFourCc) != 0)
	{
		if (fourCc == kFourCcDxt1)
		{
			const size_t needed = static_cast<size_t>((height + 3) / 4) * static_cast<size_t>((width + 3) / 4) * 8;
			DecompressDXT1(cursor.Bytes(needed), width, height, image.bits());
		}
		else if (fourCc == kFourCcDxt3)
		{
			const size_t needed = static_cast<size_t>((height + 3) / 4) * static_cast<size_t>((width + 3) / 4) * 16;
			DecompressDXT3(cursor.Bytes(needed), width, height, image.bits());
		}
		else if (fourCc == kFourCcDxt5)
		{
			const size_t needed = static_cast<size_t>((height + 3) / 4) * static_cast<size_t>((width + 3) / 4) * 16;
			DecompressDXT5(cursor.Bytes(needed), width, height, image.bits());
		}
		else if (fourCc == kFourCcDx10)
		{
			// Extended header: dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2.
			const uint32_t dxgiFormat = cursor.U32();
			cursor.Bytes(4 * 4);

			const size_t blocks = static_cast<size_t>((height + 3) / 4) * static_cast<size_t>((width + 3) / 4);
			switch (dxgiFormat)
			{
			case kDxgiBc1: case kDxgiBc1Srgb:
				DecompressDXT1(cursor.Bytes(blocks * 8), width, height, image.bits());
				break;
			case kDxgiBc2: case kDxgiBc2Srgb:
				DecompressDXT3(cursor.Bytes(blocks * 16), width, height, image.bits());
				break;
			case kDxgiBc3: case kDxgiBc3Srgb:
				DecompressDXT5(cursor.Bytes(blocks * 16), width, height, image.bits());
				break;
			case kDxgiBc7: case kDxgiBc7Srgb:
				DecompressBC7(cursor.Bytes(blocks * 16), width, height, image.bits());
				break;
			case kDxgiR8G8B8A8: case kDxgiR8G8B8A8Srgb:
			case kDxgiB8G8R8A8: case kDxgiB8G8R8A8Srgb:
			{
				const bool bgra = dxgiFormat == kDxgiB8G8R8A8 || dxgiFormat == kDxgiB8G8R8A8Srgb;
				const uint8_t *src = cursor.Bytes(static_cast<size_t>(width) * height * 4);
				uint8_t *dst = image.bits();
				for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
				{
					dst[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
					dst[i * 4 + 1] = src[i * 4 + 1];
					dst[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
					dst[i * 4 + 3] = src[i * 4 + 3];
				}
				break;
			}
			default:
				throw std::runtime_error("unsupported DXGI format in DX10 DDS (supported: BC1/2/3/7, RGBA8, BGRA8)");
			}
		}
		else
		{
			throw std::runtime_error(
				"unsupported DDS compression (supported: DXT1/DXT3/DXT5 and DX10 BC1/2/3/7; "
				"BC4-6/ATI2 are not)");
		}
	}
	else if ((pfFlags & (kPfRgb | kPfLuminance)) != 0)
	{
		const bool hasAlpha = (pfFlags & kPfAlphaPixels) != 0;
		const bool luminance = (pfFlags & kPfLuminance) != 0;
		DecodeUncompressed(cursor, width, height, bitCount, rMask, gMask, bMask, aMask,
			hasAlpha, luminance, image.bits());
	}
	else
	{
		throw std::runtime_error("unsupported DDS pixel format");
	}

	return image;
}
}
