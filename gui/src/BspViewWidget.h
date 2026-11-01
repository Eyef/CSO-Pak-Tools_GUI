#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QSet>
#include <QTimer>
#include <QVector3D>

#include <QtMath>

#include <cmath>
#include <array>
#include <memory>
#include <vector>

#include "BspMap.h"

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

namespace cso_gui
{
	// Forward direction for a fly-camera yaw/pitch pair (degrees):
	// yaw=0 looks along +X, yaw=90 along +Y, pitch=90 straight up.
	// Free function (not a member) so headless tests can use it without
	// instantiating the GL widget.
	QVector3D BspFlyForward(float yawDegrees, float pitchDegrees);

	// Layout-independent fly key: on a non-Latin layout (e.g. Russian)
	// QKeyEvent::key() reports the localized letter (Key_Cyrillic_ef for
	// the physical A key), so WASD-by-key() dies. The Windows scan code is
	// positional and layout-independent, so prefer it for the movement
	// keys; anything else passes through unchanged. Free function for
	// headless tests (scan codes are plain numbers there).
	int BspNormalizeFlyKey(int qtKey, unsigned nativeScanCode);

	// Interactive preview for GoldSrc .bsp maps. Builds one triangle mesh per
	// texture from BuildBspMeshes() and renders it with the same orbit/pan/
	// zoom camera as the .mdl viewer, with baked lightmaps. Every non-special
	// face of every model is drawn unless the optional PVS culling is on.
	class BspViewWidget final : public QOpenGLWidget, protected QOpenGLFunctions
	{
		Q_OBJECT

	public:
		explicit BspViewWidget(QWidget *parent = nullptr);
		~BspViewWidget() override;

		// textures aligns with map->Textures() (null image = missing, drawn
		// with a magenta fallback). Clears the view if map is null.
		void SetMap(std::shared_ptr<BspMap> map, std::vector<QImage> textures);
		void Clear();

		// Placed models/sprites from the map's point entities (cycler_mdl /
		// cycler_sprite .mdl files baked to triangles by the caller,
		// env_sprite images). Appended after SetMap(), uploaded together
		// with the brush meshes on the next paintGL. Mesh texture indices
		// address the accompanying textures vector (-1 = magenta fallback).
		// Prefer the batch AddPlacedSprites for sprites: every Add* call
		// resets the GPU state, so adding sprites one by one rebuilds the
		// whole scene per sprite on the next paint.
		void AddPlacedModels(std::vector<QImage> textures,
			std::vector<BspCpuMesh> meshes);
		struct PlacedSprite
		{
			int texture = -1;  // Index into glTextures_ (assigned at upload).
			QImage image;
			QVector3D origin;
			float widthUnits = 0.0f;
			float heightUnits = 0.0f;
			BspRenderClass renderClass = BspRenderClass::Opaque;
			float opacity = 1.0f;
			// SPR header type: 0 vp_parallel_upright, 1 facing_upright,
			// 2 vp_parallel (default billboard), 3 oriented, 4 vp_parallel_oriented.
			int orientation = 2;
			// Quad extents relative to the entity origin along the sprite's
			// right/up axes (frame origin * scale). All zero = centered.
			float left = 0.0f, right = 0.0f, up = 0.0f, down = 0.0f;
			float angles[3] = { 0.0f, 0.0f, 0.0f };  // pitch, yaw, roll
			// Map lighting for sprites that are not self-luminous (Normal /
			// Texture render modes, e.g. grass): when `lit`, the sprite takes
			// the lightmap texel at lightUv (atlas UV, the light found under
			// the entity like the engine's R_LightPoint) instead of being
			// drawn fullbright. Additive/glow sprites stay unlit.
			bool lit = false;
			float lightUv[2] = { 0.0f, 0.0f };
		};
		void AddPlacedSprite(QImage image, QVector3D origin,
			float widthUnits, float heightUnits,
			BspRenderClass renderClass, float opacity);
		void AddPlacedSprites(std::vector<PlacedSprite> sprites);

		// Vertical field of view in degrees (clamped to 30..120).
		float FieldOfView() const { return fovDegrees_; }
		void SetFieldOfView(float degrees);

		// How light is shown: 0 = fitted to in-game screenshots (default),
		// 1 = plain recipe of goldsrc-bsp-viewer: pow(texture * lightmap,
		// 1/1.6) for everything, no light floor, no model ambient.
		int LightingMode() const { return lightingMode_; }
		void SetLightingMode(int mode);
		void SetModelFill(float fill); // 0..1
		// Skybox faces in the order ft, bk, lf, rt, up, dn (GoldSrc suffixes
		// <sky>ft/bk/lf/rt/up/dn). Null images are skipped; all-null = no sky.
		void SetSkybox(const std::array<QImage, 6> &faces);
		// Show/hide the sky (default hidden). Faces textured "sky" are then
		// drawn with the skybox, or not at all.
		void SetSkyVisible(bool visible);
		void SetBrightness(float scale); // lightmap multiplier, 1 = unchanged
		// Game-like visibility (default off): placed models and sprites are
		// drawn only when the game would send them to a player standing at the
		// camera, i.e. when a leaf they touch is in the camera leaf's PVS. The
		// map's own geometry is never culled. Without a valid camera leaf
		// (orbit camera outside the map) everything stays visible.
		bool PvsCulling() const { return pvsCulling_; }
		void SetPvsCulling(bool enabled);

		// Engine-like back-face culling of map brush faces (on by default):
		// a face is drawn only from its front side, as GoldSrc does. Off shows
		// both sides (useful when a map's faces look wrongly missing).
		bool BackfaceCulling() const { return backfaceCull_; }
		void SetBackfaceCulling(bool enabled);

		// env_fog of the current map (distance fog, exponential-squared).
		// HasFog() tells whether the map defines one; the toggle (off by
		// default) switches the fog on to compare with the game look.
		bool HasFog() const { return fogDensity_ > 0.0f; }
		bool FogEnabled() const { return fogEnabled_; }
		void SetFogEnabled(bool enabled);

		// renderfx pulse of translucent brush signs (GoldSrc kRenderFxPulse*:
		// renderamt +- 16/64 on a 2 or 8 rad/s sine). Off by default; spinning
		// signs are not affected.
		bool PulseEnabled() const { return pulseEnabled_; }
		void SetPulseEnabled(bool enabled);

		bool Wireframe() const { return wireframe_; }
		void SetWireframe(bool enabled);

		void ResetView();

		enum class CameraMode
		{
			Orbit,
			Fly,  // WASD + mouse look, for interiors orbit can't reach.
		};

		void SetCameraMode(CameraMode mode);
		CameraMode GetCameraMode() const { return cameraMode_; }

		size_t TriangleCount() const { return triangleCount_; }

	signals:
		// Diagnostic line for the status bar while PVS culling is on: camera
		// position, its leaves and how many entities are drawn.
		void PvsStatus(const QString &text);

	protected:
		void initializeGL() override;
		void paintGL() override;
		void resizeGL(int w, int h) override;
		void mousePressEvent(QMouseEvent *event) override;
		void mouseMoveEvent(QMouseEvent *event) override;
		void mouseReleaseEvent(QMouseEvent *event) override;
		void wheelEvent(QWheelEvent *event) override;
		void keyPressEvent(QKeyEvent *event) override;
		void keyReleaseEvent(QKeyEvent *event) override;

	private slots:
		void OnFlyTick();

	private:
		struct GpuMesh
		{
			std::vector<float> verts;  // x,y,z,nx,ny,nz,u,v
			int texture = -1;
			QOpenGLVertexArrayObject vao;
			QOpenGLBuffer vbo;
			QOpenGLBuffer lightVbo;  // 2 floats per vertex: lightmap UVs
			GLsizei count = 0;
			// Brush-entity rendering from BuildBspMeshes (rendermode): the
			// pass this mesh belongs to, opacity (renderamt/255) and tint
			// (rendercolor/255), applied via uOpacity/uTint uniforms.
			BspRenderClass renderClass = BspRenderClass::Opaque;
			float opacity = 1.0f;
			float tint[3] = { 1.0f, 1.0f, 1.0f };
			bool chrome = false;
			bool sky = false;  // faces textured "sky": drawn as skybox lookup
			bool water = false;  // "!" liquid faces: only the side facing the eye
			bool billboard = false;  // crossed-plane marker: quad turned to the eye
			// Animated brush signs (BspCpuMesh::spinDegPerSec / pulseFx): the viewer
			// turns the mesh about the vertical axis through spinPivot and
			// modulates its opacity by time.
			float spinDegPerSec = 0.0f;
			float spinPivot[2] = { 0.0f, 0.0f };
			int pulseFx = 0;
			float lightMul[3] = { 1.0f, 1.0f, 1.0f };  // studio light multiplier
			bool studio = false;  // Cull back faces (placed .mdl geometry).
			bool brushEntity = false;  // Brush-entity faces: depth-biased.
			// Placed models: owning entity (unmerged meshes), or the vertex
			// range of each entity inside a merged buffer (PVS culling).
			int instance = -1;
			std::vector<BspCpuMesh::InstanceRange> instances;
			std::vector<BspCpuMesh::InstanceRange> faceSpans;  // world faces
			std::vector<BspCpuMesh::InstanceRange> brushSpans;  // brush entities
			// Blend meshes only: drawable chunks, sorted back to front each
			// frame (see paintGL's translucent pass).
			std::vector<BspCpuMesh::FaceRange> faceRanges;
		};

		static QImage FallbackTexture();

		void BuildGpuMeshes();
		void UpdateAnimTimer();  // repaint timer only while something animates
		void SetupMatrices(int width, int height);
		void ComputeBounds();
		QVector3D OrbitEye() const;
		void SeedFlyFromOrbit();
		// Recomputes which placed entities are inside the PVS of `eye`
		// (see SetPvsCulling); cheap when the eye stays in the same leaves.
		void UpdatePvs(const QVector3D &eye);
		bool InstanceVisible(int instance) const;
		bool FaceVisible(int face) const;
		bool BrushVisible(int model) const;
		// Camera basis for sprite billboards (same cameras as SetupMatrices).
		void CameraBasis(QVector3D &forward, QVector3D &right, QVector3D &up) const;

		std::shared_ptr<BspMap> map_;
		std::vector<QImage> textures_;
		// Placed-model CPU data (see AddPlacedModels/AddPlacedSprite),
		// cleared by SetMap()/Clear().
		std::vector<QImage> placedTextures_;
		std::vector<BspCpuMesh> placedMeshes_;
		std::vector<PlacedSprite> placedSprites_;

		std::vector<std::unique_ptr<QOpenGLTexture>> glTextures_;
		std::vector<std::unique_ptr<GpuMesh>> meshes_;
		bool gpuBuilt_ = false;
		size_t triangleCount_ = 0;

		std::unique_ptr<QOpenGLShaderProgram> program_;
		int mvpLoc_ = -1;
		int normalMatrixLoc_ = -1;
		int lightDirLoc_ = -1;
		int fillLightDirLoc_ = -1;
		int unlitLoc_ = -1;
		int texLoc_ = -1;
		int useTexLoc_ = -1;
		int colorLoc_ = -1;
		int opacityLoc_ = -1;
		int tintLoc_ = -1;
		int lightMulLoc_ = -1;
		int chromeLoc_ = -1;
		int additiveLoc_ = -1;
		int studioLoc_ = -1;
		int lightmapLoc_ = -1;
		// Baked lightmap atlas texture (uploaded in BuildGpuMeshes).
		std::unique_ptr<QOpenGLTexture> lightmapTex_;
		std::array<QImage, 6> skyImages_;
		std::array<std::unique_ptr<QOpenGLTexture>, 6> skyTextures_;
		int skyLoc_ = -1;
		int waterLoc_ = -1;
		int alphaTestLoc_ = -1;  // uAlphaTest: 0 for worldspawn faces
		int cullBackLoc_ = -1;  // uCullBack: brush-entity faces draw front side only
		int billboardLoc_ = -1;
		int eyeLoc_ = -1;
		bool skyVisible_ = false;
		// White-texel UV of the current atlas (for meshes without light
		// data, e.g. placed models, and for sprite billboards).
		float lightWhiteU_ = 0.5f;
		float lightWhiteV_ = 0.5f;
		float kneeOn_ = 0.0f;  // 1 = map has clipped lightmap texels (roll-off on)

		// PVS culling of placed entities (see SetPvsCulling).
		bool pvsCulling_ = false;
		bool pvsActive_ = false;   // culling in effect for the current camera
		bool pvsDirty_ = true;     // leaves/instances changed: recompute
		std::vector<int> pvsEyeLeaves_;
		std::vector<std::vector<int>> instanceLeaves_;  // by placed-entity index
		std::vector<uint8_t> instanceVisible_;
		std::vector<uint8_t> faceVisible_;  // world faces in the camera's PVS
		std::vector<std::vector<int>> modelLeaves_;  // by brush-model index
		std::vector<uint8_t> modelVisible_;
		std::vector<std::vector<int>> spriteLeaves_;    // parallel to placedSprites_
		std::vector<uint8_t> spriteVisible_;
		QString pvsStatus_;

		bool wireframe_ = false;
		bool backfaceCull_ = true;
		bool fogEnabled_ = false;  // off by default: browsing wants a clear view
		float fogDensity_ = 0.0f;
		float fogColor_[3] = { 0.0f, 0.0f, 0.0f };
		bool FogActive() const { return fogEnabled_ && fogDensity_ > 0.0f; }
		float fovDegrees_ = 60.0f;
		int lightingMode_ = 0;
		int lightModeLoc_ = -1;
		float modelFill_ = 0.5f;
		float brightness_ = 1.0f;
		int brightnessLoc_ = -1;
		int modelFillLoc_ = -1;

		float yaw_ = -90.0f;
		float pitch_ = 20.0f;
		float distance_ = 3000.0f;
		QPoint lastMouse_;
		bool dragging_ = false;
		bool panning_ = false;

		float centerX_ = 0.0f, centerY_ = 0.0f, centerZ_ = 0.0f;
		float radius_ = 1000.0f;

		CameraMode cameraMode_ = CameraMode::Orbit;
		// Fly state: free position + look angles, seeded from the orbit
		// camera when switching modes.
		QVector3D flyPos_;
		float flyYaw_ = 0.0f;
		float flyPitch_ = 0.0f;
		QSet<int> flyKeys_;
		QTimer *flyTimer_ = nullptr;
		// Repaints ~30x/s while the map has spinning / pulsing signs.
		QTimer *animTimer_ = nullptr;
		bool pulseEnabled_ = false;

		// Reusable buffers for the per-frame sprite billboard quads.
		QOpenGLBuffer spriteVbo_;
		QOpenGLVertexArrayObject spriteVao_;
		bool spriteBuffersInit_ = false;
	};
}
