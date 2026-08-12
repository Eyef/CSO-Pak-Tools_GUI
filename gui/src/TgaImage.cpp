#include "TgaImage.h"

#include <algorithm>

namespace tga
{
namespace
{
	struct Header
	{
		uint8_t idLength = 0;
		uint8_t colorMapType = 0;
		uint8_t imageType = 0;
		uint16_t colorMapFirstEntry = 0;
		uint16_t colorMapLength = 0;
		uint8_t colorMapEntrySize = 0;
		uint16_t width = 0;
		uint16_t height = 0;
		uint8_t bitsPerPixel = 0;
		uint8_t descriptor = 0;
	};

	constexpr size_t HeaderSize = 18;

	uint16_t ReadU16Le(const uint8_t *p)
	{
		return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
	}

	bool ReadHeader(const std::vector<uint8_t> &data, Header &header, size_t &offset)
	{
		if (data.size() < HeaderSize)
			return false;

		header.idLength = data[0];
		header.colorMapType = data[1];
		header.imageType = data[2];
		header.colorMapFirstEntry = ReadU16Le(&data[3]);
		header.colorMapLength = ReadU16Le(&data[5]);
		header.colorMapEntrySize = data[7];
		header.width = ReadU16Le(&data[12]);
		header.height = ReadU16Le(&data[14]);
		header.bitsPerPixel = data[16];
		header.descriptor = data[17];

		offset = HeaderSize + header.idLength;
		return true;
	}

	// Interprets `bytesPerPixel` raw bytes of true-color/grayscale pixel data.
	QRgb DecodeTruecolorPixel(const uint8_t *p, int bytesPerPixel)
	{
		switch (bytesPerPixel)
		{
		case 1:
			// Grayscale.
			return qRgb(p[0], p[0], p[0]);
		case 2:
		{
			// 1:5:5:5, little-endian word.
			const uint16_t word = ReadU16Le(p);
			const int r = (word >> 10) & 0x1F;
			const int g = (word >> 5) & 0x1F;
			const int b = word & 0x1F;
			const auto scale5 = [](int v) { return (v << 3) | (v >> 2); };
			return qRgb(scale5(r), scale5(g), scale5(b));
		}
		case 3:
			// B G R.
			return qRgb(p[2], p[1], p[0]);
		case 4:
			// B G R A.
			return qRgba(p[2], p[1], p[0], p[3]);
		default:
			return qRgb(0, 0, 0);
		}
	}

	QRgb DecodeColorMapEntry(const uint8_t *p, int bytesPerEntry)
	{
		// Color map entries use the same channel layouts as truecolor pixels.
		return DecodeTruecolorPixel(p, bytesPerEntry);
	}

	// Reads `pixelCount` fixed-size pixel records (raw or RLE-packed) into a
	// flat buffer of `pixelCount * bytesPerPixel` bytes.
	bool ReadPixelStream(const std::vector<uint8_t> &data, size_t offset,
		bool compressed, uint32_t pixelCount, int bytesPerPixel,
		std::vector<uint8_t> &outFlat)
	{
		outFlat.resize(static_cast<size_t>(pixelCount) * bytesPerPixel);

		if (!compressed)
		{
			const size_t needed = outFlat.size();
			if (offset + needed > data.size())
				return false;

			std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), needed, outFlat.begin());
			return true;
		}

		size_t pos = offset;
		uint32_t written = 0;
		while (written < pixelCount)
		{
			if (pos >= data.size())
				return false;

			const uint8_t packetHeader = data[pos++];
			const uint32_t count = (packetHeader & 0x7F) + 1;
			const bool isRle = (packetHeader & 0x80) != 0;

			if (isRle)
			{
				if (pos + static_cast<size_t>(bytesPerPixel) > data.size())
					return false;

				const uint8_t *pixel = &data[pos];
				pos += bytesPerPixel;

				for (uint32_t i = 0; i < count && written < pixelCount; ++i, ++written)
				{
					std::copy_n(pixel, bytesPerPixel,
						outFlat.begin() + static_cast<std::ptrdiff_t>(written) * bytesPerPixel);
				}
			}
			else
			{
				const size_t bytesNeeded = static_cast<size_t>(count) * bytesPerPixel;
				if (pos + bytesNeeded > data.size())
					return false;

				for (uint32_t i = 0; i < count && written < pixelCount; ++i, ++written)
				{
					std::copy_n(&data[pos + static_cast<size_t>(i) * bytesPerPixel], bytesPerPixel,
						outFlat.begin() + static_cast<std::ptrdiff_t>(written) * bytesPerPixel);
				}

				pos += bytesNeeded;
			}
		}

		return true;
	}
}

QImage Load(const std::vector<uint8_t> &data, QString *errorMessage)
{
	const auto fail = [&](const char *message) -> QImage
	{
		if (errorMessage)
			*errorMessage = QString::fromUtf8(message);
		return QImage();
	};

	Header header;
	size_t offset = 0;
	if (!ReadHeader(data, header, offset) || offset > data.size())
		return fail("not a TGA file (header too short)");

	if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384)
		return fail("invalid TGA dimensions");

	if ((header.descriptor & 0x10) != 0)
		return fail("right-to-left TGA pixel order is not supported");

	const bool topDown = (header.descriptor & 0x20) != 0;
	const bool colorMapped = header.imageType == 1 || header.imageType == 9;
	const bool truecolor = header.imageType == 2 || header.imageType == 10;
	const bool grayscale = header.imageType == 3 || header.imageType == 11;
	const bool compressed = header.imageType == 9 || header.imageType == 10 || header.imageType == 11;

	if (!colorMapped && !truecolor && !grayscale)
		return fail("unsupported TGA image type");

	// Color map (palette), if present.
	std::vector<QRgb> colorMap;
	if (header.colorMapType == 1)
	{
		const int mapBytesPerEntry = header.colorMapEntrySize / 8;
		if (mapBytesPerEntry < 1 || mapBytesPerEntry > 4)
			return fail("unsupported TGA color map entry size");

		const size_t mapBytes = static_cast<size_t>(header.colorMapLength) * mapBytesPerEntry;
		if (offset + mapBytes > data.size())
			return fail("TGA color map is truncated");

		colorMap.resize(header.colorMapLength);
		for (uint16_t i = 0; i < header.colorMapLength; ++i)
			colorMap[i] = DecodeColorMapEntry(&data[offset + static_cast<size_t>(i) * mapBytesPerEntry], mapBytesPerEntry);

		offset += mapBytes;
	}

	const int pixelBytes = header.bitsPerPixel / 8;
	if (pixelBytes < 1 || pixelBytes > 4)
		return fail("unsupported TGA bit depth");

	const uint32_t pixelCount = static_cast<uint32_t>(header.width) * header.height;
	std::vector<uint8_t> flatPixels;
	if (!ReadPixelStream(data, offset, compressed, pixelCount, pixelBytes, flatPixels))
		return fail("TGA pixel data is truncated or corrupt");

	QImage image(header.width, header.height, QImage::Format_ARGB32);

	for (int row = 0; row < header.height; ++row)
	{
		// TGA rows are bottom-up by default; flip unless the top-down flag is set.
		const int destRow = topDown ? row : (header.height - 1 - row);
		auto *destLine = reinterpret_cast<QRgb *>(image.scanLine(destRow));

		for (int col = 0; col < header.width; ++col)
		{
			const uint8_t *pixel = &flatPixels[(static_cast<size_t>(row) * header.width + col) * pixelBytes];

			if (colorMapped)
			{
				const uint32_t index = header.bitsPerPixel == 8 ? pixel[0] : ReadU16Le(pixel);
				destLine[col] = (index < colorMap.size()) ? colorMap[index] : qRgb(0, 0, 0);
			}
			else
			{
				destLine[col] = DecodeTruecolorPixel(pixel, pixelBytes);
			}
		}
	}

	return image;
}
}
