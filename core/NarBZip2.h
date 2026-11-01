#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cso_pak
{
	// Minimal bzip2 decompressor, just enough for Nexon .nar directory
	// blobs (single stream, any block size 100k-900k, no 'randomised'
	// legacy blocks). Written from the public bzip2 stream format
	// description (see NARTools' NARToolLib for how .nar uses it, and the
	// bzip2 manual for the stage order: RLE1 -> BWT -> MTF+RUNA/RUNB ->
	// Huffman -> [stream]; decode in reverse). Throws std::runtime_error
	// on malformed input, including block CRC mismatches.
	std::vector<uint8_t> DecompressBZip2(const uint8_t *data, size_t size);
}
