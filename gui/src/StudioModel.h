#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// Parsed Counter-Strike Online / GoldSource studiomdl (.mdl) model.
	// Supports studio header versions 10 and 11 (Nexon). The two versions
	// share the same binary layout for everything this parser uses.
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
		};

		struct BodyPart
		{
			std::string name;
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
		int Version() const { return version_; }
		const std::string &Name() const { return name_; }

		const std::vector<Bone> &Bones() const { return bones_; }
		const std::vector<BodyPart> &BodyParts() const { return bodyParts_; }
		const std::vector<Texture> &Textures() const { return textures_; }
		const std::vector<Sequence> &Sequences() const { return sequences_; }

		// Decoded per-sequence animation (empty if a sequence has none).
		const std::vector<DecodedSequence> &DecodedSequences() const { return decodedSequences_; }

		// Re-skin every triangle to the given sequence's pose at a fractional
		// frame index. sequenceIndex -1 (or out of range) applies the rest pose.
		void ApplyFrame(int sequenceIndex, float frame);

		// Number of skin families and the skinref table of family 0.
		int SkinFamilyCount() const { return skinFamilyCount_; }
		const std::vector<int> &SkinRefs() const { return skinRefs_; }

		// Replaces a texture's image (e.g. with one resolved from an external
		// file elsewhere in the pak, or picked by hand) after Load(). Updates
		// width/height to match. No-op for an out-of-range index or a null
		// image.
		void SetTextureImage(int index, const QImage &image);

	private:
		struct Header;

		const uint8_t *base_ = nullptr;
		size_t size_ = 0;

		bool valid_ = false;
		int version_ = 0;
		std::string name_;

		std::vector<Bone> bones_;
		std::vector<BodyPart> bodyParts_;
		std::vector<Texture> textures_;
		std::vector<Sequence> sequences_;
		std::vector<DecodedSequence> decodedSequences_;

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
		void ParseSequences(const Header &h);
		void DecodeSequenceAnimation(size_t sequenceIndex);
	};
}
