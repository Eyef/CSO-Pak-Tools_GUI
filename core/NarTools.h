#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "NarArchive.h"
#include "NarWriter.h"

// High-level .nar operations built on NarArchive (read), NarWriter (write)
// and ModelCrypt (ICE models): unpack a whole archive to a folder, pack a
// folder into an archive, and patch an existing archive with replacement
// files. Everything is a plain function over std::filesystem -- no Qt.
//
// Unpacking writes `.nar-info.json` next to the files (same schema as
// CSNZ-Nar's manifest). It records each entry's original path, storage type,
// whether the model was ICE-encrypted (20/21) and its directory/data
// position, so packing the folder again restores all of that with no
// options. Entries the manifest doesn't know about get the defaults.
namespace cso_pak
{
	// (done, total, current archive path). Called from the calling thread.
	using NarProgressFn = std::function<void(size_t done, size_t total, const std::string &path)>;

	// ---- path helpers --------------------------------------------------

	// "/cstrike/models/a.mdl" (archive bytes, EUC-KR on Windows) -> relative
	// local path "cstrike/models/a.mdl". Returns an empty path if the entry
	// path is unsafe (".." components, drive letters / ':', control chars,
	// Windows-reserved characters) or empty.
	std::filesystem::path ArchivePathToLocal(const std::string &archivePath);

	// Relative local path -> "/a/b.mdl" archive bytes. Empty string if the
	// name can't be represented (Windows: not convertible to code page 949).
	std::string LocalToArchivePath(const std::filesystem::path &relative);

	// ---- unpack --------------------------------------------------------

	struct NarExtractOptions
	{
		bool decryptModels = true;  // ICE-decrypt .mdl v20/21 while writing
		bool writeManifest = true;  // write .nar-info.json
		bool verifyCrc = false;     // report entries whose stored CRC mismatches
		// If set, only entries for which this returns true are extracted
		// (no manifest is written for a partial extraction).
		std::function<bool(const NarArchive::Entry &)> filter;
	};

	struct NarExtractStats
	{
		size_t total = 0;
		size_t written = 0;
		size_t failed = 0;
		size_t modelsDecrypted = 0;
		std::vector<std::string> errors;  // one line per failed entry
	};

	// One bad entry never stops the rest; failures are listed in `errors`.
	NarExtractStats ExtractNarToDirectory(const NarArchive &archive,
		const std::filesystem::path &outputDir, const NarExtractOptions &options = {},
		const NarProgressFn &progress = {});

	// ---- pack ----------------------------------------------------------

	struct NarPackOptions
	{
		// Store type for files the manifest doesn't know about.
		NarStoreType defaultType = NarStoreType::Encoded;
		// Archive root directory name, without slashes ("cstrike"). Used
		// when no manifest is found: archive path = "/" + root + "/" + rel.
		// Empty + hasRoot => no extra root (rel is used as is). If !hasRoot
		// the input folder's own name is the root (NARTools behaviour).
		bool hasRoot = false;
		std::string root;
		// 20/21: ICE-encrypt plain .mdl files the manifest has no info for.
		// 0 = leave models as they are.
		int encryptModels = 0;
		bool useManifest = true;
	};

	struct NarPackStats
	{
		size_t files = 0;
		size_t modelsEncrypted = 0;
		bool usedManifest = false;
		std::vector<std::string> warnings;
	};

	// Throws std::runtime_error on fatal problems (unreadable input, write
	// failure). The output is only created once everything succeeded.
	NarPackStats PackDirectoryToNar(const std::filesystem::path &inputDir,
		const std::filesystem::path &outputNar, const NarPackOptions &options = {},
		const NarProgressFn &progress = {});

	// ---- patch ---------------------------------------------------------

	struct NarPatchOptions
	{
		NarStoreType newFileType = NarStoreType::Encoded;
		// Files with no counterpart in the archive are skipped unless
		// addNew is set; they are then stored under "/<root>/<relative>".
		bool addNew = false;
		std::string root;
		int encryptNewModels = 0;  // 20/21 for brand-new plain .mdl, 0 = no
	};

	struct NarPatchStats
	{
		size_t replaced = 0;
		size_t added = 0;
		size_t preserved = 0;
		std::vector<std::string> skipped;  // replacement files that matched nothing / ambiguous
	};

	// Copies `source` into `outputNar`, replacing entries with the same path
	// from `replacementDir` (matched by full archive path, or by a unique
	// path suffix when the folder is deeper than the archive root). Entries
	// that aren't replaced are copied byte-for-byte without re-compression.
	// A replaced .mdl that was ICE-encrypted stays encrypted (same key
	// version) when the replacement file is plain.
	NarPatchStats PatchNar(const NarArchive &source, const std::filesystem::path &replacementDir,
		const std::filesystem::path &outputNar, const NarPatchOptions &options = {},
		const NarProgressFn &progress = {});

	// Name of the manifest file written by unpacking.
	constexpr const char *kNarManifestName = ".nar-info.json";
}
