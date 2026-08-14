#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

#include <map>
#include <memory>
#include <vector>

#include "StudioModel.h"

class QMouseEvent;
class QWheelEvent;

namespace cso_gui
{
	// Interactive 3D preview for studiomdl (.mdl) models.
	// Renders the parsed triangles with textures and simple diffuse lighting,
	// supports bodypart / skin selection, mouse orbit and wheel zoom.
	class ModelViewWidget final : public QOpenGLWidget, protected QOpenGLFunctions
	{
		Q_OBJECT

	public:
		explicit ModelViewWidget(QWidget *parent = nullptr);
		~ModelViewWidget() override;

		// Set the model to display. Clears the view if model is null.
		void SetModel(std::shared_ptr<StudioModel> model);
		void Clear();

		// Rebuilds the GPU textures from the model's current StudioModel::Texture
		// images on the next paint. Needed after swapping in an externally
		// resolved texture (SetTextureImage), since BuildGpuMeshes only
		// uploads to the GPU once per SetModel() call otherwise.
		void ReloadTextures();

		// Bodyparts are bodygroups: the full model is composed of one submodel
		// chosen from each bodypart, all rendered together.
		int BodyPartCount() const;
		int SubModelCount(int bodyPart) const;
		int CurrentBodyPart() const;
		int CurrentSubModel(int bodyPart) const;
		void SetBodyPart(int index);       // bodypart whose group is edited
		void SetSubModel(int bodyPart, int subModel);

		int SkinFamilyCount() const;
		int CurrentSkinFamily() const;
		void SetSkinFamily(int index);

		bool Wireframe() const { return wireframe_; }
		void SetWireframe(bool enabled);

		// Recomputes the pivot/radius/distance fresh from the model's own
		// geometry and restores the default orbit angle, discarding any
		// pan/orbit/zoom drift from this viewing session. Useful after
		// panning gets a multi-piece model's camera lost (see the pan
		// comment in mouseMoveEvent for why that could happen).
		void ResetView();

		// Where a texture's additive-render decision came from.
		enum class AdditiveSource
		{
			None,           // Not additive.
			Flag,           // STUDIO_NF_ADDITIVE is set in the file.
			NamePrefix,     // Name starts with "$0a_"/"$0b_" (see AdditiveSourceFor).
			ManualOverride, // User explicitly toggled it, either way.
		};

		// Effective additive-render state for this texture right now: the
		// file's own STUDIO_NF_ADDITIVE flag, or an empirically observed CSO
		// naming convention (texture names starting with "$0a_"/"$0b_" are
		// consistently black-background effect textures across many models,
		// even when not flagged additive), or a manual override -- whichever
		// applies, with manual override always taking precedence.
		bool IsTextureAdditive(int textureIndex) const;
		AdditiveSource AdditiveSourceFor(int textureIndex) const;

		// Sets an explicit per-texture override (either direction), taking
		// precedence over the flag/name-based automatic detection above.
		// Cleared on the next SetModel() -- texture indices aren't
		// comparable across different files.
		void SetTextureForceAdditive(int textureIndex, bool force);
		bool HasManualAdditiveOverride(int textureIndex) const;

		// Rotates the (camera-attached) key + fill lights by these angles,
		// on top of their default direction. (0, 0) is the original look.
		void SetLightAngles(float yawDegrees, float pitchDegrees);
		float LightYaw() const { return lightYaw_; }
		float LightPitch() const { return lightPitch_; }

		int SequenceCount() const;
		const std::string &SequenceLabel(int index);
		void SetSequence(int index);
		int CurrentSequence() const { return sequenceIndex_; }
		int CurrentSequenceFrames() const;
		void SetSequenceFrame(int frame);   // integer frame index (0-based)
		int CurrentSequenceFrame() const { return sequenceFrame_; }
		void ApplyPose();                    // re-skin + rebuild current pose

		// Compute the model bounds. Returns false when no geometry exists.
		bool ModelBounds(float &minX, float &minY, float &minZ,
			float &maxX, float &maxY, float &maxZ) const;

	protected:
		void initializeGL() override;
		void paintGL() override;
		void resizeGL(int w, int h) override;
		void mousePressEvent(QMouseEvent *event) override;
		void mouseMoveEvent(QMouseEvent *event) override;
		void mouseReleaseEvent(QMouseEvent *event) override;
		void wheelEvent(QWheelEvent *event) override;

	private:
		struct GpuMesh
		{
			std::vector<float> verts;  // x,y,z,nx,ny,nz,u,v
			int texture = -1;
			bool additive = false;     // STUDIO_NF_ADDITIVE texture (glow/particle FX)
			QOpenGLVertexArrayObject vao;
			QOpenGLBuffer vbo;
			GLsizei count = 0;
		};

		void BuildGpuMeshes();
		void SetupMatrices(int width, int height);
		void ComputeBounds(bool resetDistance);

		std::shared_ptr<StudioModel> model_;

		std::vector<std::unique_ptr<QOpenGLTexture>> textures_;
		std::vector<std::unique_ptr<GpuMesh>> meshes_;
		std::vector<int> meshSkin_;  // skin family selected per mesh
		bool gpuBuilt_ = false;
		// Manual per-texture additive override, see SetTextureForceAdditive.
		// Present in the map = explicitly set by the user; value = forced state.
		std::map<int, bool> additiveOverrides_;

		std::unique_ptr<QOpenGLShaderProgram> program_;
		int mvpLoc_ = -1;
		int normalMatrixLoc_ = -1;
		int lightDirLoc_ = -1;
		int fillLightDirLoc_ = -1;
		int unlitLoc_ = -1;
		int texLoc_ = -1;
		int useTexLoc_ = -1;
		int colorLoc_ = -1;

		std::vector<int> bodyGroup_;  // selected submodel per bodypart
		int activeBodyPart_ = 0;      // bodypart whose group is being edited
		int skinFamily_ = 0;
		int sequenceIndex_ = -1;      // -1 = rest pose
		int sequenceFrame_ = 0;
		bool wireframe_ = false;
		float lightYaw_ = 0.0f;
		float lightPitch_ = 0.0f;

		float yaw_ = 0.0f;
		float pitch_ = 20.0f;
		float distance_ = 120.0f;
		float modelScale_ = 1.0f;
		QPoint lastMouse_;
		bool dragging_ = false;
		bool panning_ = false;

		float centerX_ = 0.0f, centerY_ = 0.0f, centerZ_ = 0.0f;
		float radius_ = 60.0f;
	};
}
