#include "StudioModel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "Bc7Decompress.h"
#include "ModelCrypt.h"

namespace cso_gui
{
	namespace
	{
		constexpr int kStudioHeaderId = 0x54534449;  // "IDST" little-endian
		constexpr int kStudioVersionDecrypted = 9;  // ICE model after decryption
		constexpr int kStudioVersion10 = 10;
		constexpr int kStudioVersion11 = 11;
		// 2026 CSO update: same header/bone/hitbox/sequence layout, new texture
		// and geometry blocks (see ParseTextures2026 / ParseBodyParts2026).
		constexpr int kStudioVersion2026 = 2026;

		constexpr int kMaxBoneName = 32;
		constexpr int kMaxTextureName = 64;
		constexpr int kMaxSubModelName = 64;
		constexpr int kMaxBodyPartName = 64;
		constexpr int kMaxSequenceLabel = 32;

		constexpr size_t kPaletteBytes = 256 * 3;

		// Offsets within studiohdr_t (version 10/11). All int32.
		constexpr int kOffId = 0;
		constexpr int kOffVersion = 4;
		constexpr int kOffName = 8;
		constexpr int kOffLength = 72;
		constexpr int kOffNumBones = 140;
		constexpr int kOffBoneIndex = 144;
		constexpr int kOffNumTextures = 180;
		constexpr int kOffTextureIndex = 184;
		constexpr int kOffTextureDataIndex = 188;
		constexpr int kOffNumSkinRef = 192;
		constexpr int kOffNumSkinFamilies = 196;
		constexpr int kOffSkinIndex = 200;
		constexpr int kOffNumBodyParts = 204;
		constexpr int kOffBodyPartIndex = 208;
		constexpr int kOffNumSeq = 164;
		constexpr int kOffSeqIndex = 168;

		std::string ReadFixedString(const uint8_t *p, size_t maxLen)
		{
			if (!p)
				return {};
			size_t n = 0;
			while (n < maxLen && p[n] != '\0')
				++n;
			return std::string(reinterpret_cast<const char *>(p), n);
		}

		bool InRange(size_t offset, size_t count, size_t total)
		{
			return offset <= total && count <= total - offset;
		}
	}

	struct StudioModel::Header
	{
		int numBones = 0;
		int boneIndex = 0;
		int numTextures = 0;
		int textureIndex = 0;
		int textureDataIndex = 0;
		int numSkinRef = 0;
		int numSkinFamilies = 0;
		int skinIndex = 0;
		int numBodyParts = 0;
		int bodyPartIndex = 0;
		int numSeq = 0;
		int seqIndex = 0;
	};

	const uint8_t *StudioModel::Ptr(int offset) const
	{
		if (offset < 0 || !InRange(static_cast<size_t>(offset), 1, size_))
			throw std::runtime_error("model: offset out of range");
		return base_ + offset;
	}

	int32_t StudioModel::Int(int offset) const
	{
		if (offset < 0 || !InRange(static_cast<size_t>(offset), sizeof(int32_t), size_))
			throw std::runtime_error("model: int read out of range");
		int32_t value;
		std::memcpy(&value, base_ + offset, sizeof(value));
		return value;
	}

	float StudioModel::Float(int offset) const
	{
		if (offset < 0 || !InRange(static_cast<size_t>(offset), sizeof(float), size_))
			throw std::runtime_error("model: float read out of range");
		float value;
		std::memcpy(&value, base_ + offset, sizeof(value));
		return value;
	}

	void StudioModel::Load(const std::vector<uint8_t> &data)
	{
		valid_ = false;
		version_ = 0;
		encrypted_ = false;
		name_.clear();
		bones_.clear();
		bodyParts_.clear();
		textures_.clear();
		sequences_.clear();
		decodedSequences_.clear();
		inverseRest_.clear();
		skinRefs_.clear();
		skinFamilyCount_ = 0;

		if (data.size() < 244)
			throw std::runtime_error("model: file too small to be a studio model");

		// ICE-encrypted CSO models (header version 20/21) keep their texture
		// pixels and vertex arrays encrypted. Decrypt a private copy so the
		// caller's bytes stay untouched; `decrypted` is declared before the
		// guard below, so it outlives every use of base_.
		std::vector<uint8_t> decrypted;
		const std::vector<uint8_t> *bytes = &data;
		const int encryptedVersion = cso_pak::ModelCrypt::EncryptedVersion(data);
		if (encryptedVersion != 0)
		{
			decrypted = data;
			if (cso_pak::ModelCrypt::Decrypt(decrypted) != cso_pak::ModelCrypt::Result::Success)
				throw std::runtime_error("model: cannot decrypt ICE-encrypted model (version " +
					std::to_string(encryptedVersion) + ")");
			bytes = &decrypted;
			encrypted_ = true;
		}

		base_ = bytes->data();
		size_ = bytes->size();
		// base_/size_ point into the caller's buffer (or `decrypted`): reset
		// them on every exit path, including exceptions from the Parse* steps.
		struct BaseGuard
		{
			StudioModel &model;
			~BaseGuard() { model.base_ = nullptr; model.size_ = 0; }
		} baseGuard{ *this };

		if (Int(kOffId) != kStudioHeaderId)
			throw std::runtime_error("model: not a studio model (bad IDST magic)");

		version_ = Int(kOffVersion);
		// 9 is what the decrypted form of an ICE model carries (see
		// ModelCrypt); same layout as 10 for everything parsed here.
		if (version_ != kStudioVersionDecrypted && version_ != kStudioVersion10 &&
			version_ != kStudioVersion11 && version_ != kStudioVersion2026)
			throw std::runtime_error("model: unsupported studio version " + std::to_string(version_));
		if (encrypted_)
			version_ = encryptedVersion;  // report what is on disk (20/21)

		name_ = ReadFixedString(Ptr(kOffName), 64);

		Header h;
		h.numBones = Int(kOffNumBones);
		h.boneIndex = Int(kOffBoneIndex);
		h.numTextures = Int(kOffNumTextures);
		h.textureIndex = Int(kOffTextureIndex);
		h.textureDataIndex = Int(kOffTextureDataIndex);
		h.numSkinRef = Int(kOffNumSkinRef);
		h.numSkinFamilies = Int(kOffNumSkinFamilies);
		h.skinIndex = Int(kOffSkinIndex);
		h.numBodyParts = Int(kOffNumBodyParts);
		h.bodyPartIndex = Int(kOffBodyPartIndex);
		h.numSeq = Int(kOffNumSeq);
		h.seqIndex = Int(kOffSeqIndex);

		ParseBones(h);
		if (version_ == kStudioVersion2026)
		{
			ParseTextures2026(h);
			ParseSkins(h);
			ParseBodyParts2026(h);
		}
		else
		{
			ParseTextures(h);
			ParseSkins(h);
			ParseBodyParts(h);
		}
		ParseSequences(h);

		// base_ pointed into the caller's byte vector (often a temporary
		// like found.extract()), so it dangles as soon as Load returns.
		// Nothing reads it after parsing -- null it so any future misuse
		// crashes loudly instead of reading freed memory.
		base_ = nullptr;

		valid_ = true;
	}

	void StudioModel::ParseBones(const Header &h)
	{
		bones_.clear();
		if (h.numBones <= 0 || h.boneIndex <= 0)
			return;

		const size_t stride = kMaxBoneName + 4 + 4 + 6 * 4 + 6 * 4 + 6 * 4;
		if (!InRange(static_cast<size_t>(h.boneIndex),
			stride * static_cast<size_t>(h.numBones), size_))
			throw std::runtime_error("model: bone table out of range");

		bones_.reserve(static_cast<size_t>(h.numBones));
		for (int i = 0; i < h.numBones; ++i)
		{
			const int off = h.boneIndex + i * static_cast<int>(stride);
			Bone bone;
			bone.name = ReadFixedString(Ptr(off), kMaxBoneName);
			bone.parent = Int(off + kMaxBoneName);
			const int valueOff = off + kMaxBoneName + 4 + 4 + 6 * 4;
			bone.position.x = Float(valueOff);
			bone.position.y = Float(valueOff + 4);
			bone.position.z = Float(valueOff + 8);
			bone.rotation.x = Float(valueOff + 12);
			bone.rotation.y = Float(valueOff + 16);
			bone.rotation.z = Float(valueOff + 20);
			for (int s = 0; s < 6; ++s)
				bone.scale[static_cast<size_t>(s)] = Float(valueOff + 24 + s * 4);
			bones_.push_back(std::move(bone));
		}
	}

	void StudioModel::ParseTextures(const Header &h)
	{
		textures_.clear();
		if (h.numTextures <= 0 || h.textureIndex <= 0)
			return;

		const size_t stride = kMaxTextureName + 4 + 4 + 4 + 4;  // name, flags, w, h, index
		if (!InRange(static_cast<size_t>(h.textureIndex),
			stride * static_cast<size_t>(h.numTextures), size_))
			throw std::runtime_error("model: texture table out of range");

		for (int i = 0; i < h.numTextures; ++i)
		{
			const int off = h.textureIndex + i * static_cast<int>(stride);
			Texture tex;
			tex.name = ReadFixedString(Ptr(off), kMaxTextureName);
			tex.flags = Int(off + kMaxTextureName);
			tex.width = Int(off + kMaxTextureName + 4);
			tex.height = Int(off + kMaxTextureName + 8);
			const int index = Int(off + kMaxTextureName + 12);

			if (tex.width <= 0 || tex.height <= 0 || tex.width > 8192 || tex.height > 8192)
				throw std::runtime_error("model: invalid texture dimensions");
			if (index < 0 || !InRange(static_cast<size_t>(index),
				static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) + kPaletteBytes, size_))
				throw std::runtime_error("model: texture data out of range");

			const uint8_t *pixels = Ptr(index);
			const uint8_t *palette = pixels + static_cast<size_t>(tex.width) * tex.height;

			QImage image(tex.width, tex.height, QImage::Format_ARGB32);
			const size_t pixelCount = static_cast<size_t>(tex.width) * tex.height;
			for (size_t p = 0; p < pixelCount; ++p)
			{
				const int entry = pixels[p];
				// MDL stores a "pixel index + 40" style palette index for most
				// entries; the palette lookup here matches the studio palette
				// semantics used by Half-Life tools. Bits 0-4 are the index.
				const uint8_t r = palette[entry * 3 + 0];
				const uint8_t g = palette[entry * 3 + 1];
				const uint8_t b = palette[entry * 3 + 2];
				uint32_t argb = 0xFF000000u | (uint32_t(r) << 16) |
					(uint32_t(g) << 8) | uint32_t(b);
				// Palette index 255 is transparent for masked textures.
				if (tex.flags & 0x40 && entry == 255)
					argb = 0x00000000u;
				reinterpret_cast<uint32_t *>(image.bits())[p] = argb;
			}

			tex.image = std::move(image);
			textures_.push_back(std::move(tex));
		}
	}

	void StudioModel::SetTextureImage(int index, const QImage &image)
	{
		if (index < 0 || index >= static_cast<int>(textures_.size()) || image.isNull())
			return;

		Texture &tex = textures_[static_cast<size_t>(index)];
		// Same rule as for embedded textures: in a masked texture (flag 0x40)
		// the engine treats palette index 255 as transparent, whatever colour
		// that palette slot holds (blue, green, pink...). External 8-bit BMPs
		// keep their indices when Qt loads them as Indexed8, so apply it here
		// before the image is converted to RGB and the indices are lost.
		if ((tex.flags & 0x40) && image.format() == QImage::Format_Indexed8)
		{
			const QList<QRgb> colors = image.colorTable();
			QImage out(image.width(), image.height(), QImage::Format_ARGB32);
			for (int y = 0; y < image.height(); ++y)
			{
				const uchar *src = image.constScanLine(y);
				QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
				for (int x = 0; x < image.width(); ++x)
				{
					const int idx = src[x];
					dst[x] = (idx == 255)
						? 0x00000000u
						: (idx < colors.size() ? (colors[idx] | 0xFF000000u) : 0xFF000000u);
				}
			}
			tex.image = std::move(out);
		}
		else
		{
			tex.image = image.convertToFormat(QImage::Format_ARGB32);
		}
		tex.width = tex.image.width();
		tex.height = tex.image.height();
	}

	void StudioModel::ImportTexturesFrom(const StudioModel &other)
	{
		if (other.textures_.empty())
			return;

		textures_.clear();
		for (const Texture &tex : other.textures_)
		{
			Texture copy;
			copy.name = tex.name;
			copy.flags = tex.flags;
			copy.width = tex.width;
			copy.height = tex.height;
			copy.image = tex.image.isNull()
				? QImage()
				: tex.image.convertToFormat(QImage::Format_ARGB32);
			textures_.push_back(std::move(copy));
		}
	}

	void StudioModel::ParseSkins(const Header &h)
	{
		skinRefs_.clear();
		skinFamilyCount_ = h.numSkinFamilies;
		if (h.numSkinRef <= 0 || h.numSkinFamilies <= 0 || h.skinIndex <= 0)
			return;

		// The skin table is numSkinFamilies rows of numSkinRef shorts:
		// short index[skinfamilies][skinref] (see HLAM's StudioModelFileFormat
		// and ConvertSkinFamiliesToEditable). Every row must be read -- with
		// only the first row, any family > 0 indexes into the wrong row or
		// past the end, so Skin 1 renders the wrong/missing textures.
		const size_t count = static_cast<size_t>(h.numSkinRef) * h.numSkinFamilies;
		if (!InRange(static_cast<size_t>(h.skinIndex), count * sizeof(int16_t), size_))
			throw std::runtime_error("model: skin table out of range");

		skinRefs_.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			int16_t ref;
			std::memcpy(&ref, base_ + h.skinIndex + i * sizeof(int16_t), sizeof(ref));
			skinRefs_.push_back(ref);
		}
	}

	void StudioModel::ParseBodyParts(const Header &h)
	{
		bodyParts_.clear();
		if (h.numBodyParts <= 0 || h.bodyPartIndex <= 0)
			return;

		const size_t stride = kMaxBodyPartName + 4 + 4 + 4;  // name, nummodels, base, modelindex
		if (!InRange(static_cast<size_t>(h.bodyPartIndex),
			stride * static_cast<size_t>(h.numBodyParts), size_))
			throw std::runtime_error("model: bodypart table out of range");

		// Precompute bone rest matrices in world space from the bone's own
		// euler position/rotation, matching LocalBoneMatrix in ApplyFrame so
		// the default pose reproduces exactly.
		// A model without bones still needs one identity matrix: vertices
		// fall back to bone 0 below and must not index an empty vector.
		std::vector<std::array<float, 16>> boneMatrices(
			std::max<size_t>(bones_.size(), 1),
			std::array<float, 16>{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
		for (size_t i = 0; i < bones_.size(); ++i)
		{
			const Bone &bone = bones_[i];
			const Vec3 &r = bone.rotation;
			const float cx = std::cos(r.x), sx = std::sin(r.x);
			const float cy = std::cos(r.y), sy = std::sin(r.y);
			const float cz = std::cos(r.z), sz = std::sin(r.z);
			std::array<float, 16> l = {
				cy * cz, cy * sz, -sy, 0.0f,
				sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy, 0.0f,
				cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy, 0.0f,
				bone.position.x, bone.position.y, bone.position.z, 1.0f,
			};
			if (bone.parent >= 0 && bone.parent < static_cast<int>(bones_.size()))
			{
				const auto &p = boneMatrices[static_cast<size_t>(bone.parent)];
				// world[child] = local[child] * world[parent], matching ApplyAnimation.
				std::array<float, 16> out{};
				for (int r = 0; r < 4; ++r)
					for (int c = 0; c < 4; ++c)
						out[r * 4 + c] = l[r * 4 + 0] * p[0 * 4 + c] +
							l[r * 4 + 1] * p[1 * 4 + c] +
							l[r * 4 + 2] * p[2 * 4 + c] +
							l[r * 4 + 3] * p[3 * 4 + c];
				boneMatrices[i] = out;
			}
			else
			{
				boneMatrices[i] = l;
			}
		}

		auto transform = [&](const Vec3 &v, const std::array<float, 16> &m) -> Vec3
		{
			return {
				m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12],
				m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
				m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14],
			};
		};

		// For normals: rotation only, no translation (same reasoning as
		// TransformDirection below, used by ApplyFrame for animated poses).
		auto transformDirection = [&](const Vec3 &v, const std::array<float, 16> &m) -> Vec3
		{
			return {
				m[0] * v.x + m[4] * v.y + m[8] * v.z,
				m[1] * v.x + m[5] * v.y + m[9] * v.z,
				m[2] * v.x + m[6] * v.y + m[10] * v.z,
			};
		};

		for (int bp = 0; bp < h.numBodyParts; ++bp)
		{
			const int off = h.bodyPartIndex + bp * static_cast<int>(stride);
			BodyPart part;
			part.name = ReadFixedString(Ptr(off), kMaxBodyPartName);
			const int numModels = Int(off + kMaxBodyPartName);
			part.base = std::max(Int(off + kMaxBodyPartName + 4), 1);
			const int modelIndex = Int(off + kMaxBodyPartName + 8);

			if (numModels <= 0 || modelIndex <= 0)
			{
				bodyParts_.push_back(std::move(part));
				continue;
			}

			const size_t modelStride = kMaxSubModelName + 12 * 4;
			if (!InRange(static_cast<size_t>(modelIndex),
				modelStride * static_cast<size_t>(numModels), size_))
				throw std::runtime_error("model: submodel table out of range");

			for (int mi = 0; mi < numModels; ++mi)
			{
				const int moff = modelIndex + mi * static_cast<int>(modelStride);
				SubModel sub;
				sub.name = ReadFixedString(Ptr(moff), kMaxSubModelName);
				const int numMesh = Int(moff + 72);
				const int meshIndex = Int(moff + 76);
				const int numVerts = Int(moff + 80);
				const int vertInfoIndex = Int(moff + 84);
				const int vertIndex = Int(moff + 88);
				const int numNorms = Int(moff + 92);
				const int normInfoIndex = Int(moff + 96);
				const int normIndex = Int(moff + 100);

				// Read raw verts + per-vert bone, raw norms + per-norm bone.
				std::vector<Vec3> rawVerts;
				std::vector<int> vertBones;
				if (numVerts > 0 && vertIndex > 0)
				{
					if (!InRange(static_cast<size_t>(vertIndex),
						static_cast<size_t>(numVerts) * 12, size_))
						throw std::runtime_error("model: vertex array out of range");
					for (int i = 0; i < numVerts; ++i)
					{
						const int v = vertIndex + i * 12;
						rawVerts.push_back({ Float(v), Float(v + 4), Float(v + 8) });
					}
				}
				if (numVerts > 0 && vertInfoIndex > 0)
				{
					if (!InRange(static_cast<size_t>(vertInfoIndex),
						static_cast<size_t>(numVerts), size_))
						throw std::runtime_error("model: vertinfo out of range");
					for (int i = 0; i < numVerts; ++i)
						vertBones.push_back(base_[vertInfoIndex + i]);
				}

				std::vector<Vec3> rawNorms;
				std::vector<int> normBones;
				if (numNorms > 0 && normIndex > 0)
				{
					if (!InRange(static_cast<size_t>(normIndex),
						static_cast<size_t>(numNorms) * 12, size_))
						throw std::runtime_error("model: normal array out of range");
					for (int i = 0; i < numNorms; ++i)
					{
						const int n = normIndex + i * 12;
						rawNorms.push_back({ Float(n), Float(n + 4), Float(n + 8) });
					}
				}
				if (numNorms > 0 && normInfoIndex > 0)
				{
					if (!InRange(static_cast<size_t>(normInfoIndex),
						static_cast<size_t>(numNorms), size_))
						throw std::runtime_error("model: norminfo out of range");
					for (int i = 0; i < numNorms; ++i)
						normBones.push_back(base_[normInfoIndex + i]);
				}

				// Keep raw (model-space) verts/norms + bone assignment so the
				// model can be re-skinned into animated poses later.
				sub.rawVerts = rawVerts;
				sub.vertBones = vertBones;
				sub.rawNorms = rawNorms;
				sub.normBones = normBones;
				std::vector<Vec3> verts(rawVerts.size());
				for (size_t i = 0; i < rawVerts.size(); ++i)
				{
					const int bi = (i < vertBones.size() && vertBones[i] >= 0 &&
						vertBones[i] < static_cast<int>(boneMatrices.size()))
						? vertBones[i] : 0;
					verts[i] = transform(rawVerts[i], boneMatrices[static_cast<size_t>(bi)]);
				}
				std::vector<Vec3> norms(rawNorms.size());
				for (size_t i = 0; i < rawNorms.size(); ++i)
				{
					const int bi = (i < normBones.size() && normBones[i] >= 0 &&
						normBones[i] < static_cast<int>(boneMatrices.size()))
						? normBones[i] : 0;
					norms[i] = transformDirection(rawNorms[i], boneMatrices[static_cast<size_t>(bi)]);
				}
				// No normal table at all: keep the triangles with an up
				// normal instead of dropping the whole submodel.
				if (norms.empty())
					norms.push_back({ 0.0f, 0.0f, 1.0f });

				// Meshes.
				if (numMesh > 0 && meshIndex > 0)
				{
					const size_t meshStride = 4 * 5;  // numtris, triindex, skinref, numnorms, normindex
					if (!InRange(static_cast<size_t>(meshIndex),
						meshStride * static_cast<size_t>(numMesh), size_))
						throw std::runtime_error("model: mesh table out of range");

					for (int mi2 = 0; mi2 < numMesh; ++mi2)
					{
						const int moff2 = meshIndex + mi2 * static_cast<int>(meshStride);
						Mesh mesh;
						mesh.skinref = Int(moff2 + 8);
						const int triIndex = Int(moff2 + 4);
						if (triIndex <= 0)
						{
							sub.meshes.push_back(std::move(mesh));
							continue;
						}
						// Triangle commands: [n][4*n shorts][next n]... n>0 strip, n<0 fan.
						size_t cmdOff = static_cast<size_t>(triIndex);
						while (true)
						{
							int16_t count;
							if (!InRange(cmdOff, sizeof(int16_t), size_))
								break;
							std::memcpy(&count, base_ + cmdOff, sizeof(count));
							cmdOff += sizeof(int16_t);
							if (count == 0)
								break;
							const int n = count > 0 ? count : -count;
							if (n < 3)
							{
								cmdOff += static_cast<size_t>(n) * 8;
								continue;
							}
							if (!InRange(cmdOff, static_cast<size_t>(n) * 8, size_))
								break;

							std::vector<VertRef> vertsInStrip;
							vertsInStrip.reserve(static_cast<size_t>(n));
							for (int i = 0; i < n; ++i)
							{
								int16_t v, norm, s, t;
								std::memcpy(&v, base_ + cmdOff, sizeof(v));
								std::memcpy(&norm, base_ + cmdOff + 2, sizeof(norm));
								std::memcpy(&s, base_ + cmdOff + 4, sizeof(s));
								std::memcpy(&t, base_ + cmdOff + 6, sizeof(t));
								cmdOff += 8;
								// Widen before comparing: casting a large
								// vertex count down to int16_t wraps negative
								// and would discard every triangle past 32767
								// vertices.
								if (v < 0 || static_cast<size_t>(v) >= verts.size())
									continue;
								// An out-of-range normal falls back to slot 0,
								// or -1 when the submodel has no normals at
								// all (makeTri skips those triangles: indexing
								// norms[0] of an empty array would read
								// unowned memory).
								if (norm < 0 || static_cast<size_t>(norm) >= norms.size())
									norm = norms.empty() ? -1 : 0;
								vertsInStrip.push_back({ v, norm, static_cast<float>(s), static_cast<float>(t) });
							}

							auto makeTri = [&](const VertRef &a, const VertRef &b, const VertRef &c)
							{
								if (a.vert < 0 || static_cast<size_t>(a.vert) >= verts.size() ||
									b.vert < 0 || static_cast<size_t>(b.vert) >= verts.size() ||
									c.vert < 0 || static_cast<size_t>(c.vert) >= verts.size() ||
									a.norm < 0 || static_cast<size_t>(a.norm) >= norms.size() ||
									b.norm < 0 || static_cast<size_t>(b.norm) >= norms.size() ||
									c.norm < 0 || static_cast<size_t>(c.norm) >= norms.size())
									return;  // Corrupt strip: skip, don't index blindly.
								Triangle tri;
								tri.a = verts[static_cast<size_t>(a.vert)];
								tri.b = verts[static_cast<size_t>(b.vert)];
								tri.c = verts[static_cast<size_t>(c.vert)];
								tri.na = norms[static_cast<size_t>(a.norm)];
								tri.nb = norms[static_cast<size_t>(b.norm)];
								tri.nc = norms[static_cast<size_t>(c.norm)];
								tri.sa = a.s; tri.ta = a.t;
								tri.sb = b.s; tri.tb = b.t;
								tri.sc = c.s; tri.tc = c.t;
								tri.texture = mesh.skinref;
								tri.vertA = a.vert;
								tri.vertB = b.vert;
								tri.vertC = c.vert;
								tri.normA = a.norm;
								tri.normB = b.norm;
								tri.normC = c.norm;
								// Widen before comparing (same int16_t wrap
								// issue as above); missing tables give -1,
								// which ApplyFrame treats as "keep rest pose".
								const auto boneFor = [&](int vert) -> int
								{
									if (vert < 0 ||
										static_cast<size_t>(vert) >= vertBones.size())
										return -1;
									return vertBones[static_cast<size_t>(vert)];
								};
								const auto normBoneFor = [&](int norm) -> int
								{
									if (norm < 0 ||
										static_cast<size_t>(norm) >= normBones.size())
										return -1;
									return normBones[static_cast<size_t>(norm)];
								};
								tri.boneA = boneFor(a.vert);
								tri.boneB = boneFor(b.vert);
								tri.boneC = boneFor(c.vert);
								tri.normBoneA = normBoneFor(a.norm);
								tri.normBoneB = normBoneFor(b.norm);
								tri.normBoneC = normBoneFor(c.norm);
								mesh.triangles.push_back(tri);
							};

							if (count > 0)
							{
								// triangle strip
								for (int i = 0; i + 2 < static_cast<int>(vertsInStrip.size()); ++i)
								{
									if (i % 2 == 0)
										makeTri(vertsInStrip[i], vertsInStrip[i + 1], vertsInStrip[i + 2]);
									else
										makeTri(vertsInStrip[i + 1], vertsInStrip[i], vertsInStrip[i + 2]);
								}
							}
							else
							{
								// triangle fan
								for (int i = 1; i + 1 < static_cast<int>(vertsInStrip.size()); ++i)
									makeTri(vertsInStrip[0], vertsInStrip[i], vertsInStrip[i + 1]);
							}
						}
						(void)stride;
						sub.meshes.push_back(std::move(mesh));
					}
				}

				part.models.push_back(std::move(sub));
			}

			bodyParts_.push_back(std::move(part));
		}
	}

	void StudioModel::ParseSequences(const Header &h)
	{
		sequences_.clear();
		decodedSequences_.clear();
		if (h.numSeq <= 0 || h.seqIndex <= 0)
			return;

		// mstudioseqdesc_t is 176 bytes:
		// label[32] @0, fps @32, flags @36, activity @40, actweight @44,
		// numevents @48, eventindex @52, numframes @56, numpivots @60,
		// pivotindex @64, motiontype @68, motionbone @72, linearmovement @76,
		// automoveposindex @88, automoveangleindex @92, bbmin @96, bbmax @108,
		// numblends @120, animindex @124, ...
		constexpr size_t kSeqDescStride = 176;
		if (!InRange(static_cast<size_t>(h.seqIndex),
			kSeqDescStride * static_cast<size_t>(h.numSeq), size_))
			throw std::runtime_error("model: sequence table out of range");

		for (int i = 0; i < h.numSeq; ++i)
		{
			const int off = h.seqIndex + i * static_cast<int>(kSeqDescStride);
			Sequence seq;
			seq.label = ReadFixedString(Ptr(off), kMaxSequenceLabel);
			seq.fps = Float(off + 32);
			seq.numframes = Int(off + 56);
			seq.numblends = std::max(Int(off + 120), 0);
			seq.animindex = Int(off + 124);
			seq.motiontype = Int(off + 68);
			seq.motionbone = Int(off + 72);
			sequences_.push_back(std::move(seq));
		}

		// Decode each sequence's animation keyframes into usable form.
		decodedSequences_.resize(sequences_.size());
		for (size_t i = 0; i < sequences_.size(); ++i)
			DecodeSequenceAnimation(i, base_, size_, 0);
	}

	void StudioModel::DecodeSequenceAnimation(size_t sequenceIndex, const uint8_t *data,
		size_t size, std::int64_t bias)
	{
		DecodedSequence &decoded = decodedSequences_[sequenceIndex];
		const Sequence &seq = sequences_[sequenceIndex];
		if (seq.numblends <= 0 || seq.numframes <= 0 || bones_.empty() ||
			seq.animindex <= 0 || data == nullptr)
			return;

		// animindex is an address in the original model; `bias` maps it onto
		// `data` (0 when `data` is the .mdl itself).
		const std::int64_t localAnimIndex = static_cast<std::int64_t>(seq.animindex) - bias;
		if (localAnimIndex < 0)
			return;

		// mstudioanim_t: 6 x uint16 offsets (12 bytes), ordered [blend][bone].
		const size_t animStride = 12;
		const size_t totalAnims =
			static_cast<size_t>(seq.numblends) * bones_.size();
		if (!InRange(static_cast<size_t>(localAnimIndex),
			animStride * totalAnims, size))
			return;

		decoded.blends.resize(static_cast<size_t>(seq.numblends));
		for (int blend = 0; blend < seq.numblends; ++blend)
		{
			std::vector<BoneAnim> &blendAnims =
				decoded.blends[static_cast<size_t>(blend)];
			blendAnims.resize(bones_.size());
			for (size_t bone = 0; bone < bones_.size(); ++bone)
			{
				const size_t record = static_cast<size_t>(localAnimIndex) +
					(static_cast<size_t>(blend) * bones_.size() + bone) * animStride;

				BoneAnim &boneAnim = blendAnims[bone];
				for (int axis = 0; axis < 6; ++axis)
				{
					std::uint16_t offset;
					std::memcpy(&offset, data + record + static_cast<size_t>(axis) * 2,
						sizeof(offset));
					if (offset == 0)
						continue;

					// The stream is relative to this mstudioanim_t record.
					const size_t streamStart = record + offset;
					if (streamStart >= size)
						continue;  // No bytes available: static base pose.
					// Walk the RLE span headers until numframes is covered.
					size_t pos = streamStart;
					int covered = 0;
					while (covered < seq.numframes && pos < size)
					{
						if (!InRange(pos, 2, size))
							break;
						const std::uint8_t valid = data[pos];
						const std::uint8_t total = data[pos + 1];
						if (total == 0)
							break;  // malformed
						covered += total;
						pos += 2 + static_cast<size_t>(valid) * 2;
						if (!InRange(pos, 0, size))
						{
							// Span claims bytes past EOF: keep only what is
							// actually there instead of reading past the file.
							pos = size;
							break;
						}
					}

					AnimChannel &channel = boneAnim.channels[static_cast<size_t>(axis)];
					const size_t count = (pos - streamStart) / 2;
					channel.resize(count);
					for (size_t k = 0; k < count; ++k)
					{
						std::int16_t value;
						std::memcpy(&value, data + streamStart + k * 2, sizeof(value));
						channel[k] = value;
					}
				}
			}
		}
	}

	int StudioModel::AppendExternalSequences(const std::vector<uint8_t> &seqData, int groupNumber)
	{
		// Same bone layout as the model the file belongs to, so without
		// bones there is nothing the animations could drive.
		if (!valid_ || bones_.empty())
			return 0;

		constexpr size_t kSeqDescStride = 176;  // mstudioseqdesc_t, see ParseSequences
		constexpr size_t kFileHeaderBytes = 8;  // int32 numseq, int32 seqdescOffset
		constexpr int kMaxSequencesPerFile = 4096;
		// Fallback only (see the detection after decoding): files from this
		// group on normally use radians * 10240 instead of the .mdl's per-bone
		// scale.
		constexpr int kFirstUniformScaleGroup = 2;

		const uint8_t *data = seqData.data();
		const size_t size = seqData.size();
		if (size < kFileHeaderBytes + kSeqDescStride)
			throw std::runtime_error("seq: file too small");

		const auto readInt = [&](size_t offset) -> int32_t
		{
			int32_t value;
			std::memcpy(&value, data + offset, sizeof(value));
			return value;
		};
		const auto readFloat = [&](size_t offset) -> float
		{
			float value;
			std::memcpy(&value, data + offset, sizeof(value));
			return value;
		};

		const int32_t count = readInt(0);
		const int32_t tableOffset = readInt(4);
		if (count <= 0 || count > kMaxSequencesPerFile ||
			tableOffset < static_cast<int32_t>(kFileHeaderBytes) ||
			!InRange(static_cast<size_t>(tableOffset),
				kSeqDescStride * static_cast<size_t>(count), size))
			throw std::runtime_error("seq: bad header or sequence table out of range");

		// Parse everything first so a malformed file leaves the model untouched.
		std::vector<Sequence> parsed;
		parsed.reserve(static_cast<size_t>(count));
		// Lowest address any sequence data (events, pivots, animations) refers
		// to. That data starts right after the 8-byte file header, which
		// anchors the pointer -> file offset translation.
		std::int64_t lowestPointer = std::numeric_limits<std::int64_t>::max();
		const auto noteStart = [&](int count_, int index)
		{
			if (count_ > 0 && index > 0)
				lowestPointer = std::min<std::int64_t>(lowestPointer, index);
		};
		for (int32_t i = 0; i < count; ++i)
		{
			const size_t off = static_cast<size_t>(tableOffset) + static_cast<size_t>(i) * kSeqDescStride;
			Sequence seq;
			seq.label = ReadFixedString(data + off, kMaxSequenceLabel);
			seq.fps = readFloat(off + 32);
			seq.numframes = readInt(off + 56);
			seq.numblends = std::max(readInt(off + 120), 0);
			seq.animindex = readInt(off + 124);
			seq.motiontype = readInt(off + 68);
			seq.motionbone = readInt(off + 72);
			seq.externalGroup = groupNumber;
			// Provisional guess; replaced below by looking at the data itself.
			seq.uniformRotationScale = groupNumber >= kFirstUniformScaleGroup;

			noteStart(readInt(off + 48), readInt(off + 52));  // numevents, eventindex
			noteStart(readInt(off + 60), readInt(off + 64));  // numpivots, pivotindex
			noteStart(seq.numblends, seq.animindex);
			parsed.push_back(std::move(seq));
		}

		const std::int64_t bias =
			lowestPointer == std::numeric_limits<std::int64_t>::max()
				? 0
				: lowestPointer - static_cast<std::int64_t>(kFileHeaderBytes);

		// The animation values of a sequence follow its record table
		// (numblends * numbones * 12 bytes) directly. If they do not, the
		// file was made for a different skeleton -- e.g. "foo1.seq" of a
		// model "foo1.mdl" sitting next to "foo.mdl" -- and applying it
		// would just tear the model apart. Check the first sequence that
		// has any animation data and reject the whole file on a mismatch.
		for (const Sequence &seq : parsed)
		{
			if (seq.numblends <= 0 || seq.numframes <= 0 || seq.animindex <= 0)
				continue;
			const std::int64_t local = static_cast<std::int64_t>(seq.animindex) - bias;
			const size_t recordBytes =
				static_cast<size_t>(seq.numblends) * bones_.size() * 12;
			if (local < static_cast<std::int64_t>(kFileHeaderBytes) ||
				!InRange(static_cast<size_t>(local), recordBytes, size))
				throw std::runtime_error(
					"seq: animation table out of range (different skeleton?)");
			size_t lowestStream = std::numeric_limits<size_t>::max();
			for (size_t r = 0; r < recordBytes / 12; ++r)
			{
				const size_t record = static_cast<size_t>(local) + r * 12;
				for (size_t axis = 0; axis < 6; ++axis)
				{
					std::uint16_t offset;
					std::memcpy(&offset, data + record + axis * 2, sizeof(offset));
					if (offset != 0)
						lowestStream = std::min(lowestStream, record + offset);
				}
			}
			if (lowestStream == std::numeric_limits<size_t>::max())
				continue;  // static pose only, says nothing about the skeleton
			if (lowestStream != static_cast<size_t>(local) + recordBytes)
				throw std::runtime_error(
					"seq: animation layout does not match this model's skeleton");
			break;
		}

		const size_t firstNew = sequences_.size();
		decodedSequences_.resize(sequences_.size());
		for (auto &seq : parsed)
			sequences_.push_back(std::move(seq));
		decodedSequences_.resize(sequences_.size());
		for (size_t i = firstNew; i < sequences_.size(); ++i)
			DecodeSequenceAnimation(i, data, size, bias);

		// Which rotation encoding does this file use? Two exist:
		//  * the .mdl's per-bone-axis scale (mstudiobone_t::scale[3..5]): the
		//    compiler normalises every axis so that its largest value reaches
		//    +-32767, and
		//  * radians * 10240 for every bone and axis (the delta from the rest
		//    angle, wrapped to +-pi, so values never exceed pi * 10240 =
		//    32170).
		// The two ranges do not overlap, so the largest rotation value in the
		// file decides it: all 316 .mdl-scaled files of the checked corpora
		// reach 32760..32768, while all 902 radians * 10240 files stay within
		// +-32169. Neither the group number (a group-2 file such as the -shop
		// "shop_violin_*" poses is already radians * 10240) nor a comparison
		// with 32767 * scale[axis] is reliable: the .mdl's scales describe
		// its own compiled animations, and separately exported .seq files may
		// legitimately exceed them, so that test misread about half of the
		// nar-archive models whose animations come from a different compile.
		{
			constexpr int kBeyondUniformRange = 32200;  // pi * 10240 = 32170
			constexpr size_t kMinChannels = 20;         // fewer: not enough evidence
			int maxAbs = 0;
			size_t channels = 0;
			for (size_t i = firstNew; i < sequences_.size(); ++i)
				for (const auto &blend : decodedSequences_[i].blends)
					for (size_t bone = 0; bone < blend.size() && bone < bones_.size(); ++bone)
						for (size_t axis = 3; axis < 6; ++axis)
						{
							const AnimChannel &ch = blend[bone].channels[axis];
							size_t pos = 0;
							int peak = 0;
							while (pos < ch.size())
							{
								const size_t valid = static_cast<size_t>(ch[pos]) & 0xFF;
								for (size_t k = 1; k <= valid && pos + k < ch.size(); ++k)
									peak = std::max(peak, std::abs(static_cast<int>(ch[pos + k])));
								pos += 1 + valid;
							}
							if (peak == 0)
								continue;
							maxAbs = std::max(maxAbs, peak);
							++channels;
						}
			bool uniform = groupNumber >= kFirstUniformScaleGroup;  // too little data
			if (maxAbs > kBeyondUniformRange)
				uniform = false;
			else if (channels >= kMinChannels)
				uniform = true;
			for (size_t i = firstNew; i < sequences_.size(); ++i)
				sequences_[i].uniformRotationScale = uniform;

			// Some shop scenes stack a second, unrelated skeleton into the
			// same .mdl (the shopkeeper plus a passer-by). Those files are
			// radians * 10240 as a whole, but the spare skeleton's static
			// channels are only meaningful with the .mdl's own scales: read as
			// radians, every limb turns by ~pi and the figure folds into a
			// shapeless mass (criss-shop: the second character below the
			// scene). Spot such a branch and keep the .mdl scales there while
			// the main skeleton still uses radians * 10240.
			if (uniform)
			{
				constexpr float kImpossibleAngle = 2.5f;  // ~143 deg in radians
				constexpr float kSaneAngle = 0.9f;        // ~52 deg in radians
				std::vector<std::uint8_t> mark(bones_.size(), 0);
				for (size_t s = firstNew; s < sequences_.size(); ++s)
				{
					if (decodedSequences_[s].blends.empty())
						continue;
					const std::vector<BoneAnim> &blend =
						decodedSequences_[s].blends.front();
					std::fill(mark.begin(), mark.end(), std::uint8_t{ 0 });
					bool any = false;
					for (size_t b = 0; b < blend.size() && b < bones_.size(); ++b)
					{
						const float rest[3] = {
							bones_[b].rotation.x, bones_[b].rotation.y,
							bones_[b].rotation.z };
						bool hasData = false;
						bool constant = true;
						float uniformMax = 0.0f;
						float mdlMax = 0.0f;
						for (size_t axis = 3; axis < 6; ++axis)
						{
							const AnimChannel &ch = blend[b].channels[axis];
							size_t pos = 0;
							int value = 0;
							bool haveValue = false;
							while (pos < ch.size())
							{
								const size_t valid =
									static_cast<size_t>(ch[pos]) & 0xFF;
								for (size_t k = 1; k <= valid && pos + k < ch.size(); ++k)
								{
									const int v = static_cast<int>(ch[pos + k]);
									if (!haveValue)
									{
										value = v;
										haveValue = true;
									}
									else if (v != value)
									{
										constant = false;
									}
								}
								pos += 1 + valid;
							}
							if (!haveValue)
								continue;
							if (value != 0)
								hasData = true;
							uniformMax = std::max(uniformMax,
								std::abs(rest[axis - 3] +
									static_cast<float>(value) / 10240.0f));
							mdlMax = std::max(mdlMax,
								std::abs(rest[axis - 3] +
									static_cast<float>(value) * bones_[b].scale[axis]));
						}
						if (!hasData || !constant)
							continue;
						if (uniformMax >= kImpossibleAngle && mdlMax <= kSaneAngle &&
							uniformMax > 2.5f * mdlMax)
						{
							mark[b] = 1;
							any = true;
						}
					}
					if (!any)
						continue;
					// The whole branch shares one rig: switching only the bone
					// that tripped the check would leave the trunk twisted.
					for (bool changed = true; changed;)
					{
						changed = false;
						for (size_t b = 0; b < bones_.size(); ++b)
						{
							const int parent = bones_[b].parent;
							if (mark[b] || parent < 0 ||
								!mark[static_cast<size_t>(parent)])
								continue;
							mark[b] = 1;
							changed = true;
						}
					}
					sequences_[s].mdlScaledBones = mark;
				}
			}
		}

		return static_cast<int>(parsed.size());
	}

	namespace
	{
		struct Matrix4
		{
			float m[16]{};  // column-major
		};

		struct Quaternion
		{
			float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
		};

		// Local transform of one bone: translation + rotation.
		struct BonePose
		{
			StudioModel::Vec3 pos;
			Quaternion q;
		};

		Quaternion QuaternionFromEuler(const StudioModel::Vec3 &rot)
		{
			const float sx = std::sin(rot.x * 0.5f), cx = std::cos(rot.x * 0.5f);
			const float sy = std::sin(rot.y * 0.5f), cy = std::cos(rot.y * 0.5f);
			const float sz = std::sin(rot.z * 0.5f), cz = std::cos(rot.z * 0.5f);
			return { cz * cy * cx + sz * sy * sx,
				cz * cy * sx - sz * sy * cx,
				cz * sy * cx + sz * cy * sx,
				sz * cy * cx - cz * sy * sx };
		}

		Quaternion Normalize(const Quaternion &q)
		{
			const float length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (length <= std::numeric_limits<float>::epsilon())
				return {};
			return { q.w / length, q.x / length, q.y / length, q.z / length };
		}

		Quaternion Slerp(const Quaternion &from, Quaternion to, float amount)
		{
			float dot = from.w * to.w + from.x * to.x + from.y * to.y + from.z * to.z;
			if (dot < 0.0f)
			{
				dot = -dot;
				to = { -to.w, -to.x, -to.y, -to.z };
			}
			if (dot > 0.9995f)
				return Normalize({ from.w + amount * (to.w - from.w),
					from.x + amount * (to.x - from.x),
					from.y + amount * (to.y - from.y),
					from.z + amount * (to.z - from.z) });

			const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
			const float a = std::sin((1.0f - amount) * angle) / std::sin(angle);
			const float b = std::sin(amount * angle) / std::sin(angle);
			return Normalize({ a * from.w + b * to.w, a * from.x + b * to.x,
				a * from.y + b * to.y, a * from.z + b * to.z });
		}

		Matrix4 QuaternionMatrix(const Quaternion &q, const StudioModel::Vec3 &pos)
		{
			const Quaternion n = Normalize(q);
			Matrix4 out;
			out.m[0] = 1 - 2 * (n.y * n.y + n.z * n.z);
			out.m[1] = 2 * (n.x * n.y + n.w * n.z);
			out.m[2] = 2 * (n.x * n.z - n.w * n.y);
			out.m[4] = 2 * (n.x * n.y - n.w * n.z);
			out.m[5] = 1 - 2 * (n.x * n.x + n.z * n.z);
			out.m[6] = 2 * (n.y * n.z + n.w * n.x);
			out.m[8] = 2 * (n.x * n.z + n.w * n.y);
			out.m[9] = 2 * (n.y * n.z - n.w * n.x);
			out.m[10] = 1 - 2 * (n.x * n.x + n.y * n.y);
			out.m[12] = pos.x; out.m[13] = pos.y; out.m[14] = pos.z; out.m[15] = 1;
			return out;
		}

		// Build a bone's local matrix from position (translation) + rotation
		// (euler angles). Layout matches the parser's rest bake in
		// ParseBodyParts so ApplyFrame(-1) reproduces the loaded default pose.
		Matrix4 LocalBoneMatrix(const StudioModel::Vec3 &pos, const StudioModel::Vec3 &rot)
		{
			const float cx = std::cos(rot.x), sx = std::sin(rot.x);
			const float cy = std::cos(rot.y), sy = std::sin(rot.y);
			const float cz = std::cos(rot.z), sz = std::sin(rot.z);
			Matrix4 out;
			out.m[0] = cy * cz; out.m[1] = cy * sz; out.m[2] = -sy;
			out.m[4] = sx * sy * cz - cx * sz; out.m[5] = sx * sy * sz + cx * cz; out.m[6] = sx * cy;
			out.m[8] = cx * sy * cz + sx * sz; out.m[9] = cx * sy * sz - sx * cz; out.m[10] = cx * cy;
			out.m[3] = 0; out.m[7] = 0; out.m[11] = 0; out.m[15] = 1;
			out.m[12] = pos.x; out.m[13] = pos.y; out.m[14] = pos.z;
			return out;
		}

		// Column-major 4x4 product: out = a * b (b applied first, like GLM).
		Matrix4 Multiply(const Matrix4 &a, const Matrix4 &b)
		{
			Matrix4 out{};
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					out.m[r * 4 + c] = a.m[r * 4 + 0] * b.m[0 * 4 + c] +
						a.m[r * 4 + 1] * b.m[1 * 4 + c] +
						a.m[r * 4 + 2] * b.m[2 * 4 + c] +
						a.m[r * 4 + 3] * b.m[3 * 4 + c];
			return out;
		}

		StudioModel::Vec3 TransformPoint(const StudioModel::Vec3 &v, const Matrix4 &m)
		{
			return {
				m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12],
				m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
				m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14],
			};
		}

		// For direction vectors (normals), not points: applies only the
		// rotation part of the matrix, dropping the translation. LocalBoneMatrix
		// never puts scale into the rotation block, so this pure-rotation
		// submatrix is orthogonal and can be used directly (no inverse-transpose
		// needed -- for an orthogonal matrix R, (R^-1)^T == R).
		StudioModel::Vec3 TransformDirection(const StudioModel::Vec3 &v, const Matrix4 &m)
		{
			return {
				m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z,
				m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
				m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z,
			};
		}

		// Walk the RLE span stream to find the span containing `frame`.
		// Returns the index of the span header element; `k` holds the residual
		// frame offset inside that span. Mirrors HLAM's CalculateBoneQuaternion
		// / CalculateBonePosition pointer walk over mstudioanimvalue_t.
		size_t LocateSpan(const StudioModel::AnimChannel &channel, int frame, int &k)
		{
			k = frame;
			size_t pos = 0;
			while (pos < channel.size())
			{
				const int valid = static_cast<int>(channel[pos]) & 0xFF;
				const int total = (static_cast<int>(channel[pos]) >> 8) & 0xFF;
				if (total <= k)
				{
					k -= total;
					pos += 1 + valid;
				}
				else
				{
					break;
				}
			}
			return pos;
		}

		float SafeValue(const StudioModel::AnimChannel &channel, size_t pos, size_t index)
		{
			const size_t idx = pos + index;
			return idx < channel.size() ? static_cast<float>(channel[idx]) : 0.0f;
		}

		// Value of the key *after* `held` for interpolation. If the channel has no
		// such key (last span of the channel) keep the current value instead of
		// reading 0, which would mean "rest pose" and make the animation slide
		// back to the bind pose.
		float NextKeyOrHold(const StudioModel::AnimChannel &channel, size_t pos,
			size_t index, float held)
		{
			const size_t idx = pos + index;
			return idx < channel.size() ? static_cast<float>(channel[idx]) : held;
		}

		// Port of HLAM CalculateBonePosition: linear interpolation of a single
		// position axis, producing the final value for the given frame.
		float CalcBonePositionAxis(const StudioModel::AnimChannel &channel, int frame,
			float fract, float base, float scale)
		{
			if (channel.empty())
				return base;
			int k = 0;
			const size_t pos = LocateSpan(channel, frame, k);
			if (pos >= channel.size())
				return base;  // Frame past the spans: hold the base pose.
			const int valid = static_cast<int>(channel[pos]) & 0xFF;
			const int total = (static_cast<int>(channel[pos]) >> 8) & 0xFF;

			if (valid > k)
			{
				if (valid > k + 1)
				{
					return base +
						(SafeValue(channel, pos, 1 + k) * (1.0f - fract) +
							fract * SafeValue(channel, pos, 2 + k)) * scale;
				}
				return base + SafeValue(channel, pos, 1 + k) * scale;
			}

			if (total <= k + 1)
			{
				const float held = SafeValue(channel, pos, valid);
				return base +
					(held * (1.0f - fract) +
						fract * NextKeyOrHold(channel, pos, valid + 2, held)) * scale;
			}
			return base + SafeValue(channel, pos, valid) * scale;
		}

		// Port of HLAM CalculateBoneQuaternion: read the two angle values that
		// surround `frame` and write their final (rest + delta*scale) values.
		void CalcBoneQuaternionAxis(const StudioModel::AnimChannel &channel, int frame,
			float base, float scale, float &angle1, float &angle2)
		{
			angle1 = angle2 = base;
			if (channel.empty())
				return;
			int k = 0;
			const size_t pos = LocateSpan(channel, frame, k);
			if (pos >= channel.size())
				return;  // Frame past the spans: angles stay at base.
			const int valid = static_cast<int>(channel[pos]) & 0xFF;
			const int total = (static_cast<int>(channel[pos]) >> 8) & 0xFF;

			float v1, v2;
			if (valid > k)
			{
				v1 = SafeValue(channel, pos, 1 + k);
				if (valid > k + 1)
				{
					v2 = SafeValue(channel, pos, 2 + k);
				}
				else if (total > k + 1)
				{
					v2 = v1;
				}
				else
				{
					v2 = NextKeyOrHold(channel, pos, valid + 2, v1);
				}
			}
			else
			{
				v1 = SafeValue(channel, pos, valid);
				if (total > k + 1)
					v2 = v1;
				else
					v2 = NextKeyOrHold(channel, pos, valid + 2, v1);
			}

			angle1 = base + v1 * scale;
			angle2 = base + v2 * scale;
		}
	}


	namespace
	{
		// Rigid inverse of a bone world matrix (rotation block orthonormal,
		// row-vector convention: p' = p * R + t).
		Matrix4 InvertRigid(const Matrix4 &a)
		{
			Matrix4 out;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					out.m[r * 4 + c] = a.m[c * 4 + r];
			for (int c = 0; c < 3; ++c)
				out.m[12 + c] = -(a.m[12] * out.m[0 * 4 + c] + a.m[13] * out.m[1 * 4 + c] +
					a.m[14] * out.m[2 * 4 + c]);
			out.m[15] = 1.0f;
			return out;
		}

		Matrix4 ToMatrix4(const std::array<float, 16> &a)
		{
			Matrix4 out;
			std::copy(a.begin(), a.end(), out.m);
			return out;
		}
	}

	// ---- Version 2026 ------------------------------------------------------
	//
	// Texture entry (96 bytes, at studiohdr.textureindex):
	//   char name[64]; int flags; int ?(8); int width, height; int mipCount;
	//   int dataIndex; int dataSize; int 0
	// dataIndex points at a BC7 (BPTC) mip chain, mip 0 first, no DDS header,
	// no palette. Only mip 0 is decoded.
	void StudioModel::ParseTextures2026(const Header &h)
	{
		textures_.clear();
		if (h.numTextures <= 0 || h.textureIndex <= 0)
			return;

		constexpr size_t kStride = 96;
		if (!InRange(static_cast<size_t>(h.textureIndex),
			kStride * static_cast<size_t>(h.numTextures), size_))
			throw std::runtime_error("model: texture table out of range");

		for (int i = 0; i < h.numTextures; ++i)
		{
			const int off = h.textureIndex + i * static_cast<int>(kStride);
			Texture tex;
			tex.name = ReadFixedString(Ptr(off), kMaxTextureName);
			tex.flags = Int(off + 64);
			tex.width = Int(off + 72);
			tex.height = Int(off + 76);
			const int index = Int(off + 84);

			if (tex.width <= 0 || tex.height <= 0 || tex.width > 8192 || tex.height > 8192)
				throw std::runtime_error("model: invalid texture dimensions");
			const size_t mip0Bytes = static_cast<size_t>((tex.width + 3) / 4) *
				static_cast<size_t>((tex.height + 3) / 4) * 16;
			if (index <= 0 || !InRange(static_cast<size_t>(index), mip0Bytes, size_))
				throw std::runtime_error("model: texture data out of range");

			QImage image(tex.width, tex.height, QImage::Format_ARGB32);
			std::vector<uint8_t> rgba(static_cast<size_t>(tex.width) * tex.height * 4);
			DecompressBC7(Ptr(index), tex.width, tex.height, rgba.data());
			uint32_t *dst = reinterpret_cast<uint32_t *>(image.bits());
			for (size_t p = 0, n = static_cast<size_t>(tex.width) * tex.height; p < n; ++p)
				dst[p] = (uint32_t(rgba[p * 4 + 3]) << 24) | (uint32_t(rgba[p * 4 + 0]) << 16) |
					(uint32_t(rgba[p * 4 + 1]) << 8) | uint32_t(rgba[p * 4 + 2]);

			tex.image = std::move(image);
			textures_.push_back(std::move(tex));
		}
	}

	// Geometry blocks of a version 2026 file (bodypart table is unchanged):
	//   model, 76 bytes: char name[64]; float radius; int numMesh; int meshIndex
	//   mesh,  32 bytes: int numVerts, vertIndex, numIndices, indexIndex,
	//                    1 (unknown), auxIndex, 0, 0
	//                    auxIndex -> int skinRef (index into the skin table), ...
	//   vertex, 48 bytes: float pos[3], normal[3], uv[2];   (bind pose, model space;
	//                     uv: v is NOT flipped, 0..1 over the texture)
	//                     uint8 bone[4]; uint8 weight[4] (sum 255);
	//                     int flag (0/1, meaning unknown); int 0
	//   indices: uint32 triangle list, relative to the mesh's own vertices.
	void StudioModel::ParseBodyParts2026(const Header &h)
	{
		bodyParts_.clear();
		inverseRest_.clear();
		if (h.numBodyParts <= 0 || h.bodyPartIndex <= 0)
			return;

		constexpr size_t kBodyPartStride = kMaxBodyPartName + 4 + 4 + 4;
		constexpr size_t kModelStride = 76;
		constexpr size_t kMeshStride = 32;
		constexpr size_t kVertexStride = 48;
		if (!InRange(static_cast<size_t>(h.bodyPartIndex),
			kBodyPartStride * static_cast<size_t>(h.numBodyParts), size_))
			throw std::runtime_error("model: bodypart table out of range");

		// Rest-pose world matrix of every bone and its inverse; vertices are
		// stored in bind space, skinning needs them in bone space.
		std::vector<Matrix4> restWorld(bones_.size());
		inverseRest_.resize(bones_.size());
		for (size_t i = 0; i < bones_.size(); ++i)
		{
			const Matrix4 local = LocalBoneMatrix(bones_[i].position, bones_[i].rotation);
			const int parent = bones_[i].parent;
			restWorld[i] = (parent >= 0 && parent < static_cast<int>(i))
				? Multiply(local, restWorld[static_cast<size_t>(parent)])
				: local;
			const Matrix4 inv = InvertRigid(restWorld[i]);
			std::copy(inv.m, inv.m + 16, inverseRest_[i].begin());
		}

		const auto textureForSkin = [&](int skinRef) -> const Texture *
		{
			if (skinRef < 0 || skinRef >= static_cast<int>(skinRefs_.size()))
				return nullptr;
			const int tex = skinRefs_[static_cast<size_t>(skinRef)];
			if (tex < 0 || tex >= static_cast<int>(textures_.size()))
				return nullptr;
			return &textures_[static_cast<size_t>(tex)];
		};

		for (int bp = 0; bp < h.numBodyParts; ++bp)
		{
			const int off = h.bodyPartIndex + bp * static_cast<int>(kBodyPartStride);
			BodyPart part;
			part.name = ReadFixedString(Ptr(off), kMaxBodyPartName);
			const int numModels = Int(off + kMaxBodyPartName);
			part.base = std::max(Int(off + kMaxBodyPartName + 4), 1);
			const int modelIndex = Int(off + kMaxBodyPartName + 8);

			if (numModels <= 0 || modelIndex <= 0)
			{
				bodyParts_.push_back(std::move(part));
				continue;
			}
			if (!InRange(static_cast<size_t>(modelIndex),
				kModelStride * static_cast<size_t>(numModels), size_))
				throw std::runtime_error("model: submodel table out of range");

			for (int mi = 0; mi < numModels; ++mi)
			{
				const int moff = modelIndex + mi * static_cast<int>(kModelStride);
				SubModel sub;
				sub.name = ReadFixedString(Ptr(moff), kMaxSubModelName);
				const int numMesh = Int(moff + 68);
				const int meshIndex = Int(moff + 72);

				if (numMesh > 0 && meshIndex > 0)
				{
					if (!InRange(static_cast<size_t>(meshIndex),
						kMeshStride * static_cast<size_t>(numMesh), size_))
						throw std::runtime_error("model: mesh table out of range");

					for (int q = 0; q < numMesh; ++q)
					{
						const int me = meshIndex + q * static_cast<int>(kMeshStride);
						const int numVerts = Int(me);
						const int vertIndex = Int(me + 4);
						const int numIndices = Int(me + 8);
						const int indexIndex = Int(me + 12);

						Mesh mesh;
						// The texture reference is the first int of the mesh's aux
						// block (auxIndex): 0 for the body, 1 for the backpack, 2
						// for the cap in buffclass26s4tr.mdl. The int at +16 is 1 for
						// every mesh seen so far and is NOT the skin reference.
						const int auxIndex = Int(me + 20);
						mesh.skinref = (auxIndex > 0 && InRange(static_cast<size_t>(auxIndex), 4, size_))
							? Int(auxIndex) : 0;
						if (numVerts <= 0 || numIndices < 3 || vertIndex <= 0 || indexIndex <= 0)
						{
							sub.meshes.push_back(std::move(mesh));
							continue;
						}
						if (!InRange(static_cast<size_t>(vertIndex),
							kVertexStride * static_cast<size_t>(numVerts), size_) ||
							!InRange(static_cast<size_t>(indexIndex),
								sizeof(uint32_t) * static_cast<size_t>(numIndices), size_))
							throw std::runtime_error("model: mesh geometry out of range");

						const Texture *tex = textureForSkin(mesh.skinref);
						const float texW = tex ? static_cast<float>(tex->width) : 256.0f;
						const float texH = tex ? static_cast<float>(tex->height) : 256.0f;

						// Per-vertex UVs of this mesh (texel units, like the old format).
						std::vector<float> us(static_cast<size_t>(numVerts));
						std::vector<float> vs(static_cast<size_t>(numVerts));
						const size_t baseVert = sub.rawVerts.size();
						for (int i = 0; i < numVerts; ++i)
						{
							const int v = vertIndex + i * static_cast<int>(kVertexStride);
							sub.rawVerts.push_back({ Float(v), Float(v + 4), Float(v + 8) });
							sub.rawNorms.push_back({ Float(v + 12), Float(v + 16), Float(v + 20) });
							us[static_cast<size_t>(i)] = Float(v + 24) * texW;
							vs[static_cast<size_t>(i)] = Float(v + 28) * texH;

							VertexWeights vw;
							int sum = 0;
							for (int k = 0; k < 4; ++k)
							{
								vw.bones[static_cast<size_t>(k)] = base_[v + 32 + k];
								sum += base_[v + 36 + k];
							}
							for (int k = 0; k < 4; ++k)
								vw.weights[static_cast<size_t>(k)] = sum > 0
									? static_cast<float>(base_[v + 36 + k]) / static_cast<float>(sum)
									: (k == 0 ? 1.0f : 0.0f);
							sub.vertWeights.push_back(vw);
						}

						const int numTris = numIndices / 3;
						mesh.triangles.reserve(static_cast<size_t>(numTris));
						for (int t = 0; t < numTris; ++t)
						{
							uint32_t idx[3];
							std::memcpy(idx, base_ + indexIndex + static_cast<size_t>(t) * 12, sizeof(idx));
							if (idx[0] >= static_cast<uint32_t>(numVerts) ||
								idx[1] >= static_cast<uint32_t>(numVerts) ||
								idx[2] >= static_cast<uint32_t>(numVerts))
								continue;  // corrupt index: skip the triangle

							Triangle tri;
							const int g[3] = {
								static_cast<int>(baseVert + idx[0]),
								static_cast<int>(baseVert + idx[1]),
								static_cast<int>(baseVert + idx[2]) };
							tri.a = sub.rawVerts[static_cast<size_t>(g[0])];
							tri.b = sub.rawVerts[static_cast<size_t>(g[1])];
							tri.c = sub.rawVerts[static_cast<size_t>(g[2])];
							tri.na = sub.rawNorms[static_cast<size_t>(g[0])];
							tri.nb = sub.rawNorms[static_cast<size_t>(g[1])];
							tri.nc = sub.rawNorms[static_cast<size_t>(g[2])];
							tri.sa = us[idx[0]]; tri.ta = vs[idx[0]];
							tri.sb = us[idx[1]]; tri.tb = vs[idx[1]];
							tri.sc = us[idx[2]]; tri.tc = vs[idx[2]];
							tri.texture = mesh.skinref;
							tri.vertA = g[0]; tri.vertB = g[1]; tri.vertC = g[2];
							tri.normA = g[0]; tri.normB = g[1]; tri.normC = g[2];
							// Dominant bone only for the legacy fields; the real
							// (weighted) skinning happens in ApplyFrame.
							tri.boneA = sub.vertWeights[static_cast<size_t>(g[0])].bones[0];
							tri.boneB = sub.vertWeights[static_cast<size_t>(g[1])].bones[0];
							tri.boneC = sub.vertWeights[static_cast<size_t>(g[2])].bones[0];
							tri.normBoneA = tri.boneA; tri.normBoneB = tri.boneB; tri.normBoneC = tri.boneC;
							mesh.triangles.push_back(tri);
						}
						sub.meshes.push_back(std::move(mesh));
					}
				}
				part.models.push_back(std::move(sub));
			}
			bodyParts_.push_back(std::move(part));
		}
	}

	void StudioModel::SetBlending(int index, float value)
	{
		if (index >= 0 && index < 2)
			blending_[index] = std::clamp(value, 0.0f, 255.0f);
	}

	float StudioModel::Blending(int index) const
	{
		return index >= 0 && index < 2 ? blending_[index] : 0.0f;
	}

	void StudioModel::ApplyFrame(int sequenceIndex, float frame)
	{
		if (bodyParts_.empty() || bones_.empty())
			return;

		// Gather the per-bone local matrices from the selected sequence pose.
		std::vector<Matrix4> localBones;
		const bool haveSequence = sequenceIndex >= 0 &&
			sequenceIndex < static_cast<int>(sequences_.size());
		if (haveSequence && decodedSequences_.size() == sequences_.size())
		{
			const Sequence &seq = sequences_[static_cast<size_t>(sequenceIndex)];
			const DecodedSequence &decoded =
				decodedSequences_[static_cast<size_t>(sequenceIndex)];
			localBones.resize(bones_.size());

			// Same rule as the GoldSource engine (StudioCalcRotations): the last
			// frame is the end of the loop, anything beyond it restarts at 0.
			// Without this a frame in (N-1, N) has no "next" key to interpolate
			// to and the pose slides towards the bone's rest pose.
			float clampedFrame = frame;
			if (seq.numframes > 0)
			{
				if (clampedFrame > static_cast<float>(seq.numframes - 1))
					clampedFrame = 0.0f;
				else if (clampedFrame < 0.0f)
					clampedFrame = 0.0f;
			}
			int frameIdx = static_cast<int>(std::floor(clampedFrame));
			const float fract = clampedFrame - std::floor(clampedFrame);

			// Rotation quantisation step per axis: the bone's own scale for
			// sequences of the .mdl (and CSO .seq groups 1-2); later .seq groups
			// store angles as radians * 10240 (so +-pi = +-32170, the maximum seen
			// in the data is 32169), identical for every bone and axis.
			constexpr float kUniformRotScale = 1.0f / 10240.0f;

			// Local pose (position + rotation) of every bone for ONE blend of the
			// sequence (StudioCalcRotations in the engine).
			const auto computePose = [&](const std::vector<BoneAnim> *blend,
				std::vector<BonePose> &out)
			{
				out.resize(bones_.size());
				for (size_t i = 0; i < bones_.size(); ++i)
				{
					const Bone &bone = bones_[i];
					Vec3 pos = bone.position;
					Vec3 rot1 = bone.rotation;
					Vec3 rot2 = bone.rotation;
					// A few files mix both encodings: a second skeleton inside
					// a radians * 10240 file still needs the bone's own scales
					// (Sequence::mdlScaledBones).
					const bool mdlScale = !seq.uniformRotationScale ||
						(i < seq.mdlScaledBones.size() && seq.mdlScaledBones[i] != 0);
					const float rotScale[3] = {
						mdlScale ? bone.scale[3] : kUniformRotScale,
						mdlScale ? bone.scale[4] : kUniformRotScale,
						mdlScale ? bone.scale[5] : kUniformRotScale };

					if (blend)
					{
						const BoneAnim &ba = (*blend)[i];
						// Position axes X,Y,Z (channels 0..2) are interpolated
						// linearly. Rotation axes XR,YR,ZR (channels 3..5) are
						// slerped between quaternions, matching the engine.
						pos.x = CalcBonePositionAxis(ba.channels[0], frameIdx, fract,
							bone.position.x, bone.scale[0]);
						pos.y = CalcBonePositionAxis(ba.channels[1], frameIdx, fract,
							bone.position.y, bone.scale[1]);
						pos.z = CalcBonePositionAxis(ba.channels[2], frameIdx, fract,
							bone.position.z, bone.scale[2]);

						CalcBoneQuaternionAxis(ba.channels[3], frameIdx,
							bone.rotation.x, rotScale[0], rot1.x, rot2.x);
						CalcBoneQuaternionAxis(ba.channels[4], frameIdx,
							bone.rotation.y, rotScale[1], rot1.y, rot2.y);
						CalcBoneQuaternionAxis(ba.channels[5], frameIdx,
							bone.rotation.z, rotScale[2], rot1.z, rot2.z);
					}

					if (haveSequence && seq.motionbone == static_cast<int>(i))
					{
						// Zero locked motion axes so the model stays rooted
						// instead of translating along its travel direction.
						if (seq.motiontype & 1) pos.x = 0.0f;  // X
						if (seq.motiontype & 2) pos.y = 0.0f;  // Y
						if (seq.motiontype & 4) pos.z = 0.0f;  // Z
					}

					// Match HLAM: convert both keyframe Euler rotations to
					// quaternions, then slerp. Linear Euler interpolation flips
					// limbs when an angle crosses its wrap boundary.
					out[i] = { pos, Slerp(QuaternionFromEuler(rot1),
						QuaternionFromEuler(rot2), fract) };
				}
			};

			// SlerpBones of the engine: `to` moves towards `from` by s.
			const auto slerpBones = [&](std::vector<BonePose> &to,
				const std::vector<BonePose> &from, double sd)
			{
				const float t = static_cast<float>(std::clamp(sd, 0.0, 1.0));
				for (size_t i = 0; i < to.size(); ++i)
				{
					to[i].q = Slerp(to[i].q, from[i].q, t);
					to[i].pos = to[i].pos * (1.0f - t) + from[i].pos * t;
				}
			};

			// A sequence may hold several blends (CS player models: 9 = a 3x3 grid
			// of aim directions, rest neighbours). The engine mixes them with the
			// "blending" controllers; blend 0 alone is just one corner of the grid
			// (head/torso/arms visibly turned). Same algorithm as the engine and as
			// HLAM's BoneTransformer::SetUpBones.
			const auto &blends = decoded.blends;
			const size_t blendCount = blends.size();
			std::vector<BonePose> poses[4];
			if (blendCount == 9)
			{
				const double bx = blending_[0], by = blending_[1];
				double ix, iy;
				size_t pick[4];
				if (bx > 127.0)
				{
					ix = (bx - 127.0) * 2.0;
					if (by > 127.0)
					{
						iy = (by - 127.0) * 2.0;
						pick[0] = 4; pick[1] = 5; pick[2] = 7; pick[3] = 8;
					}
					else
					{
						iy = by * 2.0;
						pick[0] = 1; pick[1] = 2; pick[2] = 4; pick[3] = 5;
					}
				}
				else
				{
					ix = bx * 2.0;
					if (by <= 127.0)
					{
						iy = by * 2.0;
						pick[0] = 0; pick[1] = 1; pick[2] = 3; pick[3] = 4;
					}
					else
					{
						iy = (by - 127.0) * 2.0;
						pick[0] = 3; pick[1] = 4; pick[2] = 6; pick[3] = 7;
					}
				}
				for (int k = 0; k < 4; ++k)
					computePose(&blends[pick[k]], poses[k]);
				slerpBones(poses[0], poses[1], ix / 255.0);
				slerpBones(poses[2], poses[3], ix / 255.0);
				slerpBones(poses[0], poses[2], iy / 255.0);
			}
			else
			{
				computePose(blendCount > 0 ? &blends[0] : nullptr, poses[0]);
				if (blendCount > 1)
				{
					computePose(&blends[1], poses[1]);
					slerpBones(poses[0], poses[1], blending_[0] / 255.0);
					if (blendCount == 4)
					{
						computePose(&blends[2], poses[2]);
						computePose(&blends[3], poses[3]);
						slerpBones(poses[2], poses[3], blending_[0] / 255.0);
						slerpBones(poses[0], poses[2], blending_[1] / 255.0);
					}
				}
			}

			for (size_t i = 0; i < bones_.size(); ++i)
				localBones[i] = QuaternionMatrix(poses[0][i].q, poses[0][i].pos);
		}
		else
		{
			// Rest pose: just the bone's own position/rotation, as the parser
			// bakes for the default view.
			localBones.resize(bones_.size());
			for (size_t i = 0; i < bones_.size(); ++i)
			{
				const Bone &bone = bones_[i];
				localBones[i] = LocalBoneMatrix(bone.position, bone.rotation);
			}
		}

		// Compose into world (model) space, matching the parser's rest bake.
		std::vector<Matrix4> worldBones(bones_.size());
		for (size_t i = 0; i < bones_.size(); ++i)
		{
			if (bones_[i].parent >= 0 &&
				bones_[i].parent < static_cast<int>(bones_.size()))
				worldBones[i] = Multiply(localBones[i],
					worldBones[static_cast<size_t>(bones_[i].parent)]);
			else
				worldBones[i] = localBones[i];
		}

		// Zero the motion bone's locked axes so the model stays rooted instead
		// of translating along its travel direction (walk/run/jump/death). The
		// local position was already zeroed above during pose construction.
		(void)haveSequence;

		// Version 2026: bind-space vertices with up to four weighted bones.
		// skin[i] takes a bind-space point into bone i's current pose.
		std::vector<Matrix4> skin;
		if (inverseRest_.size() == bones_.size())
		{
			skin.resize(bones_.size());
			for (size_t i = 0; i < bones_.size(); ++i)
				skin[i] = Multiply(ToMatrix4(inverseRest_[i]), worldBones[i]);
		}

		// Re-skin every triangle.
		for (auto &bp : bodyParts_)
			for (auto &sub : bp.models)
			{
				if (!sub.vertWeights.empty() && sub.vertWeights.size() == sub.rawVerts.size() &&
					!skin.empty())
				{
					std::vector<Vec3> pos(sub.rawVerts.size());
					std::vector<Vec3> nrm(sub.rawVerts.size());
					for (size_t v = 0; v < pos.size(); ++v)
					{
						const VertexWeights &vw = sub.vertWeights[v];
						Vec3 p, n;
						float used = 0.0f;
						for (size_t k = 0; k < 4; ++k)
						{
							if (vw.weights[k] <= 0.0f || vw.bones[k] >= skin.size())
								continue;
							const Matrix4 &m = skin[vw.bones[k]];
							p = p + TransformPoint(sub.rawVerts[v], m) * vw.weights[k];
							n = n + TransformDirection(sub.rawNorms[v], m) * vw.weights[k];
							used += vw.weights[k];
						}
						if (used <= 0.0f)
						{
							p = sub.rawVerts[v];
							n = sub.rawNorms[v];
						}
						else if (used < 0.999f)
						{
							p = p * (1.0f / used);
							n = n * (1.0f / used);
						}
						const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
						if (len > 1e-6f)
							n = n * (1.0f / len);
						pos[v] = p;
						nrm[v] = n;
					}
					for (auto &mesh : sub.meshes)
						for (auto &tri : mesh.triangles)
						{
							tri.a = pos[static_cast<size_t>(tri.vertA)];
							tri.b = pos[static_cast<size_t>(tri.vertB)];
							tri.c = pos[static_cast<size_t>(tri.vertC)];
							tri.na = nrm[static_cast<size_t>(tri.normA)];
							tri.nb = nrm[static_cast<size_t>(tri.normB)];
							tri.nc = nrm[static_cast<size_t>(tri.normC)];
						}
					continue;
				}
				if (sub.rawVerts.empty() || sub.vertBones.empty())
					continue;
				for (auto &mesh : sub.meshes)
					for (auto &tri : mesh.triangles)
					{
						if (tri.vertA >= 0 && tri.vertA < static_cast<int>(sub.rawVerts.size()) &&
							tri.boneA >= 0 && tri.boneA < static_cast<int>(worldBones.size()))
							tri.a = TransformPoint(sub.rawVerts[static_cast<size_t>(tri.vertA)],
								worldBones[static_cast<size_t>(tri.boneA)]);
						if (tri.vertB >= 0 && tri.vertB < static_cast<int>(sub.rawVerts.size()) &&
							tri.boneB >= 0 && tri.boneB < static_cast<int>(worldBones.size()))
							tri.b = TransformPoint(sub.rawVerts[static_cast<size_t>(tri.vertB)],
								worldBones[static_cast<size_t>(tri.boneB)]);
						if (tri.vertC >= 0 && tri.vertC < static_cast<int>(sub.rawVerts.size()) &&
							tri.boneC >= 0 && tri.boneC < static_cast<int>(worldBones.size()))
							tri.c = TransformPoint(sub.rawVerts[static_cast<size_t>(tri.vertC)],
								worldBones[static_cast<size_t>(tri.boneC)]);

						// Normals rotate with their bone, same as the rest-pose
						// bake: transform through the bone's world matrix.
						if (tri.normA >= 0 && tri.normA < static_cast<int>(sub.rawNorms.size()) &&
							tri.normBoneA >= 0 && tri.normBoneA < static_cast<int>(worldBones.size()))
							tri.na = TransformDirection(sub.rawNorms[static_cast<size_t>(tri.normA)],
								worldBones[static_cast<size_t>(tri.normBoneA)]);
						if (tri.normB >= 0 && tri.normB < static_cast<int>(sub.rawNorms.size()) &&
							tri.normBoneB >= 0 && tri.normBoneB < static_cast<int>(worldBones.size()))
							tri.nb = TransformDirection(sub.rawNorms[static_cast<size_t>(tri.normB)],
								worldBones[static_cast<size_t>(tri.normBoneB)]);
						if (tri.normC >= 0 && tri.normC < static_cast<int>(sub.rawNorms.size()) &&
							tri.normBoneC >= 0 && tri.normBoneC < static_cast<int>(worldBones.size()))
							tri.nc = TransformDirection(sub.rawNorms[static_cast<size_t>(tri.normC)],
								worldBones[static_cast<size_t>(tri.normBoneC)]);
					}
			}
	}
}
