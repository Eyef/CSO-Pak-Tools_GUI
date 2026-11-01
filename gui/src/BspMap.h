#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// A point entity that places an external model/sprite file on the map
	// (cycler_sprite/cycler_mdl with .mdl in CSO, env_sprite with .spr,
	// supply boxes, ...). The viewer draws these on top of the brush
	// geometry: .mdl baked at rest pose (or the "sequence" frame),
	// transformed by origin/angles/scale; .spr as a camera-facing quad.
	struct BspPlacedEntity
	{
		std::string classname;
		std::string model;  // "model" value, e.g. models/crate.mdl
		bool isSprite = false;
		float origin[3] = { 0.0f, 0.0f, 0.0f };
		float angles[3] = { 0.0f, 0.0f, 0.0f };  // pitch, yaw, roll (degrees)
		float scale = 1.0f;
		int skin = 0;
		int body = 0;       // "body" key: picks one submodel per bodypart
		int sequence = 0;   // "sequence" key; the engine defaults to sequence 0 (not the
		                    // rest pose, which can differ from it by a 90 deg root yaw)
		int renderMode = 0;
		int renderAmt = 255;
		int spawnFlags = 0;
		// State after a cycler_mdl model switch that only changes the sequence
		// (_mdl0 == _mdl1, e.g. zs_victor2's jeeps going through their explode
		// animation): the animation has played to its end, so the last frame is
		// shown (the wreck), not the first one.
		bool holdLastFrame = false;
		// "renderfx" + "rendercolor". CSO's renderfx 22 is a light multiplier:
		// the model's lighting is scaled by rendercolor/255 (mappers use it to
		// darken props/bosses that would otherwise look too bright).
		int renderFx = 0;
		float renderColor[3] = { 0.0f, 0.0f, 0.0f };
	};

	// Parser for GoldSrc (version 30) .bsp map files, the format CSO maps
	// use. Layout reference: the BspParser.ts module of goldsrc-bsp-viewer
	// (header/lump walk + per-struct field order) and HLAM's
	// StudioModelFileFormat-style struct sizes.
	//
	// Load() throws std::runtime_error on truncated or unsupported data.
	// Individual corrupt textures never abort the whole map: a miptex whose
	// pixel/palette range falls outside the file simply keeps a null image.
	class BspMap
	{
	public:
		struct Vec3
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};

		struct Lump
		{
			int32_t offset = 0;
			int32_t length = 0;
		};

		enum LumpIndex
		{
			LumpEntities = 0,
			LumpPlanes = 1,
			LumpTextures = 2,
			LumpVertices = 3,
			LumpVisibility = 4,
			LumpNodes = 5,
			LumpTexInfo = 6,
			LumpFaces = 7,
			LumpLighting = 8,
			LumpClipNodes = 9,
			LumpLeaves = 10,
			LumpMarkSurfaces = 11,
			LumpEdges = 12,
			LumpSurfEdges = 13,
			LumpModels = 14,
			LumpCount = 15,
		};

		// texinfo flags (only the low bit matters for viewing).
		static constexpr int32_t TexSpecial = 1;  // sky, clip, aaatrigger...

		struct Edge
		{
			uint16_t v[2] = { 0, 0 };
		};

		struct Face
		{
			uint16_t plane = 0;
			uint16_t planeSide = 0;
			int32_t firstEdge = 0;
			uint16_t numEdges = 0;
			uint16_t texInfo = 0;
			uint8_t styles[4] = { 0, 0, 0, 0 };
			int32_t lightOffset = 0;
		};

		struct TexInfo
		{
			Vec3 s;
			float shiftS = 0.0f;
			Vec3 t;
			float shiftT = 0.0f;
			int32_t miptex = 0;
			int32_t flags = 0;
		};

		struct Plane
		{
			Vec3 normal;
			float dist = 0.0f;
			int32_t type = 0;
		};

		struct Miptex
		{
			std::string name;
			int32_t width = 0;
			int32_t height = 0;
			// Null image = external texture (lives in a .wad, offsets[0] == 0)
			// or a corrupt embedded one. QImage is RGB888 when present.
			QImage image;
		};

		struct Model
		{
			Vec3 mins, maxs, origin;
			int32_t headNodes[4] = { 0, 0, 0, 0 };
			int32_t visLeafs = 0;
			int32_t firstFace = 0;
			int32_t numFaces = 0;
		};

		struct Node
		{
			int32_t plane = 0;
			int16_t children[2] = { 0, 0 };
			int16_t mins[3] = { 0, 0, 0 };
			int16_t maxs[3] = { 0, 0, 0 };
			uint16_t firstFace = 0;
			uint16_t numFaces = 0;
		};

		struct Leaf
		{
			int32_t contents = 0;
			int32_t visOffset = 0;
			int16_t mins[3] = { 0, 0, 0 };
			int16_t maxs[3] = { 0, 0, 0 };
			uint16_t firstMarkSurface = 0;
			uint16_t numMarkSurfaces = 0;
			uint8_t ambient[4] = { 0, 0, 0, 0 };
		};

		struct ClipNode
		{
			int32_t plane = 0;
			int16_t children[2] = { 0, 0 };
		};

		// Render properties of one brush-model entity ("model" "*N" in the
		// entities lump). GoldSrc rendermodes: 0 normal, 1 color, 2 texture
		// (alpha blend by renderamt), 3 glow, 4 solid, 5 additive (adds
		// light, scaled by renderamt/255 and tinted by rendercolor).
		// func_rotating / func_door_rotating / func_rot_button store their
		// brush faces relative to this origin (the engine positions them at
		// runtime); every other brush entity stores absolute coordinates.
		struct EntityRender
		{
			int modelIndex = 0;
			std::string classname;
			float origin[3] = { 0.0f, 0.0f, 0.0f };
			int renderMode = 0;
			int renderAmt = 255;
			float renderColor[3] = { 255.0f, 255.0f, 255.0f };
			bool hasRenderColor = false;
			// "renderfx": CSO uses 24 as a coloured glow shell (pushable
			// boxes green, breakable boxes red).
			int renderFx = 0;
			// func_rotating: "speed" (deg/s) and "spawnflags" (1 start on,
			// 2 reverse, 4/8 X/Y axis); other entities leave them at 0.
			float rotSpeed = 0.0f;
			int spawnFlags = 0;
		};

		static constexpr int32_t kSupportedVersion = 30;

		BspMap() = default;

		// Parse from raw file bytes. Throws std::runtime_error on failure.
		void Load(const std::vector<uint8_t> &data);

		bool IsValid() const { return valid_; }
		int32_t Version() const { return version_; }

		const std::array<Lump, LumpCount> &Lumps() const { return lumps_; }
		const std::string &EntitiesText() const { return entities_; }
		const std::vector<Vec3> &Vertices() const { return vertices_; }
		const std::vector<Edge> &Edges() const { return edges_; }
		const std::vector<int32_t> &SurfEdges() const { return surfEdges_; }
		const std::vector<Face> &Faces() const { return faces_; }
		const std::vector<TexInfo> &TexInfos() const { return texInfos_; }
		const std::vector<Plane> &Planes() const { return planes_; }
		// One entry per texture directory slot (empty name + null image for
		// -1 slots), so TexInfo::miptex indexes straight into it.
		const std::vector<Miptex> &Textures() const { return textures_; }
		const std::vector<uint8_t> &Lighting() const { return lighting_; }
		const std::vector<uint8_t> &Visibility() const { return visibility_; }
		const std::vector<Model> &Models() const { return models_; }
		const std::vector<Node> &Nodes() const { return nodes_; }
		const std::vector<Leaf> &Leaves() const { return leaves_; }
		const std::vector<uint16_t> &MarkSurfaces() const { return markSurfaces_; }
		const std::vector<ClipNode> &ClipNodes() const { return clipNodes_; }
		// Per brush-model render properties parsed from the entities lump
		// (worldspawn = model 0 keeps defaults). Keyed by model index, so
		// only entities that actually reference "*N" appear here.
		const std::map<int, BspMap::EntityRender> &EntityRenders() const { return entityRenders_; }
		// Point entities placing .mdl/.spr files (see ParseBspPlacedEntities).
		const std::vector<BspPlacedEntity> &PlacedEntities() const { return placedEntities_; }
		// Re-reads the placed entities for the map-start (false) or the
		// after-the-switch (true) state of the cycler_mdl model switchers.
		void SetPlacedState(bool afterSwitch);
		bool PlacedAfterSwitch() const { return placedAfterSwitch_; }

	private:
		bool valid_ = false;
		int32_t version_ = 0;
		std::array<Lump, LumpCount> lumps_{};
		std::string entities_;
		std::vector<Vec3> vertices_;
		std::vector<Edge> edges_;
		std::vector<int32_t> surfEdges_;
		std::vector<Face> faces_;
		std::vector<TexInfo> texInfos_;
		std::vector<Plane> planes_;
		std::vector<Miptex> textures_;
		std::vector<uint8_t> lighting_;
		std::vector<uint8_t> visibility_;
		std::vector<Model> models_;
		std::vector<Node> nodes_;
		std::vector<Leaf> leaves_;
		std::vector<uint16_t> markSurfaces_;
		std::vector<ClipNode> clipNodes_;
		std::map<int, BspMap::EntityRender> entityRenders_;
		std::vector<BspPlacedEntity> placedEntities_;
		bool placedAfterSwitch_ = false;
	};

	// Parses the entities lump text into per brush-model render properties
	// (see EntityRender). Only entities with a "model" "*N" key produce an
	// entry. Malformed pairs are ignored, never throw. Free function so
	// headless tests can exercise it without a full BSP file.
	// afterSwitch: the state after the cut scene (hidden signs that an
	// env_render "..._on" turns on become visible, see BspMap::SetPlacedState).
	std::map<int, BspMap::EntityRender> ParseBspEntityRenders(const std::string &entities,
		bool afterSwitch = false);

	// All entities whose "model" names a .mdl/.spr file (brush "*N" models
	// excluded). Never throws. Free function for headless tests.
	// afterSwitch = false: the state at map start; true: the state after the
	// scripted switch (CSO cycler_mdl "_mdl0" -> "_mdl1", e.g. the destroyed
	// antennas and the boss of zs_obstacle1). "models/null.mdl" = not shown.
	// `models` (optional) lets trains with "model_ex" and no origin key be placed
	// at their first path_corner (needs the stub brush bounds).
	std::vector<BspPlacedEntity> ParseBspPlacedEntities(const std::string &entities,
		bool afterSwitch = false, const std::vector<BspMap::Model> *models = nullptr);

	// How a mesh is drawn by the viewer (mirrors the .mdl viewer's opaque /
	// additive split, plus a plain alpha-blend pass for rendermode 2).
	enum class BspRenderClass
	{
		Opaque = 0,   // Normal/respawn rendering, depth write on.
		Blend = 1,    // rendermode 2 "texture": alpha blend by renderamt.
		Additive = 2,  // rendermode 5: adds light (SRC_ALPHA, ONE), unlit.
	};

	// One triangle soup per texture slot, in x,y,z,nx,ny,nz,u,v floats.
	// Pure geometry math (no OpenGL), shared by the viewer widget and
	// headless tests. Faces are taken from every model at their stored
	// coordinates; TEX_SPECIAL faces (sky, clip, ...) are skipped.
	// `textures` must align with map.Textures() (null image = missing);
	// such faces still emit geometry with UV 0 and texture index -1, letting
	// the caller substitute a fallback texture.
	struct BspCpuMesh
	{
		int texture = -1;  // Texture directory slot, or -1 for fallback.
		std::vector<float> verts;
		// Brush-entity rendering (see BuildBspMeshes): render class plus
		// per-mesh opacity (renderamt/255) and color tint (rendercolor/255).
		BspRenderClass renderClass = BspRenderClass::Opaque;
		float opacity = 1.0f;
		float tint[3] = { 1.0f, 1.0f, 1.0f };
		// Studio models: multiplier applied to the light (renderfx 22).
		float lightMul[3] = { 1.0f, 1.0f, 1.0f };
		// Lightmap UVs into BuildBspLightmaps' atlas, 2 floats per emitted
		// vertex (same count as verts/8). Always filled: faces without a
		// lightmap point at the atlas' white texel (fullbright).
		std::vector<float> lightUv;
		// Baked studio (.mdl) model geometry: drawn with GoldSource's
		// back-face culling (CW front faces) like the .mdl viewer, so
		// coincident inner/outer shells don't z-fight.
		bool studio = false;
		bool chrome = false;  // STUDIO_NF_CHROME texture: env-mapped UVs
		bool sky = false;     // brush faces textured "sky" (skybox lookup)
		// Brush entity drawn as a camera-facing quad (crossed-plane markers):
		// vertex normals carry the pivot xy, the viewer turns the quad.
		bool billboard = false;
		// Spinning pictogram (func_rotating sign, e.g. the pulsing "E" prompts):
		// turns about the vertical axis through spinPivot at spinDegPerSec
		// (0 = static); the viewer animates it by time.
		float spinDegPerSec = 0.0f;
		float spinPivot[2] = { 0.0f, 0.0f };
		// renderfx pulse of a translucent/additive brush entity: 1 slow, 2 fast,
		// 3 slow wide, 4 fast wide (0 = none); the viewer modulates the opacity.
		int pulseFx = 0;
		// Alpha test of masked "{" textures. GoldSrc only cuts the transparent
		// pixels out of brushes drawn with rendermode Solid (4) (and the
		// texture/glow/additive modes); worldspawn and brush entities in Normal
		// (0) / Color (1) mode draw them opaque, the transparent colour black.
		bool alphaTest = true;
		// Placed entity this geometry belongs to (index into the map's placed
		// entities; -1 = not an entity). The viewer uses it to hide models the
		// game would not send (PVS culling). `instances` lists the vertex
		// ranges per entity once several entities were merged into one buffer.
		int instance = -1;
		struct InstanceRange
		{
			int first = 0;
			int count = 0;
			int instance = -1;
		};
		std::vector<InstanceRange> instances;
		// World (model 0) faces only: vertex range of every face, `instance`
		// holding the map face index. Lets the viewer draw only the faces of
		// leaves in the camera's PVS.
		std::vector<InstanceRange> faceSpans;
		// Brush-entity faces: vertex range per face, `instance` = model index.
		std::vector<InstanceRange> brushSpans;
		// Faces of a brush entity (model *N, not the world): drawn with a
		// small depth bias so they win over coplanar world faces instead of
		// z-fighting with them.
		bool brushEntity = false;
		// Per-face vertex ranges (brush faces of Blend meshes). Translucent
		// geometry has no depth write, so the viewer must draw it back to
		// front; the engine sorts the same way. first/count are in vertices,
		// center is the world-space polygon centroid.
		struct FaceRange
		{
			int first = 0;
			int count = 0;
			float center[3] = { 0.0f, 0.0f, 0.0f };
			int face = -1;  // map face index for world faces, else -1
			int model = -1;  // brush-model index for brush-entity faces, else -1
		};
		std::vector<FaceRange> faceRanges;
	};

	// One face's combined lightmap block decoded from all stored light styles.
	struct BspFaceLight
	{
		int width = 0;
		int height = 0;
		std::vector<uint8_t> rgb;  // width*height*3 bytes, row-major
		// Texture-space (texel) origin of the block: worldToLight subtracts
		// this before dividing by 16.
		float texMins[2] = { 0.0f, 0.0f };
	};

	// Packed lightmap atlas for a map plus per-face placement.
	struct BspLightmapData
	{
		QImage atlas;  // RGB888; pixel (0,0) is always white (fullbright)
		struct Rect
		{
			int x = 0, y = 0, w = 0, h = 0;
			bool empty = true;
		};
		// One entry per map face (indexed by faceIndex); empty when the
		// face has no lightmap (lightOffset -1, style 255, corrupt, ...).
		std::vector<Rect> rects;
		// Decoded blocks aligned with rects (for tests/debugging).
		std::vector<BspFaceLight> blocks;
	};

	// Builds the lightmap atlas from LumpLighting. Stored styles are treated as
	// active and summed (clamped to 255); dynamic style animation/toggle state
	// is not simulated. Never throws: bad faces get empty rects (fullbright).
	BspLightmapData BuildBspLightmaps(const BspMap &map);

	// Finds the lightmap texel under a world point, like the engine's
	// R_LightPoint: a ray goes straight down from the point and the first
	// lit world face it hits supplies the light. Used to light placed
	// models (props, vehicles, trees) with the lightmap at their feet.
	// The returned UV addresses `lightmaps.atlas` (the same atlas the viewer
	// builds, since packing is deterministic).
	class BspLightSampler
	{
	public:
		BspLightSampler(const BspMap &map, const BspLightmapData &lightmaps);

		// On success writes atlas UV (0..1) into uv[2] and returns true.
		bool Sample(float x, float y, float z, float *uv) const;

		// Like Sample, but when nothing lies directly below (props standing
		// on geometry the compiler discarded, e.g. towers on top of the map
		// boundary) it searches rings of growing radius around the point and
		// takes the nearest lit floor. Returns false only if none is found
		// within maxRadius.
		bool SampleNear(float x, float y, float z, float maxRadius, float *uv) const;

	private:
		struct FaceRef
		{
			float nx = 0, ny = 0, nz = 0, dist = 0;
			float minX = 0, maxX = 0, minY = 0, maxY = 0;
			std::vector<float> xy;  // polygon corners, x0,y0,x1,y1,...
			size_t face = 0;
		};
		const BspMap &map_;
		const BspLightmapData &lightmaps_;
		std::vector<FaceRef> faces_;
	};

	// Leaves of the world model whose volume a box touches (the engine's
	// SV_FindTouchedLeafs). Solid leaves are skipped. `headNode` is the first
	// node whose plane splits the box (-1 if none). Entities touching more
	// than 16 leaves are tested by the whole subtree under that node, as the
	// server does.
	struct BspBoxLeaves
	{
		std::vector<int> leaves;
		int headNode = -1;
	};
	BspBoxLeaves BspLeavesTouchingBox(const BspMap &map,
		const float mins[3], const float maxs[3]);
	// Leaves an entity occupying the box is tested against (the non-solid
	// leaves the box touches).
	std::vector<int> BspEntityLeaves(const BspMap &map,
		const float mins[3], const float maxs[3]);
	// Potentially-visible-set row of `leaf` (bit leaf-1 per leaf), appended
	// (OR-ed) into `row`, which must be (visLeafs+7)/8 bytes. Returns false
	// when the leaf has no vis data (the engine then shows everything).
	bool BspOrLeafPvs(const BspMap &map, int leaf, std::vector<uint8_t> &row);

	// One triangle soup per (texture, render class, ...) -- see the .cpp.
	// Builds the lightmap atlas first (into *lightmaps, must be non-null)
	// so every emitted vertex carries atlas UVs (white texel = fullbright).
	std::vector<BspCpuMesh> BuildBspMeshes(const BspMap &map,
		const std::vector<QImage> &textures, BspLightmapData *lightmaps);
}
