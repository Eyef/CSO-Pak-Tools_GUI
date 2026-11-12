#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "NarArchive.h"

namespace cso_pak
{
	// How an entry's data is stored inside the archive (directory field).
	enum class NarStoreType : uint32_t
	{
		Raw = 0,                // plain bytes
		Encoded = 1,            // XOR with the 16-byte path-derived key
		EncodedCompressed = 2,  // XOR( LZ(plain) )
	};

	// Compress `size` bytes with the .nar LZ variant (8 KiB window), the
	// inverse of the reader's DecompressNarLz. Deterministic; greedy matching
	// with bounded hash chains.
	std::vector<uint8_t> CompressNarLz(const uint8_t *data, size_t size);

	// Writer for Nexon .nar archives:
	//   [8-byte header] [entry blobs...] [XOR+bzip2 directory] [footer]
	// Entries are appended one by one (memory use is one entry at a time);
	// Finish() writes the directory and atomically moves the temp file onto
	// the target. If the writer is destroyed without Finish(), the temp file
	// is removed and the target is never touched. Throws std::runtime_error.
	//
	// `archivePath` is the exact byte string stored in the directory and used
	// as the key seed: CSO paths look like "/cstrike/models/v_ak47.mdl"
	// (leading '/', forward slashes, EUC-KR for non-ASCII).
	class NarWriter
	{
	public:
		explicit NarWriter(const std::filesystem::path &outputPath);
		~NarWriter();
		NarWriter(const NarWriter &) = delete;
		NarWriter &operator=(const NarWriter &) = delete;

		// Encode `data` per `type` and append it. `directoryOrder` controls
		// where the entry lands in the directory table: entries are
		// stable-sorted by it at Finish(), and the default (UINT64_MAX) sorts
		// after every explicit value, keeping insertion order among
		// themselves -- so "no explicit orders at all" means insertion order.
		// Data blocks always stay in insertion order.
		void AddFile(const std::string &archivePath, const uint8_t *data, size_t size,
			NarStoreType type, uint32_t timestamp, uint64_t directoryOrder = UINT64_MAX);
		void AddFile(const std::string &archivePath, const std::vector<uint8_t> &data,
			NarStoreType type, uint32_t timestamp, uint64_t directoryOrder = UINT64_MAX)
		{
			AddFile(archivePath, data.data(), data.size(), type, timestamp, directoryOrder);
		}

		// Copy an entry's already-encoded bytes (from NarArchive::ReadStored)
		// without re-encoding. Only valid if `archivePath` equals the source
		// entry's path (the key is path-derived); checksum, sizes, type and
		// timestamp are taken from `source`.
		void AddStored(const std::string &archivePath, const NarArchive::Entry &source,
			const std::vector<uint8_t> &storedBytes, uint64_t directoryOrder = UINT64_MAX);

		size_t Count() const { return records_.size(); }

		// Write directory + footer and move the result into place.
		void Finish();

	private:
		struct Record
		{
			std::string path;
			uint32_t type;
			uint32_t offset;
			uint32_t storedSize;
			uint32_t extractedSize;
			uint32_t timestamp;
			uint32_t checksum;
			uint64_t order;
			size_t seq;
		};

		void WriteBytes(const uint8_t *data, size_t size);
		void Append(const std::string &path, uint32_t type, const uint8_t *stored, size_t storedSize,
			size_t extractedSize, uint32_t timestamp, uint32_t checksum, uint64_t order);

		std::filesystem::path target_;
		std::filesystem::path temp_;
		std::ofstream out_;
		uint64_t position_ = 0;
		std::vector<Record> records_;
		bool finished_ = false;
	};
}
