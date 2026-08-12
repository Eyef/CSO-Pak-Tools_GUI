#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QImage>

namespace cso_gui
{
	// Decoded sprite (GoldSource/CSO ".spr"). Flattens every frame (single
	// frames and the members of frame groups) into one list, in file order.
	// Only the two versions seen in the wild are supported: v2 (palette
	// indexed) and v3 (one mini-DDS per frame, DXT5/DXT1/A8).
	struct SpriteImage
	{
		struct Frame
		{
			QImage image;
			float interval = 0.1f;   // Seconds this frame is shown (group interval, else default).
			int originX = 0;
			int originY = 0;
			int group = -1;          // Index of the SPR_GROUP this frame belongs to, or -1.
		};

		int version = 0;
		int type = 0;                // VP_PARALLEL / ORIENTED / ... (orientation in the game world).
		int texFormat = 0;           // SPR_NORMAL / ADDITIVE / INDEXALPHA / ALPHTEST shine.
		int syncType = 0;
		float boundingRadius = 0.0f;
		float beamLength = 0.0f;
		std::vector<Frame> frames;

		bool IsValid() const { return version != 0 && !frames.empty(); }
	};

	// Parse a sprite from its raw bytes (as extracted from a .pak entry).
	// Throws std::runtime_error on a malformed or unsupported file.
	std::shared_ptr<SpriteImage> LoadSpriteImage(const std::vector<uint8_t> &data);
}
