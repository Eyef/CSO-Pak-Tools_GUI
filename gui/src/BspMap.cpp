#include "BspMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "ByteCursor.h"

namespace cso_gui
{
	namespace
	{
		constexpr size_t kHeaderSize = 4 + BspMap::LumpCount * 8;
		constexpr size_t kFaceSize = 20;
		constexpr size_t kTexInfoSize = 40;
		constexpr size_t kPlaneSize = 20;
		constexpr size_t kModelSize = 64;
		constexpr size_t kNodeSize = 24;
		constexpr size_t kLeafSize = 28;
		constexpr size_t kClipNodeSize = 8;
		constexpr size_t kMaxMiptexName = 16;

		void RequireAligned(const BspMap::Lump &lump, size_t structSize, const char *what)
		{
			if (lump.length < 0 || lump.length % static_cast<int64_t>(structSize) != 0)
				throw std::runtime_error(std::string("bsp: bad ") + what + " lump size");
		}

		BspMap::Vec3 ReadVec3(ByteCursor &cursor)
		{
			BspMap::Vec3 v;
			v.x = cursor.F32();
			v.y = cursor.F32();
			v.z = cursor.F32();
			return v;
		}

		// Splits the entities lump into one key->value map per { ... }
		// block. Malformed pairs/blocks are skipped, never throws.
		std::vector<std::map<std::string, std::string>> ParseEntityKeyValues(
			const std::string &entities)
		{
			std::vector<std::map<std::string, std::string>> out;

			size_t pos = 0;
			const auto skipWs = [&]()
			{
				while (pos < entities.size() &&
					(entities[pos] == ' ' || entities[pos] == '\t' ||
						entities[pos] == '\r' || entities[pos] == '\n'))
					++pos;
			};
			const auto readQuoted = [&](std::string &s) -> bool
			{
				skipWs();
				if (pos >= entities.size() || entities[pos] != '"')
					return false;
				++pos;
				s.clear();
				while (pos < entities.size() && entities[pos] != '"' &&
					entities[pos] != '\n')
					s.push_back(entities[pos++]);
				if (pos >= entities.size() || entities[pos] != '"')
					return false;
				++pos;
				return true;
			};

			while (pos < entities.size())
			{
				skipWs();
				if (pos >= entities.size())
					break;
				if (entities[pos] != '{')
				{
					++pos;
					continue;
				}
				++pos;

				std::map<std::string, std::string> kv;
				std::string k, v;
				while (readQuoted(k))
				{
					if (!readQuoted(v))
						break;
					if (kv.find(k) == kv.end())
						kv.emplace(k, v);
					skipWs();
					if (pos < entities.size() && entities[pos] == '}')
						break;
				}
				skipWs();
				if (pos < entities.size() && entities[pos] == '}')
					++pos;
				if (!kv.empty())
					out.push_back(std::move(kv));
			}
			return out;
		}

		bool EndsWithNoCase(const std::string &s, const char *suffix);

		// Also hostage_entity (see below). naval_weapon (zs_voidzone1 / zs_lusia1): the entity origin is the gun's
		// aiming point well above the deck (z 136 over a pedestal at 16; 536 over
		// 384), but navalgun.mdl's own origin is at its base. Standing the model
		// on the entity origin made the cannon float. Lower it onto the highest
		// upward-facing surface right below the origin.
		void SnapNavalWeaponsToFloor(const BspMap &map, std::vector<BspPlacedEntity> &placed)
		{
			for (BspPlacedEntity &pe : placed)
			{
				// hostage_entity: the engine drops it to the floor when the level starts
				// (DROP_TO_FLOOR, up to 256 units). Mappers often leave the origin ~40
				// units up (cs_estate_child: 40-44 above the floor); cs_assault already
				// stands exactly on it.
				const bool isHostage = pe.classname == "hostage_entity";
				if (pe.classname != "naval_weapon" && !isHostage)
					continue;
				const float maxDrop = isHostage ? 256.0f : 400.0f;
				float best = -1e9f;
				for (const auto &M : map.Models())
					for (int i = M.firstFace; i < M.firstFace + M.numFaces; ++i)
					{
						const auto &F = map.Faces()[i];
						if (map.Planes()[F.plane].normal.z < 0.99f)
							continue;
						float mn[2] = { 1e9f, 1e9f }, mx[2] = { -1e9f, -1e9f }, z = 0;
						for (int e = 0; e < F.numEdges; ++e)
						{
							const int se = map.SurfEdges()[F.firstEdge + e];
							const auto &ed = map.Edges()[se < 0 ? -se : se];
							const auto &vt = map.Vertices()[ed.v[se < 0 ? 1 : 0]];
							mn[0] = std::min(mn[0], vt.x);
							mx[0] = std::max(mx[0], vt.x);
							mn[1] = std::min(mn[1], vt.y);
							mx[1] = std::max(mx[1], vt.y);
							z = vt.z;
						}
						if (pe.origin[0] >= mn[0] && pe.origin[0] <= mx[0] &&
							pe.origin[1] >= mn[1] && pe.origin[1] <= mx[1] &&
							z <= pe.origin[2] + 1.0f && z > pe.origin[2] - maxDrop && z > best)
							best = z;
					}
				if (best > -1e8f)
					pe.origin[2] = best;
			}
		}

		// zs_conspiracy's crane drop: a func_train "model_ex" crane (bg_crane02.mdl
		// with the light plank hung on its cables) and a plain brush func_train
		// (the dark plank, "crain_box") are started by the same multi_manager.
		// In game the dark plank is NOT on the map at the start (the hung plank
		// occupies its place); once the crane has lowered the plank, the dark
		// one takes over, falls to the ground and the crane flies off. The
		// editor/key position of the dark plank is its final spot on the floor.
		// Result: crane func_train targetnames and brush model indexes of the
		// dark planks.
		void ComputeCraneDrop(const std::vector<std::map<std::string, std::string>> &all,
			std::set<std::string> *craneNames, std::set<int> *boxModels)
		{
			for (const auto &mg : all)
			{
				const auto mc = mg.find("classname");
				if (mc == mg.end() || mc->second != "multi_manager")
					continue;
				std::string crane;
				std::vector<int> boxes;
				for (const auto &key : mg)
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tn = kv.find("targetname");
						if (cn == kv.end() || cn->second != "func_train" || tn == kv.end() ||
							tn->second != key.first)
							continue;
						const auto mex = kv.find("model_ex");
						const auto mdl = kv.find("model");
						if (mex != kv.end() && mex->second.find("crane") != std::string::npos)
							crane = tn->second;
						else if (mex == kv.end() && mdl != kv.end() && mdl->second.size() > 1 &&
							mdl->second[0] == '*' && kv.find("origin") != kv.end())
						{
							try
							{
								boxes.push_back(std::stoi(mdl->second.substr(1)));
							}
							catch (const std::exception &)
							{
							}
						}
					}
				if (crane.empty() || boxes.empty())
					continue;
				craneNames->insert(crane);
				for (int b : boxes)
					boxModels->insert(b);
			}
		}

		// Entities a cut scene switches OFF at its end: the target of an env_render
		// named "..._off" that makes it invisible (rendermode other than normal /
		// solid, renderamt 0 or missing), when no env_render named "..._on" turns the
		// same target on again (then the final state is unknown). zs_victor1's idle
		// Harrier jets: their afterburner flames (brushes "after_burner") and the red
		// "jet_glow" lights are removed together with the jets.
		std::set<std::string> ComputeSwitchedOffAfterScene(
			const std::vector<std::map<std::string, std::string>> &all)
		{
			// Only the scripts that also take an aircraft model away (cycler_mdl
			// aircraft -> null.mdl): their "..._off" renders are the flames, lights
			// and trails of those aircraft. Other maps' "..._off" scripts are not
			// verified against the game.
			std::set<std::string> aircraftSwitchers;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				const auto m0 = kv.find("_mdl0");
				const auto m1 = kv.find("_mdl1");
				if (cn == kv.end() || cn->second != "cycler_mdl" || tn == kv.end() ||
					m0 == kv.end() || m1 == kv.end() || !EndsWithNoCase(m1->second, "null.mdl"))
					continue;
				const std::string &m = m0->second;
				if (m.find("harrier") != std::string::npos || m.find("helicopter") != std::string::npos ||
					m.find("blackhawk") != std::string::npos || m.find("hind") != std::string::npos ||
					m.find("comanche") != std::string::npos)
					aircraftSwitchers.insert(tn->second);
			}
			std::set<std::string> sceneKeys;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				if (cn == kv.end() || cn->second != "multi_manager")
					continue;
				bool hasAircraft = false;
				for (const auto &p : kv)
					if (aircraftSwitchers.count(p.first) != 0)
						hasAircraft = true;
				if (hasAircraft)
					for (const auto &p : kv)
						sceneKeys.insert(p.first);
			}
			std::set<std::string> off, on;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tg = kv.find("target");
				const auto tn = kv.find("targetname");
				if (cn == kv.end() || cn->second != "env_render" || tg == kv.end() ||
					tn == kv.end() || tg->second.empty())
					continue;
				if (EndsWithNoCase(tn->second, "_on"))
					on.insert(tg->second);
				if (!EndsWithNoCase(tn->second, "_off") || sceneKeys.count(tn->second) == 0)
					continue;
				const auto rm = kv.find("rendermode");
				if (rm == kv.end() || rm->second == "0" || rm->second == "4")
					continue;
				const auto ra = kv.find("renderamt");
				bool zero = ra == kv.end();
				if (ra != kv.end())
				{
					try
					{
						zero = std::stoi(ra->second) <= 0;
					}
					catch (const std::exception &)
					{
					}
				}
				if (zero)
					off.insert(tg->second);
			}
			for (const std::string &name : on)
				off.erase(name);
			return off;
		}

		bool EndsWithNoCase(const std::string &s, const char *suffix)
		{
			const size_t n = std::strlen(suffix);
			if (s.size() < n)
				return false;
			for (size_t i = 0; i < n; ++i)
			{
				char a = s[s.size() - n + i];
				char b = suffix[i];
				if (a >= 'A' && a <= 'Z')
					a = static_cast<char>(a + 32);
				if (b >= 'A' && b <= 'Z')
					b = static_cast<char>(b + 32);
				if (a != b)
					return false;
			}
			return true;
		}

		bool IsBspSpecialTextureName(const std::string &name)
		{
			// Engine-invisible tool textures. Compared case-insensitively;
			// TEX_SPECIAL texinfo faces are skipped separately by callers.
			if (name.empty() || name.size() > 16)
				return false;
			char lower[17] = {};
			for (size_t i = 0; i < name.size(); ++i)
			{
				const char c = name[i];
				lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
			}
			const std::string_view view(lower, name.size());
			// NOTE: sky is deliberately NOT skipped -- without a skybox
			// renderer a hole in the world looks more broken than the
			// sky-textured faces (same choice as goldsrc-bsp-viewer).
			return view == "null" || view == "clip" || view == "aaatrigger" ||
				view == "hint" || view == "skip" || view == "origin" ||
				// CSO editor-only "no wall shot" marker (blue NW checker).
				view == "nowallshot";
		}

		// Collects a face's vertices (world space) through surfedges->edges.
		// Returns false when the face references anything out of range.
		bool GatherFaceVerts(const BspMap &map, const BspMap::Face &face,
			std::vector<BspMap::Vec3> &verts)
		{
			verts.clear();
			verts.reserve(face.numEdges);
			for (uint16_t e = 0; e < face.numEdges; ++e)
			{
				const size_t seIndex = static_cast<size_t>(face.firstEdge + e);
				if (seIndex >= map.SurfEdges().size())
					return false;
				const int32_t surfEdge = map.SurfEdges()[seIndex];
				const size_t edgeIndex = static_cast<size_t>(
					surfEdge >= 0 ? surfEdge : -surfEdge);
				if (edgeIndex >= map.Edges().size())
					return false;
				const BspMap::Edge &edge = map.Edges()[edgeIndex];
				const uint16_t vertIndex =
					surfEdge >= 0 ? edge.v[0] : edge.v[1];
				if (vertIndex >= map.Vertices().size())
					return false;
				verts.push_back(map.Vertices()[vertIndex]);
			}
			return verts.size() >= 3;
		}
	}

	void BspMap::Load(const std::vector<uint8_t> &data)
	{
		valid_ = false;
		version_ = 0;
		lumps_ = {};
		entities_.clear();
		entityRenders_.clear();
		placedEntities_.clear();
		vertices_.clear();
		edges_.clear();
		surfEdges_.clear();
		faces_.clear();
		texInfos_.clear();
		planes_.clear();
		textures_.clear();
		lighting_.clear();
		visibility_.clear();
		models_.clear();
		nodes_.clear();
		leaves_.clear();
		markSurfaces_.clear();
		clipNodes_.clear();

		if (data.size() < kHeaderSize)
			throw std::runtime_error("bsp: file too small for header");

		ByteCursor cursor(data);
		version_ = cursor.I32();
		if (version_ != kSupportedVersion)
			throw std::runtime_error("bsp: unsupported version " + std::to_string(version_));

		for (auto &lump : lumps_)
		{
			lump.offset = cursor.I32();
			lump.length = cursor.I32();
			if (lump.offset < 0 || lump.length < 0 ||
				static_cast<size_t>(lump.offset) + static_cast<size_t>(lump.length) > data.size())
				throw std::runtime_error("bsp: lump outside file");
		}

		// Entities: raw text block, parsed by the viewer stage.
		{
			const Lump &lump = lumps_[LumpEntities];
			if (lump.length > 0)
			{
				cursor.Seek(static_cast<size_t>(lump.offset));
				const uint8_t *bytes = cursor.Bytes(static_cast<size_t>(lump.length));
				entities_.assign(reinterpret_cast<const char *>(bytes),
					static_cast<size_t>(lump.length));
			}
		}

		// Vertices: 3 floats each.
		{
			const Lump &lump = lumps_[LumpVertices];
			RequireAligned(lump, 12, "vertices");
			cursor.Seek(static_cast<size_t>(lump.offset));
			vertices_.reserve(static_cast<size_t>(lump.length) / 12);
			for (int64_t left = lump.length; left > 0; left -= 12)
				vertices_.push_back(ReadVec3(cursor));
		}

		// Edges: 2 x uint16 vertex indices.
		{
			const Lump &lump = lumps_[LumpEdges];
			RequireAligned(lump, 4, "edges");
			cursor.Seek(static_cast<size_t>(lump.offset));
			edges_.reserve(static_cast<size_t>(lump.length) / 4);
			for (int64_t left = lump.length; left > 0; left -= 4)
			{
				Edge edge;
				const uint8_t *bytes = cursor.Bytes(4);
				edge.v[0] = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
				edge.v[1] = static_cast<uint16_t>(bytes[2] | (bytes[3] << 8));
				edges_.push_back(edge);
			}
		}

		// Surfedges: signed int32 edge indices (negative = reversed edge).
		{
			const Lump &lump = lumps_[LumpSurfEdges];
			RequireAligned(lump, 4, "surfedges");
			cursor.Seek(static_cast<size_t>(lump.offset));
			surfEdges_.reserve(static_cast<size_t>(lump.length) / 4);
			for (int64_t left = lump.length; left > 0; left -= 4)
				surfEdges_.push_back(cursor.I32());
		}

		// Faces.
		{
			const Lump &lump = lumps_[LumpFaces];
			RequireAligned(lump, kFaceSize, "faces");
			cursor.Seek(static_cast<size_t>(lump.offset));
			faces_.reserve(static_cast<size_t>(lump.length) / kFaceSize);
			for (int64_t left = lump.length; left > 0; left -= kFaceSize)
			{
				Face face;
				const uint8_t *head = cursor.Bytes(12);
				face.plane = static_cast<uint16_t>(head[0] | (head[1] << 8));
				face.planeSide = static_cast<uint16_t>(head[2] | (head[3] << 8));
				std::memcpy(&face.firstEdge, head + 4, 4);
				face.numEdges = static_cast<uint16_t>(head[8] | (head[9] << 8));
				face.texInfo = static_cast<uint16_t>(head[10] | (head[11] << 8));
				const uint8_t *tail = cursor.Bytes(8);
				std::memcpy(face.styles, tail, 4);
				std::memcpy(&face.lightOffset, tail + 4, 4);
				faces_.push_back(face);
			}
		}

		// Texinfos.
		{
			const Lump &lump = lumps_[LumpTexInfo];
			RequireAligned(lump, kTexInfoSize, "texinfo");
			cursor.Seek(static_cast<size_t>(lump.offset));
			texInfos_.reserve(static_cast<size_t>(lump.length) / kTexInfoSize);
			for (int64_t left = lump.length; left > 0; left -= kTexInfoSize)
			{
				TexInfo info;
				info.s = ReadVec3(cursor);
				info.shiftS = cursor.F32();
				info.t = ReadVec3(cursor);
				info.shiftT = cursor.F32();
				info.miptex = cursor.I32();
				info.flags = cursor.I32();
				texInfos_.push_back(info);
			}
		}

		// Planes.
		{
			const Lump &lump = lumps_[LumpPlanes];
			RequireAligned(lump, kPlaneSize, "planes");
			cursor.Seek(static_cast<size_t>(lump.offset));
			planes_.reserve(static_cast<size_t>(lump.length) / kPlaneSize);
			for (int64_t left = lump.length; left > 0; left -= kPlaneSize)
			{
				Plane plane;
				plane.normal = ReadVec3(cursor);
				plane.dist = cursor.F32();
				plane.type = cursor.I32();
				planes_.push_back(plane);
			}
		}

		// Texture directory: int32 count + per-texture offsets, -1 = empty.
		// Indexing must be preserved (texinfo.miptex is a directory slot),
		// so empty slots stay in the vector with a null image.
		{
			const Lump &lump = lumps_[LumpTextures];
			if (lump.length > 0)
			{
				cursor.Seek(static_cast<size_t>(lump.offset));
				const int32_t count = cursor.I32();
				if (count < 0 || static_cast<size_t>(count) * 4 + 4 > static_cast<size_t>(lump.length))
					throw std::runtime_error("bsp: bad texture directory");
				textures_.resize(static_cast<size_t>(count));
				for (int32_t i = 0; i < count; ++i)
				{
					const int32_t dirOffset = cursor.I32();
					if (dirOffset < 0)
						continue;  // Empty slot, keep null image.

					const size_t base = static_cast<size_t>(lump.offset) +
						static_cast<size_t>(dirOffset);
					// name[16] + w,h + 4 mip offsets = 40 bytes minimum.
					if (base + 40 > data.size())
						continue;

					Miptex &tex = textures_[static_cast<size_t>(i)];
					char rawName[kMaxMiptexName + 1] = {};
					std::memcpy(rawName, data.data() + base, kMaxMiptexName);
					tex.name = rawName;
					std::memcpy(&tex.width, data.data() + base + 16, 4);
					std::memcpy(&tex.height, data.data() + base + 20, 4);
					if (tex.width <= 0 || tex.height <= 0)
						continue;

					int32_t mipOffsets[4];
					std::memcpy(mipOffsets, data.data() + base + 24, sizeof(mipOffsets));
					if (mipOffsets[0] == 0)
						continue;  // External texture, lives in a .wad.

					// Embedded WAD3-style miptex: mip0 pixels, then palette
					// (2-byte size + 768 bytes) after the smallest mip.
					const size_t pixelBytes =
						static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height);
					const size_t pixelsBase = base + static_cast<size_t>(mipOffsets[0]);
					const size_t mip3Bytes =
						static_cast<size_t>(tex.width / 8) * static_cast<size_t>(tex.height / 8);
					const size_t paletteBase =
						base + static_cast<size_t>(mipOffsets[3]) + mip3Bytes + 2;
					if (pixelsBase + pixelBytes > data.size() ||
						paletteBase + 768 > data.size())
						continue;

					// GoldSource cutout convention (same as WAD miptex and
					// studiomdl masked textures): a '{'-prefixed texture treats
					// palette index 255 as transparent. The viewer shader
					// discards alpha < 0.5, matching the model viewer.
					const bool cutout = !tex.name.empty() && tex.name.front() == '{';
					QImage image(tex.width, tex.height,
						cutout ? QImage::Format_ARGB32 : QImage::Format_RGB888);
					const uint8_t *pixels = data.data() + pixelsBase;
					const uint8_t *palette = data.data() + paletteBase;
					for (int32_t y = 0; y < tex.height; ++y)
					{
						if (!cutout)
						{
							uint8_t *scan = image.scanLine(y);
							for (int32_t x = 0; x < tex.width; ++x)
							{
								const uint8_t index = pixels[static_cast<size_t>(y) * tex.width + x];
								scan[x * 3 + 0] = palette[index * 3 + 0];
								scan[x * 3 + 1] = palette[index * 3 + 1];
								scan[x * 3 + 2] = palette[index * 3 + 2];
							}
						}
						else
						{
							uint32_t *scan = reinterpret_cast<uint32_t *>(image.scanLine(y));
							for (int32_t x = 0; x < tex.width; ++x)
							{
								const uint8_t index = pixels[static_cast<size_t>(y) * tex.width + x];
								if (index == 255)
									scan[x] = 0;
								else
									scan[x] = 0xFF000000u |
										(static_cast<uint32_t>(palette[index * 3 + 0]) << 16) |
										(static_cast<uint32_t>(palette[index * 3 + 1]) << 8) |
										static_cast<uint32_t>(palette[index * 3 + 2]);
							}
						}
					}
					tex.image = std::move(image);
				}
			}
		}

		// Lighting + visibility: raw blobs, interpreted by the viewer stage.
		for (const LumpIndex index : { LumpLighting, LumpVisibility })
		{
			const Lump &lump = lumps_[index];
			if (lump.length <= 0)
				continue;
			cursor.Seek(static_cast<size_t>(lump.offset));
			const uint8_t *bytes = cursor.Bytes(static_cast<size_t>(lump.length));
			if (index == LumpLighting)
				lighting_.assign(bytes, bytes + lump.length);
			else
				visibility_.assign(bytes, bytes + lump.length);
		}

		// Models.
		{
			const Lump &lump = lumps_[LumpModels];
			RequireAligned(lump, kModelSize, "models");
			cursor.Seek(static_cast<size_t>(lump.offset));
			models_.reserve(static_cast<size_t>(lump.length) / kModelSize);
			for (int64_t left = lump.length; left > 0; left -= kModelSize)
			{
				Model model;
				model.mins = ReadVec3(cursor);
				model.maxs = ReadVec3(cursor);
				model.origin = ReadVec3(cursor);
				for (int32_t &headNode : model.headNodes)
					headNode = cursor.I32();
				model.visLeafs = cursor.I32();
				model.firstFace = cursor.I32();
				model.numFaces = cursor.I32();
				models_.push_back(model);
			}
		}

		// Nodes.
		{
			const Lump &lump = lumps_[LumpNodes];
			RequireAligned(lump, kNodeSize, "nodes");
			cursor.Seek(static_cast<size_t>(lump.offset));
			nodes_.reserve(static_cast<size_t>(lump.length) / kNodeSize);
			for (int64_t left = lump.length; left > 0; left -= kNodeSize)
			{
				Node node;
				node.plane = cursor.I32();
				for (int16_t &child : node.children)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					child = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				for (int16_t &value : node.mins)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					value = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				for (int16_t &value : node.maxs)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					value = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				const uint8_t *tail = cursor.Bytes(4);
				node.firstFace = static_cast<uint16_t>(tail[0] | (tail[1] << 8));
				node.numFaces = static_cast<uint16_t>(tail[2] | (tail[3] << 8));
				nodes_.push_back(node);
			}
		}

		// Leaves: 28 bytes (the ambient light levels make it longer than nodes).
		{
			const Lump &lump = lumps_[LumpLeaves];
			RequireAligned(lump, kLeafSize, "leaves");
			cursor.Seek(static_cast<size_t>(lump.offset));
			leaves_.reserve(static_cast<size_t>(lump.length) / kLeafSize);
			for (int64_t left = lump.length; left > 0; left -= kLeafSize)
			{
				Leaf leaf;
				leaf.contents = cursor.I32();
				leaf.visOffset = cursor.I32();
				for (int16_t &value : leaf.mins)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					value = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				for (int16_t &value : leaf.maxs)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					value = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				const uint8_t *middle = cursor.Bytes(4);
				leaf.firstMarkSurface = static_cast<uint16_t>(middle[0] | (middle[1] << 8));
				leaf.numMarkSurfaces = static_cast<uint16_t>(middle[2] | (middle[3] << 8));
				const uint8_t *ambient = cursor.Bytes(4);
				std::memcpy(leaf.ambient, ambient, 4);
				leaves_.push_back(leaf);
			}
		}

		// Marksurfaces: uint16 face indices.
		{
			const Lump &lump = lumps_[LumpMarkSurfaces];
			RequireAligned(lump, 2, "marksurfaces");
			cursor.Seek(static_cast<size_t>(lump.offset));
			markSurfaces_.reserve(static_cast<size_t>(lump.length) / 2);
			for (int64_t left = lump.length; left > 0; left -= 2)
			{
				const uint8_t *bytes = cursor.Bytes(2);
				markSurfaces_.push_back(static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)));
			}
		}

		// Clipnodes (collision only, kept for completeness).
		{
			const Lump &lump = lumps_[LumpClipNodes];
			RequireAligned(lump, kClipNodeSize, "clipnodes");
			cursor.Seek(static_cast<size_t>(lump.offset));
			clipNodes_.reserve(static_cast<size_t>(lump.length) / kClipNodeSize);
			for (int64_t left = lump.length; left > 0; left -= kClipNodeSize)
			{
				ClipNode node;
				node.plane = cursor.I32();
				for (int16_t &child : node.children)
				{
					const uint8_t *bytes = cursor.Bytes(2);
					child = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
				}
				clipNodes_.push_back(node);
			}
		}

		valid_ = true;

		// Brush-entity render properties (rendermode/origin/...). Never
		// throws: a malformed entities lump just yields fewer entries.
		try
		{
			entityRenders_ = ParseBspEntityRenders(entities_);
			placedEntities_ = ParseBspPlacedEntities(entities_, false, &models_);
			SnapNavalWeaponsToFloor(*this, placedEntities_);
		}
		catch (const std::exception &)
		{
			entityRenders_.clear();
			placedEntities_.clear();
		}
	}

	std::map<int, BspMap::EntityRender> ParseBspEntityRenders(const std::string &entities,
		bool afterSwitch)
	{
		std::map<int, BspMap::EntityRender> out;

		const auto allKv = ParseEntityKeyValues(entities);
		// env_render entities (target = brush entity name) switch the render
		// properties of their target when triggered (zs_carlito2's guide arrows:
		// func_illusionary rendermode 4 without renderamt, which a trigger turns
		// into a glowing additive sign). Keep name -> env_render for the arrows.
		const std::set<std::string> switchedOffRenders = ComputeSwitchedOffAfterScene(allKv);
		std::map<std::string, const std::map<std::string, std::string> *> envRenders;
		for (const auto &kv : allKv)
		{
			const auto cn = kv.find("classname");
			const auto tg = kv.find("target");
			if (cn != kv.end() && cn->second == "env_render" && tg != kv.end() &&
				!tg->second.empty())
				envRenders.emplace(tg->second, &kv);
		}
		// Targets that a later "..._off" env_render switches off again (zs_victor1's
		// laser sights: "..._r_on" in the cut scene, "..._r_off" at its end): after
		// the scene they are hidden, so the "_on" state must not be applied.
		std::set<std::string> switchedOffAgain;
		for (const auto &kv : allKv)
		{
			const auto cn = kv.find("classname");
			const auto tg = kv.find("target");
			const auto tn = kv.find("targetname");
			if (cn != kv.end() && cn->second == "env_render" && tg != kv.end() &&
				tn != kv.end() && EndsWithNoCase(tn->second, "_off"))
				switchedOffAgain.insert(tg->second);
		}

		// Platform swap scripts (zs_evacuate1's tilting bridge): one multi_manager
		// switches a brush entity off with an "..._off" env_render (renderamt 0),
		// turns a func_wall_toggle on and breaks a few func_breakables. After the
		// switch the brush target is gone, the toggles it names are flipped and the
		// breakables it names are broken. Needs both an "_off" env_render on a
		// brush target and a func_wall_toggle in the same manager.
		std::set<std::string> swapHidden, swapKeys;
		{
			std::set<std::string> toggleNames, brushNames;
			for (const auto &kv : allKv)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				if (cn == kv.end() || tn == kv.end())
					continue;
				if (cn->second == "func_wall_toggle")
					toggleNames.insert(tn->second);
				const auto mm = kv.find("model");
				// Only rotating platforms (the swing-down bridge); other brush
				// kinds switched off by a scene are not verified against the game.
				if (cn->second == "func_door_rotating" && mm != kv.end() &&
					mm->second.size() > 1 && mm->second[0] == '*')
					brushNames.insert(tn->second);
			}
			std::map<std::string, std::string> offTarget;  // env_render name -> brush target
			for (const auto &kv : allKv)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				const auto tg = kv.find("target");
				const auto rm = kv.find("rendermode");
				const auto ra = kv.find("renderamt");
				if (cn == kv.end() || cn->second != "env_render" || tn == kv.end() ||
					tg == kv.end() || rm == kv.end() ||
					(rm->second != "2" && rm->second != "5") ||
					!EndsWithNoCase(tn->second, "_off") || brushNames.count(tg->second) == 0)
					continue;
				if (ra != kv.end() && ra->second != "0")
					continue;
				offTarget[tn->second] = tg->second;
			}
			for (const auto &kv : allKv)
			{
				const auto cn = kv.find("classname");
				if (cn == kv.end() || cn->second != "multi_manager")
					continue;
				bool hasToggle = false;
				std::vector<std::string> offs;
				for (const auto &p : kv)
				{
					if (toggleNames.count(p.first) != 0)
						hasToggle = true;
					const auto ot = offTarget.find(p.first);
					if (ot != offTarget.end())
						offs.push_back(ot->second);
				}
				if (!hasToggle || offs.empty())
					continue;
				for (const auto &p : kv)
					swapKeys.insert(p.first);
				for (const auto &o : offs)
					swapHidden.insert(o);
			}
		}

		for (const auto &kv : allKv)
		{
			const auto mit = kv.find("model");
			if (mit == kv.end() || mit->second.size() < 2 || mit->second[0] != '*')
				continue;
			int index = -1;
			try
			{
				index = std::stoi(mit->second.substr(1));
			}
			catch (const std::exception &)
			{
				continue;
			}
			if (index < 0)
				continue;

			BspMap::EntityRender er;
			er.modelIndex = index;
			const auto cit = kv.find("classname");
			if (cit != kv.end())
				er.classname = cit->second;
			const auto oit = kv.find("origin");
			if (oit != kv.end())
			{
				float x = 0, y = 0, z = 0;
				if (std::sscanf(oit->second.c_str(), "%f %f %f", &x, &y, &z) == 3)
				{
					er.origin[0] = x;
					er.origin[1] = y;
					er.origin[2] = z;
				}
			}
			const auto mit2 = kv.find("rendermode");
			if (mit2 != kv.end())
			{
				try
				{
					er.renderMode = std::stoi(mit2->second);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto ait = kv.find("renderamt");
			if (ait != kv.end())
			{
				try
				{
					er.renderAmt = std::clamp(std::stoi(ait->second), 0, 255);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto rfx = kv.find("renderfx");
			if (rfx != kv.end())
			{
				try
				{
					er.renderFx = std::stoi(rfx->second);
				}
				catch (const std::exception &)
				{
				}
			}
			if (er.classname == "func_rotating")
			{
				const auto spd = kv.find("speed");
				const auto spf = kv.find("spawnflags");
				try
				{
					if (spd != kv.end())
						er.rotSpeed = std::stof(spd->second);
					if (spf != kv.end())
						er.spawnFlags = std::stoi(spf->second);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto rit = kv.find("rendercolor");
			if (rit != kv.end())
			{
				float r = 0, g = 0, b = 0;
				if (std::sscanf(rit->second.c_str(), "%f %f %f", &r, &g, &b) == 3)
				{
					er.renderColor[0] = std::clamp(r, 0.0f, 255.0f);
					er.renderColor[1] = std::clamp(g, 0.0f, 255.0f);
					er.renderColor[2] = std::clamp(b, 0.0f, 255.0f);
					er.hasRenderColor = true;
				}
			}
			// GoldSrc's renderamt defaults to 0, not 255: a brush entity with a
			// non-normal rendermode and NO renderamt key is invisible in game
			// (mappers use this for invisible blockers, e.g. zs_evacuate3's
			// func_illusionary with the blue grid_blue texture). Rendermode 4
			// (alpha-tested solid) ignores the amount, so it keeps 255.
			if (kv.find("renderamt") == kv.end() && er.renderMode != 0 &&
				er.renderMode != 4)
				er.renderAmt = 0;
			// func_wall_toggle with the "start off" flag (1) is neither drawn nor solid
			// until a trigger toggles it (zs_evacuate1's tilted bridge half); a
			// swap script flips it. A toggle that is on at the start and flipped
			// by the script is gone afterwards.
			{
				const auto tnT = kv.find("targetname");
				const bool keyed = tnT != kv.end() && swapKeys.count(tnT->second) != 0;
				if (er.classname == "func_wall_toggle" && keyed)
				{
					int flags = 0;
					const auto sf = kv.find("spawnflags");
					if (sf != kv.end())
					{
						try
						{
							flags = std::stoi(sf->second);
						}
						catch (const std::exception &)
						{
						}
					}
					const bool startOff = (flags & 1) != 0;
					const bool visibleNow = startOff ? (afterSwitch && keyed)
						: !(afterSwitch && keyed);
					if (!visibleNow)
					{
						er.renderMode = 2;
						er.renderAmt = 0;
						out[index] = er;
						continue;
					}
				}
				else if (afterSwitch && tnT != kv.end() &&
					(swapHidden.count(tnT->second) != 0 ||
						(er.classname == "func_breakable" && keyed)))
				{
					er.renderMode = 2;
					er.renderAmt = 0;
					out[index] = er;
					continue;
				}
			}
			// Switched off again by the scene's own "..._off" env_render.
			if (afterSwitch)
			{
				const auto tnOff = kv.find("targetname");
				if (tnOff != kv.end() && switchedOffRenders.count(tnOff->second) != 0)
				{
					er.renderMode = 2;
					er.renderAmt = 0;
					out[index] = er;
					continue;
				}
			}
			// A rendermode-4 entity with no renderamt that an env_render later
			// turns into a visible texture/additive sign: show that state, not
			// the opaque solid one (the black background of a non-masked sign
			// texture only disappears in the additive mode).
			// After the cut scene (afterSwitch) the same applies to hidden
			// rendermode 2/5 signs that an env_render named "..._on" turns on
			// (zs_victor2's yellow captain icon).
			// A second copy of zs_victor2 spells the hidden state out ("renderamt"
			// "0" on the func_rotating pictogram) instead of leaving the key out:
			// same switch-on by the "..._on" env_render.
			// A defence target (zbstarget) that starts invisible and is switched on by
			// an "..._on" env_render with the solid rendermode 4 (zs_conspiracy's
			// container "defence_target": the Blackhawk drops it and it stays).
			if (afterSwitch && er.classname == "zbstarget" && er.renderMode != 0 &&
				er.renderMode != 4 && er.renderAmt <= 0)
			{
				const auto tnZ = kv.find("targetname");
				if (tnZ != kv.end())
				{
					const auto envZ = envRenders.find(tnZ->second);
					if (envZ != envRenders.end())
					{
						const auto &ekv = *envZ->second;
						const auto en = ekv.find("targetname");
						const auto em = ekv.find("rendermode");
						if (en != ekv.end() && EndsWithNoCase(en->second, "_on") &&
							em != ekv.end() && em->second == "4")
						{
							er.renderMode = 4;
							er.renderAmt = 255;
							out[index] = er;
							continue;
						}
					}
				}
			}
			const bool envSwitchable = er.renderMode == 4 ||
				(afterSwitch && er.renderMode != 0);
			// Limited to the pulsing pictogram (func_rotating, renderfx 2): the same
			// pattern on other maps' signs is not verified against the game.
			const bool explicitHidden = afterSwitch && er.renderMode != 0 &&
				er.renderMode != 4 && er.renderAmt <= 0 &&
				((er.classname == "func_rotating" && er.renderFx == 2) ||
					(kv.find("targetname") != kv.end() &&
						kv.find("targetname")->second == "captain_jo_sign"));
			if (envSwitchable && (kv.find("renderamt") == kv.end() || explicitHidden))
			{
				const auto tn = kv.find("targetname");
				if (tn != kv.end())
				{
					const auto env = envRenders.find(tn->second);
					if (env != envRenders.end())
					{
						const auto &ekv = *env->second;
						const auto ename = ekv.find("targetname");
						if (er.renderMode != 4 &&
							(ename == ekv.end() || !EndsWithNoCase(ename->second, "_on") ||
								switchedOffAgain.count(tn->second) != 0))
						{
							out[index] = er;
							continue;
						}
						int emode = 0, eamt = 0;
						const auto em = ekv.find("rendermode");
						const auto ea = ekv.find("renderamt");
						try
						{
							if (em != ekv.end())
								emode = std::stoi(em->second);
							if (ea != ekv.end())
								eamt = std::clamp(std::stoi(ea->second), 0, 255);
						}
						catch (const std::exception &)
						{
						}
						int eflags = 0;
						const auto ef = ekv.find("spawnflags");
						if (ef != ekv.end())
						{
							try
							{
								eflags = std::stoi(ef->second);
							}
							catch (const std::exception &)
							{
							}
						}
						// spawnflags: 2 = don't change renderamt, 4 = don't change rendermode.
						if ((eflags & 4) == 0 && (emode == 2 || emode == 5) &&
							((eflags & 2) != 0 || eamt > 0))
						{
							er.renderMode = emode;
							er.renderAmt = (eflags & 2) != 0 ? 255 : eamt;
						}
					}
				}
			}
			out[index] = er;
		}
		return out;
	}

	void BspMap::SetPlacedState(bool afterSwitch)
	{
		placedAfterSwitch_ = afterSwitch;
		try
		{
			placedEntities_ = ParseBspPlacedEntities(entities_, afterSwitch, &models_);
			SnapNavalWeaponsToFloor(*this, placedEntities_);
			entityRenders_ = ParseBspEntityRenders(entities_, afterSwitch);
		}
		catch (const std::exception &)
		{
			placedEntities_.clear();
		}
	}

	std::vector<BspPlacedEntity> ParseBspPlacedEntities(const std::string &entities, bool afterSwitch,
		const std::vector<BspMap::Model> *models)
	{
		std::vector<BspPlacedEntity> out;

		const auto allKv = ParseEntityKeyValues(entities);
		// After the switch, a plain prop standing exactly where a switcher
		// "appears" (start model null.mdl, end model real) is the intact version
		// that the script removes: zs_obstacle1's white dishes (cycler_sprite
		// brk_radar_*_r) sit on the origin of the burnt cycler_mdl that replaces
		// them. Collect those origins so the intact props can be hidden.
		std::vector<std::array<float, 3>> replacedOrigins;
		if (afterSwitch)
		{
			for (const auto &kv : allKv)
			{
				const auto m0 = kv.find("_mdl0");
				const auto m1 = kv.find("_mdl1");
				const auto o = kv.find("origin");
				if (m0 == kv.end() || m1 == kv.end() || o == kv.end())
					continue;
				if (!EndsWithNoCase(m0->second, "null.mdl") ||
					!EndsWithNoCase(m1->second, ".mdl") ||
					EndsWithNoCase(m1->second, "null.mdl"))
					continue;
				std::array<float, 3> p = { 0, 0, 0 };
				if (std::sscanf(o->second.c_str(), "%f %f %f", &p[0], &p[1], &p[2]) == 3)
					replacedOrigins.push_back(p);
			}
		}

		// Scripted vehicles/bosses (func_train + "model_ex" aircraft with a targetname) that an
		// env_render hides again at the end of their run (rendermode != normal,
		// renderamt 0): zs_victor1's Hind/Comanche helicopters wait for the
		// cut scene, fly past and are switched off. They are not on the map
		// before the event nor after it, so they are never drawn as props.
		const std::set<std::string> switchedOffAfter = ComputeSwitchedOffAfterScene(allKv);
		std::set<std::string> scriptedHidden;
		std::set<std::string> craneDropNames;
		{
			std::set<int> unusedBoxes;
			ComputeCraneDrop(allKv, &craneDropNames, &unusedBoxes);
		}
		for (const auto &kv : allKv)
		{
			const auto cn = kv.find("classname");
			const auto tg = kv.find("target");
			const auto rm = kv.find("rendermode");
			const auto ra = kv.find("renderamt");
			if (cn == kv.end() || cn->second != "env_render" || tg == kv.end() ||
				rm == kv.end() || rm->second == "0" || rm->second == "4")
				continue;
			// A missing renderamt is 0 as well (the second zs_victor1 variant has all
			// zero-valued keys stripped from its entity lump).
			bool zero = ra == kv.end();
			try
			{
				if (ra != kv.end())
					zero = std::stoi(ra->second) <= 0;
			}
			catch (const std::exception &)
			{
			}
			if (zero)
				scriptedHidden.insert(tg->second);
		}

		// Cut-scene actors: a human model (models/player/...) shown at the start and
		// swapped for null.mdl by the script (zs_victor1's "norman" on the roof
		// with his "norman_glow*" flash sprites). He DOES stand there when the
		// level starts (confirmed in game); once the besiege scene is over the
		// model is null.mdl and the glow sprites are switched off by
		// "..._glow*_r_off", so after the switch the sprites must go too.
		std::vector<std::string> sceneActors;
		{
			for (const auto &kv : allKv)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				const auto m0 = kv.find("_mdl0");
				const auto m1 = kv.find("_mdl1");
				if (cn == kv.end() || cn->second != "cycler_mdl" || tn == kv.end() ||
					tn->second.empty() || m0 == kv.end() || m1 == kv.end())
					continue;
				if (m0->second.find("models/player/") != std::string::npos &&
					EndsWithNoCase(m1->second, "null.mdl"))
					sceneActors.push_back(tn->second);
				// Boss intro cut scenes (zs_secretplan2 boss_cutscen1/2, zs_chaser1
				// midboss_action_m1): a cycler that walks through several models
				// ("_mdl0".."_mdlN") and ends on null.mdl. The real boss is spawned by
				// the game afterwards, so the scene's copy is gone when it is over;
				// showing it next to the final state drew two bosses.
				if (m0->second.find("models/boss/") != std::string::npos ||
					m1->second.find("models/boss/") != std::string::npos)
				{
					int last = -1;
					for (int i = 2; i < 10; ++i)
						if (kv.find("_mdl" + std::to_string(i)) != kv.end())
							last = i;
					// Only when the map carries several copies of that boss (cycler
					// copies / the walking train): a lone one (zs_dronefactory) is
					// left in the after-switch view.
					const std::string bossModel =
						m1->second.find("models/boss/") != std::string::npos ? m1->second : m0->second;
					int copies = 0;
					for (const auto &o : allKv)
					{
						const auto a = o.find("model_ex");
						const auto b = o.find("_mdl0");
						const auto c = o.find("_mdl1");
						if ((a != o.end() && a->second == bossModel) ||
							(b != o.end() && b->second == bossModel) ||
							(c != o.end() && c->second == bossModel))
							++copies;
					}
					if (last >= 2 && copies >= 2 &&
						EndsWithNoCase(kv.find("_mdl" + std::to_string(last))->second, "null.mdl"))
						sceneActors.push_back(tn->second);
				}
			}
		}

		for (const auto &kv : allKv)
		{
			if (afterSwitch && !replacedOrigins.empty() &&
				kv.find("_mdl0") == kv.end() && kv.find("_mdl1") == kv.end())
			{
				const auto o = kv.find("origin");
				const auto cn = kv.find("classname");
				const auto mm = kv.find("model");
				float x = 0, y = 0, z = 0;
				if (o != kv.end() && cn != kv.end() && mm != kv.end() &&
					(cn->second == "cycler_sprite" || cn->second == "cycler_mdl") &&
					EndsWithNoCase(mm->second, ".mdl") &&
					std::sscanf(o->second.c_str(), "%f %f %f", &x, &y, &z) == 3)
				{
					bool replaced = false;
					for (const auto &p : replacedOrigins)
						if (std::fabs(p[0] - x) < 1.0f && std::fabs(p[1] - y) < 1.0f &&
							std::fabs(p[2] - z) < 1.0f)
						{
							replaced = true;
							break;
						}
					if (replaced)
						continue;
				}
			}
			auto mit = kv.find("model");
			// Entities whose model is implied by the class (mount_weapon).
			std::string impliedModel;
			if (mit == kv.end() || mit->second.empty())
			{
				// CSO weapon/knife boxes (weaponbox3, knifebox) carry the
				// item's world model in "packmdl" instead of "model".
				const auto pit = kv.find("packmdl");
				if (pit != kv.end())
					mit = pit;
				else
				{
					// CSO's cycler_mdl (bk_blackout tanks) switches between
					// models; it shows "_mdl0" first (tank2.mdl = the intact
					// tank), "_mdl1" is the wreck it changes into.
					const auto cit0 = kv.find("classname");
					const auto m0 = kv.find("_mdl0");
					if (cit0 != kv.end() && cit0->second == "mount_weapon")
					{
						// Mounted machine gun (zs_decoy): the entity has no
						// model key, the game uses mountgun.mdl for it.
						impliedModel = "models/mountgun.mdl";
					}
					else if (cit0 != kv.end() && cit0->second == "naval_weapon")
					{
						// Ship cannon of zs_voidzone1 (CSO's naval weapon): no model key,
						// the game stands models/navalgun.mdl at the entity's origin.
						impliedModel = "models/navalgun.mdl";
					}
					else if (cit0 == kv.end() || cit0->second != "cycler_mdl" ||
						m0 == kv.end() || m0->second.empty())
						continue;
					else
						mit = m0;
				}
			}
			// mount_weapon/naval_weapon have no "model" key at all: mit stays
			// end() on purpose and the model comes from impliedModel, so the
			// brushRef check must not dereference it.
			if (impliedModel.empty() && mit == kv.end())
				continue;
			std::string modelNameStorage = impliedModel.empty() ? mit->second : impliedModel;
			const std::string brushRef =
				(mit != kv.end() && !mit->second.empty() && mit->second[0] == '*')
					? mit->second
					: std::string();
			// CSO model switchers (zs_obstacle1 antennas/towers): the entity
			// shows "_mdl0" (with "_seq0") until a trigger swaps it for "_mdl1"
			// (the burnt version). The plain "model" key is only the editor
			// preview, so a map that carries both versions at one spot would
			// draw them on top of each other. "models/null.mdl" means "nothing
			// yet": such an entity starts hidden.
			bool usedStartModel = false;
			{
				const auto cls = kv.find("classname");
				const auto m0 = kv.find(afterSwitch ? "_mdl1" : "_mdl0");
				if (cls != kv.end() && m0 != kv.end() && impliedModel.empty() &&
					(cls->second == "cycler_mdl" || cls->second == "cycler_sprite") &&
					EndsWithNoCase(m0->second, ".mdl"))
				{
					if (EndsWithNoCase(m0->second, "null.mdl"))
						continue;
					modelNameStorage = m0->second;
					usedStartModel = true;
				}
			}
			// CSO brush entities (func_train cranes in zs_anothertruth2) keep
			// a stub brush for collision and name the model they really show in
			// "model_ex": the game draws that .mdl at the entity's origin.
			if (!modelNameStorage.empty() && modelNameStorage[0] == '*')
			{
				const auto mex = kv.find("model_ex");
				if (mex != kv.end() && !mex->second.empty())
					modelNameStorage = mex->second;
			}
			const std::string &modelName = modelNameStorage;
			if (modelName.empty() || modelName[0] == '*')
				continue;
			if (afterSwitch)
			{
				const auto tn = kv.find("targetname");
				if (tn != kv.end() && switchedOffAfter.count(tn->second) != 0)
					continue;
			}
			if (afterSwitch && !sceneActors.empty())
			{
				const auto tn = kv.find("targetname");
				bool actor = false;
				if (tn != kv.end())
					for (const std::string &a : sceneActors)
						if (tn->second == a || tn->second.compare(0, a.size() + 5, a + "_glow") == 0)
							actor = true;
				if (actor)
					continue;
			}
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				if (cn != kv.end() && cn->second == "func_train" &&
					kv.find("model_ex") != kv.end() && tn != kv.end() &&
					scriptedHidden.count(tn->second) != 0 &&
					(modelName.find("helicopter") != std::string::npos ||
						modelName.find("comanche") != std::string::npos ||
						modelName.find("harrier") != std::string::npos ||
						modelName.find("osprey") != std::string::npos))
					continue;
			}
			// zs_chaser1's mid boss walks in on a func_train (model_ex boss) that the
			// scene switches off again with "..._off" (renderamt missing = 0).
			if (afterSwitch)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				if (cn != kv.end() && cn->second == "func_train" &&
					kv.find("model_ex") != kv.end() && tn != kv.end() &&
					scriptedHidden.count(tn->second) != 0 &&
					modelName.find("models/boss/") != std::string::npos)
					continue;
				// zs_conspiracy's landing Blackhawk with the slung container
				// ("heli_landing", model_ex blackhawk_box.mdl): the opening scene ends
				// with "heli_landing_off"; the helicopter has flown away.
				if (cn != kv.end() && cn->second == "func_train" &&
					kv.find("model_ex") != kv.end() && tn != kv.end() &&
					scriptedHidden.count(tn->second) != 0 &&
					modelName.find("blackhawk") != std::string::npos)
					continue;
				// zs_conspiracy's crane flies off with its hung plank once the dark
				// plank has taken over (see ComputeCraneDrop).
				if (cn != kv.end() && cn->second == "func_train" && tn != kv.end() &&
					craneDropNames.count(tn->second) != 0)
					continue;
			}
			// After the scene: a switcher that only APPEARS for it (start model
			// null.mdl) and is switched off by an env_render at the end (the
			// Blackhawks, Harriers, soldiers of zs_victor1's besiege cut scene)
			// is gone again. Limited to aircraft and soldiers so effects and bosses
			// (zs_obstacle1's bighead) keep showing in the after-switch view.
			if (afterSwitch)
			{
				const auto tn = kv.find("targetname");
				const auto m0 = kv.find("_mdl0");
				if (tn != kv.end() && m0 != kv.end() &&
					EndsWithNoCase(m0->second, "null.mdl") &&
					scriptedHidden.count(tn->second) != 0 &&
					(modelName.find("blackhawk") != std::string::npos ||
						modelName.find("harrier") != std::string::npos ||
						modelName.find("helicopter") != std::string::npos ||
						modelName.find("models/player/") != std::string::npos))
					continue;
			}
			const bool isMdl = EndsWithNoCase(modelName, ".mdl");
			const bool isSpr = EndsWithNoCase(modelName, ".spr");
			if (!isMdl && !isSpr)
				continue;

			BspPlacedEntity pe;
			const auto cit = kv.find("classname");
			if (cit != kv.end())
				pe.classname = cit->second;
			pe.model = modelName;
			pe.isSprite = isSpr;
			const auto oit = kv.find("origin");
			if (oit != kv.end())
			{
				float x = 0, y = 0, z = 0;
				if (std::sscanf(oit->second.c_str(), "%f %f %f", &x, &y, &z) == 3)
				{
					pe.origin[0] = x;
					pe.origin[1] = y;
					pe.origin[2] = z;
				}
			}
			// CSO moving models (hs_sidewinder's helicopters, func_train with
			// "model_ex" and no "origin" key): the entity origin of such a train
			// is 0 0 0 (its stub brush is compiled at the first path_corner), so
			// drawing the .mdl at "origin" piled every helicopter on 0,0,0. The
			// game shows the model where the train waits at the start of its
			// path: at its first path_corner (the stub brush sits exactly
			// there). Place it at that corner.
			if (!brushRef.empty() && kv.find("model_ex") != kv.end() &&
				kv.find("origin") == kv.end())
			{
				const auto tg = kv.find("target");
				if (tg != kv.end() && !tg->second.empty())
				{
					for (const auto &pc : allKv)
					{
						const auto pcn = pc.find("classname");
						const auto pct = pc.find("targetname");
						const auto pco = pc.find("origin");
						if (pcn == pc.end() || pcn->second != "path_corner" ||
							pct == pc.end() || pct->second != tg->second || pco == pc.end())
							continue;
						float cx = 0, cy = 0, cz = 0;
						if (std::sscanf(pco->second.c_str(), "%f %f %f", &cx, &cy, &cz) != 3)
							continue;
						pe.origin[0] = cx;
						pe.origin[1] = cy;
						pe.origin[2] = cz;
						break;
					}
				}
			}
			const auto ait = kv.find("angles");
			if (ait != kv.end())
			{
				float x = 0, y = 0, z = 0;
				if (std::sscanf(ait->second.c_str(), "%f %f %f", &x, &y, &z) == 3)
				{
					pe.angles[0] = x;
					pe.angles[1] = y;
					pe.angles[2] = z;
				}
			}
			else
			{
				// Single "angle" key (yaw only) used by some entities.
				const auto sit = kv.find("angle");
				if (sit != kv.end())
				{
					try
					{
						pe.angles[1] = std::stof(sit->second);
					}
					catch (const std::exception &)
					{
					}
				}
			}
			const auto scit = kv.find("scale");
			if (scit != kv.end())
			{
				try
				{
					const float s = std::stof(scit->second);
					if (s > 0.0f && s < 100.0f)
						pe.scale = s;
				}
				catch (const std::exception &)
				{
				}
			}
			const auto skit = kv.find("skin");
			if (skit != kv.end())
			{
				try
				{
					pe.skin = std::max(0, std::stoi(skit->second));
				}
				catch (const std::exception &)
				{
				}
			}
			const auto bdit = kv.find("body");
			if (bdit != kv.end())
			{
				try
				{
					pe.body = std::max(0, std::stoi(bdit->second));
				}
				catch (const std::exception &)
				{
				}
			}
			auto sqit = kv.find("sequence");
			if (usedStartModel)
			{
				// A model switcher plays "_seq0" / "_seq1" (default 0 = idle); the plain
				// "sequence" is only the editor preview. zs_victor2's wrecked jeeps carry
				// "sequence" 5 / 1 (the explode animations, door flung open at frame 0)
				// and no "_seq0": they stand intact in sequence 0.
				sqit = kv.find(afterSwitch ? "_seq1" : "_seq0");
				pe.holdLastFrame = afterSwitch && kv.find("_seq1") != kv.end() &&
					kv.find("_mdl0") != kv.end() && kv.find("_mdl1") != kv.end() &&
					kv.find("_mdl0")->second == kv.find("_mdl1")->second;
			}
			else if (sqit == kv.end() && pe.classname == "cycler_mdl")
				sqit = kv.find("_seq0");
			if (sqit != kv.end())
			{
				try
				{
					pe.sequence = std::stoi(sqit->second);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto rmit = kv.find("rendermode");
			if (rmit != kv.end())
			{
				try
				{
					pe.renderMode = std::stoi(rmit->second);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto rait = kv.find("renderamt");
			if (rait != kv.end())
			{
				try
				{
					pe.renderAmt = std::clamp(std::stoi(rait->second), 0, 255);
				}
				catch (const std::exception &)
				{
				}
			}
			// renderamt defaults to 0 in the engine: a model with a
			// non-normal rendermode and no renderamt key is invisible (the
			// bk_blackout tank wrecks, shown later by an env_render).
			if (!isSpr && rait == kv.end() && pe.renderMode != 0 &&
				pe.renderMode != 4)
				pe.renderAmt = 0;
			// After the scripted switch (cut scene over / event started), a hidden
			// model that an env_render turns on is shown. Two kinds of switch-on:
			//  - an env_render named "..._on" (zs_victor2's Goliath robots "xt-300",
			//    rendermode 2, no renderamt, stand on the roofs from the end of the cut
			//    scene on): shown at renderamt 255, or at the env_render's own
			//    rendermode/renderamt when it carries a visible one;
			//  - any other env_render with rendermode 2/5 and renderamt > 0 whose name
			//    is not an "off" switch (zs_biolabs1's fan wind "ef_fan_wind" models are
			//    shown by "fan_area2_open", additive amt 100): its mode/amt are used.
			if (afterSwitch && !isSpr && pe.renderAmt == 0 && pe.renderMode != 0)
			{
				const auto tnm = kv.find("targetname");
				if (tnm != kv.end() && !tnm->second.empty())
				{
					for (const auto &rk : allKv)
					{
						const auto rc = rk.find("classname");
						const auto rt = rk.find("target");
						const auto rn = rk.find("targetname");
						if (rc == rk.end() || rc->second != "env_render" || rt == rk.end() ||
							rt->second != tnm->second || rn == rk.end())
							continue;
						int emode = 0, eamt = 0;
						const auto rmk = rk.find("rendermode");
						const auto rak = rk.find("renderamt");
						try
						{
							if (rmk != rk.end())
								emode = std::stoi(rmk->second);
							if (rak != rk.end())
								eamt = std::clamp(std::stoi(rak->second), 0, 255);
						}
						catch (const std::exception &)
						{
						}
						int eflags = 0;
						const auto esf = rk.find("spawnflags");
						if (esf != rk.end())
							try { eflags = std::stoi(esf->second); } catch (const std::exception &) {}
						// An env_render without a rendermode (or with 0) puts the target back to
						// normal rendering = visible (zs_victor2's "victor_team_dead" makes the
						// dead soldiers appear). Flag 4 = "don't change rendermode".
						if (emode == 0 && (eflags & 4) == 0 &&
							rn->second.find("_off") == std::string::npos &&
							!EndsWithNoCase(rn->second, "_on"))
						{
							pe.renderMode = 0;
							pe.renderAmt = 255;
							break;
						}
						const bool visibleState = (emode == 2 || emode == 5) && eamt > 0;
						if (EndsWithNoCase(rn->second, "_on"))
						{
							if (visibleState)
							{
								pe.renderMode = emode;
								pe.renderAmt = eamt;
							}
							else
								pe.renderAmt = 255;
							break;
						}
						if (visibleState && rn->second.find("_off") == std::string::npos)
						{
							pe.renderMode = emode;
							pe.renderAmt = eamt;
							break;
						}
					}
				}
			}

			const auto rfit = kv.find("renderfx");
			if (rfit != kv.end())
			{
				try
				{
					pe.renderFx = std::stoi(rfit->second);
				}
				catch (const std::exception &)
				{
				}
			}
			const auto rcit = kv.find("rendercolor");
			if (rcit != kv.end())
			{
				float r = 0, g = 0, b = 0;
				if (std::sscanf(rcit->second.c_str(), "%f %f %f", &r, &g, &b) == 3)
				{
					pe.renderColor[0] = std::clamp(r, 0.0f, 255.0f);
					pe.renderColor[1] = std::clamp(g, 0.0f, 255.0f);
					pe.renderColor[2] = std::clamp(b, 0.0f, 255.0f);
				}
			}
			const auto sfit = kv.find("spawnflags");
			if (sfit != kv.end())
			{
				try
				{
					pe.spawnFlags = std::stoi(sfit->second);
				}
				catch (const std::exception &)
				{
				}
			}
			out.push_back(std::move(pe));
		}
		return out;
	}

	BspLightmapData BuildBspLightmaps(const BspMap &map)
	{
		BspLightmapData out;
		out.rects.resize(map.Faces().size());
		out.blocks.resize(map.Faces().size());

		struct Job
		{
			size_t face = 0;
			int w = 0, h = 0;
			std::vector<uint8_t> rgb;
			float texMins[2] = { 0.0f, 0.0f };
		};
		std::vector<Job> jobs;
		const std::vector<uint8_t> &lighting = map.Lighting();
		std::vector<BspMap::Vec3> verts;

		for (size_t fi = 0; fi < map.Faces().size(); ++fi)
		{
			const BspMap::Face &face = map.Faces()[fi];
			if (face.texInfo >= map.TexInfos().size())
				continue;
			const BspMap::TexInfo &info = map.TexInfos()[face.texInfo];
			if ((info.flags & BspMap::TexSpecial) != 0)
				continue;
			if (info.miptex < 0 ||
				static_cast<size_t>(info.miptex) >= map.Textures().size())
				continue;
			if (IsBspSpecialTextureName(map.Textures()[info.miptex].name))
				continue;
			if (!GatherFaceVerts(map, face, verts))
				continue;

			float minS = 0, maxS = 0, minT = 0, maxT = 0;
			bool first = true, bad = false;
			for (const auto &v : verts)
			{
				const float s = v.x * info.s.x + v.y * info.s.y + v.z * info.s.z +
					info.shiftS;
				const float t = v.x * info.t.x + v.y * info.t.y + v.z * info.t.z +
					info.shiftT;
				if (!(s == s && t == t))
				{
					bad = true;
					break;
				}
				if (first)
				{
					minS = maxS = s;
					minT = maxT = t;
					first = false;
				}
				else
				{
					minS = std::min(minS, s);
					maxS = std::max(maxS, s);
					minT = std::min(minT, t);
					maxT = std::max(maxT, t);
				}
			}
			if (bad || first)
				continue;

			// GoldSrc lightmap grid: 16-texel cells, block covers the
			// floored/ceiled extent plus one sample per cell corner.
			const float minsS = std::floor(minS / 16.0f) * 16.0f;
			const float maxcS = std::ceil(maxS / 16.0f) * 16.0f;
			const float minsT = std::floor(minT / 16.0f) * 16.0f;
			const float maxcT = std::ceil(maxT / 16.0f) * 16.0f;
			const int w = static_cast<int>((maxcS - minsS) / 16.0f) + 1;
			const int h = static_cast<int>((maxcT - minsT) / 16.0f) + 1;
			if (w <= 0 || h <= 0 || w > 512 || h > 512)
				continue;
			if (face.lightOffset < 0)
				continue;  // No lightmap (water, sky, unlit).
			if (face.styles[0] == 255)
			{
				// "No light styles" normally means no lightmap. But some maps
				// (zs_decoy: the chee2 / hs4_heli25 hazard strips around the
				// turret pits) keep a valid lightofs together with styles
				// 255 and the game lights those faces from that data (dim
				// ochre, matching the in-game screenshots); skipping them made
				// them fullbright yellow. Only liquids and sky stay unlit.
				const std::string &tn = map.Textures()[info.miptex].name;
				const bool sky = tn.size() >= 3 &&
					(tn[0] == 's' || tn[0] == 'S') && (tn[1] == 'k' || tn[1] == 'K') &&
					(tn[2] == 'y' || tn[2] == 'Y');
				if (tn.empty() || tn[0] == '!' || sky)
					continue;
			}
			const int samples = w * h;
			// Face light samples are stored as one complete RGB block for each
			// non-255 style, in style-slot order. Style 255 is the terminator.
			// A few CSO faces have styles[0] == 255 but still carry one usable
			// light block (handled above), so preserve that legacy interpretation.
			int styleCount = 0;
			if (face.styles[0] == 255)
				styleCount = 1;
			else
				for (uint8_t style : face.styles)
				{
					if (style == 255)
						break;
					++styleCount;
				}
			if (styleCount <= 0)
				continue;
			const int64_t layerBytes = static_cast<int64_t>(samples) * 3;
			const int64_t need = layerBytes * styleCount;
			if (static_cast<int64_t>(face.lightOffset) + need >
				static_cast<int64_t>(lighting.size()))
				continue;

			Job job;
			job.face = fi;
			job.w = w;
			job.h = h;
			job.texMins[0] = minsS;
			job.texMins[1] = minsT;
			job.rgb.assign(static_cast<size_t>(layerBytes), 0);
			for (int style = 0; style < styleCount; ++style)
			{
				const size_t layerStart = static_cast<size_t>(face.lightOffset) +
					static_cast<size_t>(style) * static_cast<size_t>(layerBytes);
				for (size_t byte = 0; byte < job.rgb.size(); ++byte)
				{
					const unsigned sum = static_cast<unsigned>(job.rgb[byte]) +
						lighting[layerStart + byte];
					job.rgb[byte] = static_cast<uint8_t>(std::min(sum, 255u));
				}
			}
			jobs.push_back(std::move(job));
		}

		// Shelf-pack the blocks (face order = deterministic). UVs stay
		// strictly inside [0.5, size-0.5] of each block, so no padding is
		// needed for bilinear filtering; lightmap mipmaps stay off anyway.
		constexpr int kAtlasWidth = 2048;
		constexpr int kAtlasMaxHeight = 8192;
		int shelfX = 1;  // Column 0 is reserved for the white texel below.
		int shelfY = 0;
		int shelfH = 0;
		for (Job &job : jobs)
		{
			if (shelfX + job.w > kAtlasWidth)
			{
				shelfY += shelfH;
				shelfX = 1;
				shelfH = 0;
			}
			if (shelfY + job.h > kAtlasMaxHeight)
				continue;  // Absurd map: this face stays fullbright.
			out.rects[job.face] = { shelfX, shelfY, job.w, job.h, false };
			BspFaceLight &block = out.blocks[job.face];
			block.width = job.w;
			block.height = job.h;
			block.rgb = std::move(job.rgb);
			block.texMins[0] = job.texMins[0];
			block.texMins[1] = job.texMins[1];
			shelfX += job.w;
			shelfH = std::max(shelfH, job.h);
		}

		// Row 1 of column 0 stays black: the "no light here" texel (models with
		// no lit surface below them), next to the white fullbright one in row 0.
		const int atlasH = std::max(2, shelfY + shelfH);
		QImage atlas(kAtlasWidth, atlasH, QImage::Format_RGB888);
		atlas.fill(0);
		{
			uint8_t *scan = atlas.scanLine(0);
			scan[0] = scan[1] = scan[2] = 255;  // White texel = fullbright.
		}
		for (size_t fi = 0; fi < out.rects.size(); ++fi)
		{
			const auto &rect = out.rects[fi];
			if (rect.empty)
				continue;
			const auto &block = out.blocks[fi];
			for (int y = 0; y < rect.h; ++y)
			{
				uint8_t *scan = atlas.scanLine(rect.y + y);
				std::memcpy(scan + rect.x * 3,
					block.rgb.data() + static_cast<size_t>(y) * rect.w * 3,
					static_cast<size_t>(rect.w) * 3);
			}
		}
		out.atlas = std::move(atlas);
		return out;
	}

	namespace
	{
		// CSO cranes ("model_ex" on a func_train whose stub brush has no
		// faces) hang from the box brush entity sharing their "target". In
		// game both are snapped to the first path_corner as
		// origin = corner - (mins+maxs)/2, so the box ends up at
		// crane + (craneCentre - boxCentre). The viewer draws entities at
		// their "origin" keys, where the box sits 62 units beside and 48
		// units too low for the crane's clamps to fill its sockets. The crane
		// stays at its key origin (it matches the ceiling rail); the BOX is
		// moved to where the engine would put it. Result: box model index ->
		// extra shift of its faces.
		std::map<int, std::array<float, 3>> ComputeCraneBoxShifts(const BspMap &map)
		{
			std::map<int, std::array<float, 3>> out;
			const auto allKv = ParseEntityKeyValues(map.EntitiesText());
			const auto parseModel = [&](const std::string &v) -> int {
				if (v.size() < 2 || v[0] != '*')
					return -1;
				try
				{
					const int n = std::stoi(v.substr(1));
					return (n >= 0 && static_cast<size_t>(n) < map.Models().size()) ? n : -1;
				}
				catch (const std::exception &)
				{
					return -1;
				}
			};
			const auto parseOrigin = [](const std::map<std::string, std::string> &kv, float o[3]) {
				o[0] = o[1] = o[2] = 0.0f;
				const auto it = kv.find("origin");
				if (it != kv.end())
					std::sscanf(it->second.c_str(), "%f %f %f", &o[0], &o[1], &o[2]);
			};
			const auto centre = [&](int m, float c[3]) {
				const BspMap::Model &mod = map.Models()[static_cast<size_t>(m)];
				c[0] = (mod.mins.x + mod.maxs.x) * 0.5f;
				c[1] = (mod.mins.y + mod.maxs.y) * 0.5f;
				c[2] = (mod.mins.z + mod.maxs.z) * 0.5f;
			};
			for (const auto &ckv : allKv)
			{
				const auto mit = ckv.find("model");
				const auto mex = ckv.find("model_ex");
				const auto tgt = ckv.find("target");
				if (mit == ckv.end() || mex == ckv.end() || mex->second.empty() ||
					tgt == ckv.end() || tgt->second.empty())
					continue;
				const int cm = parseModel(mit->second);
				if (cm < 0)
					continue;
				for (const auto &bkv : allKv)
				{
					const auto bm = bkv.find("model");
					const auto bt = bkv.find("target");
					if (&bkv == &ckv || bm == bkv.end() || bt == bkv.end() ||
						bt->second != tgt->second || bkv.find("model_ex") != bkv.end())
						continue;
					const int bi = parseModel(bm->second);
					if (bi < 0 || map.Models()[static_cast<size_t>(bi)].numFaces <= 0 ||
						out.count(bi) != 0)
						continue;
					float co[3], bo[3], cc[3], bc[3];
					parseOrigin(ckv, co);
					parseOrigin(bkv, bo);
					centre(cm, cc);
					centre(bi, bc);
					// engine: box = crane + (cc - bc); viewer: box key = bo.
					out[bi] = { co[0] + cc[0] - bc[0] - bo[0],
						co[1] + cc[1] - bc[1] - bo[1],
						co[2] + cc[2] - bc[2] - bo[2] };
					break;
				}
			}
			return out;
		}
		// func_train with an origin brush (zs_allout1's event trains: six cars
		// whose faces are stored around 0,0,0, "origin" key = where the mapper
		// drew them). The engine ignores that key at run time: it snaps a train
		// to its first path_corner as origin = corner - (mins+maxs)/2 and stays
		// there until the train is triggered. Using the key put the cars high
		// above the rails. Cranes (boxes sharing a target with a "model_ex"
		// func_train) are handled by ComputeCraneBoxShifts and skipped here.
		// Result: model index -> total shift of the model's faces.
		std::map<int, std::array<float, 3>> ComputeTrainShifts(const BspMap &map,
			const std::map<int, std::array<float, 3>> &skip, bool afterSwitch)
		{
			std::map<int, std::array<float, 3>> out;
			// Normal view: draw trains where the mapper placed them (the "origin"
			// key, same as the map editors and Crafty show). The engine would
			// park them stacked on the first path_corner until triggered, which
			// is invisible in game but looks like a bug in a viewer.
			if (!afterSwitch)
				return out;
			const auto allKv = ParseEntityKeyValues(map.EntitiesText());
			for (const auto &kv : allKv)
			{
				const auto cn = kv.find("classname");
				const auto mm = kv.find("model");
				const auto tg = kv.find("target");
				const auto og = kv.find("origin");
				if (cn == kv.end() || cn->second != "func_train" || mm == kv.end() ||
					tg == kv.end() || og == kv.end() || kv.find("model_ex") != kv.end() ||
					mm->second.size() < 2 || mm->second[0] != '*')
					continue;
				int index = -1;
				try
				{
					index = std::stoi(mm->second.substr(1));
				}
				catch (const std::exception &)
				{
					continue;
				}
				if (index <= 0 || static_cast<size_t>(index) >= map.Models().size() ||
					skip.count(index) != 0 || map.Models()[static_cast<size_t>(index)].numFaces <= 0)
					continue;
				const BspMap::Model &mod = map.Models()[static_cast<size_t>(index)];
				float o[3] = { 0, 0, 0 };
				if (std::sscanf(og->second.c_str(), "%f %f %f", &o[0], &o[1], &o[2]) != 3)
					continue;
				// Only models whose faces are stored relative to an origin brush
				// (the key lies outside the stored faces).
				const bool inside =
					o[0] >= mod.mins.x - 16.0f && o[0] <= mod.maxs.x + 16.0f &&
					o[1] >= mod.mins.y - 16.0f && o[1] <= mod.maxs.y + 16.0f &&
					o[2] >= mod.mins.z - 16.0f && o[2] <= mod.maxs.z + 16.0f;
				if (inside)
					continue;
				for (const auto &pc : allKv)
				{
					const auto pcn = pc.find("classname");
					const auto pct = pc.find("targetname");
					const auto pco = pc.find("origin");
					if (pcn == pc.end() || pcn->second != "path_corner" ||
						pct == pc.end() || pct->second != tg->second || pco == pc.end())
						continue;
					float cx = 0, cy = 0, cz = 0;
					if (std::sscanf(pco->second.c_str(), "%f %f %f", &cx, &cy, &cz) != 3)
						continue;
					out[index] = { cx - (mod.mins.x + mod.maxs.x) * 0.5f,
						cy - (mod.mins.y + mod.maxs.y) * 0.5f,
						cz - (mod.mins.z + mod.maxs.z) * 0.5f };
					if (afterSwitch)
					{
						// "After the switch" (cutscene) view: the cars are out of
						// their waiting spot and running along the track. Keep the
						// mapper's spacing (the "origin" key x/y) and use the height
						// of the path's second corner, where the train cruises
						// (zs_allout1: key z 2184 is 512 above the rails, the path
						// runs at 1672 = rail level + half a car).
						const auto nx = pc.find("target");
						float nz = cz;
						if (nx != pc.end())
							for (const auto &pn : allKv)
							{
								const auto pnn = pn.find("targetname");
								const auto pno = pn.find("origin");
								float ax = 0, ay = 0, az = 0;
								if (pnn != pn.end() && pnn->second == nx->second && pno != pn.end() &&
									std::sscanf(pno->second.c_str(), "%f %f %f", &ax, &ay, &az) == 3)
								{
									nz = az;
									break;
								}
							}
						out[index] = { o[0], o[1], nz - (mod.mins.z + mod.maxs.z) * 0.5f };
					}
					break;
				}
			}
			return out;
		}
		// func_door with the "Starts open" flag (1): the engine puts the door at
		// its OPEN position when the level starts (the stored brush sits at
		// the closed one), i.e. moved along its movedir by
		// |movedir . size| - lip. zs_overflow2's yellow warning screens
		// (additive func_door "door_trap_clear", flags 33) start open, hidden
		// beside the door, and only slide in when triggered; drawing them at
		// the stored position made them z-fight with the real door.
		// Result: box model index -> shift of the model's faces.
		// afterSwitch: a door that a scene manager closes at the end of the scene
		// (a multi_manager that also switches an "..._on" env_render, e.g.
		// zs_victor2's mm_safezone3 -> safezone3_dr_in) is back at its stored,
		// closed position.
		std::map<int, std::array<float, 3>> ComputeStartOpenDoorShifts(const BspMap &map,
			bool afterSwitch = false)
		{
			std::map<int, std::array<float, 3>> out;
			const auto doorKv = ParseEntityKeyValues(map.EntitiesText());
			std::set<std::string> sceneClosed;
			if (afterSwitch)
			{
				std::set<std::string> onRenders;
				for (const auto &kv : doorKv)
				{
					const auto cn = kv.find("classname");
					const auto tn = kv.find("targetname");
					if (cn != kv.end() && cn->second == "env_render" && tn != kv.end() &&
						EndsWithNoCase(tn->second, "_on"))
						onRenders.insert(tn->second);
				}
				for (const auto &kv : doorKv)
				{
					const auto cn = kv.find("classname");
					if (cn == kv.end() || cn->second != "multi_manager")
						continue;
					bool scene = false;
					for (const auto &p : kv)
						if (onRenders.count(p.first) != 0)
							scene = true;
					if (scene)
						for (const auto &p : kv)
							sceneClosed.insert(p.first);
				}
			}
			for (const auto &kv : doorKv)
			{
				const auto cls = kv.find("classname");
				const auto mdl = kv.find("model");
				const auto sf = kv.find("spawnflags");
				if (cls == kv.end() || cls->second != "func_door" ||
					mdl == kv.end() || mdl->second.size() < 2 || mdl->second[0] != '*' ||
					sf == kv.end())
					continue;
				int flags = 0;
				int mi = -1;
				try
				{
					flags = std::stoi(sf->second);
					mi = std::stoi(mdl->second.substr(1));
				}
				catch (const std::exception &)
				{
					continue;
				}
				if ((flags & 1) == 0 || mi <= 0 ||
					static_cast<size_t>(mi) >= map.Models().size())
					continue;
				if (afterSwitch)
				{
					const auto dtn = kv.find("targetname");
					if (dtn != kv.end() && sceneClosed.count(dtn->second) != 0)
						continue;
				}
				// Movement direction from "angles" (engine's UTIL_SetMovedir).
				float ang[3] = { 0.0f, 0.0f, 0.0f };
				const auto ait = kv.find("angles");
				if (ait != kv.end())
					std::sscanf(ait->second.c_str(), "%f %f %f", &ang[0], &ang[1], &ang[2]);
				else
				{
					const auto yit = kv.find("angle");
					if (yit != kv.end())
						std::sscanf(yit->second.c_str(), "%f", &ang[1]);
				}
				float dir[3];
				if (ang[0] == 0.0f && ang[1] == -1.0f && ang[2] == 0.0f)
				{
					dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = 1.0f;
				}
				else if (ang[0] == 0.0f && ang[1] == -2.0f && ang[2] == 0.0f)
				{
					dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = -1.0f;
				}
				else
				{
					const float kRad = 3.14159265358979f / 180.0f;
					const float pitch = ang[0] * kRad, yaw = ang[1] * kRad;
					dir[0] = std::cos(pitch) * std::cos(yaw);
					dir[1] = std::cos(pitch) * std::sin(yaw);
					dir[2] = -std::sin(pitch);
				}
				const BspMap::Model &mod = map.Models()[static_cast<size_t>(mi)];
				const float size[3] = { mod.maxs.x - mod.mins.x, mod.maxs.y - mod.mins.y,
					mod.maxs.z - mod.mins.z };
				float lip = 0.0f;
				const auto lit = kv.find("lip");
				if (lit != kv.end())
					std::sscanf(lit->second.c_str(), "%f", &lip);
				const float dist = std::fabs(dir[0] * size[0]) + std::fabs(dir[1] * size[1]) +
					std::fabs(dir[2] * size[2]) - lip;
				if (dist <= 0.0f)
					continue;
				out[mi] = { dir[0] * dist, dir[1] * dist, dir[2] * dist };
			}
			return out;
		}

		// Brush func_train whose "origin" key is a leftover far away from its path
		// (zs_victor2's forklift load: the boxes carry origin 936 -264 65 but the
		// train starts at folklift_p1, 1886 -236 63, in front of the forklift
		// model). The engine puts the entity at its first path_corner, so the
		// faces (stored around the origin brush) are shifted there instead of by
		// the key. Keys that sit near the first corner are left alone.
		// Result: model index -> shift of the model's stored faces.
		std::map<int, std::array<float, 3>> ComputeStrayTrainShifts(const BspMap &map)
		{
			std::map<int, std::array<float, 3>> out;
			const auto all = ParseEntityKeyValues(map.EntitiesText());
			// Trains that share their first corner are a convoy (zs_allout1's six train
			// cars all start at "e1" and differ only by their keys): there the key IS the
			// car's place in the row, never a leftover. Effect trains (additive/blend,
			// zs_panic's lasers) keep their key as well.
			std::map<std::string, int> trainsPerTarget;
			for (const auto &kv : all)
			{
				const auto cls = kv.find("classname");
				const auto tgt = kv.find("target");
				if (cls != kv.end() && cls->second == "func_train" && tgt != kv.end())
					++trainsPerTarget[tgt->second];
			}
			for (const auto &kv : all)
			{
				const auto cls = kv.find("classname");
				const auto mdl = kv.find("model");
				const auto org = kv.find("origin");
				const auto tgt = kv.find("target");
				if (cls == kv.end() || cls->second != "func_train" || mdl == kv.end() ||
					mdl->second.size() < 2 || mdl->second[0] != '*' || org == kv.end() ||
					tgt == kv.end() || kv.find("model_ex") != kv.end())
					continue;
				if (trainsPerTarget[tgt->second] > 1)
					continue;
				{
					const auto rmk = kv.find("rendermode");
					if (rmk != kv.end() && rmk->second != "0" && rmk->second != "4")
						continue;
				}
				int mi = -1;
				try
				{
					mi = std::stoi(mdl->second.substr(1));
				}
				catch (const std::exception &)
				{
					continue;
				}
				if (mi <= 0 || static_cast<size_t>(mi) >= map.Models().size())
					continue;
				float ko[3] = { 0, 0, 0 };
				if (std::sscanf(org->second.c_str(), "%f %f %f", &ko[0], &ko[1], &ko[2]) != 3)
					continue;
				const BspMap::Model &mod = map.Models()[static_cast<size_t>(mi)];
				// Key inside the stored bounds: faces are absolute, no shift at all.
				if (ko[0] >= mod.mins.x - 16 && ko[0] <= mod.maxs.x + 16 &&
					ko[1] >= mod.mins.y - 16 && ko[1] <= mod.maxs.y + 16 &&
					ko[2] >= mod.mins.z - 16 && ko[2] <= mod.maxs.z + 16)
					continue;
				float co[3] = { 0, 0, 0 };
				bool found = false;
				for (const auto &pc : all)
				{
					const auto pcn = pc.find("classname");
					const auto pct = pc.find("targetname");
					const auto pco = pc.find("origin");
					if (pcn == pc.end() || pcn->second != "path_corner" || pct == pc.end() ||
						pct->second != tgt->second || pco == pc.end())
						continue;
					if (std::sscanf(pco->second.c_str(), "%f %f %f", &co[0], &co[1], &co[2]) == 3)
					{
						found = true;
						break;
					}
				}
				if (!found)
					continue;
				if (std::hypot(ko[0] - co[0], ko[1] - co[1]) <= 256.0f)
					continue;
				out[mi] = { co[0] - (mod.mins.x + mod.maxs.x) * 0.5f,
					co[1] - (mod.mins.y + mod.maxs.y) * 0.5f,
					co[2] - (mod.mins.z + mod.maxs.z) * 0.5f };
			}
			return out;
		}

		// Scripted effect brushes: a named func_train in additive/blend mode with a
		// visible renderamt that a multi_manager starts later (zs_panic's moving
		// laser walls, hall_laser1-5 / room_laser, switched on by mm_laser_con after
		// the player presses the button). They are not on the map at its start.
		// Result: model indexes hidden in the map-start state.
		std::set<int> ComputeScriptedEffectTrains(const BspMap &map)
		{
			std::set<int> out;
			const auto all = ParseEntityKeyValues(map.EntitiesText());
			std::set<std::string> managed;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				if (cn != kv.end() && cn->second == "multi_manager")
					for (const auto &p : kv)
						managed.insert(p.first);
			}
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto mdl = kv.find("model");
				const auto tn = kv.find("targetname");
				const auto rm = kv.find("rendermode");
				const auto ra = kv.find("renderamt");
				if (cn == kv.end() || cn->second != "func_train" || mdl == kv.end() ||
					mdl->second.size() < 2 || mdl->second[0] != '*' || tn == kv.end() ||
					rm == kv.end() || ra == kv.end() || kv.find("model_ex") != kv.end() ||
					managed.count(tn->second) == 0)
					continue;
				int mode = 0, amt = 0, mi = 0;
				try
				{
					mode = std::stoi(rm->second);
					amt = std::stoi(ra->second);
					mi = std::stoi(mdl->second.substr(1));
				}
				catch (const std::exception &)
				{
					continue;
				}
				if ((mode == 5 || mode == 3 || mode == 2) && amt > 0)
					out.insert(mi);
			}
			return out;
		}

		// Breakable props that take part in a scripted "destroy it to move on"
		// chain (zs_voidzone1's treezone: the orange target frames are zbsbreak
		// brushes, shooting one breaks a tiny hit-box brush behind it that fires a
		// multisource -> multi_manager; the central cylinder is broken by that
		// manager). After the scene all of them are gone, with the pads and
		// markers the script removes. Result: model indexes hidden after the switch.
		std::set<int> ComputeScriptedBreakables(const BspMap &map)
		{
			std::set<int> out;
			const auto all = ParseEntityKeyValues(map.EntitiesText());
			// multisource name -> the manager it fires
			std::map<std::string, std::string> sourceManager;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				const auto tg = kv.find("target");
				if (cn != kv.end() && cn->second == "multisource" && tn != kv.end() && tg != kv.end())
					sourceManager[tn->second] = tg->second;
			}
			// hit box (func_breakable) targetname -> multisource it fires
			std::map<std::string, std::string> hitBoxSource;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				const auto tg = kv.find("target");
				if (cn != kv.end() && cn->second == "func_breakable" && tn != kv.end() &&
					tg != kv.end() && sourceManager.count(tg->second) != 0)
					hitBoxSource[tn->second] = tg->second;
			}
			const auto addModel = [&out](const std::map<std::string, std::string> &kv)
			{
				const auto mdl = kv.find("model");
				if (mdl == kv.end() || mdl->second.size() < 2 || mdl->second[0] != '*')
					return;
				try
				{
					out.insert(std::stoi(mdl->second.substr(1)));
				}
				catch (const std::exception &)
				{
				}
			};
			// Target frames: a zbsbreak whose target is such a hit box. Shooting the
			// frame breaks the hit box, which fires the multisource.
			std::set<std::string> usedHitBoxes, usedSources;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tg = kv.find("target");
				if (cn == kv.end() || cn->second != "zbsbreak" || tg == kv.end())
					continue;
				// Solid frames only: the same chain on other maps (glowing generator /
				// crane markers, rendermode 2/5) is not verified to vanish after the scene.
				const auto rmk = kv.find("rendermode");
				if (rmk == kv.end() || rmk->second != "4")
					continue;
				const auto hb = hitBoxSource.find(tg->second);
				if (hb == hitBoxSource.end())
					continue;
				addModel(kv);
				usedHitBoxes.insert(tg->second);
				usedSources.insert(hb->second);
			}
			// Frames on crates that the script swaps for another model (zs_voidzone1's
			// bomb boxes: dd_bombbox -> ground-zero effect): the crate is gone after
			// the scene, so the red target frame goes with it.
			{
				std::set<std::string> swappedCrates, nullCrates;
				for (const auto &kv : all)
				{
					const auto cn = kv.find("classname");
					const auto tn = kv.find("targetname");
					const auto m0 = kv.find("_mdl0");
					const auto m1 = kv.find("_mdl1");
					if (cn != kv.end() && cn->second == "cycler_mdl" && tn != kv.end() &&
						m0 != kv.end() && m1 != kv.end() && m0->second != m1->second)
					{
						swappedCrates.insert(tn->second);
						if (EndsWithNoCase(m1->second, "null.mdl"))
							nullCrates.insert(tn->second);
					}
				}
				// zs_dronefactory: the frame fires a multi_manager whose keys name
				// such crates (machine_01, fcpipe1...): the manager removes them.
				for (const auto &kv : all)
				{
					const auto cn = kv.find("classname");
					const auto tn = kv.find("targetname");
					if (cn == kv.end() || cn->second != "multi_manager" || tn == kv.end())
						continue;
					for (const auto &p : kv)
						if (swappedCrates.count(p.first) != 0)
						{
							swappedCrates.insert(tn->second);
							break;
						}
					for (const auto &p : kv)
						if (nullCrates.count(p.first) != 0)
						{
							nullCrates.insert(tn->second);
							break;
						}
				}
				for (const auto &kv : all)
				{
					const auto cn = kv.find("classname");
					const auto tg = kv.find("target");
					const auto rmk = kv.find("rendermode");
					const auto rfx = kv.find("renderfx");
					if (cn != kv.end() && cn->second == "zbsbreak" && tg != kv.end() &&
						rmk != kv.end() && rfx != kv.end() && rfx->second == "24" &&
						((rmk->second == "4" && swappedCrates.count(tg->second) != 0) ||
						 // translucent frames (zs_secretplan1): only when the crate is
						 // really removed (null.mdl), not swapped for a broken copy
						 (rmk->second == "2" && nullCrates.count(tg->second) != 0)))
						addModel(kv);
				}
			}
			// Boss break-in wall (zs_chaser1 "mid_spawn_wall"): a hit box whose manager
			// breaks a few walls/gates and fires a trigger_push. The boss smashes it right
			// after the intro cut scene, so the after-switch view shows the opening.
			{
				bool bossMap = false;
				for (const auto &kv : all)
					for (const char *k : { "model", "_mdl0", "_mdl1", "model_ex" })
					{
						const auto it = kv.find(k);
						if (it != kv.end() && it->second.find("models/boss/") != std::string::npos)
							bossMap = true;
					}
				std::set<std::string> pushNames;
				for (const auto &kv : all)
				{
					const auto cn = kv.find("classname");
					const auto tn = kv.find("targetname");
					if (cn != kv.end() && cn->second == "trigger_push" && tn != kv.end())
						pushNames.insert(tn->second);
				}
				for (const auto &mgr : all)
				{
					const auto mc = mgr.find("classname");
					const auto mn = mgr.find("targetname");
					if (!bossMap || mc == mgr.end() || mc->second != "multi_manager" || mn == mgr.end())
						continue;
					bool pushes = false;
					for (const auto &p : mgr)
						if (pushNames.count(p.first) != 0)
							pushes = true;
					if (!pushes)
						continue;
					std::set<std::string> boxes;
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tn = kv.find("targetname");
						const auto tg = kv.find("target");
						if (cn != kv.end() && cn->second == "func_breakable" && tn != kv.end() &&
							tg != kv.end() && tg->second == mn->second)
						{
							boxes.insert(tn->second);
							addModel(kv);
						}
					}
					if (boxes.empty())
						continue;
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tn = kv.find("targetname");
						const auto tg = kv.find("target");
						if (cn == kv.end() || (cn->second != "func_breakable" && cn->second != "zbsbreak"))
							continue;
						if ((tn != kv.end() && mgr.find(tn->second) != mgr.end()) ||
							(tg != kv.end() && boxes.count(tg->second) != 0))
							addModel(kv);
					}
				}
			}
			// Bridge-drop chain (zs_evacuate1's flat bridge): the red target mounts
			// (zbsbreak) and the cable hit boxes fire a multisource whose manager
			// swings a func_door_rotating away (the flat bridge) while a hidden
			// tilted copy appears. After the event the mounts and both cables are
			// gone with the flat bridge.
			{
				std::set<std::string> doorNames, toggleNames;
				for (const auto &kv : all)
				{
					const auto cn = kv.find("classname");
					const auto tn = kv.find("targetname");
					if (cn != kv.end() && cn->second == "func_door_rotating" && tn != kv.end())
						doorNames.insert(tn->second);
					if (cn != kv.end() && cn->second == "func_wall_toggle" && tn != kv.end())
						toggleNames.insert(tn->second);
				}
				for (const auto &src : sourceManager)
				{
					const std::string &srcName = src.first;
					std::set<std::string> keys;
					bool dropsDoor = false, showsToggle = false;
					for (const auto &mgr : all)
					{
						const auto mc = mgr.find("classname");
						const auto mn = mgr.find("targetname");
						if (mc == mgr.end() || mc->second != "multi_manager" || mn == mgr.end() ||
							mn->second != src.second)
							continue;
						for (const auto &p : mgr)
						{
							keys.insert(p.first);
							if (doorNames.count(p.first) != 0)
								dropsDoor = true;
							if (toggleNames.count(p.first) != 0)
								showsToggle = true;
						}
					}
					if (!dropsDoor || !showsToggle)
						continue;
					std::set<std::string> hitBoxes;
					std::vector<const std::map<std::string, std::string> *> hitBoxKv;
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tn = kv.find("targetname");
						const auto tg = kv.find("target");
						if (cn != kv.end() && cn->second == "func_breakable" && tn != kv.end() &&
							tg != kv.end() && tg->second == srcName)
						{
							hitBoxes.insert(tn->second);
							hitBoxKv.push_back(&kv);
						}
					}
					// Only chains with red target mounts (solid zbsbreak); translucent
					// frames on wooden boards over a door (zs_chaser1) are left as they are.
					bool hasMount = false;
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tg = kv.find("target");
						const auto rmk = kv.find("rendermode");
						if (cn != kv.end() && cn->second == "zbsbreak" && tg != kv.end() &&
							(rmk == kv.end() || rmk->second != "2") &&
							(tg->second == srcName || hitBoxes.count(tg->second) != 0))
							hasMount = true;
					}
					if (!hasMount)
						continue;
					for (const auto *hk : hitBoxKv)
						addModel(*hk);
					for (const auto &kv : all)
					{
						const auto cn = kv.find("classname");
						const auto tn = kv.find("targetname");
						const auto tg = kv.find("target");
						if (cn == kv.end() || (cn->second != "zbsbreak" && cn->second != "func_breakable"))
							continue;
						if ((tg != kv.end() && (tg->second == srcName || hitBoxes.count(tg->second) != 0)) ||
							(cn->second == "func_breakable" && tn != kv.end() && keys.count(tn->second) != 0))
							addModel(kv);
					}
				}
			}
			if (usedHitBoxes.empty())
				return out;
			std::set<std::string> managerKeys;
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				if (cn == kv.end() || cn->second != "multi_manager" || tn == kv.end())
					continue;
				for (const auto &src : usedSources)
					if (sourceManager[src] == tn->second)
						for (const auto &p : kv)
							managerKeys.insert(p.first);
			}
			for (const auto &kv : all)
			{
				const auto cn = kv.find("classname");
				const auto tn = kv.find("targetname");
				if (cn == kv.end() || cn->second != "func_breakable")
					continue;
				const auto tgt = kv.find("target");
				// The hit boxes themselves (also the one the boss death breaks, which
				// fires the same multisource), and breakables the escape manager breaks.
				if ((tn != kv.end() && (usedHitBoxes.count(tn->second) != 0 ||
						managerKeys.count(tn->second) != 0)) ||
					(tgt != kv.end() && usedSources.count(tgt->second) != 0))
					addModel(kv);
			}
			return out;
		}
	}

	std::vector<BspCpuMesh> BuildBspMeshes(const BspMap &map,
		const std::vector<QImage> &textures, BspLightmapData *lightmaps)
	{
		// Lightmap atlas first: every emitted vertex below carries atlas UVs
		// (white texel = fullbright for faces without a lightmap).
		BspLightmapData ownLightmaps;
		if (lightmaps == nullptr)
			lightmaps = &ownLightmaps;
		*lightmaps = BuildBspLightmaps(map);
		const float atlasW = std::max(lightmaps->atlas.width(), 1);
		const float atlasH = std::max(lightmaps->atlas.height(), 1);
		const float whiteU = 0.5f / atlasW;
		const float whiteV = 0.5f / atlasH;
		// Meshes are grouped by (texture, render class, opacity, tint) so
		// every mesh draws with one uniform set.
		using MeshKey = std::tuple<int, int, int, int, int, int, int, int, int, int, int>;
		bool currentBrushEntity = false;  // set per model in the loop below
		bool currentSky = false;          // set per face (texture named "sky*")
		bool currentBillboard = false;    // set per face (crossed-plane marker)
		float currentSpin = 0.0f;         // set per model (spinning pictogram, deg/s)
		float currentPivot[2] = { 0.0f, 0.0f };
		int currentPulse = 0;             // set per model (renderfx 1..4)
		std::map<MeshKey, size_t> meshByKey;
		std::vector<BspCpuMesh> meshes;

		// Returns the index of the mesh the triangle was appended to.
		const auto emitTriangle = [&](int texture, BspRenderClass renderClass,
			float opacity, const float tint[3],
			const float *a, const float *b, const float *c,
			const float *na, const float *nb, const float *nc,
			const float *uva, const float *uvb, const float *uvc,
			const float *luva, const float *luvb, const float *luvc)
		{
			const MeshKey key(texture, static_cast<int>(renderClass),
				static_cast<int>(opacity * 255.0f + 0.5f),
				static_cast<int>(tint[0] * 255.0f + 0.5f),
				static_cast<int>(tint[1] * 255.0f + 0.5f),
				static_cast<int>(tint[2] * 255.0f + 0.5f),
				(currentBrushEntity ? 1 : 0) | (currentSky ? 2 : 0) |
				(currentBillboard ? 4 : 0),
				static_cast<int>(currentSpin * 100.0f),
				static_cast<int>(currentPivot[0] * 16.0f),
				static_cast<int>(currentPivot[1] * 16.0f),
				currentPulse);
			const auto found = meshByKey.find(key);
			size_t meshIndex = meshes.size();
			if (found != meshByKey.end())
			{
				meshIndex = found->second;
			}
			else
			{
				meshByKey.emplace(key, meshIndex);
				meshes.push_back({});
				meshes.back().texture = texture;
				meshes.back().renderClass = renderClass;
				meshes.back().brushEntity = currentBrushEntity;
				meshes.back().sky = currentSky;
				meshes.back().billboard = currentBillboard;
				meshes.back().spinDegPerSec = currentSpin;
				meshes.back().spinPivot[0] = currentPivot[0];
				meshes.back().spinPivot[1] = currentPivot[1];
				meshes.back().pulseFx = currentPulse;
				meshes.back().opacity = opacity;
				meshes.back().tint[0] = tint[0];
				meshes.back().tint[1] = tint[1];
				meshes.back().tint[2] = tint[2];
			}

			std::vector<float> &verts = meshes[meshIndex].verts;
			std::vector<float> &lightUv = meshes[meshIndex].lightUv;
			const float *ps[3] = { a, b, c };
			const float *ns[3] = { na, nb, nc };
			const float *uvs[3] = { uva, uvb, uvc };
			const float *luvs[3] = { luva, luvb, luvc };
			for (int i = 0; i < 3; ++i)
			{
				verts.push_back(ps[i][0]);
				verts.push_back(ps[i][1]);
				verts.push_back(ps[i][2]);
				verts.push_back(ns[i][0]);
				verts.push_back(ns[i][1]);
				verts.push_back(ns[i][2]);
				verts.push_back(uvs[i][0]);
				verts.push_back(uvs[i][1]);
				lightUv.push_back(luvs[i][0]);
				lightUv.push_back(luvs[i][1]);
			}
			return meshIndex;
		};

		const auto isRotatingClass = [](const std::string &classname) -> bool
		{
			// Brushes of rotating entities are stored relative to the
			// entity origin (compile tools move them there); the engine
			// positions them at runtime. Everything else is absolute.
			return classname == "func_rotating" ||
				classname == "func_door_rotating" ||
				classname == "func_rot_button" ||
				classname == "momentary_rot_button";
		};

		const bool hasImages = textures.size() == map.Textures().size();

		struct PolyVert
		{
			float p[3];
			float uv[2];
		};
		struct PolyFace
		{
			std::vector<PolyVert> poly;
			float normal[3];
			int texture = -1;
			size_t faceIndex = 0;
			bool sky = false;
			bool billboard = false;
		};

		auto craneBoxShifts = ComputeCraneBoxShifts(map);
		// zs_conspiracy: the dark plank is absent at the start and lies at its
		// key (= floor) position afterwards: no train shift, hidden before.
		std::set<int> craneDropBoxes;
		{
			std::set<std::string> unusedNames;
			ComputeCraneDrop(ParseEntityKeyValues(map.EntitiesText()), &unusedNames, &craneDropBoxes);
		}
		for (int b : craneDropBoxes)
			craneBoxShifts[b] = { 0.0f, 0.0f, 0.0f };
		const auto trainShifts = ComputeTrainShifts(map, craneBoxShifts, map.PlacedAfterSwitch());
		const auto strayTrainShifts = ComputeStrayTrainShifts(map);
		const auto effectTrains = map.PlacedAfterSwitch() ? std::set<int>() : ComputeScriptedEffectTrains(map);
		const auto goneBreakables = map.PlacedAfterSwitch() ? ComputeScriptedBreakables(map) : std::set<int>();
		for (const auto &[doorModel, doorShift] : ComputeStartOpenDoorShifts(map, map.PlacedAfterSwitch()))
		{
			auto &dst = craneBoxShifts[doorModel];
			for (int k = 0; k < 3; ++k)
				dst[static_cast<size_t>(k)] += doorShift[static_cast<size_t>(k)];
		}
		for (size_t modelIndex = 0; modelIndex < map.Models().size(); ++modelIndex)
		{
			const BspMap::Model &model = map.Models()[modelIndex];
			currentBrushEntity = modelIndex != 0;

			// Brush-entity render properties (worldspawn keeps defaults).
			const auto erit = map.EntityRenders().find(static_cast<int>(modelIndex));
			const BspMap::EntityRender *er =
				erit != map.EntityRenders().end() ? &erit->second : nullptr;
			if (effectTrains.count(static_cast<int>(modelIndex)) != 0)
				continue;
			if (!map.PlacedAfterSwitch() && craneDropBoxes.count(static_cast<int>(modelIndex)) != 0)
				continue;
			if (goneBreakables.count(static_cast<int>(modelIndex)) != 0)
				continue;
			const int renderMode = er != nullptr ? er->renderMode : 0;
			const int renderAmt = er != nullptr ? er->renderAmt : 255;

			// Invisible in the engine, skip the same way: a non-normal
			// rendermode with renderamt 0 (e.g. a toggled-off func_wall).
			// NOTE: a black rendercolor does NOT mean invisible -- CSO
			// mappers leave "0 0 0" as "no tint", and the game still renders
			// such additive glow/beam entities (verified against in-game
			// screenshots of red light pillars and the green buy-zone
			// frame, both additive with rendercolor 0 0 0).
			if (renderMode != 0 && renderAmt <= 0)
				continue;
			// Shelter-mode game objects (zsh_hellride): the red "S" cubes
			// (shelter_team_mineral) and the grey barricade frame
			// (shelter_team_barricade) are spawned/activated by the game mode
			// and never show on the untouched map.
			if (er != nullptr && (er->classname == "shelter_team_mineral" ||
				er->classname == "shelter_team_barricade"))
				continue;

			// Trigger-type brush entities are EF_NODRAW in the game (every
			// trigger_*, ladders, CS zones): mappers often leave real
			// textures on them (as_oilrig: the VIP safety zone in front of
			// the helicopter), so they must not be drawn.
			if (er != nullptr)
			{
				const std::string &cn = er->classname;
				if (cn.compare(0, 8, "trigger_") == 0 || cn == "func_ladder" ||
					cn == "func_buyzone" || cn == "func_bomb_target" ||
					cn == "func_hostage_rescue" || cn == "func_vip_safetyzone" ||
					cn == "func_escapezone")
					continue;
			}

			BspRenderClass renderClass = BspRenderClass::Opaque;
			float opacity = 1.0f;
			float tint[3] = { 1.0f, 1.0f, 1.0f };
			// A rendercolor of 0 0 0 (or no key at all) means "no tint":
			// only a real color modulates the texture.
			const bool hasTint = er != nullptr && er->hasRenderColor &&
				(er->renderColor[0] > 0.0f || er->renderColor[1] > 0.0f ||
					er->renderColor[2] > 0.0f);
			if (er != nullptr)
			{
				switch (renderMode)
				{
				case 5:  // Additive: glow/beams, brightness by renderamt.
				case 3:  // Glow (env_glow sprites): additive light as well.
					renderClass = BspRenderClass::Additive;
					opacity = renderAmt / 255.0f;
					// rendercolor does NOT tint additive brush entities in the
					// engine (only "color" rendermode 1 uses it): zs_decoy's
					// "E" signs carry rendercolor 0 0 255 yet show the texture's
					// own teal in game. Brightness comes from renderamt alone.
					break;
				case 2:  // Texture: plain alpha blend by renderamt.
					renderClass = BspRenderClass::Blend;
					opacity = renderAmt / 255.0f;
					break;
				case 4:  // Solid (kRenderTransAlpha): opaque, masked
					// textures are cut out by the shader's alpha test.
					// renderamt does not make it translucent.
					break;
				case 1:  // Color: flat rendercolor tint.
					if (hasTint)
					{
						tint[0] = er->renderColor[0] / 255.0f;
						tint[1] = er->renderColor[1] / 255.0f;
						tint[2] = er->renderColor[2] / 255.0f;
					}
					break;
				default:
					break;
				}
			}

			// Gather the model's face polygons, then emit them all with the
			// entity origin shift (rotating entities store origin-relative
			// faces, see below).
			std::vector<PolyFace> modelFaces;
			// renderfx 24 + rendercolor (CSO glow shell: pushable boxes green,
			// breakable boxes red). Measured on in-game screenshots: the surface
			// keeps its texture detail and is multiplied by rendercolor/255
			// (brown box * 125 255 125 -> dark green ~47 90 38; planks * 200 0 0
			// -> dark red), so it is drawn as a tint, not as additive light.
			if (er != nullptr && er->renderFx == 24 && hasTint &&
				renderClass == BspRenderClass::Opaque)
			{
				tint[0] = er->renderColor[0] / 255.0f;
				tint[1] = er->renderColor[1] / 255.0f;
				tint[2] = er->renderColor[2] / 255.0f;
			}

			for (int32_t f = 0; f < model.numFaces; ++f)
			{
				const size_t faceIndex = static_cast<size_t>(model.firstFace + f);
				if (faceIndex >= map.Faces().size())
					continue;
				const BspMap::Face &face = map.Faces()[faceIndex];
				if (face.numEdges < 3)
					continue;
				if (face.texInfo >= map.TexInfos().size())
					continue;

				const BspMap::TexInfo &info = map.TexInfos()[face.texInfo];
				// Faces textured "sky*" are kept (flagged): the viewer shows the
				// skybox through them, as the engine does. Other TEX_SPECIAL
				// faces (clip, aaatrigger, ...) stay invisible.
				bool isSky = false;
				if (info.miptex >= 0 &&
					static_cast<size_t>(info.miptex) < map.Textures().size())
				{
					const std::string &tn = map.Textures()[info.miptex].name;
					isSky = tn.size() >= 3 &&
						(tn[0] == 's' || tn[0] == 'S') &&
						(tn[1] == 'k' || tn[1] == 'K') &&
						(tn[2] == 'y' || tn[2] == 'Y');
				}
				if (!isSky && (info.flags & BspMap::TexSpecial) != 0)
					continue;  // clip, aaatrigger, ...
				if (face.plane >= map.Planes().size())
					continue;
				if (info.miptex < 0 ||
					static_cast<size_t>(info.miptex) >= map.Textures().size())
					continue;
				// Engine-invisible tool textures by name (null, clip, sky...)
				// for faces the tools didn't flag TEX_SPECIAL.
				if (!isSky && IsBspSpecialTextureName(map.Textures()[info.miptex].name))
					continue;

				// Face normal = plane normal, flipped for back-side faces.
				const BspMap::Vec3 &planeNormal = map.Planes()[face.plane].normal;
				float normal[3] = { planeNormal.x, planeNormal.y, planeNormal.z };
				if (face.planeSide != 0)
				{
					normal[0] = -normal[0];
					normal[1] = -normal[1];
					normal[2] = -normal[2];
				}
				const float length = std::sqrt(
					normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
				if (!(length > 0.0f))
					continue;
				normal[0] /= length;
				normal[1] /= length;
				normal[2] /= length;

				int texture = -1;
				float invW = 0.0f, invH = 0.0f;
				const BspMap::Miptex &miptex = map.Textures()[info.miptex];
				if (miptex.width > 0 && miptex.height > 0 &&
					hasImages && !textures[info.miptex].isNull())
				{
					texture = info.miptex;
					invW = 1.0f / miptex.width;
					invH = 1.0f / miptex.height;
				}

				// Gather the face polygon through surfedges -> edges.
				PolyFace polyFace;
				polyFace.texture = texture;
				polyFace.sky = isSky;
				polyFace.faceIndex = faceIndex;
				polyFace.normal[0] = normal[0];
				polyFace.normal[1] = normal[1];
				polyFace.normal[2] = normal[2];
				polyFace.poly.reserve(face.numEdges);
				bool bad = false;
				for (uint16_t e = 0; e < face.numEdges; ++e)
				{
					const size_t seIndex = static_cast<size_t>(face.firstEdge + e);
					if (seIndex >= map.SurfEdges().size())
					{
						bad = true;
						break;
					}
					const int32_t surfEdge = map.SurfEdges()[seIndex];
					const size_t edgeIndex = static_cast<size_t>(
						surfEdge >= 0 ? surfEdge : -surfEdge);
					if (edgeIndex >= map.Edges().size())
					{
						bad = true;
						break;
					}
					const BspMap::Edge &edge = map.Edges()[edgeIndex];
					const uint16_t vertIndex =
						surfEdge >= 0 ? edge.v[0] : edge.v[1];
					if (vertIndex >= map.Vertices().size())
					{
						bad = true;
						break;
					}
					const BspMap::Vec3 &v = map.Vertices()[vertIndex];
					PolyVert pv;
					pv.p[0] = v.x;
					pv.p[1] = v.y;
					pv.p[2] = v.z;
					pv.uv[0] = (v.x * info.s.x + v.y * info.s.y + v.z * info.s.z + info.shiftS) * invW;
					pv.uv[1] = (v.x * info.t.x + v.y * info.t.y + v.z * info.t.z + info.shiftT) * invH;
					if (!(pv.uv[0] == pv.uv[0] && pv.uv[1] == pv.uv[1]))
					{
						bad = true;  // NaN guard.
						break;
					}
					polyFace.poly.push_back(pv);
				}
				if (bad || polyFace.poly.size() < 3)
					continue;
				modelFaces.push_back(std::move(polyFace));
			}

			// Water surfaces can be stored several times on top of each other
			// (zs_rendezvous3: the sea is four stacked copies, up and down
			// faces at z=-128 and z=-129). Drawn all together they z-fight.
			// Of identical outlines facing the same way within two units of
			// each other keep only the one furthest along the normal, i.e.
			// the one nearest to a viewer on the visible side.
			{
				const auto isWater = [&](const PolyFace &pf)
				{
					return pf.texture >= 0 &&
						pf.texture < static_cast<int>(map.Textures().size()) &&
						!map.Textures()[static_cast<size_t>(pf.texture)].name.empty() &&
						map.Textures()[static_cast<size_t>(pf.texture)].name[0] == '!';
				};
				std::map<std::array<int, 9>, size_t> kept;
				std::vector<char> drop(modelFaces.size(), 0);
				bool anyDrop = false;
				for (size_t i = 0; i < modelFaces.size(); ++i)
				{
					const PolyFace &pf = modelFaces[i];
					if (!isWater(pf))
						continue;
					float mnx = pf.poly[0].p[0], mxx = mnx;
					float mny = pf.poly[0].p[1], mxy = mny;
					for (const PolyVert &pv : pf.poly)
					{
						mnx = std::min(mnx, pv.p[0]); mxx = std::max(mxx, pv.p[0]);
						mny = std::min(mny, pv.p[1]); mxy = std::max(mxy, pv.p[1]);
					}
					const std::array<int, 9> key = {
						static_cast<int>(std::lround(mnx)), static_cast<int>(std::lround(mxx)),
						static_cast<int>(std::lround(mny)), static_cast<int>(std::lround(mxy)),
						static_cast<int>(std::lround(pf.normal[0] * 100.0f)),
						static_cast<int>(std::lround(pf.normal[1] * 100.0f)),
						static_cast<int>(std::lround(pf.normal[2] * 100.0f)),
						pf.texture, static_cast<int>(pf.poly.size()) };
					const float h = pf.normal[0] * pf.poly[0].p[0] +
						pf.normal[1] * pf.poly[0].p[1] + pf.normal[2] * pf.poly[0].p[2];
					const auto found = kept.find(key);
					if (found == kept.end())
					{
						kept.emplace(key, i);
						continue;
					}
					const PolyFace &other = modelFaces[found->second];
					const float oh = other.normal[0] * other.poly[0].p[0] +
						other.normal[1] * other.poly[0].p[1] + other.normal[2] * other.poly[0].p[2];
					if (std::fabs(h - oh) > 2.0f)
						continue;
					if (h > oh)
					{
						drop[found->second] = 1;
						found->second = i;
					}
					else
						drop[i] = 1;
					anyDrop = true;
				}
				if (anyDrop)
				{
					std::vector<PolyFace> filtered;
					filtered.reserve(modelFaces.size());
					for (size_t i = 0; i < modelFaces.size(); ++i)
						if (!drop[i])
							filtered.push_back(std::move(modelFaces[i]));
					modelFaces = std::move(filtered);
				}
			}

			if (modelFaces.empty())
				continue;

			// Rotating entities store brush faces relative to their entity
			// origin (compile tools move them there); the engine positions
			// them at runtime. Everything else stores absolute coordinates.
			float shift[3] = { 0.0f, 0.0f, 0.0f };
			// The same holds for every other brush entity that carries an
			// origin brush (func_train, func_tracktrain, func_door, ...): the
			// compiler stores its faces relative to the origin and writes the
			// "origin" key (dm_dam: the dam door is a func_train whose faces
			// sit around 0,0,0 until the engine moves it to its path). Such
			// a key is trusted unless the stored faces already enclose it.
			bool applyOrigin = er != nullptr && isRotatingClass(er->classname);
			if (!applyOrigin && er != nullptr && currentBrushEntity &&
				(er->origin[0] != 0.0f || er->origin[1] != 0.0f || er->origin[2] != 0.0f))
			{
				float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
				for (const PolyFace &pf : modelFaces)
					for (const PolyVert &pv : pf.poly)
						for (int k = 0; k < 3; ++k)
						{
							mn[k] = std::min(mn[k], pv.p[k]);
							mx[k] = std::max(mx[k], pv.p[k]);
						}
				const bool inside =
					er->origin[0] >= mn[0] - 16.0f && er->origin[0] <= mx[0] + 16.0f &&
					er->origin[1] >= mn[1] - 16.0f && er->origin[1] <= mx[1] + 16.0f &&
					er->origin[2] >= mn[2] - 16.0f && er->origin[2] <= mx[2] + 16.0f;
				applyOrigin = !inside;
			}
			if (applyOrigin)
			{
				shift[0] = er->origin[0];
				shift[1] = er->origin[1];
				shift[2] = er->origin[2];
			}
			{
				const auto tr = trainShifts.find(static_cast<int>(modelIndex));
				if (tr != trainShifts.end())
				{
					shift[0] = tr->second[0];
					shift[1] = tr->second[1];
					shift[2] = tr->second[2];
				}
			}
			{
				const auto sy = strayTrainShifts.find(static_cast<int>(modelIndex));
				if (sy != strayTrainShifts.end())
				{
					shift[0] = sy->second[0];
					shift[1] = sy->second[1];
					shift[2] = sy->second[2];
				}
			}
			{
				const auto cb = craneBoxShifts.find(static_cast<int>(modelIndex));
				if (cb != craneBoxShifts.end())
				{
					shift[0] += cb->second[0];
					shift[1] += cb->second[1];
					shift[2] += cb->second[2];
				}
			}

			// Crossed-plane markers (zs_overflow1 "safemark": two vertical
			// planes crossing on one axis, additive/blended). The game shows
			// them as ONE quad that always faces the player, so keep a single
			// plane and let the viewer turn it (billboard).
			// A glowing func_rotating that is one flat plane (zs_victor2's yellow
			// captain figure, a 2-unit thick pictogram spinning on its axis) is
			// shown the same way: it is edge-on and invisible from half the angles
			// at its fixed pose, while in game it turns and is seen from all sides.
			// Limited to the pulsing (renderfx 2) pictograms: other spinning signs
			// (zs_envymask's door "E" markers) are not verified against the game.
			const bool spinningSign = er != nullptr && er->classname == "func_rotating" &&
				er->renderFx == 2;
			if (er != nullptr && currentBrushEntity &&
				(renderMode == 5 || renderMode == 3 || renderMode == 2))
			{
				std::vector<size_t> vertical;
				bool sloped = false;
				float bmn[2] = { 1e30f, 1e30f }, bmx[2] = { -1e30f, -1e30f };
				for (size_t i = 0; i < modelFaces.size(); ++i)
				{
					const PolyFace &pf = modelFaces[i];
					const float nz = std::fabs(pf.normal[2]);
					if (nz < 0.01f)
					{
						vertical.push_back(i);
						for (const PolyVert &pv : pf.poly)
						{
							bmn[0] = std::min(bmn[0], pv.p[0] + shift[0]);
							bmx[0] = std::max(bmx[0], pv.p[0] + shift[0]);
							bmn[1] = std::min(bmn[1], pv.p[1] + shift[1]);
							bmx[1] = std::max(bmx[1], pv.p[1] + shift[1]);
						}
					}
					else if (nz < 0.99f)
						sloped = true;
				}
				if (!sloped && vertical.size() >= (spinningSign ? 2u : 4u))
				{
					const float px = 0.5f * (bmn[0] + bmx[0]);
					const float py = 0.5f * (bmn[1] + bmx[1]);
					bool ok = true;
					std::vector<std::array<float, 2>> dirs;
					for (size_t idx : vertical)
					{
						const PolyFace &pf = modelFaces[idx];
						const float l = std::hypot(pf.normal[0], pf.normal[1]);
						const float nx = pf.normal[0] / l, ny = pf.normal[1] / l;
						const float d = nx * (pf.poly[0].p[0] + shift[0] - px) +
							ny * (pf.poly[0].p[1] + shift[1] - py);
						if (std::fabs(d) > 1.5f)
						{
							ok = false;
							break;
						}
						bool known = false;
						for (const auto &q : dirs)
							if (std::fabs(q[0] * nx + q[1] * ny) > 0.99f)
								known = true;
						if (!known)
							dirs.push_back({ nx, ny });
					}
					if (ok && dirs.size() >= (spinningSign ? 1u : 2u) &&
						(!spinningSign || dirs.size() == 1))
					{
						const float n0x = dirs[0][0], n0y = dirs[0][1];
						std::vector<PolyFace> rebuilt;
						for (size_t i = 0; i < modelFaces.size(); ++i)
						{
							const bool isVert = std::find(vertical.begin(), vertical.end(), i) !=
								vertical.end();
							if (!isVert)
							{
								rebuilt.push_back(std::move(modelFaces[i]));
								continue;
							}
							PolyFace pf = std::move(modelFaces[i]);
							const float l = std::hypot(pf.normal[0], pf.normal[1]);
							// Keep only the halves of ONE plane, seen from one side.
							if ((pf.normal[0] * n0x + pf.normal[1] * n0y) / l < 0.99f)
								continue;
							for (PolyVert &pv : pf.poly)
							{
								const float wx = pv.p[0] + shift[0] - px;
								const float wy = pv.p[1] + shift[1] - py;
								const float s = wx * (-n0y) + wy * n0x;
								pv.p[0] = px - shift[0];
								pv.p[1] = py + s - shift[1];
							}
							pf.normal[0] = px;
							pf.normal[1] = py;
							pf.normal[2] = 0.0f;
							pf.billboard = true;
							rebuilt.push_back(std::move(pf));
						}
						modelFaces = std::move(rebuilt);
					}
				}
			}

			// Animated brush signs. renderfx 1..4 pulse the opacity of translucent /
			// additive entities (GoldSrc: renderamt +- 16 or 64, slow/fast). A
			// func_rotating that is a thin, small sign (the "E" prompts) turns about
			// its vertical axis; pictograms already collapsed to a camera-facing
			// quad (renderfx 2 above) are not turned again.
			currentSpin = 0.0f;
			currentPulse = 0;
			if (er != nullptr && currentBrushEntity && renderMode != 0 && renderMode != 4 &&
				er->renderFx >= 1 && er->renderFx <= 4)
				currentPulse = er->renderFx;
			if (er != nullptr && currentBrushEntity && er->classname == "func_rotating" &&
				renderMode != 0 && renderMode != 4 && renderAmt > 0 && !spinningSign &&
				(er->spawnFlags & 1) != 0 && (er->spawnFlags & (4 | 8)) == 0 &&
				std::fabs(er->rotSpeed) > 0.01f)
			{
				const float dx = model.maxs.x - model.mins.x;
				const float dy = model.maxs.y - model.mins.y;
				const float dz = model.maxs.z - model.mins.z;
				if (std::min(dx, dy) <= 4.0f && std::max(dx, dy) <= 64.0f && dz <= 96.0f)
				{
					currentSpin = (er->spawnFlags & 2) != 0 ? -er->rotSpeed : er->rotSpeed;
					currentPivot[0] = 0.5f * (model.mins.x + model.maxs.x) + shift[0];
					currentPivot[1] = 0.5f * (model.mins.y + model.maxs.y) + shift[1];
				}
			}

			for (const PolyFace &polyFace : modelFaces)
			{
				currentSky = polyFace.sky;
				currentBillboard = polyFace.billboard;
				const std::vector<PolyVert> &polygon = polyFace.poly;
				// Lightmap block for this face (white texel = fullbright).
				// Light coords use the STORED vertex positions: rotating
				// submodels keep the lightmap baked at their stored spot.
				const BspLightmapData::Rect *lmRect = nullptr;
				const BspFaceLight *lmBlock = nullptr;
				const BspMap::TexInfo *lmInfo = nullptr;
				if (!polyFace.billboard && polyFace.faceIndex < lightmaps->rects.size() &&
					!lightmaps->rects[polyFace.faceIndex].empty &&
					polyFace.faceIndex < lightmaps->blocks.size() &&
					lightmaps->blocks[polyFace.faceIndex].width > 0 &&
					polyFace.faceIndex < map.Faces().size())
				{
					const BspMap::Face &lmFace = map.Faces()[polyFace.faceIndex];
					if (lmFace.texInfo < map.TexInfos().size())
					{
						lmRect = &lightmaps->rects[polyFace.faceIndex];
						lmBlock = &lightmaps->blocks[polyFace.faceIndex];
						lmInfo = &map.TexInfos()[lmFace.texInfo];
					}
				}
				const auto lightUvFor = [&](const float *p, float *luv)
				{
					if (lmRect == nullptr || lmBlock == nullptr || lmInfo == nullptr)
					{
						luv[0] = whiteU;
						luv[1] = whiteV;
						return;
					}
					const float sT = p[0] * lmInfo->s.x + p[1] * lmInfo->s.y +
						p[2] * lmInfo->s.z + lmInfo->shiftS;
					const float tT = p[0] * lmInfo->t.x + p[1] * lmInfo->t.y +
						p[2] * lmInfo->t.z + lmInfo->shiftT;
					const float lx = std::clamp((sT - lmBlock->texMins[0]) / 16.0f,
						0.0f, static_cast<float>(lmBlock->width - 1));
					const float ly = std::clamp((tT - lmBlock->texMins[1]) / 16.0f,
						0.0f, static_cast<float>(lmBlock->height - 1));
					luv[0] = (lmRect->x + lx + 0.5f) / atlasW;
					luv[1] = (lmRect->y + ly + 0.5f) / atlasH;
				};
				// Fan triangulation in stored order (culling is off, so the
				// winding only has to be consistent, not a specific handedness).
				size_t faceMesh = static_cast<size_t>(-1);
				int faceTris = 0;
				const float lx0 = 0.0f;
				const float ly0 = 0.0f;
				const float lz0 = 0.0f;
				for (size_t i = 1; i + 1 < polygon.size(); ++i)
				{
					float a[3] = {
						polygon[0].p[0] + shift[0] + lx0,
						polygon[0].p[1] + shift[1] + ly0,
						polygon[0].p[2] + shift[2] + lz0 };
					float b[3] = {
						polygon[i].p[0] + shift[0] + lx0,
						polygon[i].p[1] + shift[1] + ly0,
						polygon[i].p[2] + shift[2] + lz0 };
					float c[3] = {
						polygon[i + 1].p[0] + shift[0] + lx0,
						polygon[i + 1].p[1] + shift[1] + ly0,
						polygon[i + 1].p[2] + shift[2] + lz0 };
					float luva[2], luvb[2], luvc[2];
					lightUvFor(polygon[0].p, luva);
					lightUvFor(polygon[i].p, luvb);
					lightUvFor(polygon[i + 1].p, luvc);
					faceMesh = emitTriangle(polyFace.texture, renderClass, opacity, tint,
						a, b, c,
						polyFace.normal, polyFace.normal, polyFace.normal,
						polygon[0].uv, polygon[i].uv, polygon[i + 1].uv,
						luva, luvb, luvc);
					++faceTris;
				}
				if (currentBrushEntity && faceTris > 0)
				{
					BspCpuMesh::InstanceRange span;
					span.count = faceTris * 3;
					span.first = static_cast<int>(meshes[faceMesh].verts.size() / 8) -
						span.count;
					span.instance = static_cast<int>(modelIndex);
					meshes[faceMesh].brushSpans.push_back(span);
				}
				if (!currentBrushEntity && faceTris > 0)
				{
					BspCpuMesh::InstanceRange span;
					span.count = faceTris * 3;
					span.first = static_cast<int>(meshes[faceMesh].verts.size() / 8) -
						span.count;
					span.instance = static_cast<int>(polyFace.faceIndex);
					meshes[faceMesh].faceSpans.push_back(span);
				}
				if (renderClass == BspRenderClass::Blend && faceTris > 0)
				{
					// All triangles of one face land in the same mesh,
					// contiguously, so the range is the tail of its buffer.
					BspCpuMesh::FaceRange range;
					range.count = faceTris * 3;
					range.first = static_cast<int>(meshes[faceMesh].verts.size() / 8) -
						range.count;
					float c3[3] = { 0.0f, 0.0f, 0.0f };
					for (const PolyVert &pv : polygon)
						for (int k = 0; k < 3; ++k)
							c3[k] += pv.p[k] + shift[k];
					for (int k = 0; k < 3; ++k)
						range.center[k] = c3[k] / static_cast<float>(polygon.size());
					if (!currentBrushEntity)
						range.face = static_cast<int>(polyFace.faceIndex);
					else
						range.model = static_cast<int>(modelIndex);
					meshes[faceMesh].faceRanges.push_back(range);
				}
			}
		}

		return meshes;
	}

	BspLightSampler::BspLightSampler(const BspMap &map,
		const BspLightmapData &lightmaps)
		: map_(map), lightmaps_(lightmaps)
	{
		if (map.Models().empty())
			return;
		const BspMap::Model &world = map.Models()[0];
		std::vector<BspMap::Vec3> verts;
		for (int32_t i = 0; i < world.numFaces; ++i)
		{
			const size_t fi = static_cast<size_t>(world.firstFace + i);
			if (fi >= map.Faces().size() || fi >= lightmaps.rects.size() ||
				lightmaps.rects[fi].empty)
				continue;
			const BspMap::Face &face = map.Faces()[fi];
			if (face.plane >= map.Planes().size() ||
				face.texInfo >= map.TexInfos().size())
				continue;
			const BspMap::Plane &plane = map.Planes()[face.plane];
			if (std::fabs(plane.normal.z) < 0.05f)
				continue;  // Walls can't be hit by a vertical ray.
			if (!GatherFaceVerts(map, face, verts))
				continue;
			FaceRef ref;
			ref.nx = plane.normal.x;
			ref.ny = plane.normal.y;
			ref.nz = plane.normal.z;
			ref.dist = plane.dist;
			ref.face = fi;
			ref.minX = ref.maxX = verts[0].x;
			ref.minY = ref.maxY = verts[0].y;
			for (const auto &v : verts)
			{
				ref.minX = std::min(ref.minX, v.x);
				ref.maxX = std::max(ref.maxX, v.x);
				ref.minY = std::min(ref.minY, v.y);
				ref.maxY = std::max(ref.maxY, v.y);
				ref.xy.push_back(v.x);
				ref.xy.push_back(v.y);
			}
			faces_.push_back(std::move(ref));
		}
	}

	bool BspLightSampler::SampleNear(float x, float y, float z,
		float maxRadius, float *uv) const
	{
		if (Sample(x, y, z, uv))
			return true;
		for (float radius = 32.0f; radius <= maxRadius; radius *= 2.0f)
		{
			for (int i = 0; i < 12; ++i)
			{
				const float a = static_cast<float>(i) * (6.2831853f / 12.0f);
				if (Sample(x + std::cos(a) * radius, y + std::sin(a) * radius, z, uv))
					return true;
			}
		}
		return false;
	}

	bool BspLightSampler::Sample(float x, float y, float z, float *uv) const
	{
		constexpr float kMaxDrop = 4096.0f;
		const FaceRef *best = nullptr;
		float bestZ = z - kMaxDrop;
		float bestPz = 0.0f;
		for (const FaceRef &f : faces_)
		{
			if (x < f.minX || x > f.maxX || y < f.minY || y > f.maxY)
				continue;
			const float pz = (f.dist - f.nx * x - f.ny * y) / f.nz;
			if (pz > z || pz <= bestZ)
				continue;
			// Point-in-polygon (even-odd) on the XY projection.
			bool inside = false;
			const size_t n = f.xy.size() / 2;
			for (size_t i = 0, j = n - 1; i < n; j = i++)
			{
				const float xi = f.xy[i * 2], yi = f.xy[i * 2 + 1];
				const float xj = f.xy[j * 2], yj = f.xy[j * 2 + 1];
				if (((yi > y) != (yj > y)) &&
					(x < (xj - xi) * (y - yi) / (yj - yi) + xi))
					inside = !inside;
			}
			if (!inside)
				continue;
			best = &f;
			bestZ = pz;
			bestPz = pz;
		}
		if (best == nullptr || lightmaps_.atlas.isNull())
			return false;

		const BspMap::Face &face = map_.Faces()[best->face];
		const BspMap::TexInfo &info = map_.TexInfos()[face.texInfo];
		const BspLightmapData::Rect &rect = lightmaps_.rects[best->face];
		const BspFaceLight &block = lightmaps_.blocks[best->face];
		if (block.width <= 0 || block.height <= 0)
			return false;
		const float s = x * info.s.x + y * info.s.y + bestPz * info.s.z + info.shiftS;
		const float t = x * info.t.x + y * info.t.y + bestPz * info.t.z + info.shiftT;
		const float lx = std::clamp((s - block.texMins[0]) / 16.0f,
			0.0f, static_cast<float>(block.width - 1));
		const float ly = std::clamp((t - block.texMins[1]) / 16.0f,
			0.0f, static_cast<float>(block.height - 1));
		uv[0] = (rect.x + lx + 0.5f) / static_cast<float>(lightmaps_.atlas.width());
		uv[1] = (rect.y + ly + 0.5f) / static_cast<float>(lightmaps_.atlas.height());
		return true;
	}

	namespace
	{
		constexpr int32_t kContentsSolid = -2;

		// Collects the non-solid leaves under `start` (a node index, or a
		// leaf encoded as ~index).
		[[maybe_unused]] void CollectSubtreeLeaves(const BspMap &map, int start, std::vector<int> &out)
		{
			std::vector<int> stack{ start };
			while (!stack.empty())
			{
				const int c = stack.back();
				stack.pop_back();
				if (c < 0)
				{
					const int leaf = ~c;
					if (leaf > 0 && leaf < static_cast<int>(map.Leaves().size()) &&
						map.Leaves()[static_cast<size_t>(leaf)].contents != kContentsSolid)
						out.push_back(leaf);
					continue;
				}
				if (c >= static_cast<int>(map.Nodes().size()))
					continue;
				const BspMap::Node &node = map.Nodes()[static_cast<size_t>(c)];
				stack.push_back(node.children[0]);
				stack.push_back(node.children[1]);
			}
		}
	}

	BspBoxLeaves BspLeavesTouchingBox(const BspMap &map,
		const float mins[3], const float maxs[3])
	{
		BspBoxLeaves result;
		if (map.Models().empty() || map.Nodes().empty())
			return result;
		std::vector<int> stack{ map.Models()[0].headNodes[0] };
		while (!stack.empty())
		{
			const int c = stack.back();
			stack.pop_back();
			if (c < 0)
			{
				const int leaf = ~c;
				if (leaf > 0 && leaf < static_cast<int>(map.Leaves().size()) &&
					map.Leaves()[static_cast<size_t>(leaf)].contents != kContentsSolid)
					result.leaves.push_back(leaf);
				continue;
			}
			if (c >= static_cast<int>(map.Nodes().size()))
				continue;
			const BspMap::Node &node = map.Nodes()[static_cast<size_t>(c)];
			if (node.plane < 0 || node.plane >= static_cast<int32_t>(map.Planes().size()))
				continue;
			const BspMap::Plane &pl = map.Planes()[static_cast<size_t>(node.plane)];
			// Box corners nearest/farthest along the plane normal.
			const float n[3] = { pl.normal.x, pl.normal.y, pl.normal.z };
			float dmax = 0.0f, dmin = 0.0f;
			for (int k = 0; k < 3; ++k)
			{
				dmax += n[k] * (n[k] >= 0.0f ? maxs[k] : mins[k]);
				dmin += n[k] * (n[k] >= 0.0f ? mins[k] : maxs[k]);
			}
			const bool front = dmax >= pl.dist;  // some of the box in front
			const bool back = dmin < pl.dist;    // some of the box behind
			if (front && back && result.headNode < 0)
				result.headNode = c;
			if (front)
				stack.push_back(node.children[0]);
			if (back)
				stack.push_back(node.children[1]);
		}
		std::sort(result.leaves.begin(), result.leaves.end());
		result.leaves.erase(std::unique(result.leaves.begin(), result.leaves.end()),
			result.leaves.end());
		return result;
	}

	std::vector<int> BspEntityLeaves(const BspMap &map,
		const float mins[3], const float maxs[3])
	{
		// Only leaves the box really touches. The server widens entities with
		// more than 16 leaves to the whole subtree of their head node, but that
		// kept long props (poles with wires, cranes) alive in map segments far
		// away from the camera, which the game does not show, so it is not
		// applied here.
		return BspLeavesTouchingBox(map, mins, maxs).leaves;
	}

	bool BspOrLeafPvs(const BspMap &map, int leaf, std::vector<uint8_t> &row)
	{
		if (leaf <= 0 || leaf >= static_cast<int>(map.Leaves().size()) ||
			map.Models().empty())
			return false;
		const int32_t offset = map.Leaves()[static_cast<size_t>(leaf)].visOffset;
		const std::vector<uint8_t> &vis = map.Visibility();
		if (offset < 0 || static_cast<size_t>(offset) >= vis.size())
			return false;
		const size_t rowBytes = static_cast<size_t>((map.Models()[0].visLeafs + 7) >> 3);
		if (row.size() < rowBytes)
			return false;
		size_t in = static_cast<size_t>(offset);
		size_t out = 0;
		while (out < rowBytes && in < vis.size())
		{
			if (vis[in] != 0)
			{
				row[out++] |= vis[in++];
				continue;
			}
			if (in + 1 >= vis.size())
				break;
			out += vis[in + 1];  // run of zero bytes
			in += 2;
		}
		return true;
	}
}
