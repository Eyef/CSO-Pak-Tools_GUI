#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace cso_gui
{
	// Bounds-checked little-endian byte cursor over an in-memory buffer.
	// Shared by SpriteImage (.spr) and DdsImage (.dds) parsing, since both
	// formats are simple little-endian binary layouts read sequentially.
	class ByteCursor
	{
	public:
		explicit ByteCursor(const std::vector<uint8_t> &data)
			: data_(data)
		{
		}

		uint8_t U8()
		{
			return Bytes(1)[0];
		}

		int16_t I16()
		{
			const uint8_t *p = Bytes(2);
			return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
		}

		int32_t I32()
		{
			return static_cast<int32_t>(ReadU32(Bytes(4)));
		}

		uint32_t U32()
		{
			return ReadU32(Bytes(4));
		}

		float F32()
		{
			const uint32_t v = ReadU32(Bytes(4));
			float f;
			std::memcpy(&f, &v, sizeof(f));
			return f;
		}

		// Grab `count` raw bytes (advancing past them) for pixel/palette blobs.
		const uint8_t *Bytes(size_t count)
		{
			if (count > data_.size() || offset_ > data_.size() - count)
				throw std::runtime_error("unexpected end of file");
			const uint8_t *p = data_.data() + offset_;
			offset_ += count;
			return p;
		}

		// Jumps to an absolute offset, e.g. to read a directory/table that
		// sits somewhere other than right at the start of the file.
		void Seek(size_t offset)
		{
			if (offset > data_.size())
				throw std::runtime_error("seek past end of file");
			offset_ = offset;
		}

		size_t Offset() const { return offset_; }
		size_t Size() const { return data_.size(); }

	private:
		static uint32_t ReadU32(const uint8_t *p)
		{
			return static_cast<uint32_t>(p[0]) |
				(static_cast<uint32_t>(p[1]) << 8) |
				(static_cast<uint32_t>(p[2]) << 16) |
				(static_cast<uint32_t>(p[3]) << 24);
		}

		const std::vector<uint8_t> &data_;
		size_t offset_ = 0;
	};
}
