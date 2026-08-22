#include "ModelViewWidget.h"

#include <QMatrix4x4>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cso_gui
{
	namespace
	{
		// Near-plane distance used for the first-person projection in
		// SetupMatrices, duplicated here so ComputeBounds can skip vertices
		// that would be clipped anyway when estimating the required FOV.
		constexpr float kFirstPersonNear = 1.0f;

		constexpr const char *kVertexShader = R"(
#version 330 core
in vec3 aPosition;
in vec3 aNormal;
in vec2 aTexCoord;
uniform mat4 uMvp;
uniform mat4 uNormalMatrix;
uniform float uUseTex;
out vec3 vNormal;
out vec2 vTexCoord;
void main()
{
	gl_Position = uMvp * vec4(aPosition, 1.0);
	vNormal = mat3(uNormalMatrix) * aNormal;
	vTexCoord = aTexCoord;
}
)";

		constexpr const char *kFragmentShader = R"(
#version 330 core
in vec3 vNormal;
in vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uUseTex;
uniform vec3 uLightDir;
uniform vec3 uFillLightDir;
uniform vec4 uColor;
uniform float uUnlit;
out vec4 FragColor;
void main()
{
	vec3 n = normalize(vNormal);
	float diffMain = max(dot(n, normalize(uLightDir)), 0.0);
	float diffFill = max(dot(n, normalize(uFillLightDir)), 0.0);
	// The fill term is purely additive on top of the original single-light
	// formula (0.35 + 0.65*diffMain) rather than splitting its weight with
	// the key light, so a surface the key light already lit well is at
	// least as bright as before -- only surfaces the key light misses (and
	// the fill catches instead) get brighter than the old flat 0.35 floor.
	float light = min(0.35 + 0.65 * diffMain + 0.35 * diffFill, 1.0);
	vec4 base = vec4(uColor.rgb, 1.0);
	if (uUseTex > 0.5)
		base = texture(uTexture, vTexCoord);
	if (base.a < 0.5)
		discard;
	// Additive/glow textures (STUDIO_NF_ADDITIVE, e.g. fire, eye glow,
	// particle FX) represent self-illumination, not a lit surface -- draw
	// them at their own texture brightness instead of modulating by scene
	// lighting, which would incorrectly dim or tint them.
	vec3 shaded = mix(base.rgb * light, base.rgb, uUnlit);
	FragColor = vec4(shaded, base.a);
}
)";
	}

	ModelViewWidget::ModelViewWidget(QWidget *parent)
		: QOpenGLWidget(parent)
	{
		setFocusPolicy(Qt::StrongFocus);
	}

	ModelViewWidget::~ModelViewWidget()
	{
		// QOpenGLWidget cleans up its context; our buffers are child objects.
	}

	void ModelViewWidget::SetModel(std::shared_ptr<StudioModel> model)
	{
		SetModel(std::move(model), false);
	}

	void ModelViewWidget::SetModel(std::shared_ptr<StudioModel> model, bool firstPerson)
	{
		model_ = std::move(model);
		firstPersonModel_ = firstPerson;
		cameraMode_ = firstPerson ? CameraMode::FirstPerson : CameraMode::Orbit;
		bodyGroup_.clear();
		activeBodyPart_ = 0;
		skinFamily_ = 0;
		sequenceIndex_ = -1;
		sequenceFrame_ = 0;
		wireframe_ = false;
		gpuBuilt_ = false;
		additiveOverrides_.clear();
		// Rest reference pose faces -Y (the default camera at yaw=0 on the +X
		// side sees the character's right profile, +Y side is the back). Orbit
		// to the -Y side so the rest pose opens front-facing, matching the
		// sequence view.
		yaw_ = -90.0f;
		pitch_ = 20.0f;
		distance_ = 120.0f;
		if (model_)
		{
			// Default bodygroup: submodel 0 from every bodypart, as the engine
			// does. This renders the complete character instead of one piece.
			bodyGroup_.resize(model_->BodyParts().size(), 0);
			ComputeBounds(true);

			if (firstPersonModel_ && !model_->Sequences().empty())
			{
				int selected = -1;
				for (int i = 0; i < static_cast<int>(model_->Sequences().size()); ++i)
				{
					const QString label = QString::fromStdString(model_->Sequences()[static_cast<size_t>(i)].label);
					if (label.compare(QStringLiteral("idle"), Qt::CaseInsensitive) == 0)
					{
						selected = i;
						break;
					}
				}
				if (selected < 0)
				{
					for (int i = 0; i < static_cast<int>(model_->Sequences().size()); ++i)
					{
						const QString label = QString::fromStdString(model_->Sequences()[static_cast<size_t>(i)].label);
						if (label.startsWith(QStringLiteral("idle"), Qt::CaseInsensitive)
							&& label.size() > 4 && label.mid(4).toInt() > 0)
						{
							selected = i;
							break;
						}
					}
				}
				if (selected < 0)
				{
					for (int i = 0; i < static_cast<int>(model_->Sequences().size()); ++i)
					{
						if (QString::fromStdString(model_->Sequences()[static_cast<size_t>(i)].label)
							.startsWith(QStringLiteral("idle"), Qt::CaseInsensitive))
						{
							selected = i;
							break;
						}
					}
				}
				if (selected < 0)
					selected = 0;
				sequenceIndex_ = selected;
				sequenceFrame_ = 0;
				model_->ApplyFrame(sequenceIndex_, 0.0f);
				ComputeBounds(false);
			}
		}

		update();
	}

	void ModelViewWidget::SetCameraMode(CameraMode mode)
	{
		if (mode == CameraMode::FirstPerson && !firstPersonModel_)
			return;
		if (cameraMode_ == mode)
			return;
		cameraMode_ = mode;
		emit CameraModeChanged(static_cast<int>(cameraMode_));
		update();
	}

	void ModelViewWidget::SetFirstPersonFieldOfView(float fov)
	{
		firstPersonFov_ = std::clamp(fov, 30.0f, 120.0f);
		emit FirstPersonFieldOfViewChanged(firstPersonFov_);
		update();
	}

	void ModelViewWidget::LeaveFirstPerson()
	{
		if (cameraMode_ != CameraMode::FirstPerson)
			return;

		cameraMode_ = CameraMode::Orbit;
		yaw_ = -90.0f;
		pitch_ = 20.0f;
		ComputeBounds(true);
		emit CameraModeChanged(static_cast<int>(cameraMode_));
	}

	void ModelViewWidget::Clear()
	{
		SetModel(nullptr);
	}

	int ModelViewWidget::BodyPartCount() const
	{
		return model_ ? static_cast<int>(model_->BodyParts().size()) : 0;
	}

	int ModelViewWidget::SubModelCount(int bodyPart) const
	{
		if (!model_ || bodyPart < 0 ||
			bodyPart >= static_cast<int>(model_->BodyParts().size()))
			return 0;
		return static_cast<int>(model_->BodyParts()[static_cast<size_t>(bodyPart)].models.size());
	}

	int ModelViewWidget::CurrentBodyPart() const
	{
		return activeBodyPart_;
	}

	int ModelViewWidget::CurrentSubModel(int bodyPart) const
	{
		if (bodyPart < 0 || bodyPart >= static_cast<int>(bodyGroup_.size()))
			return 0;
		return bodyGroup_[static_cast<size_t>(bodyPart)];
	}

	void ModelViewWidget::SetBodyPart(int index)
	{
		activeBodyPart_ = index;
	}

	void ModelViewWidget::SetSubModel(int bodyPart, int subModel)
	{
		if (bodyPart < 0 || bodyPart >= static_cast<int>(bodyGroup_.size()))
			return;
		const int count = SubModelCount(bodyPart);
		if (subModel < 0 || subModel >= count)
			return;
		if (bodyGroup_[static_cast<size_t>(bodyPart)] == subModel)
			return;
		bodyGroup_[static_cast<size_t>(bodyPart)] = subModel;
		gpuBuilt_ = false;
		update();
	}

	int ModelViewWidget::SkinFamilyCount() const
	{
		return model_ ? model_->SkinFamilyCount() : 0;
	}

	int ModelViewWidget::CurrentSkinFamily() const
	{
		return skinFamily_;
	}

	void ModelViewWidget::SetSkinFamily(int index)
	{
		skinFamily_ = index;
		gpuBuilt_ = false;
		update();
	}

	void ModelViewWidget::SetWireframe(bool enabled)
	{
		wireframe_ = enabled;
		update();
	}

	void ModelViewWidget::ResetView()
	{
		if (cameraMode_ == CameraMode::FirstPerson || firstPersonModel_)
		{
			cameraMode_ = CameraMode::FirstPerson;
			firstPersonFov_ = 74.0f;
			emit CameraModeChanged(static_cast<int>(cameraMode_));
			emit FirstPersonFieldOfViewChanged(firstPersonFov_);
			update();
			return;
		}

		cameraMode_ = CameraMode::Orbit;
		// Same default orbit angle SetModel() starts a fresh model at (see
		// the comment there about why -90/20, not 0/20).
		yaw_ = -90.0f;
		pitch_ = 20.0f;
		ComputeBounds(/*resetDistance=*/true); // Reads from model_ geometry directly, so this is unaffected by any pan drift.
		update();
	}

	bool ModelViewWidget::IsTextureAdditive(int textureIndex) const
	{
		return AdditiveSourceFor(textureIndex) != AdditiveSource::None;
	}

	ModelViewWidget::AdditiveSource ModelViewWidget::AdditiveSourceFor(int textureIndex) const
	{
		const auto overrideIt = additiveOverrides_.find(textureIndex);
		if (overrideIt != additiveOverrides_.end())
			return overrideIt->second ? AdditiveSource::ManualOverride : AdditiveSource::None;

		if (!model_ || textureIndex < 0 || textureIndex >= static_cast<int>(model_->Textures().size()))
			return AdditiveSource::None;

		const auto &tex = model_->Textures()[static_cast<size_t>(textureIndex)];
		if ((tex.flags & 0x20) != 0)
			return AdditiveSource::Flag;

		// Empirically observed CSO naming convention (confirmed across ~20
		// models by hand): textures named with these prefixes are
		// consistently black-background effect textures, even when the
		// file's own STUDIO_NF_ADDITIVE flag isn't set.
		const QString name = QString::fromStdString(tex.name);
		if (name.startsWith(QStringLiteral("$0a"), Qt::CaseInsensitive) ||
			name.startsWith(QStringLiteral("$0b"), Qt::CaseInsensitive))
			return AdditiveSource::NamePrefix;

		return AdditiveSource::None;
	}

	void ModelViewWidget::SetTextureForceAdditive(int textureIndex, bool force)
	{
		additiveOverrides_[textureIndex] = force;

		// Meshes already built (GPU buffers/textures don't need touching,
		// just which render pass each one belongs to) -- update in place
		// rather than a full rebuild. If nothing's been built yet, the next
		// BuildGpuMeshes() will pick this up via IsTextureAdditive() anyway.
		for (auto &gpu : meshes_)
		{
			if (gpu->texture == textureIndex)
				gpu->additive = IsTextureAdditive(gpu->texture);
		}

		update();
	}

	bool ModelViewWidget::HasManualAdditiveOverride(int textureIndex) const
	{
		return additiveOverrides_.count(textureIndex) > 0;
	}

	void ModelViewWidget::SetLightAngles(float yawDegrees, float pitchDegrees)
	{
		lightYaw_ = yawDegrees;
		lightPitch_ = pitchDegrees;
		update();
	}

	void ModelViewWidget::ReloadTextures()
	{
		if (!model_)
			return;
		// External textures swapped in after SetModel need the GPU side rebuilt:
		// BuildGpuMeshes uploads each StudioModel::Texture to a QOpenGLTexture
		// once per build, so flip the dirty flag to rebuild on the next paint.
		gpuBuilt_ = false;
		update();
	}

	int ModelViewWidget::SequenceCount() const
	{
		return model_ ? static_cast<int>(model_->Sequences().size()) : 0;
	}

	const std::string &ModelViewWidget::SequenceLabel(int index)
	{
		static const std::string kEmpty;
		if (!model_ || index < 0 ||
			index >= static_cast<int>(model_->Sequences().size()))
			return kEmpty;
		return model_->Sequences()[static_cast<size_t>(index)].label;
	}

	int ModelViewWidget::CurrentSequenceFrames() const
	{
		if (!model_ || sequenceIndex_ < 0 ||
			sequenceIndex_ >= static_cast<int>(model_->Sequences().size()))
			return 0;
		return std::max(model_->Sequences()[static_cast<size_t>(sequenceIndex_)].numframes, 0);
	}

	void ModelViewWidget::SetSequence(int index)
	{
		if (sequenceIndex_ == index)
			return;
		sequenceIndex_ = index;
		sequenceFrame_ = 0.0f;
		// Sequences' blended rest-frame faces +X, so look from the +X side;
		// the raw rest reference pose faces -Y instead, so orbit back there.
		yaw_ = (index < 0) ? -90.0f : 0.0f;
		ApplyPose();
		// Establish the pivot once for the newly selected sequence. Do not do
		// this for every frame: root motion and pose bounds would move the camera.
		ComputeBounds(false);
	}

	void ModelViewWidget::SetSequenceFrame(float frame)
	{
		const int frames = CurrentSequenceFrames();
		if (frames > 0)
			frame = std::clamp(frame, 0.0f, static_cast<float>(frames - 1));
		if (frame < 0)
			frame = 0;
		if (std::abs(sequenceFrame_ - frame) < 0.0001f)
			return;
		sequenceFrame_ = frame;
		ApplyPose();
	}

	void ModelViewWidget::ApplyPose()
	{
		if (!model_)
			return;
		// Re-skin the model into the requested sequence pose.
		model_->ApplyFrame(sequenceIndex_,
			sequenceIndex_ < 0 ? 0.0f : static_cast<float>(sequenceFrame_));
		gpuBuilt_ = false;  // rebuild vertex data next paint
		update();
	}

	void ModelViewWidget::ComputeBounds(bool resetDistance)
	{
		if (!model_)
		{
			centerX_ = centerY_ = centerZ_ = 0.0f;
			radius_ = 60.0f;
			if (resetDistance)
				distance_ = radius_ * 3.0f;
			return;
		}

		float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
		bool first = true;
		for (const auto &bp : model_->BodyParts())
		{
			for (const auto &sub : bp.models)
			{
				for (const auto &mesh : sub.meshes)
				{
					for (const auto &tri : mesh.triangles)
					{
						const StudioModel::Vec3 pts[3] = { tri.a, tri.b, tri.c };
						for (const auto &p : pts)
						{
							if (first)
							{
								minX = maxX = p.x;
								minY = maxY = p.y;
								minZ = maxZ = p.z;
								first = false;
							}
							else
							{
								minX = std::min(minX, p.x);
								maxX = std::max(maxX, p.x);
								minY = std::min(minY, p.y);
								maxY = std::max(maxY, p.y);
								minZ = std::min(minZ, p.z);
								maxZ = std::max(maxZ, p.z);
							}
						}
					}
				}
			}
		}

		if (!first)
		{
			centerX_ = (minX + maxX) * 0.5f;
			centerY_ = (minY + maxY) * 0.5f;
			centerZ_ = (minZ + maxZ) * 0.5f;
			radius_ = std::max({ maxX - minX, maxY - minY, maxZ - minZ }) * 0.5f;
			if (radius_ < 1.0f)
				radius_ = 1.0f;
		}
		else
		{
			centerX_ = centerY_ = centerZ_ = 0.0f;
			radius_ = 60.0f;
		}
		if (resetDistance)
			distance_ = radius_ * 3.0f;
	}

	bool ModelViewWidget::ModelBounds(float &minX, float &minY, float &minZ,
		float &maxX, float &maxY, float &maxZ) const
	{
		if (!model_ || model_->BodyParts().empty())
			return false;
		minX = centerX_ - radius_;
		minY = centerY_ - radius_;
		minZ = centerZ_ - radius_;
		maxX = centerX_ + radius_;
		maxY = centerY_ + radius_;
		maxZ = centerZ_ + radius_;
		return true;
	}

	void ModelViewWidget::initializeGL()
	{
		initializeOpenGLFunctions();
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		// Studio models wind triangles clockwise (when viewed from outside),
		// opposite of OpenGL's default CCW front-face convention. HLAM renders
		// them with glCullFace(GL_FRONT) so we must cull the front faces and
		// keep the outward-facing (CW) surfaces visible.
		glCullFace(GL_FRONT);
		glClearColor(0.16f, 0.18f, 0.20f, 1.0f);

		program_ = std::make_unique<QOpenGLShaderProgram>();
		program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
		program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
		program_->link();

		mvpLoc_ = program_->uniformLocation("uMvp");
		normalMatrixLoc_ = program_->uniformLocation("uNormalMatrix");
		lightDirLoc_ = program_->uniformLocation("uLightDir");
		fillLightDirLoc_ = program_->uniformLocation("uFillLightDir");
		unlitLoc_ = program_->uniformLocation("uUnlit");
		texLoc_ = program_->uniformLocation("uTexture");
		useTexLoc_ = program_->uniformLocation("uUseTex");
		colorLoc_ = program_->uniformLocation("uColor");
	}

	void ModelViewWidget::BuildGpuMeshes()
	{
		meshes_.clear();
		textures_.clear();
		if (!model_ || !program_)
			return;

		const int bpCount = BodyPartCount();
		if (bpCount == 0)
			return;

		// Build textures once (shared across bodyparts).
		// The parser stores pixels in file order (row 0 first), and HLAM uploads
		// them unchanged so that file row 0 lands at t=0. Do not mirror vertically,
		// otherwise the texture maps upside-down and the wrong regions appear on
		// each body part.
		for (const auto &tex : model_->Textures())
		{
			auto glTex = std::make_unique<QOpenGLTexture>(tex.image);
			glTex->setMinificationFilter(QOpenGLTexture::Linear);
			glTex->setMagnificationFilter(QOpenGLTexture::Linear);
			glTex->setWrapMode(QOpenGLTexture::ClampToEdge);
			textures_.push_back(std::move(glTex));
		}

		// skin family -> texture index mapping.
		const int numSkins = SkinFamilyCount();
		const auto &skinRefs = model_->SkinRefs();
		const int perFamily = skinRefs.empty()
			? 0 : static_cast<int>(skinRefs.size()) / std::max(numSkins, 1);
		auto skinToTex = [&](int skinref) -> int
		{
			if (numSkins <= 0 || perFamily <= 0)
				return skinref;
			const int family = std::clamp(skinFamily_, 0, numSkins - 1);
			const int idx = family * perFamily + skinref;
			if (idx < 0 || idx >= static_cast<int>(skinRefs.size()))
				return -1;
			const int ref = skinRefs[static_cast<size_t>(idx)];
			if (ref < 0 || ref >= static_cast<int>(model_->Textures().size()))
				return -1;
			return ref;
		};

		// Render all bodyparts together (bodygroups), each picking the submodel
		// selected in bodyGroup_. This composes the complete model.
		for (int bpIndex = 0; bpIndex < bpCount; ++bpIndex)
		{
			const auto &bp = model_->BodyParts()[static_cast<size_t>(bpIndex)];
			int subIndex = 0;
			if (bpIndex < static_cast<int>(bodyGroup_.size()))
				subIndex = bodyGroup_[static_cast<size_t>(bpIndex)];
			if (subIndex < 0 || subIndex >= static_cast<int>(bp.models.size()))
				continue;
			const auto &sub = bp.models[static_cast<size_t>(subIndex)];
			for (const auto &mesh : sub.meshes)
			{
				if (mesh.triangles.empty())
					continue;
				const int texIndex = skinToTex(mesh.skinref);
				// Normalize texel UVs by the resolved texture's width/height.
				float uScale = 1.0f, vScale = 1.0f;
				if (texIndex >= 0 && texIndex < static_cast<int>(model_->Textures().size()))
				{
					const auto &t = model_->Textures()[static_cast<size_t>(texIndex)];
					if (t.width > 0)
						uScale = 1.0f / t.width;
					if (t.height > 0)
						vScale = 1.0f / t.height;
				}
				auto gpu = std::make_unique<GpuMesh>();
				gpu->texture = texIndex;
				gpu->additive = IsTextureAdditive(texIndex);
				gpu->count = static_cast<GLsizei>(mesh.triangles.size() * 3);
				gpu->verts.reserve(static_cast<size_t>(gpu->count) * 8);
				for (const auto &tri : mesh.triangles)
				{
					const StudioModel::Vec3 *p[3] = { &tri.a, &tri.b, &tri.c };
					const StudioModel::Vec3 *n[3] = { &tri.na, &tri.nb, &tri.nc };
					const float s[3] = { tri.sa, tri.sb, tri.sc };
					const float t[3] = { tri.ta, tri.tb, tri.tc };
					for (int i = 0; i < 3; ++i)
					{
						gpu->verts.push_back(p[i]->x);
						gpu->verts.push_back(p[i]->y);
						gpu->verts.push_back(p[i]->z);
						gpu->verts.push_back(n[i]->x);
						gpu->verts.push_back(n[i]->y);
						gpu->verts.push_back(n[i]->z);
						gpu->verts.push_back(s[i] * uScale);
						gpu->verts.push_back(t[i] * vScale);
					}
				}

				gpu->vbo.create();
				gpu->vbo.bind();
				gpu->vbo.allocate(gpu->verts.data(),
					static_cast<int>(gpu->verts.size()) * sizeof(float));
				gpu->vao.create();
				gpu->vao.bind();
				const int posLoc = program_->attributeLocation("aPosition");
				const int normLoc = program_->attributeLocation("aNormal");
				const int uvLoc = program_->attributeLocation("aTexCoord");
				const int stride = 8 * sizeof(float);
				program_->enableAttributeArray(posLoc);
				program_->setAttributeBuffer(posLoc, GL_FLOAT, 0, 3, stride);
				program_->enableAttributeArray(normLoc);
				program_->setAttributeBuffer(normLoc, GL_FLOAT, 3 * sizeof(float), 3, stride);
				program_->enableAttributeArray(uvLoc);
				program_->setAttributeBuffer(uvLoc, GL_FLOAT, 6 * sizeof(float), 2, stride);
				gpu->vao.release();
				gpu->vbo.release();
				meshes_.push_back(std::move(gpu));
			}
		}
	}

	void ModelViewWidget::SetupMatrices(int width, int height)
	{
		const float aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;

		const bool firstPerson = cameraMode_ == CameraMode::FirstPerson;
		const float baseFov = firstPerson ? firstPersonFov_ : 45.0f;

		// QMatrix4x4::perspective() takes a *vertical* FOV; the horizontal
		// FOV then falls out of aspect (width/height). If we always fed it
		// baseFov directly, a tall/narrow first-person viewport (aspect < 1)
		// would end up with a horizontal FOV *smaller* than baseFov,
		// clipping the sides of the view-model. To guarantee the configured
		// FOV is always satisfied horizontally in that case, we solve for
		// the vertical angle that keeps the *horizontal* angle pinned to
		// baseFov whenever the first-person viewport is taller than wide.
		//
		// This is deliberately based only on the window's aspect ratio, not
		// on the model's actual geometry: an earlier version tried to widen
		// the FOV further based on the mesh's real angular extent, but .mdl
		// view-models can contain secondary particle/glow-effect meshes
		// positioned far off to the side (not reliably distinguishable from
		// real geometry), which blew the computed FOV out into a fisheye
		// view and made the FOV field on the panel stop having any visible
		// effect. If a specific weapon still doesn't fully fit, widen the
		// FOV field manually.
		float verticalFov = baseFov;
		if (firstPerson && aspect < 1.0f)
		{
			const float horizontalHalfRad = qDegreesToRadians(baseFov * 0.5f);
			const float verticalHalfRad = std::atan(std::tan(horizontalHalfRad) / aspect);
			verticalFov = qRadiansToDegrees(verticalHalfRad) * 2.0f;
		}

		QMatrix4x4 projection;
		projection.perspective(verticalFov, aspect,
			firstPerson ? kFirstPersonNear : 0.1f, firstPerson ? 16777216.0f : 100000.0f);

		const float s = modelScale_;
		QMatrix4x4 model;
		// Studio models live in their native Z-up frame (Forward=+X, Up=+Z,
		// Right=+Y), exactly as HLAM's SetupPosition leaves them with default
		// zero angles. Do NOT rotate the model into Y-up space - HLAM orients
		// the camera instead, with default view direction +X and up +Z, so the
		// model's front faces the viewer without mirroring.
		model.scale(s);
		if (firstPerson)
			model.translate(0.0f, 0.0f, -1.0f);

		if (firstPerson)
		{
			QMatrix4x4 view;
			view.lookAt(QVector3D(0, 0, 0), QVector3D(1, 0, 0), QVector3D(0, 0, 1));
			const QMatrix4x4 mvp = projection * view * model;
			bool invertible = false;
			const QMatrix4x4 normalFinal = (view * model).inverted(&invertible).transposed();
			QMatrix4x4 lightRotation;
			lightRotation.rotate(lightPitch_, 1.0f, 0.0f, 0.0f);
			lightRotation.rotate(lightYaw_, 0.0f, 1.0f, 0.0f);
			program_->bind();
			program_->setUniformValue(mvpLoc_, mvp);
			program_->setUniformValue(normalMatrixLoc_, normalFinal);
			program_->setUniformValue(lightDirLoc_, lightRotation.mapVector(QVector3D(0.4f, 0.6f, 1.0f)));
			program_->setUniformValue(fillLightDirLoc_, lightRotation.mapVector(QVector3D(-0.5f, -0.3f, 0.6f)));
			program_->setUniformValue(texLoc_, 0);
			program_->release();
			return;
		}

		const QVector3D centerModel(centerX_, centerY_, centerZ_);
		const QVector3D centerWorld = model.map(centerModel);

		// Orbit camera in the same Z-up frame as HLAM. pitch=yaw=0 looks along
		// -X at the model center with up=+Z, giving the front-facing default
		// view that matches HLAM's Camera (forward=+X, up=+Z).
		const float pitchRad = qDegreesToRadians(pitch_);
		const float yawRad = qDegreesToRadians(yaw_);
		const float cp = std::cos(pitchRad);
		QVector3D dirToCamera(
			cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad));
		dirToCamera.normalize();

		const QVector3D eye = centerWorld + dirToCamera * distance_;

		QMatrix4x4 view;
		view.lookAt(eye, centerWorld, QVector3D(0, 0, 1));

		const QMatrix4x4 mvp = projection * view * model;
		bool invertible = false;
		const QMatrix4x4 normalMatrix = view * model;
		const QMatrix4x4 normalFinal = normalMatrix.inverted(&invertible).transposed();

		// Both lights are defined directly in view/camera space (not
		// transformed by the model or view matrices), so they stay fixed
		// relative to the camera as you orbit -- a "headlight" pair, one key
		// light from the upper right and a dimmer fill from the lower left.
		// lightYaw_/lightPitch_ let the user rotate both if a particular
		// model's own surface normals still end up poorly lit either way.
		QMatrix4x4 lightRotation;
		lightRotation.rotate(lightPitch_, 1.0f, 0.0f, 0.0f);
		lightRotation.rotate(lightYaw_, 0.0f, 1.0f, 0.0f);
		const QVector3D keyLight = lightRotation.mapVector(QVector3D(0.4f, 0.6f, 1.0f));
		const QVector3D fillLight = lightRotation.mapVector(QVector3D(-0.5f, -0.3f, 0.6f));

		program_->bind();
		program_->setUniformValue(mvpLoc_, mvp);
		program_->setUniformValue(normalMatrixLoc_, normalFinal);
		program_->setUniformValue(lightDirLoc_, keyLight);
		program_->setUniformValue(fillLightDirLoc_, fillLight);
		program_->setUniformValue(texLoc_, 0);
		program_->release();
	}

	void ModelViewWidget::paintGL()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		// Resetting a potentially stuck state from Qt/driver
		glDisable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_TRUE);

		if (!model_ || !program_)
			return;

		if (!gpuBuilt_)
		{
			BuildGpuMeshes();
			gpuBuilt_ = true;
		}

		SetupMatrices(width(), height());

		program_->bind();
		program_->setUniformValue(useTexLoc_, wireframe_ ? 0.0f : 1.0f);
		program_->setUniformValue(colorLoc_, QVector4D(0.7f, 0.7f, 0.7f, 1.0f));
		program_->setUniformValue(unlitLoc_, 0.0f);

		if (wireframe_)
		{
			glDisable(GL_CULL_FACE);
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		}

		const auto bindMeshTexture = [&](const std::unique_ptr<GpuMesh> &gpu)
		{
			if (gpu->texture >= 0 && gpu->texture < static_cast<int>(textures_.size()) && !wireframe_)
				textures_[static_cast<size_t>(gpu->texture)]->bind(0);
			else if (!textures_.empty())
				textures_[0]->bind(0);
		};

		// Opaque pass: everything except additive-flagged (glow/particle FX)
		// meshes, drawn normally. In wireframe mode additive meshes are just
		// included here too -- there's nothing to blend, it's all outlines.
		bool hasAdditive = false;
		for (const auto &gpu : meshes_)
		{
			if (!wireframe_ && gpu->additive)
			{
				hasAdditive = true;
				continue;
			}

			bindMeshTexture(gpu);
			gpu->vao.bind();
			glDrawArrays(GL_TRIANGLES, 0, gpu->count);
			gpu->vao.release();
		}

		// Additive pass: STUDIO_NF_ADDITIVE textures (fire, glow, particle FX)
		// represent self-illumination that's meant to blend into whatever is
		// behind it, not an opaque surface -- draw them blended (GL_ONE so
		// they add light rather than being alpha-composited), unlit (see the
		// shader), and without writing depth so they layer like light instead
		// of occluding/z-fighting with geometry behind them.
		if (hasAdditive)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glDepthMask(GL_FALSE);
			program_->setUniformValue(unlitLoc_, 1.0f);

			for (const auto &gpu : meshes_)
			{
				if (!gpu->additive)
					continue;

				bindMeshTexture(gpu);
				gpu->vao.bind();
				glDrawArrays(GL_TRIANGLES, 0, gpu->count);
				gpu->vao.release();
			}

			program_->setUniformValue(unlitLoc_, 0.0f);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
		}

		if (wireframe_)
		{
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			glEnable(GL_CULL_FACE);
		}

		program_->release();
	}

	void ModelViewWidget::resizeGL(int w, int h)
	{
		glViewport(0, 0, w, h);
	}

	void ModelViewWidget::mousePressEvent(QMouseEvent *event)
	{
		if (cameraMode_ == CameraMode::FirstPerson)
			LeaveFirstPerson();
		if (event->button() == Qt::LeftButton)
		{
			lastMouse_ = event->pos();
			dragging_ = true;
		}
		else if (event->button() == Qt::RightButton)
		{
			lastMouse_ = event->pos();
			panning_ = true;
		}
	}

	void ModelViewWidget::mouseMoveEvent(QMouseEvent *event)
	{
		const QPoint delta = event->pos() - lastMouse_;
		lastMouse_ = event->pos();

		if (dragging_)
		{
			// Horizontal matches HLAM feel (_yaw -= dx). Vertical is flipped so a
			// downward drag (Qt delta.y > 0) tilts the model forward.
			yaw_ -= delta.x() * 0.5f;
			pitch_ += delta.y() * 0.5f;
			pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
			update();
		}
		else if (panning_)
		{
			// Pan by moving the orbit pivot itself, using the camera's
			// *current* right/up basis (recomputed from yaw_/pitch_, not
			// just valid at the default angle) -- not a separate, decoupled
			// view-space offset. That old approach only matched "pan in
			// screen space" at yaw=pitch=0; at any other orbit angle it
			// translated the scene along the wrong (world, not camera) axes,
			// and since it never moved the actual pivot, orbiting afterward
			// (left-drag) would swing the view right back away from wherever
			// panning had put things on screen. Moving the pivot directly
			// fixes both: pan tracks the mouse at any angle, and subsequent
			// orbiting continues around wherever you just panned to.
			const float pitchRad = qDegreesToRadians(pitch_);
			const float yawRad = qDegreesToRadians(yaw_);
			const float cp = std::cos(pitchRad);
			QVector3D dirToCamera(cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad));
			dirToCamera.normalize();
			const QVector3D forward = -dirToCamera;
			const QVector3D worldUp(0.0f, 0.0f, 1.0f);
			QVector3D right = QVector3D::crossProduct(forward, worldUp);
			if (right.lengthSquared() < 1e-8f) // Looking straight up/down: worldUp is degenerate, pick any right.
				right = QVector3D(0.0f, 1.0f, 0.0f);
			right.normalize();
			const QVector3D camUp = QVector3D::crossProduct(right, forward).normalized();

			// Scale by distance so the drag-to-motion feel stays consistent
			// whether zoomed in close or looking at the whole scene.
			const float scale = distance_ * 0.0016f;
			const QVector3D offset = -right * (delta.x() * scale) + camUp * (delta.y() * scale);
			centerX_ += offset.x();
			centerY_ += offset.y();
			centerZ_ += offset.z();
			update();
		}
	}

	void ModelViewWidget::mouseReleaseEvent(QMouseEvent *event)
	{
		if (event->button() == Qt::LeftButton)
			dragging_ = false;
		else if (event->button() == Qt::RightButton)
			panning_ = false;
	}

	void ModelViewWidget::wheelEvent(QWheelEvent *event)
	{
		if (cameraMode_ == CameraMode::FirstPerson)
		{
			LeaveFirstPerson();
		}
		const int delta = event->angleDelta().y();
		distance_ *= (delta > 0) ? 0.9f : 1.1f;
		// The minimum used to be radius_*0.5 -- fine for a single compact
		// model, but radius_ is the bounding radius of *everything* in the
		// file. For a model with far-apart pieces (e.g. a boss standing
		// well away from its own attack-effect meshes), that alone could
		// keep you from ever zooming in close on just one part. Scale the
		// floor down a lot instead, with a small absolute minimum so it's
		// never degenerate.
		const float minDistance = std::max(radius_ * 0.02f, 0.5f);
		distance_ = std::clamp(distance_, minDistance, radius_ * 20.0f);
		update();
	}
}
