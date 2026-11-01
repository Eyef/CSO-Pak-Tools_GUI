#include "BspViewWidget.h"
#include <QMatrix4x4>

#include <QDebug>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <tuple>

namespace cso_gui
{
	namespace
	{
		constexpr const char *kVertexShader = R"(
#version 330 core
// Fixed attribute slots (never query the driver): writing to a slot the
// compiler eliminated (e.g. aNormal when lighting ignores normals) is
// harmless, while enableAttributeArray(-1) is a GL error that corrupts
// memory in some drivers (heap crashes elsewhere, long after the call).
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aLightUv;
uniform mat4 uMvp;
uniform mat4 uNormalMatrix;
uniform float uUseTex;
uniform float uBillboard;
uniform vec4 uSpin;  // spinning sign: xy pivot, z angle (rad), w = 1 when active
uniform vec3 uEye;
out vec3 vNormal;
out vec2 vTexCoord;
out vec2 vLightUv;
out vec3 vWorldNormal;
out vec3 vPos;
void main()
{
	vec3 pos = aPosition;
	if (uBillboard > 0.5)
	{
		// Crossed-plane markers collapsed to one quad: aNormal.xy holds the
		// pivot, the quad is turned about the vertical axis so that it faces
		// the eye (rel = (0, s, z) in the canonical orientation).
		vec2 dir = uEye.xy - aNormal.xy;
		float len = length(dir);
		vec2 c = len > 1e-4 ? dir / len : vec2(1.0, 0.0);
		vec3 rel = aPosition - vec3(aNormal.xy, 0.0);
		pos = vec3(aNormal.xy, 0.0) +
			vec3(rel.x * c.x - rel.y * c.y, rel.x * c.y + rel.y * c.x, rel.z);
	}
	vec3 nrm = aNormal;
	if (uSpin.w > 0.5)
	{
		// Pictogram turning about the vertical axis through the pivot.
		float cs = cos(uSpin.z);
		float sn = sin(uSpin.z);
		vec2 r = pos.xy - uSpin.xy;
		pos.xy = uSpin.xy + vec2(r.x * cs - r.y * sn, r.x * sn + r.y * cs);
		nrm.xy = vec2(nrm.x * cs - nrm.y * sn, nrm.x * sn + nrm.y * cs);
	}
	vWorldNormal = nrm;  // Mesh data is already in world space.
	vPos = pos;
	gl_Position = uMvp * vec4(pos, 1.0);
	vNormal = mat3(uNormalMatrix) * nrm;
	vTexCoord = aTexCoord;
	vLightUv = aLightUv;
}
)";

		constexpr const char *kFragmentShader = R"(
#version 330 core
in vec3 vNormal;
in vec3 vWorldNormal;
in vec3 vPos;
in vec2 vTexCoord;
in vec2 vLightUv;
uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform float uUseTex;
uniform vec3 uLightDir;
uniform vec3 uFillLightDir;
uniform vec4 uColor;
uniform float uUnlit;
uniform float uOpacity;
uniform vec3 uTint;
uniform vec3 uLightMul;
uniform float uChrome;
uniform float uSky;
uniform float uWater;
uniform float uCullBack;
uniform float uNoAlphaTest;  // 1 = worldspawn face: never cut out
uniform vec3 uEye;
uniform sampler2D uSky0;
uniform sampler2D uSky1;
uniform sampler2D uSky2;
uniform sampler2D uSky3;
uniform sampler2D uSky4;
uniform sampler2D uSky5;
uniform float uAdditive;
uniform float uStudio;
uniform float uLightMode;
uniform float uModelFill;
uniform float uBrightness;
uniform vec2 uWhiteUv;
uniform float uKneeOn;
uniform vec3 uFogColor;
uniform float uFogDensity;  // env_fog density; 0 = no fog
out vec4 FragColor;
// Exponential-squared distance fog (the engine's GL fog): 1 = clear, 0 = all fog.
float FogFactor()
{
	if (uFogDensity <= 0.0)
		return 1.0;
	float d = length(vPos - uEye) * uFogDensity;
	return clamp(exp(-d * d), 0.0, 1.0);
}
void main()
{
	if (uSky > 0.5)
	{
		// Quake/GoldSrc sky box lookup (st_to_vec): face axes +X,-X,+Y,-Y,+Z,-Z
		// = rt, lf, bk, ft, up, dn; s runs along R, t (image up) along U.
		const vec3 kR[6] = vec3[6](vec3(0,-1,0), vec3(0,1,0), vec3(1,0,0),
			vec3(-1,0,0), vec3(0,-1,0), vec3(0,-1,0));
		const vec3 kU[6] = vec3[6](vec3(0,0,1), vec3(0,0,1), vec3(0,0,1),
			vec3(0,0,1), vec3(-1,0,0), vec3(1,0,0));
		vec3 d = vPos - uEye;
		// Sky brush faces face INTO the playable space. Seen from outside (orbit
		// camera) they would hide the whole map, so only their front side shows.
		if (dot(normalize(vWorldNormal), -d) < 0.0)
			discard;
		vec3 a = abs(d);
		int axis;
		float dc;
		if (a.x >= a.y && a.x >= a.z) { axis = d.x > 0.0 ? 0 : 1; dc = a.x; }
		else if (a.y >= a.z)          { axis = d.y > 0.0 ? 2 : 3; dc = a.y; }
		else                          { axis = d.z > 0.0 ? 4 : 5; dc = a.z; }
		dc = max(dc, 1e-4);
		float s = dot(d, kR[axis]) / dc;
		float t = dot(d, kU[axis]) / dc;
		// Keep the lookup off the outermost texel row/column of each face.
		// Some sky images carry a 1-pixel flat/blank border (zs_behind's
		// "strange" sky: the left column of strangeup.tga is a constant
		// 121,121,153 strip). Sampling at 1/512 lies midway between texel 0
		// and texel 1, so bilinear filtering blends half of that border into
		// the picture and draws a thin line along the cube edge. Clamping to
		// the CENTRE of the second texel (1.5 / size) reads clean pixels only.
		vec2 suv = vec2(0.5 + 0.5 * s, 0.5 - 0.5 * t);
		vec2 isz;
		if (axis == 0) isz = 1.0 / max(vec2(textureSize(uSky0, 0)), vec2(4.0));
		else if (axis == 1) isz = 1.0 / max(vec2(textureSize(uSky1, 0)), vec2(4.0));
		else if (axis == 2) isz = 1.0 / max(vec2(textureSize(uSky2, 0)), vec2(4.0));
		else if (axis == 3) isz = 1.0 / max(vec2(textureSize(uSky3, 0)), vec2(4.0));
		else if (axis == 4) isz = 1.0 / max(vec2(textureSize(uSky4, 0)), vec2(4.0));
		else isz = 1.0 / max(vec2(textureSize(uSky5, 0)), vec2(4.0));
		suv = clamp(suv, 1.5 * isz, vec2(1.0) - 1.5 * isz);
		vec3 sky;
		if (axis == 0) sky = textureLod(uSky0, suv, 0.0).rgb;
		else if (axis == 1) sky = textureLod(uSky1, suv, 0.0).rgb;
		else if (axis == 2) sky = textureLod(uSky2, suv, 0.0).rgb;
		else if (axis == 3) sky = textureLod(uSky3, suv, 0.0).rgb;
		else if (axis == 4) sky = textureLod(uSky4, suv, 0.0).rgb;
		else sky = textureLod(uSky5, suv, 0.0).rgb;
		// The sky is drawn without fog: in game the skybox keeps its picture
		// (zs_lastride2 shows a light overcast sky) while only map geometry
		// fades into the env_fog colour.
		FragColor = vec4(sky, 1.0);
		return;
	}
	// Liquid surfaces are stored once per side (and sometimes stacked); like the
	// engine, draw only the side that faces the eye.
	if (uWater > 0.5 && dot(normalize(vWorldNormal), uEye - vPos) < 0.0)
		discard;
	// Brush entities (func_wall, func_illusionary, signs, grates...): the
	// engine draws a face only when the eye is on its front side. Thin boxes
	// with a texture on both faces (additive "E" signs, hazard-stripe strips,
	// masked grates) otherwise show their far/back side too -- as a mirrored
	// double image or a stripe that glows through the surface in front of it.
	if (uCullBack > 0.5 && dot(normalize(vWorldNormal), uEye - vPos) < 0.0)
		discard;
	vec3 n = normalize(vNormal);
	float diffMain = max(dot(n, normalize(uLightDir)), 0.0);
	float diffFill = max(dot(n, normalize(uFillLightDir)), 0.0);
	float light = min(0.35 + 0.65 * diffMain + 0.35 * diffFill, 1.0);
	vec4 base = vec4(uColor.rgb, 1.0);
	if (uUseTex > 0.5)
		base = texture(uTexture, uChrome > 0.5 ? n.xy * 0.5 + 0.5 : vTexCoord);
	if (base.a < 0.5)
	{
		// Masked texel. worldspawn faces never cut it out (the engine draws
		// the transparent colour as opaque black); brush entities with
		// rendermode Solid/Texture/..., models and sprites alpha-test it.
		if (uNoAlphaTest < 0.5)
			discard;
		base = vec4(0.0, 0.0, 0.0, 1.0);
	}
	if (uAdditive > 0.5)
	{
		// Brush glow/beams (rendermode 5): self-illumination that adds
		// light, like the .mdl viewer's additive pass. Brightness comes
		// from the entity's renderamt (*uOpacity), tint from rendercolor.
		FragColor = vec4(base.rgb * uTint * uOpacity * FogFactor(), 1.0);
		return;
	}
	// Baked map lighting: the lightmap already holds the scene's lamps and
	// shadows, so unlike the .mdl viewer there is no headlight term --
	// a small floor keeps fully unlit corners readable instead of void.
	// (vNormal stays for future use; lighting here is lightmap-only.)
	// Same recipe as goldsrc-bsp-viewer: texture * lightmap, then one
	// display gamma over the whole product (this also lifts the texture,
	// as the engine's texgamma does). Raise kGamma to brighten, lower to
	// darken; 1.0 = plain multiply.
	const float kGamma = 1.6;
	vec3 lm = texture(uLightmap, vLightUv).rgb;
	// Over-exposed lightmaps. Maps lit by a very strong light_environment
	// (zs_evacuate3, zs_electron2) store sun-facing surfaces clipped at 255,
	// and drawing 255 as full white makes them far lighter than in game,
	// where those walls stay mid-grey. The brightest values are therefore
	// compressed by a MONOTONIC curve (never darker for a brighter texel,
	// otherwise the half-lit penumbra texels between shadow and sun end up
	// brighter than the sunlit area and show up as a white rim along every
	// shadow edge, as on zs_2gate). Only maps that really have clipped
	// texels switch it on (uKneeOn, decided per map on the CPU); faces
	// without a lightmap use the atlas' white texel and stay fullbright.
	if (uKneeOn > 0.5 && uLightMode < 0.5 && distance(vLightUv, uWhiteUv) > 1e-4)
	{
		const float kKneeStart = 0.55;  // below this the light is untouched
		const float kKneeLen = 0.25;   // width of the smooth roll-off
		const float kKneeSlope = 0.10; // what is left of the slope above it
		float lmPeak = max(lm.r, max(lm.g, lm.b));
		if (lmPeak > kKneeStart)
		{
			float u = lmPeak - kKneeStart;
			float v = min(u, kKneeLen) / kKneeLen;
			float comp = kKneeStart + kKneeSlope * u +
				(1.0 - kKneeSlope) * kKneeLen / 3.0 * (1.0 - (1.0 - v) * (1.0 - v) * (1.0 - v));
			lm *= comp / lmPeak;
		}
	}
	// Placed .mdl models get an ambient term on top of the light under
	// them (the engine lights studio models with ambient + shade, so they
	// never go as dark as a bare lightmap texel). Fitted against in-game
	// screenshots: a van in shadow (lightmap ~0.1) shows as ~0.65.
	const float kModelAmbient = 0.5;
	vec3 mapLight = max(lm, vec3(0.02));
	// Light comes from above (GoldSrc studio shading: light vector 0,0,-1),
	// so down-facing surfaces get slightly less, up-facing slightly more.
	// Calibrated on in-game ze_exodus screenshots: a model in shadow
	// (lightmap ~0.1) shows ~0.6, a basketball in the lit court (lightmap
	// ~0.69) ~0.75 -- i.e. CSO compresses model light into a narrow band,
	// light = 0.57 + 0.26 * lightmap, unlike brushes. The old lm + 0.5
	// clamp gave 1.0 there (~18% too bright). Tune kModelBase/kModelGain.
	const float kModelBase = 0.57;
	const float kModelGain = 0.26;
	float upFacing = clamp(normalize(vWorldNormal).z * 0.5 + 0.5, 0.0, 1.0);
	float dirScale = mix(0.88, 1.06, upFacing);
	// The engine keeps the HUE of the light (colour normalised to its
	// brightest channel) and takes the brightness from ambient + shade, so
	// a greenish lightmap tints a model green instead of being washed to
	// white by a flat additive term.
	float lmMax = max(lm.r, max(lm.g, lm.b));
	vec3 lightHue = lmMax > 0.02 ? lm / lmMax : vec3(1.0);
	float modelIntensity = (kModelBase + kModelGain * lmMax) * dirScale;
	vec3 fittedLight = clamp(lightHue * modelIntensity, 0.0, 1.0);
	// uModelFill: 0 = model lit exactly like the map under it (blends into
	// the environment), 1 = fully game-fitted band (brighter, in-shadow
	// models are lifted). Slider in the UI, default in between.
	vec3 modelLight = mix(clamp(lm * dirScale, 0.0, 1.0), fittedLight, uModelFill);
	// The engine never lights a model fully: ambient is clamped to half and
	// only faces turned to the light get the rest. Sunlit models (lightmap
	// ~1.0, e.g. logs on bright sand) saturated both formulas, so the boost
	// slider did nothing there; cap the intensity (hue preserved).
	const float kModelMax = 0.8;
	float mlMax = max(modelLight.r, max(modelLight.g, modelLight.b));
	if (mlMax > kModelMax) modelLight *= kModelMax / mlMax;
	if (uLightMode > 0.5)
	{
		// goldsrc-bsp-viewer: texture * lightmap, nothing else.
		mapLight = lm;
		modelLight = lm;
	}
	// renderfx 22 (CSO light multiplier): scales the light of studio models
	// by rendercolor/255 before the display gamma. Brush meshes pass 1.
	// Raised to a power > 1: the engine's multiplier acts on the light, and
	// our fitted model light sits higher than in game, so a plain multiply
	// left rubble slabs too bright (zs_electron2). 1.0 = plain multiply.
	const float kLightMulPower = 1.0;
	modelLight *= pow(uLightMul, vec3(kLightMulPower));
	vec3 sceneLight = mix(mix(mapLight, modelLight, uStudio) * uBrightness, vec3(1.0), uUnlit);
	vec3 shaded = pow(base.rgb * sceneLight, vec3(1.0 / kGamma)) * uTint;
	shaded = mix(uFogColor, shaded, FogFactor());
	FragColor = vec4(shaded, base.a * uOpacity);
}
)";

		// Colour bleed for alpha-tested images (what Quake II / GoldSrc's GL
		// uploader does too). A masked texel has alpha 0 but still carries the
		// key colour in RGB -- pure blue (0,0,255) in .spr / .wad. Bilinear
		// filtering and generated mipmaps blend that RGB into the neighbouring
		// opaque texels (the 0.5 alpha test keeps those), which draws a blue
		// fringe around every grass blade / leaf. Fix: give every transparent
		// texel the colour of the nearest opaque ones, so the blend stays
		// inside the picture's own palette. Alpha is left untouched.
		QImage BleedTransparentColors(const QImage &source)
		{
			if (source.isNull())
				return source;
			QImage image = source.convertToFormat(QImage::Format_RGBA8888);
			const int w = image.width(), h = image.height();
			std::vector<uint8_t> filled(static_cast<size_t>(w) * h, 0);
			size_t remaining = 0;
			for (int y = 0; y < h; ++y)
			{
				const uint8_t *row = image.constScanLine(y);
				for (int x = 0; x < w; ++x)
				{
					if (row[x * 4 + 3] >= 128)
						filled[static_cast<size_t>(y) * w + x] = 1;
					else
						++remaining;
				}
			}
			if (remaining == 0 || remaining == static_cast<size_t>(w) * h)
				return image;  // nothing masked, or nothing to borrow from

			// Grow the opaque region one ring per pass; each new texel takes
			// the average of its already-filled 8-neighbours.
			while (remaining > 0)
			{
				std::vector<uint8_t> next = filled;
				size_t progressed = 0;
				for (int y = 0; y < h; ++y)
				{
					uint8_t *row = image.scanLine(y);
					for (int x = 0; x < w; ++x)
					{
						if (filled[static_cast<size_t>(y) * w + x])
							continue;
						int count = 0, sum[3] = { 0, 0, 0 };
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								const int nx = x + dx, ny = y + dy;
								if (nx < 0 || ny < 0 || nx >= w || ny >= h)
									continue;
								if (!filled[static_cast<size_t>(ny) * w + nx])
									continue;
								const uint8_t *p = image.constScanLine(ny) + nx * 4;
								sum[0] += p[0];
								sum[1] += p[1];
								sum[2] += p[2];
								++count;
							}
						if (count == 0)
							continue;
						row[x * 4 + 0] = static_cast<uint8_t>(sum[0] / count);
						row[x * 4 + 1] = static_cast<uint8_t>(sum[1] / count);
						row[x * 4 + 2] = static_cast<uint8_t>(sum[2] / count);
						next[static_cast<size_t>(y) * w + x] = 1;
						++progressed;
					}
				}
				if (progressed == 0)
					break;
				filled.swap(next);
				remaining -= progressed;
			}
			return image;
		}

		// Mip chain of a masked ("{") texture the way the engine's .wad stores
		// it: every level is rebuilt from the previous one with a binary
		// alpha -- a texel stays opaque when at least 2 of its 4 parents are
		// (ties count as opaque) and takes the average colour of the opaque
		// parents. Coverage therefore GROWS with distance (zs_lastride_44's
		// fence: 32% opaque at level 0, 35 / 45 / 61% at levels 1-3), so far
		// wires look thick as in game. GL's own generated mips average the
		// alpha instead, and the 0.5 alpha test then eats thin wires away.
		// Returns false when the image has no transparent texel (normal
		// textures keep the usual path).
		bool BuildMaskedMipChain(const QImage &source, std::vector<QImage> &levels)
		{
			levels.clear();
			if (source.isNull())
				return false;
			QImage base = source.convertToFormat(QImage::Format_RGBA8888);
			bool anyTransparent = false;
			for (int y = 0; y < base.height() && !anyTransparent; ++y)
			{
				const uint8_t *row = base.constScanLine(y);
				for (int x = 0; x < base.width(); ++x)
					if (row[x * 4 + 3] < 128)
					{
						anyTransparent = true;
						break;
					}
			}
			if (!anyTransparent)
				return false;
			// Binarise level 0 so every level follows the same rule.
			for (int y = 0; y < base.height(); ++y)
			{
				uint8_t *row = base.scanLine(y);
				for (int x = 0; x < base.width(); ++x)
					row[x * 4 + 3] = row[x * 4 + 3] >= 128 ? 255 : 0;
			}
			// Level 0 is sampled bilinearly too: hide the key colour (blue)
			// of the masked texels. Higher levels are built from this one.
			levels.push_back(BleedTransparentColors(base));
			while (levels.back().width() > 1 || levels.back().height() > 1)
			{
				const QImage &prev = levels.back();
				const int w = std::max(1, prev.width() / 2);
				const int h = std::max(1, prev.height() / 2);
				QImage next(w, h, QImage::Format_RGBA8888);
				for (int y = 0; y < h; ++y)
				{
					uint8_t *dst = next.scanLine(y);
					for (int x = 0; x < w; ++x)
					{
						int count = 0, sum[3] = { 0, 0, 0 };
						for (int dy = 0; dy < 2; ++dy)
							for (int dx = 0; dx < 2; ++dx)
							{
								const int sx = std::min(prev.width() - 1, x * 2 + dx);
								const int sy = std::min(prev.height() - 1, y * 2 + dy);
								const uint8_t *p = prev.constScanLine(sy) + sx * 4;
								if (p[3] != 0)
								{
									++count;
									sum[0] += p[0];
									sum[1] += p[1];
									sum[2] += p[2];
								}
							}
						uint8_t *d = dst + x * 4;
						if (count >= 2)
						{
							d[0] = static_cast<uint8_t>(sum[0] / count);
							d[1] = static_cast<uint8_t>(sum[1] / count);
							d[2] = static_cast<uint8_t>(sum[2] / count);
							d[3] = 255;
						}
						else
						{
							d[0] = d[1] = d[2] = 0;
							d[3] = 0;
						}
					}
				}
				levels.push_back(std::move(next));
			}
			return true;
		}

		// Studio props are sent as point-like entities in these BSPs. Using
		// even a small hull around their origin can touch a neighboring visible
		// leaf and keep a long prop (such as the elevator cables) alive through
		// a wall. Test the origin's single BSP leaf instead; if it lies in solid
		// space, return no leaf and let the caller keep the entity conservatively.
		std::vector<int> BspPointEntityLeaves(const BspMap &map,
			float x, float y, float z)
		{
			if (map.Models().empty())
				return {};
			const auto &nodes = map.Nodes();
			const auto &planes = map.Planes();
			const auto &leaves = map.Leaves();
			int child = map.Models()[0].headNodes[0];
			for (size_t steps = 0; child >= 0 && steps <= nodes.size(); ++steps)
			{
				if (static_cast<size_t>(child) >= nodes.size())
					return {};
				const BspMap::Node &node = nodes[static_cast<size_t>(child)];
				if (node.plane < 0 || static_cast<size_t>(node.plane) >= planes.size())
					return {};
				const BspMap::Plane &plane = planes[static_cast<size_t>(node.plane)];
				const float distance = x * plane.normal.x + y * plane.normal.y +
					z * plane.normal.z - plane.dist;
				child = node.children[distance >= 0.0f ? 0 : 1];
			}
			if (child >= 0)
				return {};
			const int leaf = ~child;
			if (leaf <= 0 || static_cast<size_t>(leaf) >= leaves.size() ||
				leaves[static_cast<size_t>(leaf)].contents == -2)
				return {};
			return { leaf };
		}
	}

	BspViewWidget::BspViewWidget(QWidget *parent)
		: QOpenGLWidget(parent)
	{
		setFocusPolicy(Qt::StrongFocus);
		flyTimer_ = new QTimer(this);
		flyTimer_->setInterval(16);
		connect(flyTimer_, &QTimer::timeout, this, &BspViewWidget::OnFlyTick);
	}

	BspViewWidget::~BspViewWidget()
	{
		// QOpenGLWidget cleans up its context; our buffers are child objects.
	}

	QVector3D BspFlyForward(float yawDegrees, float pitchDegrees)
	{
		const float yaw = qDegreesToRadians(yawDegrees);
		const float pitch = qDegreesToRadians(pitchDegrees);
		const float cp = std::cos(pitch);
		return QVector3D(cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch));
	}

	int BspNormalizeFlyKey(int qtKey, unsigned nativeScanCode)
	{
		// Windows Set-1 scan codes for the main-block letter keys and
		// Space. Positional: identical on Latin and Cyrillic layouts, so
		// physical WASD works with any active language.
		switch (nativeScanCode)
		{
		case 16: return Qt::Key_Q;
		case 17: return Qt::Key_W;
		case 18: return Qt::Key_E;
		case 30: return Qt::Key_A;
		case 31: return Qt::Key_S;
		case 32: return Qt::Key_D;
		case 46: return Qt::Key_C;
		case 57: return Qt::Key_Space;
		default: break;
		}
		return qtKey;
	}

	void BspViewWidget::SetMap(std::shared_ptr<BspMap> map, std::vector<QImage> textures)
	{
		map_ = std::move(map);
		textures_ = std::move(textures);
		placedTextures_.clear();
		placedMeshes_.clear();
		placedSprites_.clear();
		skyImages_ = {};
		instanceLeaves_.clear();
		spriteLeaves_.clear();
		modelLeaves_.clear();
		pvsActive_ = false;
		pvsDirty_ = true;
		wireframe_ = false;
		// env_fog (CSO maps like zs_lastride2): colour from rendercolor,
		// exponential-squared density from "density".
		fogDensity_ = 0.0f;
		fogColor_[0] = fogColor_[1] = fogColor_[2] = 0.0f;
		if (map_)
		{
			const std::string &ents = map_->EntitiesText();
			size_t pos = 0;
			while ((pos = ents.find("\"env_fog\"", pos)) != std::string::npos)
			{
				const size_t open = ents.rfind('{', pos);
				const size_t close = ents.find('}', pos);
				if (open == std::string::npos || close == std::string::npos)
					break;
				const std::string block = ents.substr(open, close - open);
				const auto value = [&](const char *key) -> std::string {
					const std::string k = std::string("\"") + key + "\"";
					size_t p = block.find(k);
					if (p == std::string::npos)
						return {};
					p = block.find('"', p + k.size());
					if (p == std::string::npos)
						return {};
					const size_t e = block.find('"', p + 1);
					return e == std::string::npos ? std::string() : block.substr(p + 1, e - p - 1);
				};
				if (value("classname") == "env_fog")
				{
					float r = 0, g = 0, b = 0, density = 0;
					const std::string col = value("rendercolor");
					std::sscanf(col.c_str(), "%f %f %f", &r, &g, &b);
					const std::string den = value("density");
					std::sscanf(den.c_str(), "%f", &density);
					if (density > 0.0f)
					{
						fogDensity_ = density;
						fogColor_[0] = std::clamp(r, 0.0f, 255.0f) / 255.0f;
						fogColor_[1] = std::clamp(g, 0.0f, 255.0f) / 255.0f;
						fogColor_[2] = std::clamp(b, 0.0f, 255.0f) / 255.0f;
					}
					break;
				}
				pos = close;
			}
		}
		gpuBuilt_ = false;
		triangleCount_ = 0;
		yaw_ = -90.0f;
		pitch_ = 20.0f;
		if (map_)
			ComputeBounds();
		else
		{
			centerX_ = centerY_ = centerZ_ = 0.0f;
			radius_ = 1000.0f;
			distance_ = radius_ * 3.0f;
		}
		if (cameraMode_ == CameraMode::Fly)
			SeedFlyFromOrbit();

		update();
	}

	void BspViewWidget::Clear()
	{
		SetMap(nullptr, {});
	}

	void BspViewWidget::AddPlacedModels(std::vector<QImage> textures,
		std::vector<BspCpuMesh> meshes)
	{
		const int base = static_cast<int>(placedTextures_.size());
		for (auto &mesh : meshes)
		{
			if (mesh.texture >= 0)
				mesh.texture += base;
			placedMeshes_.push_back(std::move(mesh));
		}
		for (auto &image : textures)
			placedTextures_.push_back(std::move(image));
		gpuBuilt_ = false;
		update();
	}

	void BspViewWidget::AddPlacedSprite(QImage image, QVector3D origin,
		float widthUnits, float heightUnits,
		BspRenderClass renderClass, float opacity)
	{
		if (image.isNull() || widthUnits <= 0.0f || heightUnits <= 0.0f)
			return;
		PlacedSprite sprite;
		sprite.image = std::move(image);
		sprite.origin = origin;
		sprite.widthUnits = widthUnits;
		sprite.heightUnits = heightUnits;
		sprite.renderClass = renderClass;
		sprite.opacity = opacity;
		placedSprites_.push_back(std::move(sprite));
		gpuBuilt_ = false;
		update();
	}

	void BspViewWidget::AddPlacedSprites(std::vector<PlacedSprite> sprites)
	{
		for (auto &sprite : sprites)
		{
			if (sprite.image.isNull() || sprite.widthUnits <= 0.0f ||
				sprite.heightUnits <= 0.0f)
				continue;
			// The texture slot is assigned at upload time.
			sprite.texture = -1;
			placedSprites_.push_back(std::move(sprite));
		}
		gpuBuilt_ = false;
		update();
	}

	void BspViewWidget::CameraBasis(QVector3D &forward, QVector3D &right,
		QVector3D &up) const
	{
		if (cameraMode_ == CameraMode::Fly)
			forward = BspFlyForward(flyYaw_, flyPitch_);
		else
			forward = (QVector3D(centerX_, centerY_, centerZ_) - OrbitEye()).normalized();
		const QVector3D worldUp(0.0f, 0.0f, 1.0f);
		right = QVector3D::crossProduct(forward, worldUp);
		if (right.lengthSquared() < 1e-8f)
			right = QVector3D(0.0f, 1.0f, 0.0f);
		right.normalize();
		up = QVector3D::crossProduct(right, forward).normalized();
	}

	void BspViewWidget::SetFieldOfView(float degrees)
	{
		fovDegrees_ = std::clamp(degrees, 30.0f, 120.0f);
		update();
	}

	void BspViewWidget::SetLightingMode(int mode)
	{
		lightingMode_ = mode == 1 ? 1 : 0;
		update();
	}

	void BspViewWidget::SetSkybox(const std::array<QImage, 6> &faces)
	{
		skyImages_ = faces;
		gpuBuilt_ = false;  // textures are uploaded with the rest of the scene
		update();
	}

	void BspViewWidget::SetSkyVisible(bool visible)
	{
		skyVisible_ = visible;
		pvsDirty_ = true;
		update();
	}

	void BspViewWidget::SetPvsCulling(bool enabled)
	{
		pvsCulling_ = enabled;
		pvsDirty_ = true;
		if (!enabled)
			pvsActive_ = false;
		update();
	}

	bool BspViewWidget::FaceVisible(int face) const
	{
		return face < 0 || static_cast<size_t>(face) >= faceVisible_.size() ||
			faceVisible_[static_cast<size_t>(face)] != 0;
	}

	bool BspViewWidget::BrushVisible(int model) const
	{
		return model < 0 || static_cast<size_t>(model) >= modelVisible_.size() ||
			modelVisible_[static_cast<size_t>(model)] != 0;
	}

	bool BspViewWidget::InstanceVisible(int instance) const
	{
		return instance < 0 || static_cast<size_t>(instance) >= instanceVisible_.size() ||
			instanceVisible_[static_cast<size_t>(instance)] != 0;
	}

	void BspViewWidget::UpdatePvs(const QVector3D &eye)
	{
		// The status line belongs to the PVS option: with it off (the PVS may
		// still run for the skybox) the status bar must stay clean.
		const auto report = [this](const QString &text)
		{
			emit PvsStatus(pvsCulling_ ? text : QString());
		};
		// The PVS also decides which world faces are drawn while the skybox is
		// shown: the game never draws other map segments (boss arena, cut-scene
		// room) behind a sky brush, and without this they would show through
		// the sky faces, which do not hide what stands behind them.
		if (!(pvsCulling_ || skyVisible_) || !map_ || map_->Models().empty() ||
			map_->Visibility().empty())
		{
			pvsActive_ = false;
			emit PvsStatus(QString());
			return;
		}
		// Like the server's fat PVS: every leaf within 8 units of the eye.
		const float mn[3] = { eye.x() - 8.0f, eye.y() - 8.0f, eye.z() - 8.0f };
		const float mx[3] = { eye.x() + 8.0f, eye.y() + 8.0f, eye.z() + 8.0f };
		const std::vector<int> leaves = BspLeavesTouchingBox(*map_, mn, mx).leaves;
		if (!pvsDirty_ && leaves == pvsEyeLeaves_)
		{
			report(pvsStatus_ + QStringLiteral(" | eye %1 %2 %3")
				.arg(qRound(eye.x())).arg(qRound(eye.y())).arg(qRound(eye.z())));
			return;
		}
		pvsEyeLeaves_ = leaves;
		pvsDirty_ = false;
		pvsActive_ = false;
		if (leaves.empty())
		{
			pvsStatus_ = QStringLiteral("PVS off here: camera is not inside the map's open space");
			report(pvsStatus_);
			return;  // Camera outside the map (solid): everything visible.
		}
		std::vector<uint8_t> row(
			static_cast<size_t>((map_->Models()[0].visLeafs + 7) >> 3), 0);
		for (int leaf : leaves)
			if (!BspOrLeafPvs(*map_, leaf, row))
			{
				pvsStatus_ = QStringLiteral("PVS off here: eye leaf %1 has no vis data").arg(leaf);
				report(pvsStatus_);
				return;  // A leaf without vis data: the engine shows everything.
			}
		const auto visible = [&row](const std::vector<int> &leafList)
		{
			if (leafList.empty())
				return true;  // Touches no open leaf: don't hide on a guess.
			for (int leaf : leafList)
			{
				const size_t bit = static_cast<size_t>(leaf - 1);
				if ((bit >> 3) < row.size() && (row[bit >> 3] & (1u << (bit & 7))))
					return true;
			}
			return false;
		};
		instanceVisible_.assign(instanceLeaves_.size(), 1);
		for (size_t i = 0; i < instanceLeaves_.size(); ++i)
			instanceVisible_[i] = visible(instanceLeaves_[i]) ? 1 : 0;
		faceVisible_.assign(map_->Faces().size(), 0);
		// The client draws map faces from the PVS of the leaf that holds the
		// eye point itself (R_MarkLeaves); the "fat" union over leaves within
		// 8 units is only for the server's entity visibility. Using the union
		// for faces lets neighbouring leaves leak in (the sky and buildings
		// behind a sealed room's grates on zs_lastride2).
		std::vector<uint8_t> faceRow = row;
		{
			const std::vector<int> eyeLeaf = BspPointEntityLeaves(
				*map_, eye.x(), eye.y(), eye.z());
			if (eyeLeaf.size() == 1)
			{
				std::vector<uint8_t> single(row.size(), 0);
				if (BspOrLeafPvs(*map_, eyeLeaf[0], single))
					faceRow = std::move(single);
			}
		}
		{
			const auto &leafList = map_->Leaves();
			const auto &marks = map_->MarkSurfaces();
			const int visLeafs = map_->Models()[0].visLeafs;
			for (int leaf = 1; leaf <= visLeafs && leaf < static_cast<int>(leafList.size()); ++leaf)
			{
				const size_t bit = static_cast<size_t>(leaf - 1);
				if (!((bit >> 3) < faceRow.size() && (faceRow[bit >> 3] & (1u << (bit & 7)))))
					continue;
				const auto &lf = leafList[static_cast<size_t>(leaf)];
				for (size_t k = 0; k < lf.numMarkSurfaces; ++k)
				{
					const size_t mi = static_cast<size_t>(lf.firstMarkSurface) + k;
					if (mi >= marks.size())
						break;
					if (marks[mi] < faceVisible_.size())
						faceVisible_[marks[mi]] = 1;
				}
			}
		}
		modelVisible_.assign(modelLeaves_.size(), 1);
		for (size_t i = 0; i < modelLeaves_.size(); ++i)
			modelVisible_[i] = visible(modelLeaves_[i]) ? 1 : 0;
		spriteVisible_.assign(spriteLeaves_.size(), 1);
		for (size_t i = 0; i < spriteLeaves_.size(); ++i)
			spriteVisible_[i] = visible(spriteLeaves_[i]) ? 1 : 0;
		pvsActive_ = true;
		{
			int modelsVisible = 0, modelsTotal = 0, spritesVisible = 0;
			for (size_t i = 0; i < instanceLeaves_.size(); ++i)
			{
				if (instanceLeaves_[i].empty() && (i >= map_->PlacedEntities().size()))
					continue;
				++modelsTotal;
				modelsVisible += instanceVisible_[i];
			}
			for (uint8_t v : spriteVisible_)
				spritesVisible += v;
			QStringList leafText;
			for (int leaf : leaves)
				leafText << QString::number(leaf);
			pvsStatus_ = QStringLiteral("PVS on: eye leaves %1 | %2/%3 entities, %4/%5 sprites drawn")
				.arg(leafText.join(QLatin1Char(',')))
				.arg(modelsVisible).arg(modelsTotal)
				.arg(spritesVisible).arg(spriteVisible_.size());
		}
		report(pvsStatus_ + QStringLiteral(" | eye %1 %2 %3")
			.arg(qRound(eye.x())).arg(qRound(eye.y())).arg(qRound(eye.z())));
	}

	void BspViewWidget::SetModelFill(float fill)
	{
		modelFill_ = std::clamp(fill, 0.0f, 1.0f);
		update();
	}

	void BspViewWidget::SetBrightness(float scale)
	{
		brightness_ = std::clamp(scale, 0.25f, 4.0f);
		update();
	}

	void BspViewWidget::SetFogEnabled(bool enabled)
	{
		fogEnabled_ = enabled;
		update();
	}

	void BspViewWidget::SetBackfaceCulling(bool enabled)
	{
		backfaceCull_ = enabled;
		update();
	}

	void BspViewWidget::SetWireframe(bool enabled)
	{
		wireframe_ = enabled;
		update();
	}

	void BspViewWidget::ResetView()
	{
		yaw_ = -90.0f;
		pitch_ = 20.0f;
		ComputeBounds();
		distance_ = radius_ * 3.0f;
		if (cameraMode_ == CameraMode::Fly)
			SeedFlyFromOrbit();
		update();
	}

	void BspViewWidget::SetCameraMode(CameraMode mode)
	{
		if (cameraMode_ == mode)
			return;
		cameraMode_ = mode;
		flyKeys_.clear();
		if (cameraMode_ == CameraMode::Fly)
		{
			SeedFlyFromOrbit();
			flyTimer_->start();
			setFocus(Qt::OtherFocusReason);  // Grab keys for WASD immediately.
		}
		else
		{
			flyTimer_->stop();
		}
		update();
	}

	QVector3D BspViewWidget::OrbitEye() const
	{
		const float pitchRad = qDegreesToRadians(pitch_);
		const float yawRad = qDegreesToRadians(yaw_);
		const float cp = std::cos(pitchRad);
		QVector3D dirToCamera(
			cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad));
		dirToCamera.normalize();
		return QVector3D(centerX_, centerY_, centerZ_) + dirToCamera * distance_;
	}

	void BspViewWidget::SeedFlyFromOrbit()
	{
		// Start flying from where the orbit camera is, looking at the map.
		flyPos_ = OrbitEye();
		const QVector3D forward =
			(QVector3D(centerX_, centerY_, centerZ_) - flyPos_).normalized();
		flyPitch_ = qRadiansToDegrees(std::asin(std::clamp(forward.z(), -1.0f, 1.0f)));
		flyYaw_ = qRadiansToDegrees(std::atan2(forward.y(), forward.x()));
	}

	void BspViewWidget::OnFlyTick()
	{
		if (cameraMode_ != CameraMode::Fly || flyKeys_.isEmpty())
			return;

		const QVector3D forward = BspFlyForward(flyYaw_, flyPitch_);
		const QVector3D worldUp(0.0f, 0.0f, 1.0f);
		QVector3D right = QVector3D::crossProduct(forward, worldUp);
		if (right.lengthSquared() < 1e-8f)
			right = QVector3D(0.0f, 1.0f, 0.0f);
		right.normalize();

		QVector3D move;
		if (flyKeys_.contains(Qt::Key_W))
			move += forward;
		if (flyKeys_.contains(Qt::Key_S))
			move -= forward;
		if (flyKeys_.contains(Qt::Key_D))
			move += right;
		if (flyKeys_.contains(Qt::Key_A))
			move -= right;
		if (flyKeys_.contains(Qt::Key_E) || flyKeys_.contains(Qt::Key_Space))
			move += worldUp;
		if (flyKeys_.contains(Qt::Key_Q) || flyKeys_.contains(Qt::Key_C))
			move -= worldUp;
		if (move.lengthSquared() < 1e-8f)
			return;
		move.normalize();

		// Map-sized default speed (units/second), Shift = 4x boost. Kept
		// slow on purpose: rooms are tiny next to the whole-map radius,
		// and Shift covers long distances.
		float speed = radius_ * 0.25f;
		if (flyKeys_.contains(Qt::Key_Shift))
			speed *= 4.0f;
		flyPos_ += move * speed * (16.0f / 1000.0f);
		update();
	}

	QImage BspViewWidget::FallbackTexture()
	{
		// Magenta/gray checker so missing map textures are obvious instead
		// of silently black.
		constexpr int kSize = 64;
		QImage image(kSize, kSize, QImage::Format_RGB888);
		for (int y = 0; y < kSize; ++y)
		{
			uint8_t *scan = image.scanLine(y);
			for (int x = 0; x < kSize; ++x)
			{
				const bool magenta = ((x / 8) + (y / 8)) % 2 == 0;
				scan[x * 3 + 0] = magenta ? 255 : 128;
				scan[x * 3 + 1] = magenta ? 0 : 128;
				scan[x * 3 + 2] = magenta ? 255 : 128;
			}
		}
		return image;
	}

	void BspViewWidget::ComputeBounds()
	{
		if (!map_ || map_->Vertices().empty())
		{
			centerX_ = centerY_ = centerZ_ = 0.0f;
			radius_ = 1000.0f;
			distance_ = radius_ * 3.0f;
			return;
		}

		float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
		bool first = true;
		for (const auto &v : map_->Vertices())
		{
			if (first)
			{
				minX = maxX = v.x;
				minY = maxY = v.y;
				minZ = maxZ = v.z;
				first = false;
			}
			else
			{
				minX = std::min(minX, v.x);
				maxX = std::max(maxX, v.x);
				minY = std::min(minY, v.y);
				maxY = std::max(maxY, v.y);
				minZ = std::min(minZ, v.z);
				maxZ = std::max(maxZ, v.z);
			}
		}

		centerX_ = (minX + maxX) * 0.5f;
		centerY_ = (minY + maxY) * 0.5f;
		centerZ_ = (minZ + maxZ) * 0.5f;
		radius_ = std::max({ maxX - minX, maxY - minY, maxZ - minZ }) * 0.5f;
		if (radius_ < 1.0f)
			radius_ = 1.0f;
		distance_ = radius_ * 3.0f;
	}

	void BspViewWidget::initializeGL()
	{
		initializeOpenGLFunctions();
		glEnable(GL_DEPTH_TEST);
		// No face culling: map faces are viewed from both sides (orbiting
		// outside as well as walking the rooms inside).
		glDisable(GL_CULL_FACE);
		glClearColor(0.16f, 0.18f, 0.20f, 1.0f);

		program_ = std::make_unique<QOpenGLShaderProgram>();
		if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
			!program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
			!program_->link())
		{
			qWarning().noquote() << QStringLiteral("BSP shader failed: %1").arg(program_->log());
			program_.reset();
			return;
		}

		mvpLoc_ = program_->uniformLocation("uMvp");
		normalMatrixLoc_ = program_->uniformLocation("uNormalMatrix");
		lightDirLoc_ = program_->uniformLocation("uLightDir");
		fillLightDirLoc_ = program_->uniformLocation("uFillLightDir");
		unlitLoc_ = program_->uniformLocation("uUnlit");
		texLoc_ = program_->uniformLocation("uTexture");
		useTexLoc_ = program_->uniformLocation("uUseTex");
		colorLoc_ = program_->uniformLocation("uColor");
		opacityLoc_ = program_->uniformLocation("uOpacity");
		tintLoc_ = program_->uniformLocation("uTint");
		lightMulLoc_ = program_->uniformLocation("uLightMul");
		chromeLoc_ = program_->uniformLocation("uChrome");
		skyLoc_ = program_->uniformLocation("uSky");
		waterLoc_ = program_->uniformLocation("uWater");
		cullBackLoc_ = program_->uniformLocation("uCullBack");
		alphaTestLoc_ = program_->uniformLocation("uNoAlphaTest");
		billboardLoc_ = program_->uniformLocation("uBillboard");
		eyeLoc_ = program_->uniformLocation("uEye");
		additiveLoc_ = program_->uniformLocation("uAdditive");
		studioLoc_ = program_->uniformLocation("uStudio");
		lightModeLoc_ = program_->uniformLocation("uLightMode");
		modelFillLoc_ = program_->uniformLocation("uModelFill");
		brightnessLoc_ = program_->uniformLocation("uBrightness");
		lightmapLoc_ = program_->uniformLocation("uLightmap");
	}

	void BspViewWidget::BuildGpuMeshes()
	{
		meshes_.clear();
		if (animTimer_ != nullptr)
			animTimer_->stop();
		glTextures_.clear();
		for (size_t i = 0; i < skyTextures_.size(); ++i)
		{
			skyTextures_[i].reset();
			if (skyImages_[i].isNull())
				continue;
			auto sky = std::make_unique<QOpenGLTexture>(skyImages_[i]);
			sky->setMinificationFilter(QOpenGLTexture::Linear);
			sky->setMagnificationFilter(QOpenGLTexture::Linear);
			sky->setWrapMode(QOpenGLTexture::ClampToEdge);
			skyTextures_[i] = std::move(sky);
		}
		triangleCount_ = 0;
		if (!map_ || !program_)
			return;

		// One GL texture per miptex slot, aligned with map->Textures().
		for (size_t i = 0; i < map_->Textures().size(); ++i)
		{
			QImage image = (i < textures_.size() && !textures_[i].isNull())
				? textures_[i]
				: FallbackTexture();
			std::vector<QImage> maskedLevels;
			std::unique_ptr<QOpenGLTexture> glTex;
			if (BuildMaskedMipChain(image, maskedLevels))
			{
				glTex = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
				glTex->setFormat(QOpenGLTexture::RGBA8_UNorm);
				glTex->setSize(maskedLevels[0].width(), maskedLevels[0].height());
				glTex->setMipLevels(static_cast<int>(maskedLevels.size()));
				glTex->allocateStorage();
				for (size_t level = 0; level < maskedLevels.size(); ++level)
					glTex->setData(static_cast<int>(level), QOpenGLTexture::RGBA,
						QOpenGLTexture::UInt8, maskedLevels[level].constBits());
				glTex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
				glTex->setMagnificationFilter(QOpenGLTexture::Linear);
				glTex->setWrapMode(QOpenGLTexture::Repeat);
			}
			else
			{
				glTex = std::make_unique<QOpenGLTexture>(image);
				glTex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
				glTex->setMagnificationFilter(QOpenGLTexture::Linear);
				glTex->setWrapMode(QOpenGLTexture::Repeat);
				glTex->generateMipMaps();
			}
			glTextures_.push_back(std::move(glTex));
		}
		// Placed-model textures follow the map slots (mesh indices were
		// rebased onto placedTextures_ by AddPlacedModels, shifted again
		// past the map slots below), then sprite images, then the shared
		// magenta fallback last.
		for (auto &placed : placedTextures_)
		{
			QImage image = placed.isNull() ? FallbackTexture() : BleedTransparentColors(placed);
			auto glTex = std::make_unique<QOpenGLTexture>(image);
			glTex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
			glTex->setMagnificationFilter(QOpenGLTexture::Linear);
			glTex->setWrapMode(QOpenGLTexture::ClampToEdge);
			glTex->generateMipMaps();
			glTextures_.push_back(std::move(glTex));
		}
		// Sprites repeating the same image share one GL texture.
		std::map<qint64, int> spriteTextureByKey;
		for (auto &sprite : placedSprites_)
		{
			if (!sprite.image.isNull())
			{
				const auto known = spriteTextureByKey.find(sprite.image.cacheKey());
				if (known != spriteTextureByKey.end())
				{
					sprite.texture = known->second;
					continue;
				}
				spriteTextureByKey.emplace(sprite.image.cacheKey(),
					static_cast<int>(glTextures_.size()));
			}
			sprite.texture = static_cast<int>(glTextures_.size());
			// Key-colour (blue) texels of alpha-tested sprites must not leak into
			// the edges through bilinear/mip filtering.
			QImage image = sprite.image.isNull() ? FallbackTexture() : BleedTransparentColors(sprite.image);
			auto glTex = std::make_unique<QOpenGLTexture>(image);
			glTex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
			glTex->setMagnificationFilter(QOpenGLTexture::Linear);
			glTex->setWrapMode(QOpenGLTexture::ClampToEdge);
			glTex->generateMipMaps();
			glTextures_.push_back(std::move(glTex));
		}
		auto fallbackTex = std::make_unique<QOpenGLTexture>(FallbackTexture());
		fallbackTex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
		fallbackTex->setMagnificationFilter(QOpenGLTexture::Linear);
		fallbackTex->setWrapMode(QOpenGLTexture::Repeat);
		fallbackTex->generateMipMaps();
		glTextures_.push_back(std::move(fallbackTex));
		const int fallbackIndex = static_cast<int>(glTextures_.size()) - 1;

		const std::vector<QImage> images = textures_;
		BspLightmapData lightmaps;
		std::vector<BspCpuMesh> cpuMeshes = BuildBspMeshes(*map_, images, &lightmaps);
		// Baked lightmap atlas (Linear/Linear, ClampToEdge, no mipmaps --
		// mipmaps would bleed neighbour faces into each other).
		lightmapTex_ = std::make_unique<QOpenGLTexture>(lightmaps.atlas);
		lightmapTex_->setMinificationFilter(QOpenGLTexture::Linear);
		lightmapTex_->setMagnificationFilter(QOpenGLTexture::Linear);
		lightmapTex_->setWrapMode(QOpenGLTexture::ClampToEdge);
		lightWhiteU_ = 0.5f / std::max(lightmaps.atlas.width(), 1);
		lightWhiteV_ = 0.5f / std::max(lightmaps.atlas.height(), 1);
		// Does this map store clipped (255) lightmap texels in quantity? Only
		// then the highlight roll-off of the shader is applied (see uKneeOn).
		{
			size_t lit = 0, clipped = 0;
			const QImage &atlas = lightmaps.atlas;
			for (int y = 0; y < atlas.height(); ++y)
			{
				const uchar *scan = atlas.constScanLine(y);
				for (int x = 1; x < atlas.width(); ++x)  // column 0 is reserved
				{
					const int peak = std::max({ scan[x * 3], scan[x * 3 + 1], scan[x * 3 + 2] });
					if (peak == 0)
						continue;
					++lit;
					if (peak >= 250)
						++clipped;
				}
			}
			kneeOn_ = (lit > 0 && clipped * 100 >= lit) ? 1.0f : 0.0f;  // >= 1%
		}
		// Placed models ride along in the same mesh list. They carry no
		// light data, so point them at the atlas' white (fullbright) texel.
		// Work on copies: mutating placedMeshes_ here would shift texture
		// indices twice on a second build.
		// Instances sharing (texture, render class, opacity, tint) are merged
		// into one buffer: a thousand props become a few dozen draw calls.
		const int placedBase = static_cast<int>(map_->Textures().size());
		std::map<std::tuple<int, int, float, float, float, float,
		float, float, float, bool>, size_t> mergedIndex;
		for (const auto &mesh : placedMeshes_)
		{
			BspCpuMesh copy = mesh;
			if (copy.texture >= 0)
				copy.texture += placedBase;
			if (copy.lightUv.size() != copy.verts.size() / 8 * 2)
			{
				copy.lightUv.assign(copy.verts.size() / 8 * 2, 0.0f);
				for (size_t i = 0; i < copy.lightUv.size(); i += 2)
				{
					copy.lightUv[i] = lightWhiteU_;
					copy.lightUv[i + 1] = lightWhiteV_;
				}
			}
			const auto key = std::make_tuple(copy.texture,
				static_cast<int>(copy.renderClass), copy.opacity,
				copy.tint[0], copy.tint[1], copy.tint[2],
				copy.lightMul[0], copy.lightMul[1], copy.lightMul[2], copy.chrome);
			// Translucent placed models are never merged: each instance
			// must be its own sortable chunk (one range, mesh centroid).
			if (copy.renderClass == BspRenderClass::Blend)
			{
				copy.studio = true;
				BspCpuMesh::FaceRange range;
				range.first = 0;
				range.count = static_cast<int>(copy.verts.size() / 8);
				for (int v = 0; v < range.count; ++v)
					for (int k = 0; k < 3; ++k)
						range.center[k] += copy.verts[static_cast<size_t>(v) * 8 + k];
				for (int k = 0; k < 3; ++k)
					range.center[k] /= static_cast<float>(std::max(range.count, 1));
				copy.faceRanges.assign(1, range);
				cpuMeshes.push_back(std::move(copy));
				continue;
			}
			const auto found = mergedIndex.find(key);
			if (found == mergedIndex.end())
			{
				copy.studio = true;
				copy.instances.assign(1, { 0,
					static_cast<int>(copy.verts.size() / 8), copy.instance });
				// The buffer now holds many entities: visibility is decided per
				// range, not by the entity that happened to come first.
				copy.instance = -1;
				mergedIndex.emplace(key, cpuMeshes.size());
				cpuMeshes.push_back(std::move(copy));
			}
			else
			{
				BspCpuMesh &target = cpuMeshes[found->second];
				// Remember which vertices belong to which entity, so PVS
				// culling can skip single entities inside the merged buffer.
				const int addFirst = static_cast<int>(target.verts.size() / 8);
				const int addCount = static_cast<int>(copy.verts.size() / 8);
				if (!target.instances.empty() &&
					target.instances.back().instance == copy.instance &&
					target.instances.back().first + target.instances.back().count == addFirst)
					target.instances.back().count += addCount;
				else
					target.instances.push_back({ addFirst, addCount, copy.instance });
				target.verts.insert(target.verts.end(),
					copy.verts.begin(), copy.verts.end());
				target.lightUv.insert(target.lightUv.end(),
					copy.lightUv.begin(), copy.lightUv.end());
			}
		}

		// Leaves every brush entity (func_wall, doors, ...) touches, from the
		// extent of its emitted faces.
		{
			std::map<int, std::array<float, 6>> bounds;
			for (const auto &cpu : cpuMeshes)
				for (const auto &span : cpu.brushSpans)
				{
					auto it = bounds.find(span.instance);
					for (int v = span.first; v < span.first + span.count; ++v)
					{
						const float *p = &cpu.verts[static_cast<size_t>(v) * 8];
						if (it == bounds.end())
							it = bounds.emplace(span.instance, std::array<float, 6>{
								p[0], p[1], p[2], p[0], p[1], p[2] }).first;
						for (int k = 0; k < 3; ++k)
						{
							it->second[static_cast<size_t>(k)] = std::min(it->second[static_cast<size_t>(k)], p[k]);
							it->second[static_cast<size_t>(k) + 3] = std::max(it->second[static_cast<size_t>(k) + 3], p[k]);
						}
					}
				}
			modelLeaves_.clear();
			if (!bounds.empty())
				modelLeaves_.assign(static_cast<size_t>(bounds.rbegin()->first) + 1, {});
			for (const auto &entry : bounds)
			{
				const float mn[3] = { entry.second[0] - 1.0f, entry.second[1] - 1.0f, entry.second[2] - 1.0f };
				const float mx[3] = { entry.second[3] + 1.0f, entry.second[4] + 1.0f, entry.second[5] + 1.0f };
				modelLeaves_[static_cast<size_t>(entry.first)] = BspEntityLeaves(*map_, mn, mx);
			}
		}
		// Leaves every placed entity touches (for PVS culling): the box of all
		// its meshes, padded by one unit like the engine's absmin/absmax.
		{
			int maxId = -1;
			for (const auto &mesh : placedMeshes_)
				maxId = std::max(maxId, mesh.instance);
			struct Box { float mn[3]; float mx[3]; bool valid; };
			std::vector<Box> boxes(static_cast<size_t>(maxId + 1),
				Box{ { 0, 0, 0 }, { 0, 0, 0 }, false });
			for (const auto &mesh : placedMeshes_)
			{
				if (mesh.instance < 0)
					continue;
				Box &b = boxes[static_cast<size_t>(mesh.instance)];
				for (size_t v = 0; v + 7 < mesh.verts.size(); v += 8)
				{
					if (!b.valid)
					{
						for (int k = 0; k < 3; ++k)
							b.mn[k] = b.mx[k] = mesh.verts[v + static_cast<size_t>(k)];
						b.valid = true;
					}
					for (int k = 0; k < 3; ++k)
					{
						const float c = mesh.verts[v + static_cast<size_t>(k)];
						b.mn[k] = std::min(b.mn[k], c);
						b.mx[k] = std::max(b.mx[k], c);
					}
				}
			}
			// Use the entity's origin leaf rather than its model bounds or a
			// padded hull. Long Studio props can cross PVS boundaries while their
			// origin is fully hidden; the game culls those entities as a unit.
			// Mesh bounds remain a fallback for any instance without a map entity.
			const auto &placedEntities = map_->PlacedEntities();
			instanceLeaves_.assign(boxes.size(), {});
			for (size_t i = 0; i < boxes.size(); ++i)
			{
				if (i < placedEntities.size())
				{
					const auto &origin = placedEntities[i].origin;
					instanceLeaves_[i] = BspPointEntityLeaves(
						*map_, origin[0], origin[1], origin[2]);
					// Origin inside solid (the end pieces of the elevator cables on
					// zs_anothertruth2 sit in the shaft wall). The engine links such
					// an entity by the box origin +-1 (these .mdl files have a zero
					// bounding box in their header, so the entity has no size of its
					// own) and skips solid leaves. Do the same. If that box touches
					// no open leaf the engine has no leaf to test and never sends the
					// entity, so hide it: leaf 0 is the solid leaf, it is in no PVS
					// row, and InstanceVisible() then reports "not visible". (Testing
					// the whole model box instead made the cables stay visible
					// through the wall whenever their long box reached any visible
					// leaf.)
					if (instanceLeaves_[i].empty())
					{
						const float mn[3] = { origin[0] - 1.0f, origin[1] - 1.0f, origin[2] - 1.0f };
						const float mx[3] = { origin[0] + 1.0f, origin[1] + 1.0f, origin[2] + 1.0f };
						instanceLeaves_[i] = BspEntityLeaves(*map_, mn, mx);
						if (instanceLeaves_[i].empty())
							instanceLeaves_[i] = { 0 };
					}
				}
				else if (boxes[i].valid)
				{
					float mn[3], mx[3];
					for (int k = 0; k < 3; ++k)
					{
						mn[k] = boxes[i].mn[k] - 1.0f;
						mx[k] = boxes[i].mx[k] + 1.0f;
					}
					instanceLeaves_[i] = BspEntityLeaves(*map_, mn, mx);
				}
				else
					continue;
			}
			spriteLeaves_.assign(placedSprites_.size(), {});
			for (size_t i = 0; i < placedSprites_.size(); ++i)
			{
				const PlacedSprite &sp = placedSprites_[i];
				float r = std::max({ std::fabs(sp.left), std::fabs(sp.right),
					std::fabs(sp.up), std::fabs(sp.down) });
				if (r <= 0.0f)
					r = 0.5f * std::max(sp.widthUnits, sp.heightUnits);
				r += 1.0f;
				const float mn[3] = { sp.origin.x() - r, sp.origin.y() - r, sp.origin.z() - r };
				const float mx[3] = { sp.origin.x() + r, sp.origin.y() + r, sp.origin.z() + r };
				spriteLeaves_[i] = BspEntityLeaves(*map_, mn, mx);
			}
			pvsDirty_ = true;
			pvsActive_ = false;
		}

		const int posLoc = 0;
		const int normLoc = 1;
		const int uvLoc = 2;
		const int lightUvLoc = 3;
		const int stride = 8 * sizeof(float);
		const int lightStride = 2 * sizeof(float);

		for (const auto &cpu : cpuMeshes)
		{
			if (cpu.verts.empty())
				continue;
			auto gpu = std::make_unique<GpuMesh>();
			gpu->texture = (cpu.texture >= 0 &&
				cpu.texture < static_cast<int>(glTextures_.size()) - 1)
				? cpu.texture
				: fallbackIndex;
			gpu->renderClass = cpu.renderClass;
			gpu->opacity = cpu.opacity;
			gpu->tint[0] = cpu.tint[0];
			gpu->tint[1] = cpu.tint[1];
			gpu->tint[2] = cpu.tint[2];
			gpu->studio = cpu.studio;
			gpu->chrome = cpu.chrome;
			gpu->sky = cpu.sky;
			gpu->billboard = cpu.billboard;
			gpu->spinDegPerSec = cpu.spinDegPerSec;
			gpu->spinPivot[0] = cpu.spinPivot[0];
			gpu->spinPivot[1] = cpu.spinPivot[1];
			gpu->pulseFx = cpu.pulseFx;
			gpu->water = !cpu.studio && cpu.texture >= 0 &&
				cpu.texture < static_cast<int>(map_->Textures().size()) &&
				!map_->Textures()[static_cast<size_t>(cpu.texture)].name.empty() &&
				map_->Textures()[static_cast<size_t>(cpu.texture)].name[0] == '!';
			for (int k = 0; k < 3; ++k)
				gpu->lightMul[k] = cpu.lightMul[k];
			gpu->brushEntity = cpu.brushEntity;
			gpu->instance = cpu.instance;
			gpu->instances = cpu.instances;
			gpu->faceSpans = cpu.faceSpans;
			gpu->brushSpans = cpu.brushSpans;
			gpu->faceRanges = cpu.faceRanges;
			gpu->verts = cpu.verts;
			gpu->count = static_cast<GLsizei>(cpu.verts.size() / 8);
			triangleCount_ += static_cast<size_t>(gpu->count) / 3;
			std::vector<float> lightData = cpu.lightUv;
			if (lightData.size() != static_cast<size_t>(gpu->count) * 2)
			{
				lightData.assign(static_cast<size_t>(gpu->count) * 2, 0.0f);
				for (size_t i = 0; i < lightData.size(); i += 2)
				{
					lightData[i] = lightWhiteU_;
					lightData[i + 1] = lightWhiteV_;
				}
			}

			gpu->vbo.create();
			gpu->vbo.bind();
			gpu->vbo.allocate(gpu->verts.data(),
				static_cast<int>(gpu->verts.size()) * sizeof(float));
			gpu->lightVbo.create();
			gpu->lightVbo.bind();
			gpu->lightVbo.allocate(lightData.data(),
				static_cast<int>(lightData.size()) * sizeof(float));
			gpu->vao.create();
			gpu->vao.bind();
			// NOTE: setAttributeBuffer records the CURRENTLY BOUND array
			// buffer into the VAO, so (re)bind the right buffer before
			// each attribute group.
			gpu->vbo.bind();
			program_->enableAttributeArray(posLoc);
			program_->setAttributeBuffer(posLoc, GL_FLOAT, 0, 3, stride);
			program_->enableAttributeArray(normLoc);
			program_->setAttributeBuffer(normLoc, GL_FLOAT, 3 * sizeof(float), 3, stride);
			program_->enableAttributeArray(uvLoc);
			program_->setAttributeBuffer(uvLoc, GL_FLOAT, 6 * sizeof(float), 2, stride);
			gpu->lightVbo.bind();
			program_->enableAttributeArray(lightUvLoc);
			program_->setAttributeBuffer(lightUvLoc, GL_FLOAT, 0, 2, lightStride);
			gpu->vao.release();
			gpu->vbo.release();
			gpu->lightVbo.release();
			meshes_.push_back(std::move(gpu));
		}
		UpdateAnimTimer();
	}

	void BspViewWidget::UpdateAnimTimer()
	{
		// Spinning signs (and pulsing ones, when enabled) need a steady repaint.
		bool animated = false;
		for (const auto &m : meshes_)
			if (m->spinDegPerSec != 0.0f || (pulseEnabled_ && m->pulseFx != 0))
				animated = true;
		if (animated)
		{
			if (animTimer_ == nullptr)
			{
				animTimer_ = new QTimer(this);
				animTimer_->setInterval(33);
				connect(animTimer_, &QTimer::timeout, this, [this]() { update(); });
			}
			animTimer_->start();
		}
		else if (animTimer_ != nullptr)
		{
			animTimer_->stop();
		}
	}

	void BspViewWidget::SetPulseEnabled(bool enabled)
	{
		if (pulseEnabled_ == enabled)
			return;
		pulseEnabled_ = enabled;
		UpdateAnimTimer();
		update();
	}

	void BspViewWidget::SetupMatrices(int width, int height)
	{
		const float aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;

		QMatrix4x4 projection;
		// GoldSrc itself uses a near plane of 4; 1 gives 4x less depth
		// precision and made coplanar surfaces flicker at a distance.
		projection.perspective(fovDegrees_, aspect, 4.0f, 100000.0f);

		QMatrix4x4 model;  // Identity: BSP verts are already in world space.

		QMatrix4x4 view;
		if (cameraMode_ == CameraMode::Fly)
		{
			const QVector3D forward = BspFlyForward(flyYaw_, flyPitch_);
			view.lookAt(flyPos_, flyPos_ + forward, QVector3D(0, 0, 1));
		}
		else
		{
			const QVector3D centerWorld(centerX_, centerY_, centerZ_);
			const QVector3D eye = OrbitEye();

			view.lookAt(eye, centerWorld, QVector3D(0, 0, 1));
		}

		const QMatrix4x4 mvp = projection * view * model;
		bool invertible = false;
		const QMatrix4x4 normalFinal = (view * model).inverted(&invertible).transposed();

		QMatrix4x4 lightRotation;  // Fixed headlight pair, like the .mdl viewer.
		const QVector3D keyLight = lightRotation.mapVector(QVector3D(0.4f, 0.6f, 1.0f));
		const QVector3D fillLight = lightRotation.mapVector(QVector3D(-0.5f, -0.3f, 0.6f));

		program_->bind();
		program_->setUniformValue(mvpLoc_, mvp);
		program_->setUniformValue(normalMatrixLoc_, normalFinal);
		program_->setUniformValue(lightDirLoc_, keyLight);
		program_->setUniformValue(fillLightDirLoc_, fillLight);
		program_->setUniformValue(texLoc_, 0);
		program_->setUniformValue(lightmapLoc_, 1);
		program_->release();
	}

	void BspViewWidget::paintGL()
	{
		// With fog the void behind distant geometry is black in game (nothing
		// is drawn there), not the viewer's neutral grey.
		if (FogActive())
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		else
			glClearColor(0.16f, 0.18f, 0.20f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDisable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_TRUE);

		if (!map_ || !program_)
			return;

		if (!gpuBuilt_)
		{
			BuildGpuMeshes();
			gpuBuilt_ = true;
		}

		SetupMatrices(width(), height());
		UpdatePvs(cameraMode_ == CameraMode::Fly ? flyPos_ : OrbitEye());

		program_->bind();
		program_->setUniformValue(useTexLoc_, wireframe_ ? 0.0f : 1.0f);
		program_->setUniformValue(colorLoc_, QVector4D(0.7f, 0.7f, 0.7f, 1.0f));
		program_->setUniformValue(unlitLoc_, 0.0f);
		program_->setUniformValue(additiveLoc_, 0.0f);
		program_->setUniformValue(lightModeLoc_, lightingMode_ == 1 ? 1.0f : 0.0f);
		program_->setUniformValue(modelFillLoc_, modelFill_);
		program_->setUniformValue(brightnessLoc_, brightness_);
		program_->setUniformValue("uWhiteUv", QVector2D(lightWhiteU_, lightWhiteV_));
		program_->setUniformValue("uKneeOn", kneeOn_);
		program_->setUniformValue("uFogColor",
			QVector3D(fogColor_[0], fogColor_[1], fogColor_[2]));
		// The map's density is fed to the viewer's exp^2 fog scaled by this
		// factor: the game's fog is much less noticeable in open areas than
		// the raw value gives (zs_lastride2). Tune here; 1.0 = raw density.
		constexpr float kFogDensityScale = 0.5f;
		program_->setUniformValue("uFogDensity",
			FogActive() ? fogDensity_ * kFogDensityScale : 0.0f);
		if (lightmapTex_ != nullptr)
			lightmapTex_->bind(1);

		// Skybox. The engine draws no cube: the map's own brush faces textured
		// "sky" show the sky box, looked up by the direction from the eye to the
		// fragment (see the uSky branch of the fragment shader). Bind the six
		// face textures per Quake's axis order: +X rt, -X lf, +Y bk, -Y ft,
		// +Z up, -Z dn  (skyTextures_ is stored as ft, bk, lf, rt, up, dn).
		program_->setUniformValue(eyeLoc_,
			cameraMode_ == CameraMode::Fly ? flyPos_ : OrbitEye());
		bool haveSky = false;
		if (!wireframe_ && skyVisible_)
		{
			static const int kAxisToStored[6] = { 3, 2, 1, 0, 4, 5 };
			for (int axis = 0; axis < 6; ++axis)
			{
				const auto &tex = skyTextures_[static_cast<size_t>(kAxisToStored[axis])];
				if (tex == nullptr)
					continue;
				tex->bind(2 + axis);
				haveSky = true;
			}
			glActiveTexture(GL_TEXTURE0);
			if (haveSky)
			{
				for (int axis = 0; axis < 6; ++axis)
					program_->setUniformValue(
						QByteArray("uSky") + QByteArray::number(axis), 2 + axis);
				const QVector3D skyEye = cameraMode_ == CameraMode::Fly ? flyPos_ : OrbitEye();
				program_->setUniformValue("uEye", skyEye);
			}
		}

		// first/count (vertices) select a sub-range; count < 0 = whole mesh.
		const bool entityCull = pvsActive_ && pvsCulling_;
		// Map faces follow the PVS like in game when the skybox is shown or the
		// user turned PVS culling on (then everything not visible from the
		// camera's leaf stays hidden: e.g. what lies behind a grate of a sealed
		// room is black in game, since vis treats the grate brushes as solid).
		const bool worldCull = pvsActive_ && (haveSky || pvsCulling_);
		// Animation clock for spinning / pulsing brush signs.
		static const auto animEpoch = std::chrono::steady_clock::now();
		const float animT = std::chrono::duration<float>(
			std::chrono::steady_clock::now() - animEpoch).count();
		bool spinWasOn = true;  // forces a reset on the first drawn mesh
		const auto drawMesh = [&](const std::unique_ptr<GpuMesh> &gpu,
			GLint first = 0, GLsizei count = -1)
		{
			// Sky faces show the skybox only when one was loaded; otherwise they
			// stay invisible as before (the clear colour shows through).
			if (gpu->sky && !haveSky)
				return;
			if (entityCull && gpu->instance >= 0 && !InstanceVisible(gpu->instance))
				return;
			program_->setUniformValue(skyLoc_, gpu->sky ? 1.0f : 0.0f);
		program_->setUniformValue(waterLoc_, gpu->water ? 1.0f : 0.0f);
			program_->setUniformValue(cullBackLoc_,
				(!wireframe_ && backfaceCull_ && !gpu->studio && !gpu->sky &&
					!gpu->billboard && !gpu->water) ? 1.0f : 0.0f);
			// World (worldspawn) faces are never alpha-tested.
			program_->setUniformValue(alphaTestLoc_,
				(!gpu->brushEntity && !gpu->studio && !gpu->sky && !gpu->billboard &&
					!gpu->water) ? 1.0f : 0.0f);
			program_->setUniformValue(billboardLoc_, gpu->billboard ? 1.0f : 0.0f);
			if (gpu->texture >= 0 && gpu->texture < static_cast<int>(glTextures_.size()))
				glTextures_[static_cast<size_t>(gpu->texture)]->bind(0);
			// Studio models wind triangles clockwise: cull FRONT faces
			// (as HLAM/GoldSource and the .mdl viewer do). Brush faces
			// are seen from both sides, so culling stays off for them.
			program_->setUniformValue(studioLoc_, gpu->studio ? 1.0f : 0.0f);
			if (gpu->studio && !wireframe_)
			{
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT);
			}
			else
			{
				glDisable(GL_CULL_FACE);
			}
			// Brush entities (and everything translucent) sit on or right
			// above world surfaces: bias them toward the eye so coplanar
			// faces (glow arrows, water, signs on walls) stop z-fighting.
			if (!wireframe_ && (gpu->brushEntity ||
				gpu->renderClass != BspRenderClass::Opaque))
			{
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(0.0f, -10.0f);
			}
			else
			{
				glDisable(GL_POLYGON_OFFSET_FILL);
			}
			// renderfx pulse (GoldSrc: renderamt +- 16 (slow/fast) or 64 (wide),
			// sine at 2 or 8 rad/s, as Xash3D's CL_FxBlend); the engine's per-entity phase
			// offset (index * 363) is not modelled. Only when enabled in the UI.
			float meshOpacity = gpu->opacity;
			if (pulseEnabled_ && gpu->pulseFx >= 1 && gpu->pulseFx <= 4)
			{
				const float amp = (gpu->pulseFx >= 3 ? 64.0f : 16.0f) / 255.0f;
				const float freq = (gpu->pulseFx == 2 || gpu->pulseFx == 4) ? 8.0f : 2.0f;
				meshOpacity = std::clamp(meshOpacity + amp * std::sin(freq * animT), 0.0f, 1.0f);
			}
			program_->setUniformValue(opacityLoc_, meshOpacity);
			if (gpu->spinDegPerSec != 0.0f)
			{
				const float angle = std::fmod(gpu->spinDegPerSec * animT, 360.0f) *
					3.14159265f / 180.0f;
				program_->setUniformValue("uSpin",
					QVector4D(gpu->spinPivot[0], gpu->spinPivot[1], angle, 1.0f));
				spinWasOn = true;
			}
			else if (spinWasOn)
			{
				program_->setUniformValue("uSpin", QVector4D(0.0f, 0.0f, 0.0f, 0.0f));
				spinWasOn = false;
			}
			program_->setUniformValue(tintLoc_,
				QVector3D(gpu->tint[0], gpu->tint[1], gpu->tint[2]));
			program_->setUniformValue(chromeLoc_, gpu->chrome ? 1.0f : 0.0f);
			program_->setUniformValue(lightMulLoc_,
				QVector3D(gpu->lightMul[0], gpu->lightMul[1], gpu->lightMul[2]));
			gpu->vao.bind();
			const bool cullFaces = count < 0 && worldCull && !gpu->faceSpans.empty();
			const bool cullBrush = count < 0 && worldCull && !gpu->brushSpans.empty();
			const bool cullInstances = count < 0 && entityCull && !gpu->instances.empty();
			if (cullFaces || cullBrush || cullInstances)
			{
				// Merged buffer of many entities / world faces: draw only the
				// runs that are in the PVS (adjacent visible ones as one call).
				const auto &spans = cullFaces ? gpu->faceSpans
					: (cullBrush ? gpu->brushSpans : gpu->instances);
				GLint runFirst = 0;
				GLsizei runCount = 0;
				for (const auto &r : spans)
				{
					if (cullFaces ? !FaceVisible(r.instance)
						: (cullBrush ? !BrushVisible(r.instance) : !InstanceVisible(r.instance)))
						continue;
					if (runCount > 0 && runFirst + runCount == r.first)
					{
						runCount += r.count;
						continue;
					}
					if (runCount > 0)
						glDrawArrays(GL_TRIANGLES, runFirst, runCount);
					runFirst = r.first;
					runCount = r.count;
				}
				if (runCount > 0)
					glDrawArrays(GL_TRIANGLES, runFirst, runCount);
			}
			else
			{
				glDrawArrays(GL_TRIANGLES, first, count < 0 ? gpu->count : count);
			}
			gpu->vao.release();
		};

		if (wireframe_)
		{
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		}

		// Opaque pass: everything except translucent/additive brush meshes
		// (in wireframe mode those join in too -- outlines don't blend).
		bool hasBlend = false;
		bool hasAdditive = false;
		// Sky faces first and without depth writes: in the game they only
		// paint the sky colour and everything drawn afterwards (models, world
		// geometry) shows over them even when it stands behind the sky brush
		// (e.g. the cranes of zs_rendezvous1 beyond the central sky wall).
		// Depth-testing them like walls cut those objects off.
		if (haveSky)
		{
			glDepthMask(GL_FALSE);
			for (const auto &gpu : meshes_)
				if (gpu->sky)
					drawMesh(gpu);
			glDepthMask(GL_TRUE);
		}
		for (const auto &gpu : meshes_)
		{
			if (gpu->sky)
				continue;
			if (!wireframe_ && gpu->renderClass != BspRenderClass::Opaque)
			{
				if (gpu->renderClass == BspRenderClass::Blend)
					hasBlend = true;
				else
					hasAdditive = true;
				continue;
			}
			drawMesh(gpu);
		}

		// Translucent pass: rendermode 2 "texture" brush faces, plain alpha
		// blend by renderamt, no depth write (but depth test stays on).
		if (hasBlend)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			// Glow arrows / signs lie ~1 unit above floors; with a 1..500000
			// depth range they z-fight (flicker) far from the camera. Pull
			// translucent brush faces slightly toward the eye.
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -8.0f);
			// No depth write means draw order IS the compositing order. Mesh
			// order (by texture/opacity key) is arbitrary, so a masked
			// rendermode-2 window or sign drawn after a pane of glass would
			// overwrite it untinted ("black objects show through glass").
			// Like the engine, sort back to front (per face / per placed
			// model) by distance from the eye.
			const QVector3D eye = cameraMode_ == CameraMode::Fly ? flyPos_ : OrbitEye();
			struct BlendChunk
			{
				const std::unique_ptr<GpuMesh> *mesh;
				int first;
				int count;
				float dist2;
			};
			std::vector<BlendChunk> chunks;
			for (const auto &gpu : meshes_)
			{
				if (gpu->renderClass != BspRenderClass::Blend)
					continue;
				if (entityCull && gpu->instance >= 0 && !InstanceVisible(gpu->instance))
					continue;
				if (gpu->faceRanges.empty())
				{
					chunks.push_back({ &gpu, 0, gpu->count, 0.0f });
					continue;
				}
				for (const auto &r : gpu->faceRanges)
				{
					if (worldCull && r.face >= 0 && !FaceVisible(r.face))
						continue;
					if (worldCull && r.model >= 0 && !BrushVisible(r.model))
						continue;
					const float dx = r.center[0] - eye.x();
					const float dy = r.center[1] - eye.y();
					const float dz = r.center[2] - eye.z();
					chunks.push_back({ &gpu, r.first, r.count,
						dx * dx + dy * dy + dz * dz });
				}
			}
			std::stable_sort(chunks.begin(), chunks.end(),
				[](const BlendChunk &a, const BlendChunk &b) { return a.dist2 > b.dist2; });
			for (const BlendChunk &chunk : chunks)
				drawMesh(*chunk.mesh, chunk.first, chunk.count);
			glDisable(GL_POLYGON_OFFSET_FILL);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
		}

		// Additive pass: rendermode 5 glow/beams, same recipe as the .mdl
		// viewer's additive pass (adds light instead of alpha-compositing,
		// unlit, no depth write so it layers like light).
		if (hasAdditive)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glDepthMask(GL_FALSE);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -8.0f);
			program_->setUniformValue(unlitLoc_, 1.0f);
			program_->setUniformValue(additiveLoc_, 1.0f);
			for (const auto &gpu : meshes_)
			{
				if (gpu->renderClass != BspRenderClass::Additive)
					continue;
				drawMesh(gpu);
			}
			program_->setUniformValue(additiveLoc_, 0.0f);
			program_->setUniformValue(unlitLoc_, 0.0f);
			glDisable(GL_POLYGON_OFFSET_FILL);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
		}

		if (wireframe_)
		{
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}
		glDisable(GL_CULL_FACE);

		// Placed sprites (env_sprite .spr files): camera-facing quads drawn
		// last so translucent glows layer over everything behind them.
		// Sprites are self-lit; per-sprite blend state follows rendermode.
		// Sprites are drawn with their own vertices: no sign spin on them.
		program_->setUniformValue("uSpin", QVector4D(0.0f, 0.0f, 0.0f, 0.0f));
		if (!placedSprites_.empty())
		{
			if (!spriteBuffersInit_)
			{
				spriteVbo_.create();
				spriteVao_.create();
				spriteBuffersInit_ = true;
			}
			QVector3D forward, right, up;
			CameraBasis(forward, right, up);
			const int posLoc = 0;
			const int normLoc = 1;
			const int uvLoc = 2;
			const int lightUvLoc = 3;
			const int stride = 10 * sizeof(float);
			program_->setUniformValue(useTexLoc_, 1.0f);
			program_->setUniformValue(colorLoc_, QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
			program_->setUniformValue(unlitLoc_, 1.0f);
			program_->setUniformValue(alphaTestLoc_, 0.0f);  // sprites are alpha-tested
			spriteVao_.bind();
			spriteVbo_.bind();
			spriteVbo_.allocate(6 * 10 * sizeof(float));
			program_->enableAttributeArray(posLoc);
			program_->setAttributeBuffer(posLoc, GL_FLOAT, 0, 3, stride);
			program_->enableAttributeArray(normLoc);
			program_->setAttributeBuffer(normLoc, GL_FLOAT, 3 * sizeof(float), 3, stride);
			program_->enableAttributeArray(uvLoc);
			program_->setAttributeBuffer(uvLoc, GL_FLOAT, 6 * sizeof(float), 2, stride);
			program_->enableAttributeArray(lightUvLoc);
			program_->setAttributeBuffer(lightUvLoc, GL_FLOAT, 8 * sizeof(float), 2, stride);
			for (size_t spriteIndex = 0; spriteIndex < placedSprites_.size(); ++spriteIndex)
			{
				const auto &sprite = placedSprites_[spriteIndex];
				if (entityCull && spriteIndex < spriteVisible_.size() &&
					!spriteVisible_[spriteIndex])
					continue;
				// Orientation as in the engine's R_DrawSpriteModel.
				QVector3D sr = right, su = up;
				const QVector3D worldUp(0.0f, 0.0f, 1.0f);
				switch (sprite.orientation)
				{
				case 0:  // vp_parallel_upright: view-parallel, world-vertical.
					su = worldUp;
					break;
				case 1:  // facing_upright: turns toward the eye around Z.
				{
					const QVector3D eyePos = cameraMode_ == CameraMode::Fly
						? flyPos_ : OrbitEye();
					QVector3D d = sprite.origin - eyePos;
					d.setZ(0.0f);
					if (d.lengthSquared() > 1e-8f)
					{
						d.normalize();
						sr = QVector3D(d.y(), -d.x(), 0.0f);
					}
					su = worldUp;
					break;
				}
				case 3:  // oriented: fixed by the entity's angles.
				{
					const float pitch = qDegreesToRadians(sprite.angles[0]);
					const float yaw = qDegreesToRadians(sprite.angles[1]);
					const float roll = qDegreesToRadians(sprite.angles[2]);
					const float sp = std::sin(pitch), cp = std::cos(pitch);
					const float sy = std::sin(yaw), cy = std::cos(yaw);
					const float sro = std::sin(roll), cro = std::cos(roll);
					sr = QVector3D(-sro * sp * cy + cro * sy,
						-sro * sp * sy - cro * cy, -sro * cp);
					su = QVector3D(cro * sp * cy + sro * sy,
						cro * sp * sy - sro * cy, cro * cp);
					break;
				}
				case 4:  // vp_parallel_oriented: view-parallel, rolled.
				{
					const float roll = qDegreesToRadians(sprite.angles[2]);
					const float sro = std::sin(roll), cro = std::cos(roll);
					sr = right * cro + up * sro;
					su = right * -sro + up * cro;
					break;
				}
				default:
					break;
				}
				float qLeft = sprite.left, qRight = sprite.right;
				float qUp = sprite.up, qDown = sprite.down;
				if (qLeft == 0.0f && qRight == 0.0f && qUp == 0.0f && qDown == 0.0f)
				{
					qLeft = -sprite.widthUnits * 0.5f;
					qRight = sprite.widthUnits * 0.5f;
					qDown = -sprite.heightUnits * 0.5f;
					qUp = sprite.heightUnits * 0.5f;
				}
				if (sr.lengthSquared() > 1e-8f)
					sr.normalize();
				if (su.lengthSquared() > 1e-8f)
					su.normalize();
				const QVector3D normal = QVector3D::crossProduct(sr, su);
				const QVector3D c0 = sprite.origin + sr * qLeft + su * qDown;
				const QVector3D c1 = sprite.origin + sr * qRight + su * qDown;
				const QVector3D c2 = sprite.origin + sr * qLeft + su * qUp;
				const QVector3D c3 = sprite.origin + sr * qRight + su * qUp;
				// (pos, normal, uv, lightUv) x6; v=0 is the image top edge.
				// Sprites are unlit, so the lightmap UV is the white texel.
				const float corners[4][3] = {
					{ c0.x(), c0.y(), c0.z() }, { c1.x(), c1.y(), c1.z() },
					{ c2.x(), c2.y(), c2.z() }, { c3.x(), c3.y(), c3.z() } };
				const float uvs[4][2] = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };
				const int order[6] = { 0, 1, 2, 1, 3, 2 };
				float verts[6 * 10];
				for (int i = 0; i < 6; ++i)
				{
					const int k = order[i];
					verts[i * 10 + 0] = corners[k][0];
					verts[i * 10 + 1] = corners[k][1];
					verts[i * 10 + 2] = corners[k][2];
					verts[i * 10 + 3] = normal.x();
					verts[i * 10 + 4] = normal.y();
					verts[i * 10 + 5] = normal.z();
					verts[i * 10 + 6] = uvs[k][0];
					verts[i * 10 + 7] = uvs[k][1];
					verts[i * 10 + 8] = sprite.lit ? sprite.lightUv[0] : lightWhiteU_;
					verts[i * 10 + 9] = sprite.lit ? sprite.lightUv[1] : lightWhiteV_;
				}
				spriteVbo_.write(0, verts, sizeof(verts));
				// Lit sprites (grass, bushes...) are shaded by the map's
				// lightmap like the world; glows stay fullbright.
				program_->setUniformValue(unlitLoc_, sprite.lit ? 0.0f : 1.0f);
				program_->setUniformValue(studioLoc_, 0.0f);
				const bool additive = !wireframe_ &&
					sprite.renderClass == BspRenderClass::Additive;
				const bool blend = !wireframe_ &&
					sprite.renderClass == BspRenderClass::Blend;
				if (additive || blend)
				{
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
					glDepthMask(GL_FALSE);
				}
				program_->setUniformValue(chromeLoc_, 0.0f);
				program_->setUniformValue(additiveLoc_, additive ? 1.0f : 0.0f);
				program_->setUniformValue(opacityLoc_, sprite.opacity);
				program_->setUniformValue(tintLoc_, QVector3D(1.0f, 1.0f, 1.0f));
				if (sprite.texture >= 0 &&
					sprite.texture < static_cast<int>(glTextures_.size()))
					glTextures_[static_cast<size_t>(sprite.texture)]->bind(0);
				glDrawArrays(GL_TRIANGLES, 0, 6);
				if (additive || blend)
				{
					glDepthMask(GL_TRUE);
					glDisable(GL_BLEND);
					program_->setUniformValue(additiveLoc_, 0.0f);
				}
			}
			spriteVao_.release();
			spriteVbo_.release();
			program_->setUniformValue(unlitLoc_, 0.0f);
		}

		program_->release();
	}

	void BspViewWidget::resizeGL(int w, int h)
	{
		glViewport(0, 0, w, h);
	}

	void BspViewWidget::mousePressEvent(QMouseEvent *event)
	{
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

	void BspViewWidget::mouseMoveEvent(QMouseEvent *event)
	{
		const QPoint delta = event->pos() - lastMouse_;
		lastMouse_ = event->pos();

		if (dragging_)
		{
			if (cameraMode_ == CameraMode::Fly)
			{
				// Mouse look. Same feel as orbit, same axis convention.
				flyYaw_ -= delta.x() * 0.3f;
				flyPitch_ -= delta.y() * 0.3f;
				flyPitch_ = std::clamp(flyPitch_, -89.0f, 89.0f);
			}
			else
			{
				yaw_ -= delta.x() * 0.5f;
				pitch_ += delta.y() * 0.5f;
				pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
			}
			update();
		}
		else if (panning_ && cameraMode_ == CameraMode::Orbit)
		{
			const float pitchRad = qDegreesToRadians(pitch_);
			const float yawRad = qDegreesToRadians(yaw_);
			const float cp = std::cos(pitchRad);
			QVector3D dirToCamera(cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad));
			dirToCamera.normalize();
			const QVector3D forward = -dirToCamera;
			const QVector3D worldUp(0.0f, 0.0f, 1.0f);
			QVector3D right = QVector3D::crossProduct(forward, worldUp);
			if (right.lengthSquared() < 1e-8f)
				right = QVector3D(0.0f, 1.0f, 0.0f);
			right.normalize();
			const QVector3D camUp = QVector3D::crossProduct(right, forward).normalized();

			const float scale = distance_ * 0.0016f;
			const QVector3D offset = -right * (delta.x() * scale) + camUp * (delta.y() * scale);
			centerX_ += offset.x();
			centerY_ += offset.y();
			centerZ_ += offset.z();
			update();
		}
	}

	void BspViewWidget::mouseReleaseEvent(QMouseEvent *event)
	{
		if (event->button() == Qt::LeftButton)
			dragging_ = false;
		else if (event->button() == Qt::RightButton)
			panning_ = false;
	}

	void BspViewWidget::wheelEvent(QWheelEvent *event)
	{
		const int delta = event->angleDelta().y();
		if (cameraMode_ == CameraMode::Fly)
		{
			// Dolly along the view direction.
			const float step = radius_ * 0.05f * (delta > 0 ? 1.0f : -1.0f);
			flyPos_ += BspFlyForward(flyYaw_, flyPitch_) * step;
			update();
			return;
		}
		distance_ *= (delta > 0) ? 0.9f : 1.1f;
		const float minDistance = std::max(radius_ * 0.02f, 0.5f);
		distance_ = std::clamp(distance_, minDistance, radius_ * 20.0f);
		update();
	}

	void BspViewWidget::keyPressEvent(QKeyEvent *event)
	{
		if (cameraMode_ == CameraMode::Fly && !event->isAutoRepeat())
			flyKeys_.insert(BspNormalizeFlyKey(event->key(),
				static_cast<unsigned>(event->nativeScanCode())));
		QOpenGLWidget::keyPressEvent(event);
	}

	void BspViewWidget::keyReleaseEvent(QKeyEvent *event)
	{
		if (!event->isAutoRepeat())
			flyKeys_.remove(BspNormalizeFlyKey(event->key(),
				static_cast<unsigned>(event->nativeScanCode())));
		QOpenGLWidget::keyReleaseEvent(event);
	}
}
