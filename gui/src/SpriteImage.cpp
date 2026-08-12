#include "SpriteImage.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "ByteCursor.h"
#include "DxtDecompress.h"

// Parsing/decode logic ported from the GitHub "sprite" viewer sources
// (sprite_src/SpriteFile*.cpp, SpriteLoader.cpp, dxt.hpp), rewritten from
// Windows COM IStream style onto a bounds-checked byte cursor so it plugs
// straight into the pak-entry byte buffers like the other decoders here.
// DXT decompression and the byte cursor live in DxtDecompress.h/ByteCursor.h,
// shared with the standalone .dds decoder (DdsImage.h/.cpp) since a .spr v3
// frame's embedded blob is a real DDS_HEADER with the same DXT1/DXT5 payload.
//
// On-disk layouts (all little-endian):
//
//   SPR v2 (palette indexed, classic GoldSource):
//     header  id,i32 version type texFormat, f32 radius, i32 width height
//             frames, f32 beam, i32 sync
//     palette i16 count (1..256), then count * {r,g,b} COLOR24
//     frames  per frame: i32 type (0=single, 1=group)
//             single: i32 originX originY w h, then w*h palette indices
//             group : i32 count, count*f32 intervals, then count singles
//
//   SPR v3 (Nexon):
//     header  same 10 fields (no palette)
//     frames  per frame: one embedded DDS blob = "DDS " + 124-byte DDS_HEADER
//             (dwSize=0x7C, pixel-format dwSize=0x20, exactly 1 mipmap) +
//             payload; DXT5 / DXT1 / A8 payloads are handled.

namespace cso_gui
{
namespace
{
	using Cursor = ByteCursor;

	constexpr uint32_t kMagicIdsp = 0x50534449;     // "IDSP"
	constexpr uint32_t kFourCcDds = 0x20534444;     // "DDS "
	constexpr uint32_t kFourCcDxt1 = 0x31545844;    // "DXT1"
	constexpr uint32_t kFourCcDxt5 = 0x35545844;    // "DXT5"
	constexpr uint32_t kPfAlphaPixels = 0x00000001; // DDPF_ALPHAPIXELS

	// Engine SPR_* texFormat values, used to synthesize meaningful alpha.
	constexpr int kTexNormal = 0;
	constexpr int kTexAdditive = 1;
	constexpr int kTexIndexAlpha = 2;
	constexpr int kTexAlphaTest = 3;

	// Frames larger than this are rejected as corrupt (sprites are small).
	constexpr int kMaxDimension = 8192;

	// One flattened single frame while parsing (groups are exploded into it).
	struct RawFrame
	{
		int originX = 0;
		int originY = 0;
		int width = 0;
		int height = 0;
		float interval = 0.1f;
		int group = -1;
		uint32_t format = 0;          // v3 only: FourCC of the payload.
		bool alpha8 = false;          // v3 only: payload is plain 8-bit alpha.
		std::vector<uint8_t> pixels;  // v2 palette indices / v3 compressed bytes.
	};

	QImage MakeImage(int width, int height)
	{
		return MakeRgbaImage(width, height);
	}

	// v2: map palette indices through the COLOR24 palette, synthesizing an
	// alpha channel to match the sprite's shine (texFormat) mode.
	QImage BuildIndexedFrame(const RawFrame &frame, const std::vector<uint8_t> &palette, int texFormat)
	{
		QImage image = MakeImage(frame.width, frame.height);
		const int paletteSize = static_cast<int>(palette.size() / 3);
		uint8_t *out = image.bits();

		for (size_t i = 0; i < frame.pixels.size(); ++i)
		{
			const int index = frame.pixels[i];
			const int clamped = (index < paletteSize) ? index : 0;
			const uint8_t r = palette[static_cast<size_t>(clamped) * 3 + 0];
			const uint8_t g = palette[static_cast<size_t>(clamped) * 3 + 1];
			const uint8_t b = palette[static_cast<size_t>(clamped) * 3 + 2];

			uint8_t a = 255;
			switch (texFormat)
			{
			case kTexNormal:
				break;
			case kTexAdditive:
				a = std::max(r, std::max(g, b)); // Luminance-as-intensity glow.
				break;
			case kTexIndexAlpha:
				a = static_cast<uint8_t>(index); // Palette index is the alpha ramp.
				break;
			case kTexAlphaTest:
				a = (index == paletteSize - 1) ? 0 : 255; // Last entry is the key colour.
				break;
			default:
				break;
			}

			out[i * 4 + 0] = r;
			out[i * 4 + 1] = g;
			out[i * 4 + 2] = b;
			out[i * 4 + 3] = a;
		}
		return image;
	}

	// v3: decompress one embedded-DDS payload to RGBA.
	QImage BuildCompressedFrame(const RawFrame &frame)
	{
		QImage image = MakeImage(frame.width, frame.height);
		if (frame.alpha8)
		{
			// A8 mask: render as white * alpha.
			for (size_t i = 0; i < frame.pixels.size(); ++i)
			{
				const uint8_t a = frame.pixels[i];
				uint8_t *dst = image.bits() + i * 4;
				dst[0] = dst[1] = dst[2] = 255;
				dst[3] = a;
			}
			return image;
		}

		if (frame.format == kFourCcDxt5)
			DecompressDXT5(frame.pixels.data(), frame.width, frame.height, image.bits());
		else if (frame.format == kFourCcDxt1)
			DecompressDXT1(frame.pixels.data(), frame.width, frame.height, image.bits());
		else
			throw std::runtime_error("unsupported sprite frame compression");
		return image;
	}

	int ReadDimension(Cursor &cursor)
	{
		const int v = cursor.I32();
		if (v < 1 || v > kMaxDimension)
			throw std::runtime_error("sprite frame has an invalid size");
		return v;
	}

	RawFrame ReadSingleFrameV2(Cursor &cursor, float interval, int group)
	{
		RawFrame frame;
		frame.originX = cursor.I32();
		frame.originY = cursor.I32();
		frame.width = ReadDimension(cursor);
		frame.height = ReadDimension(cursor);
		frame.interval = interval;
		frame.group = group;

		const size_t count = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
		const uint8_t *p = cursor.Bytes(count);
		frame.pixels.assign(p, p + count);
		return frame;
	}

	std::vector<uint8_t> LoadVersion2(Cursor &cursor, SpriteImage &out, int frameCount)
	{
		// Palette: 16-bit count then count COLOR24 triplets.
		const int paletteCount = cursor.I16();
		if (paletteCount < 1 || paletteCount > 256)
			throw std::runtime_error("sprite palette size out of range");

		std::vector<uint8_t> palette(static_cast<size_t>(paletteCount) * 3);
		for (int i = 0; i < paletteCount; ++i)
		{
			palette[static_cast<size_t>(i) * 3 + 0] = cursor.U8(); // R
			palette[static_cast<size_t>(i) * 3 + 1] = cursor.U8(); // G
			palette[static_cast<size_t>(i) * 3 + 2] = cursor.U8(); // B
		}

		std::vector<RawFrame> rawFrames;
		rawFrames.reserve(static_cast<size_t>(frameCount));

		for (int i = 0; i < frameCount; ++i)
		{
			const int type = cursor.I32();
			if (type == 0) // SPR_SINGLE
			{
				rawFrames.push_back(ReadSingleFrameV2(cursor, 0.1f, -1));
			}
			else if (type == 1) // SPR_GROUP
			{
				const int subCount = cursor.I32();
				if (subCount < 1 || subCount > 4096)
					throw std::runtime_error("sprite group frame count out of range");

				std::vector<float> intervals(static_cast<size_t>(subCount));
				for (int j = 0; j < subCount; ++j)
					intervals[static_cast<size_t>(j)] = cursor.F32();

				for (int j = 0; j < subCount; ++j)
					rawFrames.push_back(ReadSingleFrameV2(cursor,
						intervals[static_cast<size_t>(j)] > 0.0f ? intervals[static_cast<size_t>(j)] : 0.1f, i));
			}
			else
			{
				throw std::runtime_error("unknown sprite frame type");
			}
		}

		// Materialize QImages now that we have both palette and frames.
		for (const RawFrame &raw : rawFrames)
		{
			SpriteImage::Frame f;
			f.image = BuildIndexedFrame(raw, palette, out.texFormat);
			f.interval = raw.interval;
			f.originX = raw.originX;
			f.originY = raw.originY;
			f.group = raw.group;
			out.frames.push_back(std::move(f));
		}

		return palette;
	}

	void LoadVersion3(Cursor &cursor, SpriteImage &out, int frameCount)
	{
		std::vector<RawFrame> rawFrames;
		rawFrames.reserve(static_cast<size_t>(frameCount));

		for (int i = 0; i < frameCount; ++i)
		{
			if (cursor.U32() != kFourCcDds)
				throw std::runtime_error("sprite v3 frame is not an embedded DDS");

			// Fixed-size DDS_HEADER (124 bytes), kept as raw reads to avoid
			// struct padding worries.
			const uint32_t dwSize = cursor.U32();
			if (dwSize != 0x7C)
				throw std::runtime_error("unsupported DDS header size");
			cursor.U32(); // dwFlags
			const uint32_t ddHeight = cursor.U32();
			const uint32_t ddWidth = cursor.U32();
			cursor.U32(); // dwPitchOrLinearSize
			cursor.U32(); // dwDepth
			const uint32_t mipCount = cursor.U32();
			if (mipCount != 1)
				throw std::runtime_error("only single-mipmap DDS sprites are supported");
			cursor.Bytes(11 * 4); // dwReserved1

			const uint32_t pfSize = cursor.U32();
			if (pfSize != 0x20)
				throw std::runtime_error("unsupported DDS pixel-format size");
			const uint32_t pfFlags = cursor.U32();
			const uint32_t fourCc = cursor.U32();
			cursor.U32(); // dwRGBBitCount
			cursor.U32(); // dwRBitMask
			cursor.U32(); // dwGBitMask
			cursor.U32(); // dwBBitMask
			cursor.U32(); // dwABitMask

			cursor.U32(); // dwCaps
			cursor.U32(); // dwCaps2
			cursor.U32(); // dwCaps3
			cursor.U32(); // dwCaps4
			cursor.U32(); // dwReserved2

			RawFrame frame;
			if (ddWidth < 1 || ddWidth > static_cast<uint32_t>(kMaxDimension) ||
				ddHeight < 1 || ddHeight > static_cast<uint32_t>(kMaxDimension))
				throw std::runtime_error("sprite v3 frame has an invalid size");
			frame.width = static_cast<int>(ddWidth);
			frame.height = static_cast<int>(ddHeight);
			frame.format = fourCc;

			size_t dataSize = 0;
			const bool isAlphaOnly = (pfFlags & kPfAlphaPixels) != 0 && fourCc == 0;
			if (fourCc == kFourCcDxt5)
				dataSize = static_cast<size_t>((ddHeight + 3) / 4) * static_cast<size_t>((ddWidth + 3) / 4) * 16;
			else if (fourCc == kFourCcDxt1)
				dataSize = static_cast<size_t>((ddHeight + 3) / 4) * static_cast<size_t>((ddWidth + 3) / 4) * 8;
			else if (isAlphaOnly)
			{
				frame.alpha8 = true;
				dataSize = static_cast<size_t>(ddWidth) * static_cast<size_t>(ddHeight);
			}
			else
			{
				throw std::runtime_error("unsupported DDS compression in sprite v3");
			}

			const uint8_t *p = cursor.Bytes(dataSize);
			frame.pixels.assign(p, p + dataSize);
			rawFrames.push_back(std::move(frame));
		}

		for (const RawFrame &raw : rawFrames)
		{
			SpriteImage::Frame f;
			f.image = BuildCompressedFrame(raw);
			f.interval = raw.interval;
			f.originX = raw.originX;
			f.originY = raw.originY;
			f.group = raw.group;
			out.frames.push_back(std::move(f));
		}
	}
} // namespace

std::shared_ptr<SpriteImage> LoadSpriteImage(const std::vector<uint8_t> &data)
{
	auto sprite = std::make_shared<SpriteImage>();
	Cursor cursor(data);

	// Shared 10-field header.
	if (cursor.U32() != kMagicIdsp)
		throw std::runtime_error("not a sprite file (bad \"IDSP\" magic)");
	sprite->version = cursor.I32();
	sprite->type = cursor.I32();
	sprite->texFormat = cursor.I32();
	sprite->boundingRadius = cursor.F32();
	/* width  */ cursor.I32();
	/* height */ cursor.I32();
	const int frameCount = cursor.I32();
	sprite->beamLength = cursor.F32();
	sprite->syncType = cursor.I32();
	if (frameCount < 1 || frameCount > 100000)
		throw std::runtime_error("sprite frame count out of range");

	if (sprite->version == 2)
		LoadVersion2(cursor, *sprite, frameCount);
	else if (sprite->version == 3)
		LoadVersion3(cursor, *sprite, frameCount);
	else
		throw std::runtime_error("unsupported sprite version (only v2/v3 are known)");

	if (sprite->frames.empty())
		throw std::runtime_error("sprite contains no displayable frames");
	return sprite;
}
} // namespace cso_gui
