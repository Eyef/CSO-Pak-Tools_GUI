#include "StudioModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cso_gui
{
	namespace
	{
		constexpr int kStudioHeaderId = 0x54534449;  // "IDST" little-endian
		constexpr int kStudioVersion10 = 10;
		constexpr int kStudioVersion11 = 11;

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
		name_.clear();
		bones_.clear();
		bodyParts_.clear();
		textures_.clear();
		sequences_.clear();
		decodedSequences_.clear();
		skinRefs_.clear();
		skinFamilyCount_ = 0;

		if (data.size() < 244)
			throw std::runtime_error("model: file too small to be a studio model");

		base_ = data.data();
		size_ = data.size();

		if (Int(kOffId) != kStudioHeaderId)
			throw std::runtime_error("model: not a studio model (bad IDST magic)");

		version_ = Int(kOffVersion);
		if (version_ != kStudioVersion10 && version_ != kStudioVersion11)
			throw std::runtime_error("model: unsupported studio version " + std::to_string(version_));

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
		ParseTextures(h);
		ParseSkins(h);
		ParseBodyParts(h);
		ParseSequences(h);

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
		tex.image = image.convertToFormat(QImage::Format_ARGB32);
		tex.width = tex.image.width();
		tex.height = tex.image.height();
	}

	void StudioModel::ParseSkins(const Header &h)
	{
		skinRefs_.clear();
		skinFamilyCount_ = h.numSkinFamilies;
		if (h.numSkinRef <= 0 || h.numSkinFamilies <= 0 || h.skinIndex <= 0)
			return;

		const size_t count = static_cast<size_t>(h.numSkinRef) * h.numSkinFamilies;
		if (!InRange(static_cast<size_t>(h.skinIndex), count * sizeof(int16_t), size_))
			throw std::runtime_error("model: skin table out of range");

		for (int i = 0; i < h.numSkinRef; ++i)
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
		std::vector<std::array<float, 16>> boneMatrices(bones_.size());
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
								if (v < 0 || v >= static_cast<int16_t>(verts.size()))
									continue;
								if (norm < 0 || norm >= static_cast<int16_t>(norms.size()))
									norm = 0;
								vertsInStrip.push_back({ v, norm, static_cast<float>(s), static_cast<float>(t) });
							}

							auto makeTri = [&](const VertRef &a, const VertRef &b, const VertRef &c)
							{
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
								tri.boneA = (a.vert >= 0 && a.vert < static_cast<int16_t>(vertBones.size()))
									? vertBones[static_cast<size_t>(a.vert)] : -1;
								tri.boneB = (b.vert >= 0 && b.vert < static_cast<int16_t>(vertBones.size()))
									? vertBones[static_cast<size_t>(b.vert)] : -1;
								tri.boneC = (c.vert >= 0 && c.vert < static_cast<int16_t>(vertBones.size()))
									? vertBones[static_cast<size_t>(c.vert)] : -1;
								tri.normBoneA = (a.norm >= 0 && a.norm < static_cast<int16_t>(normBones.size()))
									? normBones[static_cast<size_t>(a.norm)] : -1;
								tri.normBoneB = (b.norm >= 0 && b.norm < static_cast<int16_t>(normBones.size()))
									? normBones[static_cast<size_t>(b.norm)] : -1;
								tri.normBoneC = (c.norm >= 0 && c.norm < static_cast<int16_t>(normBones.size()))
									? normBones[static_cast<size_t>(c.norm)] : -1;
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
			DecodeSequenceAnimation(i);
	}

	void StudioModel::DecodeSequenceAnimation(size_t sequenceIndex)
	{
		DecodedSequence &decoded = decodedSequences_[sequenceIndex];
		const Sequence &seq = sequences_[sequenceIndex];
		if (seq.numblends <= 0 || seq.numframes <= 0 || bones_.empty() ||
			seq.animindex <= 0)
			return;

		// mstudioanim_t: 6 x uint16 offsets (12 bytes), ordered [blend][bone].
		const size_t animStride = 12;
		const size_t totalAnims =
			static_cast<size_t>(seq.numblends) * bones_.size();
		if (!InRange(static_cast<size_t>(seq.animindex),
			animStride * totalAnims, size_))
			return;

		decoded.blends.resize(static_cast<size_t>(seq.numblends));
		for (int blend = 0; blend < seq.numblends; ++blend)
		{
			std::vector<BoneAnim> &blendAnims =
				decoded.blends[static_cast<size_t>(blend)];
			blendAnims.resize(bones_.size());
			for (size_t bone = 0; bone < bones_.size(); ++bone)
			{
				const size_t record = static_cast<size_t>(seq.animindex) +
					(static_cast<size_t>(blend) * bones_.size() + bone) * animStride;

				BoneAnim &boneAnim = blendAnims[bone];
				for (int axis = 0; axis < 6; ++axis)
				{
					std::uint16_t offset;
					std::memcpy(&offset, base_ + record + static_cast<size_t>(axis) * 2,
						sizeof(offset));
					if (offset == 0)
						continue;

					// The stream is relative to this mstudioanim_t record.
					const size_t streamStart = record + offset;
					// Walk the RLE span headers until numframes is covered.
					size_t pos = streamStart;
					int covered = 0;
					while (covered < seq.numframes && pos < size_)
					{
						if (!InRange(pos, 2, size_))
							break;
						const std::uint8_t valid = base_[pos];
						const std::uint8_t total = base_[pos + 1];
						if (total == 0)
							break;  // malformed
						covered += total;
						pos += 2 + static_cast<size_t>(valid) * 2;
						if (!InRange(pos, 0, size_))
							break;
					}

					AnimChannel &channel = boneAnim.channels[static_cast<size_t>(axis)];
					const size_t count = (pos - streamStart) / 2;
					channel.resize(count);
					for (size_t k = 0; k < count; ++k)
					{
						std::int16_t value;
						std::memcpy(&value, base_ + streamStart + k * 2, sizeof(value));
						channel[k] = value;
					}
				}
			}
		}
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

		// Port of HLAM CalculateBonePosition: linear interpolation of a single
		// position axis, producing the final value for the given frame.
		float CalcBonePositionAxis(const StudioModel::AnimChannel &channel, int frame,
			float fract, float base, float scale)
		{
			if (channel.empty())
				return base;
			int k = 0;
			const size_t pos = LocateSpan(channel, frame, k);
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
				return base +
					(SafeValue(channel, pos, valid) * (1.0f - fract) +
						fract * SafeValue(channel, pos, valid + 2)) * scale;
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
					v2 = SafeValue(channel, pos, valid + 2);
				}
			}
			else
			{
				v1 = SafeValue(channel, pos, valid);
				if (total > k + 1)
					v2 = v1;
				else
					v2 = SafeValue(channel, pos, valid + 2);
			}

			angle1 = base + v1 * scale;
			angle2 = base + v2 * scale;
		}
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

			int frameIdx = static_cast<int>(std::floor(frame));
			if (seq.numframes > 0)
				frameIdx = frameIdx % seq.numframes;
			const float fract = frame - std::floor(frame);

			const std::vector<BoneAnim> *blend = nullptr;
			if (!decoded.blends.empty())
				blend = &decoded.blends[0];

			for (size_t i = 0; i < bones_.size(); ++i)
			{
				const Bone &bone = bones_[i];
				Vec3 pos = bone.position;
				Vec3 rot1 = bone.rotation;
				Vec3 rot2 = bone.rotation;

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
						bone.rotation.x, bone.scale[3], rot1.x, rot2.x);
					CalcBoneQuaternionAxis(ba.channels[4], frameIdx,
						bone.rotation.y, bone.scale[4], rot1.y, rot2.y);
					CalcBoneQuaternionAxis(ba.channels[5], frameIdx,
						bone.rotation.z, bone.scale[5], rot1.z, rot2.z);
				}

				if (haveSequence && seq.motionbone == static_cast<int>(i))
				{
					// Zero locked motion axes so the model stays rooted
					// instead of translating along its travel direction.
					if (seq.motiontype & 1) pos.x = 0.0f;  // X
					if (seq.motiontype & 2) pos.y = 0.0f;  // Y
					if (seq.motiontype & 4) pos.z = 0.0f;  // Z
				}

				// Match HLAM: convert both keyframe Euler rotations to quaternions,
				// then slerp. Linear Euler interpolation flips limbs when an angle
				// crosses its wrap boundary.
				localBones[i] = QuaternionMatrix(Slerp(
					QuaternionFromEuler(rot1), QuaternionFromEuler(rot2), fract), pos);
			}
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

		// Re-skin every triangle.
		for (auto &bp : bodyParts_)
			for (auto &sub : bp.models)
			{
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
