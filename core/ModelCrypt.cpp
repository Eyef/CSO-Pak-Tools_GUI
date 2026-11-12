#include "ModelCrypt.h"

#include <cstring>

#include "IceCipher.h"

namespace cso_pak::ModelCrypt
{
	namespace
	{
		constexpr uint32_t kStudioId = 0x54534449u;  // "IDST"
		constexpr size_t kMinSize = 244;             // studiohdr_t size

		// studiohdr_t
		constexpr size_t kOffVersion = 4;
		constexpr size_t kOffNumTextures = 180;
		constexpr size_t kOffNumBodyParts = 204;

		// mstudiotexture_t (80 bytes)
		constexpr int64_t kTextureStride = 80;
		constexpr size_t kTexWidth = 68;
		constexpr size_t kTexHeight = 72;
		constexpr size_t kTexIndex = 76;

		// mstudiobodyparts_t (76 bytes)
		constexpr int64_t kBodyPartStride = 76;
		constexpr size_t kBpNumModels = 64;
		constexpr size_t kBpModelIndex = 72;

		// mstudiomodel_t (112 bytes)
		constexpr int64_t kModelStride = 112;
		constexpr size_t kMdlNumVerts = 80;
		constexpr size_t kMdlVertIndex = 88;

		constexpr uint8_t kKey20[32] = {
			0x32, 0xA6, 0x21, 0xE0, 0xAB, 0x6B, 0xF4, 0x2C,
			0x93, 0xC6, 0xF1, 0x96, 0xFB, 0x38, 0x75, 0x68,
			0xBA, 0x70, 0x13, 0x86, 0xE0, 0xB3, 0x71, 0xF4,
			0xE3, 0x9B, 0x07, 0x22, 0x0C, 0xFE, 0x88, 0x3A,
		};
		constexpr uint8_t kKey21[32] = {
			0x22, 0x7A, 0x19, 0x6F, 0x7B, 0x86, 0x7D, 0xE0,
			0x8C, 0xC6, 0xF1, 0x96, 0xFB, 0x38, 0x75, 0x68,
			0x88, 0x7A, 0x78, 0x86, 0x78, 0x86, 0x67, 0x70,
			0xD9, 0x91, 0x07, 0x3A, 0x14, 0x74, 0xFE, 0x22,
		};

		int32_t ReadI32(const std::vector<uint8_t> &d, size_t off)
		{
			int32_t v;
			std::memcpy(&v, d.data() + off, sizeof(v));
			return v;
		}

		bool Fits(const std::vector<uint8_t> &d, int64_t off, int64_t len)
		{
			return off >= 0 && len >= 0 &&
				off <= static_cast<int64_t>(d.size()) &&
				len <= static_cast<int64_t>(d.size()) - off;
		}

		// The game (and NARTools) run ICE over a region in chunks of up to 1024
		// bytes and give up at the first chunk whose size isn't a multiple of 8
		// -- that can only be the last one. Everything in that final partial
		// chunk stays exactly as stored. This matters: e.g. a 508x507 texture
		// is 257556+768 = 258324 bytes, so its last 276 bytes (the tail of the
		// palette) are NOT encrypted, and "decrypting" them garbles the palette.
		// (Decrypting every complete 8-byte block, as CSNZ-Nar does, is wrong.)
		constexpr size_t kChunk = 1024;

		void Process(const IceCipher &ice, std::vector<uint8_t> &d, int64_t off,
			int64_t len, bool encrypt)
		{
			// Regions that don't fit are skipped (damaged / truncated model).
			if (off < 0 || len <= 0 || !Fits(d, off, len))
				return;
			uint8_t *p = d.data() + off;
			size_t remaining = static_cast<size_t>(len);
			while (remaining > 0)
			{
				const size_t chunk = remaining < kChunk ? remaining : kChunk;
				if ((chunk & 7) != 0)
					break;
				if (encrypt)
					ice.Encrypt(p, chunk);
				else
					ice.Decrypt(p, chunk);
				p += chunk;
				remaining -= chunk;
			}
		}

		void Transform(const IceCipher &ice, std::vector<uint8_t> &d, bool encrypt)
		{
			// Textures: pixels + palette.
			{
				const int32_t count = ReadI32(d, kOffNumTextures);
				int64_t table = ReadI32(d, kOffNumTextures + 4);
				for (int32_t i = 0; i < count && table >= 0; ++i, table += kTextureStride)
				{
					if (!Fits(d, table, kTextureStride))
						break;
					const int32_t w = ReadI32(d, static_cast<size_t>(table) + kTexWidth);
					const int32_t h = ReadI32(d, static_cast<size_t>(table) + kTexHeight);
					const int32_t index = ReadI32(d, static_cast<size_t>(table) + kTexIndex);
					if (w < 0 || h < 0 || index < 0)
						continue;
					Process(ice, d, index, static_cast<int64_t>(w) * h + 768, encrypt);
				}
			}

			// Body parts -> sub-models -> vertex arrays.
			const int32_t partCount = ReadI32(d, kOffNumBodyParts);
			int64_t partTable = ReadI32(d, kOffNumBodyParts + 4);
			for (int32_t i = 0; i < partCount && partTable >= 0; ++i, partTable += kBodyPartStride)
			{
				if (!Fits(d, partTable, kBodyPartStride))
					break;
				const int32_t modelCount = ReadI32(d, static_cast<size_t>(partTable) + kBpNumModels);
				int64_t modelTable = ReadI32(d, static_cast<size_t>(partTable) + kBpModelIndex);
				if (modelTable < 0)
					continue;
				for (int32_t j = 0; j < modelCount && modelTable >= 0; ++j, modelTable += kModelStride)
				{
					if (!Fits(d, modelTable, kModelStride))
						break;
					const int32_t numVerts = ReadI32(d, static_cast<size_t>(modelTable) + kMdlNumVerts);
					const int32_t vertIndex = ReadI32(d, static_cast<size_t>(modelTable) + kMdlVertIndex);
					if (vertIndex < 0 || numVerts < 0)
						continue;
					Process(ice, d, vertIndex, static_cast<int64_t>(numVerts) * 12, encrypt);
				}
			}
		}

		void WriteVersion(std::vector<uint8_t> &d, int32_t version)
		{
			std::memcpy(d.data() + kOffVersion, &version, sizeof(version));
		}
	}

	int EncryptedVersion(const uint8_t *data, size_t size)
	{
		if (size < kMinSize)
			return 0;
		uint32_t id;
		int32_t version;
		std::memcpy(&id, data, 4);
		std::memcpy(&version, data + kOffVersion, 4);
		if (id != kStudioId)
			return 0;
		return (version == 20 || version == 21) ? version : 0;
	}

	Result Decrypt(std::vector<uint8_t> &data, int *originalVersion)
	{
		if (data.size() < kMinSize || data.size() > 0x7FFFFFFFu ||
			ReadI32(data, 0) != static_cast<int32_t>(kStudioId))
			return Result::NotModel;
		const int32_t version = ReadI32(data, kOffVersion);
		if (version != 20 && version != 21)
			return Result::NotEncrypted;

		const IceCipher ice(version == 21 ? kKey21 : kKey20);
		// Region offsets are read from the header, which Decrypt() never
		// touches except for the version field -- order doesn't matter.
		Transform(ice, data, false);
		WriteVersion(data, kPlainVersion);
		if (originalVersion != nullptr)
			*originalVersion = version;
		return Result::Success;
	}

	Result Encrypt(std::vector<uint8_t> &data, int version)
	{
		if (data.size() < kMinSize || data.size() > 0x7FFFFFFFu ||
			ReadI32(data, 0) != static_cast<int32_t>(kStudioId))
			return Result::NotModel;
		if (version != 20 && version != 21)
			return Result::Invalid;
		const int32_t current = ReadI32(data, kOffVersion);
		if (current == 20 || current == 21)
			return Result::AlreadyEncrypted;

		const IceCipher ice(version == 21 ? kKey21 : kKey20);
		Transform(ice, data, true);
		WriteVersion(data, version);
		return Result::Success;
	}
}
