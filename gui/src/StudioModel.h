#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// Parsed Counter-Strike Online / GoldSource studiomdl (.mdl) model.
	// Supports studio header versions 10 and 11 (Nexon), plus ICE-encrypted
	// CSO models (versions 20/21, decrypted on the fly) and their decrypted
	// form (version 9). All share the same binary layout for everything this
	// parser uses.
	//
	// Also reads the newer "version 2026" files (CSO 2026 update): same
	// header/bone/hitbox/sequence layout, but BC7-compressed DDS textures,
	// indexed triangle lists instead of strips and vertices that carry up to
	// four bone weights (see ParseBodyParts2026).
	class StudioModel
	{
	public:
		struct Vec3
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;

			Vec3 operator+(const Vec3 &rhs) const { return { x + rhs.x, y + rhs.y, z + rhs.z }; }
			Vec3 operator-(const Vec3 &rhs) const { return { x - rhs.x, y - rhs.y, z - rhs.z }; }
			Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
		};

		struct Bone
		{
			std::string name;
			int parent = -1;
			Vec3 position;  // local rest position (bone space)
			Vec3 rotation;  // local rest rotation, radians (pitch, yaw, roll)
			std::array<float, 6> scale{};  // per-axis scale for animation deltas
		};

		// Per-triangle vertex.
		struct VertRef
		{
			int vert = 0;    // index into submodel vertex array
			int norm = 0;    // index into submodel normal array
			float s = 0.0f;  // texture coordinate in texels
			float t = 0.0f;
		};

		// A triangle in model space (post-bone-transform).
		struct Triangle
		{
			Vec3 a, b, c;
			Vec3 na, nb, nc;
			float sa = 0.0f, ta = 0.0f;
			float sb = 0.0f, tb = 0.0f;
			float sc = 0.0f, tc = 0.0f;
			int texture = 0;  // index into textures_

			int vertA = -1;  // index into SubModel::rawVerts for each corner
			int vertB = -1;
			int vertC = -1;
			int normA = -1;  // index into SubModel::rawNorms
			int normB = -1;
			int normC = -1;
			int boneA = -1;  // bone index for vertex a (and its normal)
			int boneB = -1;
			int boneC = -1;
			int normBoneA = -1;  // bone index used to rotate each normal
			int normBoneB = -1;
			int normBoneC = -1;
		};

		struct Mesh
		{
			std::string name;
			std::vector<Triangle> triangles;
			int skinref = 0;
		};

		// Up to four bone influences of one vertex (weights sum to 1).
		struct VertexWeights
		{
			std::array<std::uint8_t, 4> bones{};
			std::array<float, 4> weights{};
		};

		struct SubModel
		{
			std::string name;
			std::vector<Mesh> meshes;
			// Raw (model/bone space) vertices and their bone assignment, kept
			// separately so the model can be re-skinned for animation.
			std::vector<Vec3> rawVerts;
			std::vector<int> vertBones;
			std::vector<Vec3> rawNorms;
			std::vector<int> normBones;
			// Version 2026 only: per-vertex blend weights (parallel to
			// rawVerts; rawVerts/rawNorms are then in model space of the bind
			// pose and every normal index equals its vertex index). Empty for
			// the classic one-bone-per-vertex files.
			std::vector<VertexWeights> vertWeights;
		};

		struct BodyPart
		{
			std::string name;
			int base = 1;  // body divisor: submodel = (body / base) % models.size()
			std::vector<SubModel> models;
		};

		struct Texture
		{
			std::string name;
			int flags = 0;
			int width = 0;
			int height = 0;
			QImage image;  // ARGB32
		};

		struct Sequence
		{
			std::string label;
			float fps = 0.0f;
			int numframes = 0;
			int numblends = 0;
			int animindex = 0;
			int motiontype = 0;  // STUDIO_X/Y/Z bitmask of locked motion axes
			int motionbone = 0;  // bone index whose motion is locked
			// 0 = stored inside the .mdl itself; N > 0 = comes from the
			// external CSO companion file "<model name>N.seq" (see
			// AppendExternalSequences).
			int externalGroup = 0;
			// Quantisation of the rotation channels. false: the usual
			// mstudiobone_t::scale[3..5] of this .mdl. true: every rotation axis
			// is radians * 10240 (pi = 32170). Many CSO .seq files (usually groups >= 3,
			// but some models already from group 2; detected per file from the data) were encoded that way and ignore
			// the .mdl's per-axis bone scales --
			// decoding them with those scales shrinks/distorts the angles
			// (stretched knees, trembling forearms and hands).
			bool uniformRotationScale = false;
			// Bones that keep the .mdl's per-axis scales even when
			// uniformRotationScale is set. Some shop scenes stack a second,
			// unrelated skeleton into the same .mdl, and its static channels in
			// those radians * 10240 files only make sense with the .mdl's own
			// scales; read as radians they fold every limb into the torso.
			// Non-zero = per-bone scales for that bone index. Empty in the
			// usual case.
			std::vector<std::uint8_t> mdlScaledBones;
		};

		// Raw sequence from the file for one (bone, axis) channel. Each entry is
		// one 16-bit mstudioanimvalue_t field: an element is either a
		// { valid, total } span header (low byte = valid, high byte = total) or
		// a signed 16-bit delta value. Walked at apply time.
		using AnimChannel = std::vector<std::int16_t>;

		// Per-bone animation data for a single blend: 6 channels
		// (X, Y, Z, XR, YR, ZR) in that order.
		struct BoneAnim
		{
			std::array<AnimChannel, 6> channels;
		};

		// Fully decoded animation for one sequence.
		struct DecodedSequence
		{
			std::vector<std::vector<BoneAnim>> blends;  // [blend][bone]
		};

		StudioModel() = default;
		~StudioModel() = default;

		// Parse from raw file bytes. Throws std::runtime_error on failure.
		void Load(const std::vector<uint8_t> &data);

		bool IsValid() const { return valid_; }
		int Version() const { return version_; }  // as stored in the file (20/21 if encrypted)
		bool WasEncrypted() const { return encrypted_; }
		const std::string &Name() const { return name_; }

		const std::vector<Bone> &Bones() const { return bones_; }
		const std::vector<BodyPart> &BodyParts() const { return bodyParts_; }
		const std::vector<Texture> &Textures() const { return textures_; }
		const std::vector<Sequence> &Sequences() const { return sequences_; }

		// Decoded per-sequence animation (empty if a sequence has none).
		const std::vector<DecodedSequence> &DecodedSequences() const { return decodedSequences_; }

		// CSO keeps a part of a model's animations outside the .mdl, in
		// companion files named "<model name><N>.seq" (buffclass21s1ct.mdl +
		// buffclass21s1ct1.seq, ...2.seq, ...). Layout of one .seq file:
		//   int32 numseq, int32 seqdescOffset          (8-byte header)
		//   [mstudioevent_t...] [mstudioanim_t + values...]   (data from 8)
		//   mstudioseqdesc_t[numseq]                   (at seqdescOffset)
		// The seqdesc pointers (eventindex/pivotindex/animindex) are NOT
		// file offsets: they keep the addresses they had in the original,
		// unsplit .mdl. The data block always starts at file offset 8, so
		// file offset = pointer - (lowest pointer - 8). The animations use
		// this model's skeleton, so call it after Load().
		//
		// Appends the file's sequences to Sequences()/DecodedSequences()
		// (tagged with externalGroup = groupNumber) and returns how many
		// were added. Throws std::runtime_error for a malformed file. The
		// bytes are only read during the call.
		int AppendExternalSequences(const std::vector<uint8_t> &seqData, int groupNumber);

		// Re-skin every triangle to the given sequence's pose at a fractional
		// frame index. sequenceIndex -1 (or out of range) applies the rest pose.
		void ApplyFrame(int sequenceIndex, float frame);

		// Position of the two "blending" controllers (0..255) that mix the blends
		// of a multi-blend sequence (CS player models: 9 blends = 3x3 grid of aim
		// directions). Default 127.5 = centre of the range, which is what
		// HLAM shows and what a model looking straight ahead uses. Applies to
		// the next ApplyFrame().
		void SetBlending(int index, float value);
		float Blending(int index) const;

		// Number of skin families and the skinref table of family 0.
		int SkinFamilyCount() const { return skinFamilyCount_; }
		const std::vector<int> &SkinRefs() const { return skinRefs_; }

		// Replaces a texture's image (e.g. with one resolved from an external
		// file elsewhere in the pak, or picked by hand) after Load(). Updates
		// width/height to match. No-op for an out-of-range index or a null
		// image.
		void SetTextureImage(int index, const QImage &image);

		// Imports the whole texture table (names, flags, pixels) from
		// another model. For CSO's split world models: geometry-only files
		// like w_50ae_big.mdl (no texture table, meshes reference skinrefs
		// 0/1/...) pair with a w_50ae_bigt.mdl twin that holds nothing but
		// the textures in matching order.
		void ImportTexturesFrom(const StudioModel &other);

	private:
		struct Header;

		const uint8_t *base_ = nullptr;
		size_t size_ = 0;

		bool valid_ = false;
		int version_ = 0;
		bool encrypted_ = false;
		std::string name_;

		std::vector<Bone> bones_;
		std::vector<BodyPart> bodyParts_;
		std::vector<Texture> textures_;
		std::vector<Sequence> sequences_;
		std::vector<DecodedSequence> decodedSequences_;
		// Version 2026: inverse of every bone's rest-pose world matrix
		// (rigid, 16 floats each, same layout as the internal Matrix4), used
		// to turn bind-space vertices into bone-local ones when skinning.
		std::vector<std::array<float, 16>> inverseRest_;

		float blending_[2] = { 127.5f, 127.5f };
		int skinFamilyCount_ = 0;
		std::vector<int> skinRefs_;

		const uint8_t *Ptr(int offset) const;
		int32_t Int(int offset) const;
		float Float(int offset) const;

		void ParseHeader(const Header &h);
		void ParseBones(const Header &h);
		void ParseTextures(const Header &h);
		void ParseSkins(const Header &h);
		void ParseBodyParts(const Header &h);
		void ParseTextures2026(const Header &h);
		void ParseBodyParts2026(const Header &h);
		void ParseSequences(const Header &h);
		// Decodes sequences_[sequenceIndex] from `data`. `bias` converts the
		// sequence's animindex into an offset inside `data` (0 for the .mdl
		// itself, see AppendExternalSequences for .seq files).
		void DecodeSequenceAnimation(size_t sequenceIndex, const uint8_t *data, size_t size,
			std::int64_t bias);
	};
}
