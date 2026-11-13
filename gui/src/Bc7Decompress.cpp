#include "Bc7Decompress.h"

#include <cstddef>

// BC7 / BPTC decoder (all 8 modes). Layout follows the public
// D3D11 / OpenGL ARB_texture_compression_bptc specification.
namespace cso_gui
{
namespace
{
// Generated tables: partition masks (2 bits per pixel, pixel 0 in the low bits)
	constexpr uint32_t kPart2[64] = {
		0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u,
		0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
		0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u,
		0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
		0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u,
		0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
		0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u,
		0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u,
		0x44444444u, 0x55005500u, 0x11441144u, 0x05055050u,
		0x05500550u, 0x11114444u, 0x41144114u, 0x44111144u,
		0x15055054u, 0x01055040u, 0x05041050u, 0x05455150u,
		0x14414114u, 0x50050550u, 0x41411414u, 0x00141400u,
		0x00041504u, 0x00105410u, 0x10541000u, 0x04150400u,
		0x50410514u, 0x41051450u, 0x05415014u, 0x14054150u,
		0x41050514u, 0x41505014u, 0x40011554u, 0x54150140u,
		0x50505500u, 0x00555050u, 0x15151010u, 0x54540404u,
	};
	constexpr uint32_t kPart3[64] = {
		0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u,
		0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
		0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u,
		0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
		0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u,
		0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
		0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u,
		0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
		0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u,
		0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
		0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u,
		0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
		0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u,
		0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
		0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u,
		0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u,
	};
	constexpr uint8_t kAnchor2[64] = {
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
		15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
		6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
	};
	constexpr uint8_t kAnchor3a[64] = {
		3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
		3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
		8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
		3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
	};
	constexpr uint8_t kAnchor3b[64] = {
		15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
		15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
		15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
		15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
	};

	constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
	constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
	constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

	struct ModeInfo
	{
		int subsets;
		int partitionBits;
		int rotationBits;
		int indexSelBits;
		int colorBits;
		int alphaBits;
		int endpointPBit; // 1 = one unique p-bit per endpoint
		int sharedPBit;   // 1 = one p-bit per subset
		int indexBits;
		int indexBits2;
	};

	constexpr ModeInfo kModes[8] = {
		{3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
		{2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
		{3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
		{2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
		{1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
		{1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
		{1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
		{2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
	};

	class BitReader
	{
	public:
		explicit BitReader(const uint8_t *block) : m_block(block) {}

		uint32_t Read(int count)
		{
			uint32_t value = 0;
			for (int i = 0; i < count; ++i)
			{
				const uint32_t bit = (m_block[m_pos >> 3] >> (m_pos & 7)) & 1u;
				value |= bit << i;
				++m_pos;
			}
			return value;
		}

	private:
		const uint8_t *m_block;
		int m_pos = 0;
	};

	inline int Interpolate(int a, int b, int weight)
	{
		return (a * (64 - weight) + b * weight + 32) >> 6;
	}

	inline int Weight(int bits, int index)
	{
		return bits == 2 ? kWeights2[index] : (bits == 3 ? kWeights3[index] : kWeights4[index]);
	}

	inline int Expand(int value, int bits)
	{
		// Replicate the top bits of a `bits`-wide value up to 8 bits.
		value <<= (8 - bits);
		return value | (value >> bits);
	}

	void DecodeBlock(const uint8_t *block, uint8_t rgba[16][4])
	{
		int mode = 0;
		while (mode < 8 && ((block[0] >> mode) & 1) == 0)
			++mode;
		if (mode >= 8)
		{
			// Reserved: spec says all zeros.
			for (int i = 0; i < 16; ++i)
				rgba[i][0] = rgba[i][1] = rgba[i][2] = rgba[i][3] = 0;
			return;
		}

		const ModeInfo &mi = kModes[mode];
		BitReader br(block);
		br.Read(mode + 1);

		const int partition = static_cast<int>(br.Read(mi.partitionBits));
		const int rotation = static_cast<int>(br.Read(mi.rotationBits));
		const int indexSel = static_cast<int>(br.Read(mi.indexSelBits));

		const int numEndpoints = mi.subsets * 2;
		int ep[6][4] = {};
		for (int ch = 0; ch < 3; ++ch)
			for (int i = 0; i < numEndpoints; ++i)
				ep[i][ch] = static_cast<int>(br.Read(mi.colorBits));
		if (mi.alphaBits)
			for (int i = 0; i < numEndpoints; ++i)
				ep[i][3] = static_cast<int>(br.Read(mi.alphaBits));

		int pbit[6] = {};
		if (mi.endpointPBit)
		{
			for (int i = 0; i < numEndpoints; ++i)
				pbit[i] = static_cast<int>(br.Read(1));
		}
		else if (mi.sharedPBit)
		{
			for (int s = 0; s < mi.subsets; ++s)
				pbit[2 * s] = pbit[2 * s + 1] = static_cast<int>(br.Read(1));
		}
		const bool hasPBits = mi.endpointPBit || mi.sharedPBit;

		// Expand endpoints to 8 bits.
		for (int i = 0; i < numEndpoints; ++i)
		{
			for (int ch = 0; ch < 4; ++ch)
			{
				const bool isAlpha = ch == 3;
				const int bits = isAlpha ? mi.alphaBits : mi.colorBits;
				if (bits == 0)
				{
					ep[i][ch] = 255;
					continue;
				}
				int v = ep[i][ch];
				int totalBits = bits;
				// p-bits apply to color channels, and to alpha only when the mode stores alpha.
				if (hasPBits)
				{
					v = (v << 1) | pbit[i];
					++totalBits;
				}
				ep[i][ch] = Expand(v, totalBits);
			}
		}

		// Subset layout.
		uint32_t partMask = 0; // all pixels subset 0 for single-subset modes
		int anchor[3] = {0, 0, 0};
		if (mi.subsets == 2)
		{
			partMask = kPart2[partition];
			anchor[1] = kAnchor2[partition];
		}
		else if (mi.subsets == 3)
		{
			partMask = kPart3[partition];
			anchor[1] = kAnchor3a[partition];
			anchor[2] = kAnchor3b[partition];
		}

		// Read index sets.
		int colorIdx[16];
		int alphaIdx[16];
		for (int set = 0; set < (mi.indexBits2 ? 2 : 1); ++set)
		{
			const int bits = set == 0 ? mi.indexBits : mi.indexBits2;
			int *dst = set == 0 ? colorIdx : alphaIdx;
			for (int p = 0; p < 16; ++p)
			{
				const int subset = static_cast<int>((partMask >> (2 * p)) & 3u);
				const bool isAnchor = p == anchor[subset];
				dst[p] = static_cast<int>(br.Read(isAnchor ? bits - 1 : bits));
			}
		}
		if (!mi.indexBits2)
			for (int p = 0; p < 16; ++p)
				alphaIdx[p] = colorIdx[p];

		for (int p = 0; p < 16; ++p)
		{
			const int subset = static_cast<int>((partMask >> (2 * p)) & 3u);
			const int *e0 = ep[2 * subset];
			const int *e1 = ep[2 * subset + 1];

			int cBits = mi.indexBits;
			int aBits = mi.indexBits2 ? mi.indexBits2 : mi.indexBits;
			int cIdx = colorIdx[p];
			int aIdx = alphaIdx[p];
			if (mi.indexBits2 && indexSel)
			{
				// Mode 4: selector swaps which index set drives color vs alpha.
				cBits = mi.indexBits2;
				aBits = mi.indexBits;
				cIdx = alphaIdx[p];
				aIdx = colorIdx[p];
			}

			const int cw = Weight(cBits, cIdx);
			const int aw = Weight(aBits, aIdx);
			int r = Interpolate(e0[0], e1[0], cw);
			int g = Interpolate(e0[1], e1[1], cw);
			int b = Interpolate(e0[2], e1[2], cw);
			int a = mi.alphaBits ? Interpolate(e0[3], e1[3], aw) : 255;

			int t;
			switch (rotation)
			{
			case 1: t = a; a = r; r = t; break;
			case 2: t = a; a = g; g = t; break;
			case 3: t = a; a = b; b = t; break;
			default: break;
			}

			rgba[p][0] = static_cast<uint8_t>(r);
			rgba[p][1] = static_cast<uint8_t>(g);
			rgba[p][2] = static_cast<uint8_t>(b);
			rgba[p][3] = static_cast<uint8_t>(a);
		}
	}
}

void DecompressBC7(const uint8_t *input, int width, int height, uint8_t *output)
{
	const int blocksX = (width + 3) / 4;
	const int blocksY = (height + 3) / 4;

	for (int by = 0; by < blocksY; ++by)
	{
		for (int bx = 0; bx < blocksX; ++bx)
		{
			uint8_t px[16][4];
			DecodeBlock(input + static_cast<size_t>(by * blocksX + bx) * 16, px);

			for (int y = 0; y < 4; ++y)
			{
				const int yy = by * 4 + y;
				if (yy >= height)
					break;
				for (int x = 0; x < 4; ++x)
				{
					const int xx = bx * 4 + x;
					if (xx >= width)
						break;
					uint8_t *dst = output + (static_cast<size_t>(yy) * width + xx) * 4;
					const uint8_t *src = px[y * 4 + x];
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					dst[3] = src[3];
				}
			}
		}
	}
}
}
