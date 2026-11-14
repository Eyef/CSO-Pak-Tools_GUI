#include "MdlConverter.h"

#include "TextureQuantizer.h"

#include <QImage>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cso_gui
{
	namespace
	{
		constexpr std::size_t kV10HeaderSize = 244;
		constexpr std::size_t kBodyPartStride = 76;
		constexpr std::size_t kModelStride = 112;
		constexpr std::size_t kMeshStride = 20;
		constexpr std::size_t kTextureStride = 80;
		constexpr std::size_t kSequenceStride = 176;
		constexpr std::size_t kBoneStride = 112;
		constexpr int kMaxStripLength = 128;
		constexpr int kMaxVertices = 32767;  // v10 indices are 16-bit
		constexpr int kFlagMasked = 0x40;

		// ------------------------------------------------------------------ I/O

		void RequireRange(const std::vector<std::uint8_t> &data, long long offset,
			long long size, const char *what)
		{
			if (offset < 0 || size < 0 || offset + size > static_cast<long long>(data.size()))
			{
				throw std::runtime_error(std::string(what) + " lies outside the file (offset " +
					std::to_string(offset) + ", size " + std::to_string(size) + ")");
			}
		}

		std::uint32_t ReadU32(const std::vector<std::uint8_t> &data, std::size_t offset)
		{
			if (offset + 4 > data.size())
				throw std::runtime_error("model: read past the end of the file");
			return static_cast<std::uint32_t>(data[offset]) |
				(static_cast<std::uint32_t>(data[offset + 1]) << 8) |
				(static_cast<std::uint32_t>(data[offset + 2]) << 16) |
				(static_cast<std::uint32_t>(data[offset + 3]) << 24);
		}

		std::int32_t ReadI32(const std::vector<std::uint8_t> &data, std::size_t offset)
		{
			return std::bit_cast<std::int32_t>(ReadU32(data, offset));
		}

		void WriteU32(std::vector<std::uint8_t> &data, std::size_t offset, std::uint32_t value)
		{
			data[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
			data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
			data[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
			data[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
		}

		void WriteI32(std::vector<std::uint8_t> &data, std::size_t offset, std::int32_t value)
		{
			WriteU32(data, offset, std::bit_cast<std::uint32_t>(value));
		}

		void WriteF32(std::vector<std::uint8_t> &data, std::size_t offset, float value)
		{
			WriteU32(data, offset, std::bit_cast<std::uint32_t>(value));
		}

		void AppendU32(std::vector<std::uint8_t> &data, std::uint32_t value)
		{
			data.push_back(static_cast<std::uint8_t>(value & 0xFF));
			data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
			data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
			data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
		}

		void AppendI32(std::vector<std::uint8_t> &data, std::int32_t value)
		{
			AppendU32(data, std::bit_cast<std::uint32_t>(value));
		}

		void AppendF32(std::vector<std::uint8_t> &data, float value)
		{
			AppendU32(data, std::bit_cast<std::uint32_t>(value));
		}

		void AppendI16(std::vector<std::uint8_t> &data, std::int16_t value)
		{
			data.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xFF));
			data.push_back(static_cast<std::uint8_t>((static_cast<std::uint16_t>(value) >> 8) & 0xFF));
		}

		void Pad4(std::vector<std::uint8_t> &data)
		{
			while ((data.size() & 3u) != 0)
				data.push_back(0);
		}

		void WriteName(std::vector<std::uint8_t> &data, std::size_t offset,
			std::size_t fieldSize, const std::string &name)
		{
			std::fill(data.begin() + static_cast<std::ptrdiff_t>(offset),
				data.begin() + static_cast<std::ptrdiff_t>(offset + fieldSize), 0);
			const std::size_t count = std::min(fieldSize, name.size());
			std::memcpy(data.data() + offset, name.data(), count);
		}

		// -------------------------------------------------------------- header

		struct Header
		{
			std::int32_t version = 0;
			std::int32_t length = 0;
			std::int32_t numBones = 0, boneIndex = 0;
			std::int32_t numBoneControllers = 0, boneControllerIndex = 0;
			std::int32_t numHitboxes = 0, hitboxIndex = 0;
			std::int32_t numSeq = 0, seqIndex = 0;
			std::int32_t numSeqGroups = 0, seqGroupIndex = 0;
			std::int32_t numTextures = 0, textureIndex = 0, textureDataIndex = 0;
			std::int32_t numSkinRef = 0, numSkinFamilies = 0, skinIndex = 0;
			std::int32_t numBodyParts = 0, bodyPartIndex = 0;
			std::int32_t numAttachments = 0, attachmentIndex = 0;
			std::int32_t soundTable = 0, soundIndex = 0;
			std::int32_t soundGroups = 0, soundGroupIndex = 0;
			std::int32_t numTransitions = 0, transitionIndex = 0;
		};

		Header ReadHeader(const std::vector<std::uint8_t> &data)
		{
			if (data.size() < kV10HeaderSize)
				throw std::runtime_error("file is too small to be a studio model");
			if (std::memcmp(data.data(), "IDST", 4) != 0)
				throw std::runtime_error("not a GoldSrc studio model (missing IDST signature)");

			Header h;
			h.version = ReadI32(data, 4);
			h.length = ReadI32(data, 72);
			h.numBones = ReadI32(data, 140);
			h.boneIndex = ReadI32(data, 144);
			h.numBoneControllers = ReadI32(data, 148);
			h.boneControllerIndex = ReadI32(data, 152);
			h.numHitboxes = ReadI32(data, 156);
			h.hitboxIndex = ReadI32(data, 160);
			h.numSeq = ReadI32(data, 164);
			h.seqIndex = ReadI32(data, 168);
			h.numSeqGroups = ReadI32(data, 172);
			h.seqGroupIndex = ReadI32(data, 176);
			h.numTextures = ReadI32(data, 180);
			h.textureIndex = ReadI32(data, 184);
			h.textureDataIndex = ReadI32(data, 188);
			h.numSkinRef = ReadI32(data, 192);
			h.numSkinFamilies = ReadI32(data, 196);
			h.skinIndex = ReadI32(data, 200);
			h.numBodyParts = ReadI32(data, 204);
			h.bodyPartIndex = ReadI32(data, 208);
			h.numAttachments = ReadI32(data, 212);
			h.attachmentIndex = ReadI32(data, 216);
			h.soundTable = ReadI32(data, 220);
			h.soundIndex = ReadI32(data, 224);
			h.soundGroups = ReadI32(data, 228);
			h.soundGroupIndex = ReadI32(data, 232);
			h.numTransitions = ReadI32(data, 236);
			h.transitionIndex = ReadI32(data, 240);
			return h;
		}

		// ------------------------------------------------------------- geometry

		struct VertexKey
		{
			std::uint32_t x = 0;
			std::uint32_t y = 0;
			std::uint32_t z = 0;
			std::uint32_t bone = 0;

			bool operator==(const VertexKey &other) const = default;
		};

		struct VertexKeyHash
		{
			std::size_t operator()(const VertexKey &key) const
			{
				std::uint64_t h = (static_cast<std::uint64_t>(key.x) * 0x9E3779B97F4A7C15ull) ^
					(static_cast<std::uint64_t>(key.y) * 0xC2B2AE3D27D4EB4Full) ^
					(static_cast<std::uint64_t>(key.z) * 0x165667B19E3779F9ull) ^
					static_cast<std::uint64_t>(key.bone);
				h ^= h >> 33;
				h *= 0xFF51AFD7ED558CCDull;
				h ^= h >> 33;
				return static_cast<std::size_t>(h);
			}
		};

		struct Corner
		{
			std::int16_t vert = 0;
			std::int16_t norm = 0;
			std::int16_t s = 0;
			std::int16_t t = 0;
		};

		struct OutMesh
		{
			int skinRef = 0;
			int numTris = 0;
			std::vector<std::uint8_t> commands;
		};

		struct OutModel
		{
			std::string name;
			float radius = 0.0f;
			std::vector<std::uint8_t> vertBones;
			std::vector<StudioModel::Vec3> verts;
			std::vector<std::uint8_t> normBones;
			std::vector<StudioModel::Vec3> norms;
			std::vector<OutMesh> meshes;
		};

		StudioModel::Vec3 TransformPoint(const std::array<float, 16> &m,
			const StudioModel::Vec3 &v)
		{
			return { m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12],
				m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
				m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] };
		}

		StudioModel::Vec3 RotateVector(const std::array<float, 16> &m,
			const StudioModel::Vec3 &v)
		{
			return { m[0] * v.x + m[4] * v.y + m[8] * v.z,
				m[1] * v.x + m[5] * v.y + m[9] * v.z,
				m[2] * v.x + m[6] * v.y + m[10] * v.z };
		}

		StudioModel::Vec3 Normalize(const StudioModel::Vec3 &v)
		{
			const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
			if (length < 1e-12f)
				return v;
			return { v.x / length, v.y / length, v.z / length };
		}

		std::uint64_t EdgeKey(int from, int to)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(from)) << 32) |
				static_cast<std::uint32_t>(to);
		}

		// Greedy triangle-strip builder, same algorithm as the reference
		// converter: output strips decode (GL parity, winding preserved) to
		// exactly the input triangles.
		std::vector<std::vector<int>> BuildStrips(const std::vector<std::array<int, 3>> &tris,
			int maxLength)
		{
			const int count = static_cast<int>(tris.size());
			std::vector<std::vector<int>> strips;
			if (count == 0)
				return strips;

			std::unordered_map<std::uint64_t, std::vector<int>> edges;
			edges.reserve(static_cast<std::size_t>(count) * 3);
			for (int i = 0; i < count; ++i)
			{
				const auto &t = tris[static_cast<std::size_t>(i)];
				edges[EdgeKey(t[0], t[1])].push_back(i);
				edges[EdgeKey(t[1], t[2])].push_back(i);
				edges[EdgeKey(t[2], t[0])].push_back(i);
			}

			std::vector<int> degree(static_cast<std::size_t>(count), 0);
			for (int i = 0; i < count; ++i)
			{
				const auto &t = tris[static_cast<std::size_t>(i)];
				for (const auto &[u, v] : { std::pair{ t[0], t[1] }, std::pair{ t[1], t[2] },
					std::pair{ t[2], t[0] } })
				{
					const auto it = edges.find(EdgeKey(v, u));
					if (it == edges.end())
						continue;
					for (const int j : it->second)
						if (j != i)
							++degree[static_cast<std::size_t>(i)];
				}
			}

			std::vector<bool> used(static_cast<std::size_t>(count), false);
			const auto third = [&tris](int tri, int u, int v)
			{
				const auto &t = tris[static_cast<std::size_t>(tri)];
				if (t[0] == u && t[1] == v)
					return t[2];
				if (t[1] == u && t[2] == v)
					return t[0];
				return t[1];
			};

			std::vector<int> s;
			std::vector<int> local;
			const auto grow = [&](int start, int rotation)
			{
				const auto &t = tris[static_cast<std::size_t>(start)];
				s.clear();
				if (rotation == 0)
					s = { t[0], t[1], t[2] };
				else if (rotation == 1)
					s = { t[1], t[2], t[0] };
				else
					s = { t[2], t[0], t[1] };
				local.clear();
				local.push_back(start);
				std::unordered_set<int> seen;
				seen.insert(start);
				while (static_cast<int>(s.size()) < maxLength)
				{
					const int n = static_cast<int>(s.size());
					const bool even = (n - 2) % 2 == 0;
					const int u = even ? s[static_cast<std::size_t>(n - 2)] : s[static_cast<std::size_t>(n - 1)];
					const int v = even ? s[static_cast<std::size_t>(n - 1)] : s[static_cast<std::size_t>(n - 2)];
					int next = -1;
					const auto it = edges.find(EdgeKey(u, v));
					if (it != edges.end())
					{
						for (const int j : it->second)
						{
							if (!used[static_cast<std::size_t>(j)] && seen.count(j) == 0)
							{
								next = j;
								break;
							}
						}
					}
					if (next < 0)
						break;
					s.push_back(third(next, u, v));
					local.push_back(next);
					seen.insert(next);
				}
			};

			std::vector<int> order(static_cast<std::size_t>(count));
			std::iota(order.begin(), order.end(), 0);
			std::stable_sort(order.begin(), order.end(),
				[&degree](int lhs, int rhs) { return degree[static_cast<std::size_t>(lhs)] <
					degree[static_cast<std::size_t>(rhs)]; });

			std::vector<int> bestStrip;
			std::vector<int> bestLocal;
			for (const int start : order)
			{
				if (used[static_cast<std::size_t>(start)])
					continue;
				bestStrip.clear();
				bestLocal.clear();
				for (int rotation = 0; rotation < 3; ++rotation)
				{
					grow(start, rotation);
					if (s.size() > bestStrip.size())
					{
						bestStrip = s;
						bestLocal = local;
					}
				}
				for (const int j : bestLocal)
					used[static_cast<std::size_t>(j)] = true;
				strips.push_back(bestStrip);
			}
			return strips;
		}

		struct OutTexture
		{
			std::string name;
			int flags = 0;
			int width = 0;
			int height = 0;
			int sourceWidth = 0;
			int sourceHeight = 0;
			IndexedTexture image;
		};

		// ------------------------------------------------------------ textures

		bool EndsWithNoCase(const std::string &text, const std::string &suffix)
		{
			if (text.size() < suffix.size())
				return false;
			const std::size_t offset = text.size() - suffix.size();
			for (std::size_t i = 0; i < suffix.size(); ++i)
			{
				const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + i])));
				const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
				if (a != b)
					return false;
			}
			return true;
		}

		std::vector<OutTexture> ConvertTextures(const StudioModel &model,
			const MdlConvertSettings &settings, MdlConvertReport &report)
		{
			std::vector<OutTexture> result;
			result.reserve(model.Textures().size());
			for (const auto &texture : model.Textures())
			{
				OutTexture out;
				out.flags = texture.flags;
				out.name = texture.name.empty() ? std::string("texture") : texture.name;
				out.sourceWidth = texture.width;
				out.sourceHeight = texture.height;

				// Pull the picture out as tightly packed R,G,B,A bytes.
				std::vector<std::uint8_t> rgba;
				int width = 0;
				int height = 0;
				{
					const QImage source = texture.image;
					if (!source.isNull())
					{
						const QImage argb = source.convertToFormat(QImage::Format_ARGB32);
						width = argb.width();
						height = argb.height();
						rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
						for (int y = 0; y < height; ++y)
						{
							const uchar *line = argb.constScanLine(y);
							for (int x = 0; x < width; ++x)
							{
								std::uint32_t pixel = 0;  // 0xAARRGGBB
								std::memcpy(&pixel, line + static_cast<std::size_t>(x) * 4, 4);
								std::uint8_t *q = &rgba[(static_cast<std::size_t>(y) *
									static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
								q[0] = static_cast<std::uint8_t>((pixel >> 16) & 255);
								q[1] = static_cast<std::uint8_t>((pixel >> 8) & 255);
								q[2] = static_cast<std::uint8_t>(pixel & 255);
								q[3] = static_cast<std::uint8_t>((pixel >> 24) & 255);
							}
						}
					}
				}
				if (rgba.empty())
				{
					// Unreadable texture (e.g. a broken/unknown compression):
					// write the usual magenta error colour instead of dropping
					// the slot, so the skin table stays intact.
					width = 64;
					height = 64;
					rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
					for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
					{
						rgba[i] = 255;
						rgba[i + 1] = 0;
						rgba[i + 2] = 255;
						rgba[i + 3] = 255;
					}
					out.sourceWidth = out.sourceWidth > 0 ? out.sourceWidth : 64;
					out.sourceHeight = out.sourceHeight > 0 ? out.sourceHeight : 64;
					report.warnings.push_back("texture '" + out.name +
						"' could not be decoded; a placeholder was written");
				}

				if (settings.maxTextureSize > 0 &&
					(width > settings.maxTextureSize || height > settings.maxTextureSize))
				{
					const double factor = static_cast<double>(settings.maxTextureSize) /
						static_cast<double>(std::max(width, height));
					const int newWidth = std::max(8,
						static_cast<int>(std::lround(width * factor / 8.0)) * 8);
					const int newHeight = std::max(8,
						static_cast<int>(std::lround(height * factor / 8.0)) * 8);
					report.warnings.push_back("texture '" + out.name + "' downscaled from " +
						std::to_string(width) + "x" + std::to_string(height) + " to " +
						std::to_string(newWidth) + "x" + std::to_string(newHeight));
					rgba = ResizeRgba(rgba.data(), width, height, newWidth, newHeight);
					width = newWidth;
					height = newHeight;
				}

				if (settings.renameDdsToBmp && EndsWithNoCase(out.name, ".dds"))
					out.name = out.name.substr(0, out.name.size() - 4) + ".bmp";
				if (out.name.size() > 63)
					out.name.resize(63);

				const bool masked = (texture.flags & kFlagMasked) != 0;
				out.image = QuantizeRgba(rgba.data(), width, height, masked, settings.alphaThreshold,
					settings.ditherTextures, settings.paletteRefineIterations);
				out.width = out.image.width;
				out.height = out.image.height;
				if (out.width <= 0 || out.height <= 0)
					throw std::runtime_error("texture '" + out.name + "' has an invalid size");
				result.push_back(std::move(out));
			}
			report.textures = static_cast<int>(result.size());
			return result;
		}

		// ------------------------------------------------------------ one model

		OutModel ConvertSubModel(const StudioModel::SubModel &sub,
			const std::vector<std::array<float, 16>> &inverseRest,
			const std::vector<int> &skinFamily, const std::vector<OutTexture> &textures,
			int boneCount, MdlConvertReport &report)
		{
			OutModel out;
			out.name = sub.name;
			out.radius = sub.radius;

			for (const auto &weights : sub.vertWeights)
			{
				int used = 0;
				float heaviest = 0.0f;
				for (int k = 0; k < 4; ++k)
				{
					if (weights.weights[static_cast<std::size_t>(k)] > 0.0f)
						++used;
					heaviest = std::max(heaviest, weights.weights[static_cast<std::size_t>(k)]);
				}
				if (used > 1)
					++report.blendedVertices;
				if (heaviest < 0.75f)
					++report.weakBlendVertices;
			}
			report.sourceVertices += static_cast<int>(sub.rawVerts.size());

			std::unordered_map<VertexKey, int, VertexKeyHash> vertLookup;
			std::unordered_map<VertexKey, int, VertexKeyHash> normLookup;
			vertLookup.reserve(sub.rawVerts.size());
			normLookup.reserve(sub.rawNorms.size());

			// The heaviest of the four blend weights, not necessarily the first
			// one: the file does not guarantee that they are sorted.
			const auto dominantBone = [&sub](int vertexIndex, int fallback)
			{
				if (vertexIndex >= 0 &&
					static_cast<std::size_t>(vertexIndex) < sub.vertWeights.size())
				{
					const auto &weights = sub.vertWeights[static_cast<std::size_t>(vertexIndex)];
					int best = 0;
					for (int k = 1; k < 4; ++k)
					{
						if (weights.weights[static_cast<std::size_t>(k)] >
							weights.weights[static_cast<std::size_t>(best)])
							best = k;
					}
					return static_cast<int>(weights.bones[static_cast<std::size_t>(best)]);
				}
				return fallback;
			};

			for (const auto &mesh : sub.meshes)
			{
				if (mesh.triangles.empty())
					continue;

				const int skinRef = mesh.skinref;
				if (skinRef < 0 || skinRef >= static_cast<int>(skinFamily.size()))
					throw std::runtime_error("model '" + sub.name + "': submesh skin reference " +
						std::to_string(skinRef) + " is out of range");
				const int textureIndex = skinFamily[static_cast<std::size_t>(skinRef)];
				if (textureIndex < 0 || textureIndex >= static_cast<int>(textures.size()))
					throw std::runtime_error("model '" + sub.name +
						"': skin reference points to a missing texture");
				const OutTexture &texture = textures[static_cast<std::size_t>(textureIndex)];
				const float scaleS = texture.sourceWidth > 0
					? static_cast<float>(texture.width) / static_cast<float>(texture.sourceWidth)
					: 1.0f;
				const float scaleT = texture.sourceHeight > 0
					? static_cast<float>(texture.height) / static_cast<float>(texture.sourceHeight)
					: 1.0f;

				std::unordered_map<std::uint64_t, int> cornerLookup;
				std::vector<Corner> corners;
				std::vector<std::array<int, 3>> triangles;
				corners.reserve(mesh.triangles.size() * 3);
				triangles.reserve(mesh.triangles.size());

				for (const auto &tri : mesh.triangles)
				{
					// Bind-space data straight from the model (tri.a/.na are
					// re-posed by ApplyFrame and must not be used here).
					const int vertexIds[3] = { tri.vertA, tri.vertB, tri.vertC };
					const int normalIds[3] = { tri.normA, tri.normB, tri.normC };
					for (int k = 0; k < 3; ++k)
					{
						if (vertexIds[k] < 0 || static_cast<std::size_t>(vertexIds[k]) >= sub.rawVerts.size() ||
							normalIds[k] < 0 || static_cast<std::size_t>(normalIds[k]) >= sub.rawNorms.size())
							throw std::runtime_error("model '" + sub.name + "': vertex index out of range");
					}
					const StudioModel::Vec3 *positions[3] = { &sub.rawVerts[static_cast<std::size_t>(vertexIds[0])],
						&sub.rawVerts[static_cast<std::size_t>(vertexIds[1])],
						&sub.rawVerts[static_cast<std::size_t>(vertexIds[2])] };
					const StudioModel::Vec3 *normals[3] = { &sub.rawNorms[static_cast<std::size_t>(normalIds[0])],
						&sub.rawNorms[static_cast<std::size_t>(normalIds[1])],
						&sub.rawNorms[static_cast<std::size_t>(normalIds[2])] };
					const int bones[3] = { dominantBone(tri.vertA, tri.boneA),
						dominantBone(tri.vertB, tri.boneB),
						dominantBone(tri.vertC, tri.boneC) };
					const float s[3] = { tri.sa, tri.sb, tri.sc };
					const float t[3] = { tri.ta, tri.tb, tri.tc };

					int ids[3] = { 0, 0, 0 };
					for (int k = 0; k < 3; ++k)
					{
						if (bones[k] < 0 || bones[k] >= boneCount)
							throw std::runtime_error("model '" + sub.name +
								"': vertex refers to bone " + std::to_string(bones[k]) +
								" but the skeleton has " + std::to_string(boneCount) + " bones");
						const auto &inverse = inverseRest[static_cast<std::size_t>(bones[k])];

						const StudioModel::Vec3 localPosition = TransformPoint(inverse, *positions[k]);
						const VertexKey positionKey{ std::bit_cast<std::uint32_t>(localPosition.x),
							std::bit_cast<std::uint32_t>(localPosition.y),
							std::bit_cast<std::uint32_t>(localPosition.z),
							static_cast<std::uint32_t>(bones[k]) };
						auto positionIt = vertLookup.find(positionKey);
						int vertexIndex = 0;
						if (positionIt != vertLookup.end())
						{
							vertexIndex = positionIt->second;
						}
						else
						{
							vertexIndex = static_cast<int>(out.verts.size());
							if (vertexIndex >= kMaxVertices)
								throw std::runtime_error("model '" + sub.name + "' needs more than " +
									std::to_string(kMaxVertices) +
									" vertices; the classic format has 16-bit indices");
							out.verts.push_back(localPosition);
							out.vertBones.push_back(static_cast<std::uint8_t>(bones[k]));
							vertLookup.emplace(positionKey, vertexIndex);
						}

						const StudioModel::Vec3 localNormal = Normalize(RotateVector(inverse, *normals[k]));
						const VertexKey normalKey{ std::bit_cast<std::uint32_t>(localNormal.x),
							std::bit_cast<std::uint32_t>(localNormal.y),
							std::bit_cast<std::uint32_t>(localNormal.z),
							static_cast<std::uint32_t>(bones[k]) };
						auto normalIt = normLookup.find(normalKey);
						int normalIndex = 0;
						if (normalIt != normLookup.end())
						{
							normalIndex = normalIt->second;
						}
						else
						{
							normalIndex = static_cast<int>(out.norms.size());
							if (normalIndex >= kMaxVertices)
								throw std::runtime_error("model '" + sub.name + "' needs more than " +
									std::to_string(kMaxVertices) + " normals");
							out.norms.push_back(localNormal);
							out.normBones.push_back(static_cast<std::uint8_t>(bones[k]));
							normLookup.emplace(normalKey, normalIndex);
						}

						const auto clampTexCoord = [](float value)
						{
							const long rounded = std::lround(value);
							return static_cast<std::int16_t>(std::clamp<long>(rounded, -32768, 32767));
						};
						const Corner corner{ static_cast<std::int16_t>(vertexIndex),
							static_cast<std::int16_t>(normalIndex),
							clampTexCoord(s[k] * scaleS), clampTexCoord(t[k] * scaleT) };
						const std::uint64_t cornerKey =
							static_cast<std::uint64_t>(static_cast<std::uint16_t>(corner.vert)) |
							(static_cast<std::uint64_t>(static_cast<std::uint16_t>(corner.norm)) << 16) |
							(static_cast<std::uint64_t>(static_cast<std::uint16_t>(corner.s)) << 32) |
							(static_cast<std::uint64_t>(static_cast<std::uint16_t>(corner.t)) << 48);
						auto cornerIt = cornerLookup.find(cornerKey);
						if (cornerIt != cornerLookup.end())
						{
							ids[k] = cornerIt->second;
						}
						else
						{
							ids[k] = static_cast<int>(corners.size());
							corners.push_back(corner);
							cornerLookup.emplace(cornerKey, ids[k]);
						}
					}

					if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
						continue;  // degenerate after merging
					triangles.push_back({ ids[0], ids[1], ids[2] });
				}

				if (triangles.empty())
					continue;

				const std::vector<std::vector<int>> strips = BuildStrips(triangles, kMaxStripLength);

				// Self-check: decode the strips the way the engine does and make
				// sure they reproduce exactly the triangles we started from.
				// (A strip bug would otherwise only show up as a subtly broken
				// surface much later.)
				{
					std::vector<std::array<int, 3>> decoded;
					decoded.reserve(triangles.size());
					for (const auto &strip : strips)
					{
						for (std::size_t i = 0; i + 2 < strip.size(); ++i)
						{
							if ((i % 2) == 0)
								decoded.push_back({ strip[i], strip[i + 1], strip[i + 2] });
							else
								decoded.push_back({ strip[i + 1], strip[i], strip[i + 2] });
						}
					}
					const auto canonical = [](std::array<int, 3> t)
					{
						if (t[1] < t[0] && t[1] < t[2])
							return std::array<int, 3>{ t[1], t[2], t[0] };
						if (t[2] < t[0] && t[2] < t[1])
							return std::array<int, 3>{ t[2], t[0], t[1] };
						return t;
					};
					std::vector<std::array<int, 3>> expected = triangles;
					for (auto &t : expected)
						t = canonical(t);
					for (auto &t : decoded)
						t = canonical(t);
					std::sort(expected.begin(), expected.end());
					std::sort(decoded.begin(), decoded.end());
					if (expected != decoded)
						throw std::runtime_error("internal error: the triangle strips of model '" +
							sub.name + "' do not reproduce its triangles");
				}

				OutMesh outMesh;
				outMesh.skinRef = skinRef;
				outMesh.numTris = static_cast<int>(triangles.size());
				for (const auto &strip : strips)
				{
					AppendI16(outMesh.commands, static_cast<std::int16_t>(strip.size()));
					for (const int cornerIndex : strip)
					{
						const Corner &corner = corners[static_cast<std::size_t>(cornerIndex)];
						AppendI16(outMesh.commands, corner.vert);
						AppendI16(outMesh.commands, corner.norm);
						AppendI16(outMesh.commands, corner.s);
						AppendI16(outMesh.commands, corner.t);
					}
				}
				AppendI16(outMesh.commands, 0);
				report.triangles += outMesh.numTris;
				report.strips += static_cast<int>(strips.size());
				out.meshes.push_back(std::move(outMesh));
			}

			report.vertices += static_cast<int>(out.verts.size());
			report.normals += static_cast<int>(out.norms.size());
			return out;
		}

		// ------------------------------------------------------------ assembly

		void RelocateFrontPointer(std::vector<std::uint8_t> &front, std::size_t offset,
			std::int64_t boneIndex, std::int64_t bodyPartIndex, std::int64_t shift,
			const char *what)
		{
			const std::int32_t value = ReadI32(front, offset);
			if (value == 0)
				return;
			if (static_cast<std::int64_t>(value) < boneIndex ||
				static_cast<std::int64_t>(value) > bodyPartIndex)
			{
				throw std::runtime_error(std::string(what) +
					" points outside the skeleton/animation block; the model uses external "
					"sequence groups that cannot be converted");
			}
			WriteI32(front, offset, static_cast<std::int32_t>(static_cast<std::int64_t>(value) + shift));
		}

		void VerifyConverted(const std::vector<std::uint8_t> &data, const MdlConvertReport &report)
		{
			const std::string problem = ValidateV10Model(data);
			if (!problem.empty())
				throw std::runtime_error("converted model failed validation: " + problem);

			// Re-parse with the viewer's own loader: it must see the same
			// amount of geometry and every texture.
			StudioModel probe;
			try
			{
				probe.Load(data);
			}
			catch (const std::exception &ex)
			{
				throw std::runtime_error(std::string("converted model does not load: ") + ex.what());
			}
			int triangles = 0;
			int vertices = 0;
			int normals = 0;
			for (const auto &part : probe.BodyParts())
			{
				for (const auto &sub : part.models)
				{
					vertices += static_cast<int>(sub.rawVerts.size());
					normals += static_cast<int>(sub.rawNorms.size());
					for (const auto &mesh : sub.meshes)
						triangles += static_cast<int>(mesh.triangles.size());
				}
			}
			if (triangles != report.triangles || vertices != report.vertices ||
				normals != report.normals)
			{
				throw std::runtime_error("converted model reads back with " +
					std::to_string(triangles) + " triangles / " + std::to_string(vertices) +
					" vertices / " + std::to_string(normals) + " normals instead of " +
					std::to_string(report.triangles) + " / " + std::to_string(report.vertices) +
					" / " + std::to_string(report.normals));
			}
			if (static_cast<int>(probe.Textures().size()) != report.textures)
				throw std::runtime_error("converted model lost textures (" +
					std::to_string(probe.Textures().size()) + " of " +
					std::to_string(report.textures) + " visible)");
		}
	}

	bool IsModel2026(const std::vector<std::uint8_t> &data)
	{
		return data.size() >= 8 && std::memcmp(data.data(), "IDST", 4) == 0 &&
			ReadI32(data, 4) == 2026;
	}

	std::vector<std::uint8_t> ConvertModel2026ToV10(const StudioModel &model,
		const std::vector<std::uint8_t> &source, const MdlConvertSettings &settings,
		MdlConvertReport *reportPointer)
	{
		MdlConvertReport report;
		const Header h = ReadHeader(source);
		if (h.version == 10)
			throw std::runtime_error("model is already in the classic v10 format");
		if (h.version == 20 || h.version == 21)
			throw std::runtime_error("model is ICE-encrypted; decrypt it first");
		if (h.version != 2026)
			throw std::runtime_error("only 2026 models can be converted (this one is version " +
				std::to_string(h.version) + ")");
		if (!model.IsValid())
			throw std::runtime_error("model did not parse");
		if (model.BodyParts().empty())
			throw std::runtime_error("model has no body parts");

		const std::vector<StudioModel::Bone> &bones = model.Bones();
		const int boneCount = static_cast<int>(bones.size());
		const std::vector<std::array<float, 16>> &inverseRest = model.RestInverseMatrices();
		if (boneCount <= 0 || static_cast<int>(inverseRest.size()) != boneCount)
			throw std::runtime_error("model skeleton could not be read");

		for (const auto &sequence : model.Sequences())
		{
			if (sequence.numframes > 0 && sequence.externalGroup == 0 && sequence.animStride != 12)
				throw std::runtime_error("the model's own animation records use 32-bit offsets, "
					"which the classic format cannot store");
		}

		// ---- layout sanity: the front block (skeleton, sequences, attachments,
		// sounds, transitions) is copied verbatim, so all its pointers must
		// live inside it.
		const std::int64_t boneIndex = h.boneIndex;
		const std::int64_t bodyPartIndex = h.bodyPartIndex;
		if (boneIndex < static_cast<std::int64_t>(kV10HeaderSize) || boneIndex >= bodyPartIndex ||
			bodyPartIndex > static_cast<std::int64_t>(source.size()))
		{
			throw std::runtime_error("unexpected model layout (skeleton at " +
				std::to_string(boneIndex) + ", body parts at " + std::to_string(bodyPartIndex) + ")");
		}
		const std::int64_t shift = static_cast<std::int64_t>(kV10HeaderSize) - boneIndex;

		struct FrontField
		{
			std::int32_t value;
			const char *name;
		};
		const FrontField frontFields[] = {
			{ h.boneIndex, "bone table" }, { h.boneControllerIndex, "bone controller table" },
			{ h.hitboxIndex, "hitbox table" }, { h.seqIndex, "sequence table" },
			{ h.seqGroupIndex, "sequence group table" }, { h.attachmentIndex, "attachment table" },
			{ h.soundIndex, "sound table" }, { h.soundGroupIndex, "sound group table" },
			{ h.transitionIndex, "transition table" } };
		for (const auto &field : frontFields)
		{
			if (field.value != 0 && (field.value < boneIndex || field.value > bodyPartIndex))
				throw std::runtime_error(std::string("the model's ") + field.name +
					" lies outside the skeleton/animation block; this layout is not supported");
		}
		if (h.textureIndex < bodyPartIndex || h.skinIndex < bodyPartIndex ||
			h.textureDataIndex < bodyPartIndex)
		{
			throw std::runtime_error("unexpected model layout (textures or skin before the body parts)");
		}

		// ---- skin table, verbatim (all families, so the body groups survive).
		const std::int64_t skinCount = static_cast<std::int64_t>(h.numSkinRef) * h.numSkinFamilies;
		std::vector<std::int16_t> skin;
		if (skinCount > 0)
		{
			RequireRange(source, h.skinIndex, skinCount * 2, "skin table");
			skin.resize(static_cast<std::size_t>(skinCount));
			for (std::int64_t i = 0; i < skinCount; ++i)
			{
				const std::uint16_t raw = static_cast<std::uint16_t>(
					source[static_cast<std::size_t>(h.skinIndex + i * 2)]) |
					(static_cast<std::uint16_t>(
						source[static_cast<std::size_t>(h.skinIndex + i * 2 + 1)]) << 8);
				skin[static_cast<std::size_t>(i)] = std::bit_cast<std::int16_t>(raw);
				if (skin[static_cast<std::size_t>(i)] < 0 ||
					skin[static_cast<std::size_t>(i)] >= h.numTextures)
					throw std::runtime_error("the skin table references a texture that does not exist");
			}
		}
		std::vector<int> skinFamily;
		for (std::int64_t i = 0; i < h.numSkinRef && i < skinCount; ++i)
			skinFamily.push_back(skin[static_cast<std::size_t>(i)]);

		// ---- textures.
		std::vector<OutTexture> textures = ConvertTextures(model, settings, report);

		// ---- geometry, one output model per source model.
		std::vector<std::vector<OutModel>> outBodyParts;
		outBodyParts.reserve(model.BodyParts().size());
		for (const auto &part : model.BodyParts())
		{
			std::vector<OutModel> row;
			row.reserve(part.models.size());
			for (const auto &sub : part.models)
			{
				OutModel outModel = ConvertSubModel(sub, inverseRest, skinFamily, textures,
					boneCount, report);
				row.push_back(std::move(outModel));
			}
			outBodyParts.push_back(std::move(row));
		}
		report.bones = boneCount;
		report.bodyParts = static_cast<int>(model.BodyParts().size());
		for (const auto &row : outBodyParts)
			report.models += static_cast<int>(row.size());

		// ---- front block: copy and relocate the sequence pointers.
		if (h.numSeq > 0)
		{
			RequireRange(source, h.seqIndex, static_cast<std::int64_t>(h.numSeq) * kSequenceStride,
				"sequence table");
			if (h.seqIndex < boneIndex ||
				h.seqIndex + static_cast<std::int64_t>(h.numSeq) * static_cast<std::int64_t>(kSequenceStride) > bodyPartIndex)
				throw std::runtime_error("the sequence table lies outside the skeleton/animation block");
		}
		std::vector<std::uint8_t> front(
			source.begin() + static_cast<std::ptrdiff_t>(boneIndex),
			source.begin() + static_cast<std::ptrdiff_t>(bodyPartIndex));
		for (std::int32_t sequence = 0; sequence < h.numSeq; ++sequence)
		{
			const std::size_t base = static_cast<std::size_t>(h.seqIndex - boneIndex) +
				static_cast<std::size_t>(sequence) * kSequenceStride;
			RelocateFrontPointer(front, base + 52, boneIndex, bodyPartIndex, shift, "a sequence event pointer");
			RelocateFrontPointer(front, base + 64, boneIndex, bodyPartIndex, shift, "a sequence pivot pointer");
			RelocateFrontPointer(front, base + 88, boneIndex, bodyPartIndex, shift, "a sequence automove position pointer");
			RelocateFrontPointer(front, base + 92, boneIndex, bodyPartIndex, shift, "a sequence automove angle pointer");
			RelocateFrontPointer(front, base + 124, boneIndex, bodyPartIndex, shift, "a sequence animation pointer");
		}

		std::vector<std::uint8_t> out(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(kV10HeaderSize));
		out.insert(out.end(), front.begin(), front.end());

		// ---- body part and model headers.
		const std::size_t bodyPartOffset = out.size();
		const std::size_t numBodyParts = outBodyParts.size();
		std::size_t totalModels = 0;
		for (const auto &row : outBodyParts)
			totalModels += row.size();
		const std::size_t modelsOffset = bodyPartOffset + kBodyPartStride * numBodyParts;
		out.resize(modelsOffset + kModelStride * totalModels, 0);

		struct ModelSlot
		{
			std::size_t record = 0;
			const OutModel *model = nullptr;
		};
		std::vector<ModelSlot> modelSlots;
		modelSlots.reserve(totalModels);
		std::size_t modelPosition = modelsOffset;
		for (std::size_t i = 0; i < numBodyParts; ++i)
		{
			const std::size_t record = bodyPartOffset + kBodyPartStride * i;
			WriteName(out, record, 64, model.BodyParts()[i].name);
			WriteI32(out, record + 64, static_cast<std::int32_t>(outBodyParts[i].size()));
			WriteI32(out, record + 68, model.BodyParts()[i].base);
			WriteI32(out, record + 72, static_cast<std::int32_t>(modelPosition));
			for (const OutModel &outModel : outBodyParts[i])
			{
				modelSlots.push_back({ modelPosition, &outModel });
				modelPosition += kModelStride;
			}
		}

		// ---- per-model geometry.
		for (const ModelSlot &slot : modelSlots)
		{
			const OutModel &outModel = *slot.model;
			Pad4(out);
			if (outModel.meshes.empty())
			{
				const std::size_t here = out.size();
				WriteName(out, slot.record, 64, outModel.name);
				WriteI32(out, slot.record + 64, 0);                       // type
				WriteF32(out, slot.record + 68, outModel.radius);
				WriteI32(out, slot.record + 72, 0);                       // nummesh
				WriteI32(out, slot.record + 76, static_cast<std::int32_t>(here));
				WriteI32(out, slot.record + 80, 0);                       // numverts
				WriteI32(out, slot.record + 84, static_cast<std::int32_t>(here));
				WriteI32(out, slot.record + 88, static_cast<std::int32_t>(here));
				WriteI32(out, slot.record + 92, 0);                       // numnorms
				WriteI32(out, slot.record + 96, static_cast<std::int32_t>(here));
				WriteI32(out, slot.record + 100, static_cast<std::int32_t>(here));
				WriteI32(out, slot.record + 104, 0);                      // numgroups
				WriteI32(out, slot.record + 108, 0);                      // groupindex
				continue;
			}

			const std::size_t vertInfo = out.size();
			out.insert(out.end(), outModel.vertBones.begin(), outModel.vertBones.end());
			Pad4(out);
			const std::size_t normInfo = out.size();
			out.insert(out.end(), outModel.normBones.begin(), outModel.normBones.end());
			Pad4(out);
			const std::size_t verts = out.size();
			for (const auto &vertex : outModel.verts)
			{
				AppendF32(out, vertex.x);
				AppendF32(out, vertex.y);
				AppendF32(out, vertex.z);
			}
			const std::size_t norms = out.size();
			for (const auto &normal : outModel.norms)
			{
				AppendF32(out, normal.x);
				AppendF32(out, normal.y);
				AppendF32(out, normal.z);
			}
			const std::size_t meshTable = out.size();
			out.resize(out.size() + kMeshStride * outModel.meshes.size(), 0);
			for (std::size_t i = 0; i < outModel.meshes.size(); ++i)
			{
				const OutMesh &mesh = outModel.meshes[i];
				const std::size_t commands = out.size();
				out.insert(out.end(), mesh.commands.begin(), mesh.commands.end());
				const std::size_t record = meshTable + kMeshStride * i;
				WriteI32(out, record + 0, mesh.numTris);
				WriteI32(out, record + 4, static_cast<std::int32_t>(commands));
				WriteI32(out, record + 8, mesh.skinRef);
				WriteI32(out, record + 12, static_cast<std::int32_t>(outModel.norms.size()));
				WriteI32(out, record + 16, 0);
			}

			WriteName(out, slot.record, 64, outModel.name);
			WriteI32(out, slot.record + 64, 0);                           // type
			WriteF32(out, slot.record + 68, outModel.radius);
			WriteI32(out, slot.record + 72, static_cast<std::int32_t>(outModel.meshes.size()));
			WriteI32(out, slot.record + 76, static_cast<std::int32_t>(meshTable));
			WriteI32(out, slot.record + 80, static_cast<std::int32_t>(outModel.verts.size()));
			WriteI32(out, slot.record + 84, static_cast<std::int32_t>(vertInfo));
			WriteI32(out, slot.record + 88, static_cast<std::int32_t>(verts));
			WriteI32(out, slot.record + 92, static_cast<std::int32_t>(outModel.norms.size()));
			WriteI32(out, slot.record + 96, static_cast<std::int32_t>(normInfo));
			WriteI32(out, slot.record + 100, static_cast<std::int32_t>(norms));
			WriteI32(out, slot.record + 104, 0);                          // numgroups
			WriteI32(out, slot.record + 108, 0);                          // groupindex
		}

		// ---- texture table, skin table and texture pixels (same order/layout
		// as the reference converter).
		Pad4(out);
		const std::size_t textureOffset = out.size();
		out.resize(out.size() + kTextureStride * textures.size(), 0);
		const std::size_t skinOffset = out.size();
		for (const std::int16_t value : skin)
			AppendI16(out, value);
		Pad4(out);
		const std::size_t textureDataOffset = out.size();
		for (std::size_t i = 0; i < textures.size(); ++i)
		{
			const OutTexture &texture = textures[i];
			const std::size_t pixels = out.size();
			out.insert(out.end(), texture.image.indices.begin(), texture.image.indices.end());
			out.insert(out.end(), texture.image.palette.begin(), texture.image.palette.end());
			const std::size_t record = textureOffset + kTextureStride * i;
			WriteName(out, record, 64, texture.name);
			WriteI32(out, record + 64, texture.flags);
			WriteI32(out, record + 68, texture.width);
			WriteI32(out, record + 72, texture.height);
			WriteI32(out, record + 76, static_cast<std::int32_t>(pixels));
		}

		// ---- header patch.
		WriteI32(out, 4, 10);
		WriteI32(out, 144, h.boneIndex != 0 ? static_cast<std::int32_t>(h.boneIndex + shift) : 0);
		WriteI32(out, 152, h.boneControllerIndex != 0 ? static_cast<std::int32_t>(h.boneControllerIndex + shift) : 0);
		WriteI32(out, 160, h.hitboxIndex != 0 ? static_cast<std::int32_t>(h.hitboxIndex + shift) : 0);
		WriteI32(out, 168, h.seqIndex != 0 ? static_cast<std::int32_t>(h.seqIndex + shift) : 0);
		WriteI32(out, 176, h.seqGroupIndex != 0 ? static_cast<std::int32_t>(h.seqGroupIndex + shift) : 0);
		WriteI32(out, 216, h.attachmentIndex != 0 ? static_cast<std::int32_t>(h.attachmentIndex + shift) : 0);
		WriteI32(out, 224, h.soundIndex != 0 ? static_cast<std::int32_t>(h.soundIndex + shift) : 0);
		WriteI32(out, 232, h.soundGroupIndex != 0 ? static_cast<std::int32_t>(h.soundGroupIndex + shift) : 0);
		WriteI32(out, 240, h.transitionIndex != 0 ? static_cast<std::int32_t>(h.transitionIndex + shift) : 0);
		WriteI32(out, 208, static_cast<std::int32_t>(bodyPartOffset));
		WriteI32(out, 184, static_cast<std::int32_t>(textureOffset));
		WriteI32(out, 200, static_cast<std::int32_t>(skinOffset));
		WriteI32(out, 188, static_cast<std::int32_t>(textureDataOffset));
		WriteI32(out, 72, static_cast<std::int32_t>(out.size()));

		VerifyConverted(out, report);

		if (report.blendedVertices > 0)
		{
			report.warnings.push_back(std::to_string(report.blendedVertices) + " of " +
				std::to_string(report.sourceVertices) + " vertices were skinned to several bones (" +
				std::to_string(report.weakBlendVertices) +
				" with less than 75% on the main bone); the classic format keeps only the "
				"heaviest bone, so animated limbs may show seams");
		}
		if (reportPointer != nullptr)
			*reportPointer = std::move(report);
		return out;
	}

	// ------------------------------------------------------------------ checker

	std::string ValidateV10Model(const std::vector<std::uint8_t> &data)
	{
		if (data.size() < kV10HeaderSize)
			return "file is too small";
		if (std::memcmp(data.data(), "IDST", 4) != 0)
			return "missing IDST signature";
		if (ReadI32(data, 4) != 10)
			return "version is not 10";
		if (ReadI32(data, 72) != static_cast<std::int32_t>(data.size()))
			return "length field does not match the file size";

		const Header h = ReadHeader(data);

		if (h.numBones < 0 || h.numTextures < 0 || h.numBodyParts < 0)
			return "negative table size";

		const auto inRange = [&data](long long offset, long long size)
		{
			return offset >= 0 && size >= 0 && offset + size <= static_cast<long long>(data.size());
		};

		if (!inRange(h.boneIndex, static_cast<long long>(h.numBones) * kBoneStride))
			return "bone table outside the file";
		for (std::int32_t i = 0; i < h.numBones; ++i)
		{
			const std::int32_t parent = ReadI32(data, static_cast<std::size_t>(
				h.boneIndex + static_cast<std::int64_t>(i) * static_cast<std::int64_t>(kBoneStride) + 32));
			if (parent < -1 || parent >= i)
				return "bone " + std::to_string(i) + " has an invalid parent";
		}
		if (!inRange(h.hitboxIndex, static_cast<long long>(h.numHitboxes) * 32))
			return "hitbox table outside the file";
		if (!inRange(h.attachmentIndex, static_cast<long long>(h.numAttachments) * 88))
			return "attachment table outside the file";
		if (!inRange(h.boneControllerIndex, static_cast<long long>(h.numBoneControllers) * 24))
			return "bone controller table outside the file";
		if (!inRange(h.seqIndex, static_cast<long long>(h.numSeq) * static_cast<long long>(kSequenceStride)))
			return "sequence table outside the file";
		if (!inRange(h.textureIndex, static_cast<long long>(h.numTextures) * kTextureStride))
			return "texture table outside the file";

		const long long skinCount = static_cast<long long>(h.numSkinRef) * h.numSkinFamilies;
		if (!inRange(h.skinIndex, skinCount * 2))
			return "skin table outside the file";
		for (long long i = 0; i < skinCount; ++i)
		{
			const std::uint16_t raw = static_cast<std::uint16_t>(
				data[static_cast<std::size_t>(h.skinIndex + i * 2)]) |
				(static_cast<std::uint16_t>(data[static_cast<std::size_t>(h.skinIndex + i * 2 + 1)]) << 8);
			if (static_cast<std::int16_t>(raw) < 0 || static_cast<std::int16_t>(raw) >= h.numTextures)
				return "skin table entry is not a texture";
		}

		for (std::int32_t i = 0; i < h.numTextures; ++i)
		{
			const std::size_t record = static_cast<std::size_t>(h.textureIndex) +
				static_cast<std::size_t>(i) * kTextureStride;
			const std::int32_t width = ReadI32(data, record + 68);
			const std::int32_t height = ReadI32(data, record + 72);
			const std::int32_t pixels = ReadI32(data, record + 76);
			if (width <= 0 || height <= 0)
				return "texture " + std::to_string(i) + " has an invalid size";
			if (!inRange(pixels, static_cast<long long>(width) * height + 768))
				return "texture " + std::to_string(i) + " pixels outside the file";
		}

		const long long modelTableSize = static_cast<long long>(h.numBodyParts) * 76;
		if (!inRange(h.bodyPartIndex, modelTableSize))
			return "body part table outside the file";

		for (std::int32_t part = 0; part < h.numBodyParts; ++part)
		{
			const std::size_t record = static_cast<std::size_t>(h.bodyPartIndex) +
				static_cast<std::size_t>(part) * kBodyPartStride;
			const std::int32_t numModels = ReadI32(data, record + 64);
			const std::int32_t modelIndex = ReadI32(data, record + 72);
			if (numModels < 0)
				return "body part has a negative model count";
			if (!inRange(modelIndex, static_cast<long long>(numModels) * kModelStride))
				return "body part models outside the file";
			for (std::int32_t m = 0; m < numModels; ++m)
			{
				const std::size_t model = static_cast<std::size_t>(modelIndex) +
					static_cast<std::size_t>(m) * kModelStride;
				const std::int32_t numMesh = ReadI32(data, model + 72);
				const std::int32_t meshIndex = ReadI32(data, model + 76);
				const std::int32_t numVerts = ReadI32(data, model + 80);
				const std::int32_t vertInfo = ReadI32(data, model + 84);
				const std::int32_t vertIndex = ReadI32(data, model + 88);
				const std::int32_t numNorms = ReadI32(data, model + 92);
				const std::int32_t normInfo = ReadI32(data, model + 96);
				const std::int32_t normIndex = ReadI32(data, model + 100);
				if (numMesh < 0 || numVerts < 0 || numNorms < 0)
					return "model has a negative count";
				if (!inRange(vertInfo, numVerts) || !inRange(normInfo, numNorms))
					return "model bone index table outside the file";
				if (!inRange(vertIndex, static_cast<long long>(numVerts) * 12) ||
					!inRange(normIndex, static_cast<long long>(numNorms) * 12))
					return "model vertex table outside the file";
				if (!inRange(meshIndex, static_cast<long long>(numMesh) * kMeshStride))
					return "model mesh table outside the file";
				for (std::int32_t i = 0; i < numVerts; ++i)
				{
					if (data[static_cast<std::size_t>(vertInfo) + static_cast<std::size_t>(i)] >= h.numBones)
						return "model vertex bone index out of range";
				}
				for (std::int32_t i = 0; i < numNorms; ++i)
				{
					if (data[static_cast<std::size_t>(normInfo) + static_cast<std::size_t>(i)] >= h.numBones)
						return "model normal bone index out of range";
				}

				for (std::int32_t k = 0; k < numMesh; ++k)
				{
					const std::size_t mesh = static_cast<std::size_t>(meshIndex) +
						static_cast<std::size_t>(k) * kMeshStride;
					const std::int32_t numTris = ReadI32(data, mesh + 0);
					const std::int32_t commands = ReadI32(data, mesh + 4);
					const std::int32_t skinRef = ReadI32(data, mesh + 8);
					if (numTris < 0 || commands < 0 || skinRef < 0 || skinRef >= h.numSkinRef)
						return "mesh has invalid fields";
					std::size_t position = static_cast<std::size_t>(commands);
					long long decoded = 0;
					while (true)
					{
						if (position + 2 > data.size())
							return "triangle command stream runs past the end of the file";
						const std::int16_t count = static_cast<std::int16_t>(
							static_cast<std::uint16_t>(data[position]) |
							(static_cast<std::uint16_t>(data[position + 1]) << 8));
						position += 2;
						if (count == 0)
							break;
						const int vertices = std::abs(static_cast<int>(count));
						if (position + static_cast<std::size_t>(vertices) * 8 > data.size())
							return "triangle command stream runs past the end of the file";
						for (int c = 0; c < vertices; ++c)
						{
							const std::size_t corner = position + static_cast<std::size_t>(c) * 8;
							const std::int16_t vertex = static_cast<std::int16_t>(
								static_cast<std::uint16_t>(data[corner]) |
								(static_cast<std::uint16_t>(data[corner + 1]) << 8));
							const std::int16_t normal = static_cast<std::int16_t>(
								static_cast<std::uint16_t>(data[corner + 2]) |
								(static_cast<std::uint16_t>(data[corner + 3]) << 8));
							if (vertex < 0 || vertex >= numVerts || normal < 0 || normal >= numNorms)
								return "triangle references a vertex/normal that does not exist";
						}
						position += static_cast<std::size_t>(vertices) * 8;
						decoded += std::max(0, vertices - 2);
					}
					if (decoded != numTris)
						return "mesh triangle count does not match its command stream";
				}
			}
		}
		return {};
	}
}
