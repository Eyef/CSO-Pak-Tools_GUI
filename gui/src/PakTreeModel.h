#pragma once

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QIcon>

#include "PakArchive.h"
#include "NarArchive.h"
#include "Wad3Archive.h"

// Presents a loaded cso_pak::PakArchive as a folder tree, the way Explorer
// shows a directory. Entry paths inside the pak use '/' as separator; each
// path component becomes a folder node except the last, which is a file
// node referencing the entry it came from.
//
// .wad entries (WAD2/WAD3 archives, an "archive inside the archive") get
// special treatment: SetArchive tries parsing each one right away, and on
// success gives it child nodes for its own lumps -- so a .wad shows up
// expandable like a folder, with a distinct icon, and its lumps can be
// clicked and previewed directly without a separate extract-then-reopen
// step. A .wad that fails to parse just falls back to being a normal,
// non-expandable file node.
class PakTreeModel : public QAbstractItemModel
{
	Q_OBJECT

public:
	explicit PakTreeModel(QObject *parent = nullptr);

	// Rebuilds the tree from the given archives. The archives must outlive
	// the model (MainWindow keeps them alive for as long as the model is used).
	// Folders with the same virtual path are merged; files with the same
	// virtual path from different archives are all shown as siblings, so no
	// duplicate is hidden -- use the Source column/tooltip to tell them apart.
	void SetArchives(const std::vector<const cso_pak::PakArchive*> &archives);
	void SetArchive(const cso_pak::PakArchive *archive);
	// Same, plus Nexon .nar archives (shown in the same merged tree; a .nar
	// node carries narIndex instead of pakIndex below).
	void SetAllArchives(const std::vector<const cso_pak::PakArchive*> &archives,
		const std::vector<const cso_pak::NarArchive*> &narArchives);
	void SetNarArchives(const std::vector<const cso_pak::NarArchive*> &narArchives);

	// A reference to one concrete file in one concrete archive.
	struct ResolvedRef
	{
		int pakIndex = -1;    // Index into the pak archives passed to SetArchives().
		int narIndex = -1;    // Index into the nar archives (mutually exclusive with pakIndex).
		int entryIndex = -1;  // Index into that archive's Entries().
		bool isNar = false;
	};

	// Returns the entry index (into its own archive's Entries()) for a plain
	// file node (this includes a .wad's own node), or -1 for a folder or
	// a WAD-lump node.
	int EntryIndexForIndex(const QModelIndex &index) const;
	// Index of the archive this node came from, or -1 for folders/invalid.
	// .nar nodes report -1 here (see NarIndexForIndex).
	int PakIndexForIndex(const QModelIndex &index) const;
	// Archive this node came from, or nullptr for folders/invalid/.nar.
	const cso_pak::PakArchive *ArchiveForIndex(const QModelIndex &index) const;
	// .nar counterparts of the above: -1/nullptr unless IsNarFile().
	int NarIndexForIndex(const QModelIndex &index) const;
	const cso_pak::NarArchive *NarArchiveForIndex(const QModelIndex &index) const;
	bool IsNarFile(const QModelIndex &index) const;

	bool IsFolder(const QModelIndex &index) const;

	// True for a node representing one lump inside a parsed .wad (i.e. a
	// child of a .wad node, not the .wad node itself).
	bool IsWadLump(const QModelIndex &index) const;
	// Valid together for a WAD-lump node (see IsWadLump); -1 otherwise.
	// wadArchiveIndex indexes WadArchiveAt(); wadEntryIndex indexes that
	// archive's own Entries().
	int WadArchiveIndexForIndex(const QModelIndex &index) const;
	int WadEntryIndexForIndex(const QModelIndex &index) const;
	const cso_gui::Wad3Archive *WadArchiveAt(int wadArchiveIndex) const;

	// Finds the tree node of a concrete archive entry (pakIndex >= 0 for a
	// .pak entry, narIndex >= 0 for a .nar one), or an invalid index when
	// that entry is not currently in the tree -- e.g. a file from an
	// auto-loaded .nar that View > "Show auto-loaded NAR archives" hides.
	QModelIndex FindEntry(int pakIndex, int narIndex, int entryIndex) const;
	// Index of `archive` among the .nar archives actually shown in the
	// tree, or -1 when it is loaded but hidden (auto-loaded NARs). Search
	// results index the full MainWindow list, so they must be translated
	// before they can be revealed.
	int NarIndexForArchive(const cso_pak::NarArchive *archive) const;

	// Returns the resolved (pak, entry) refs of `index` itself if it's a
	// plain file, or of every plain file nested under it if it's a folder
	// (including the whole tree when `index` is invalid, i.e. the root).
	// WAD-lump nodes are skipped -- they aren't real top-level pak entries,
	// so there's nothing here to extract them as (see README).
	std::vector<ResolvedRef> CollectResolvedRefs(const QModelIndex &index) const;

	QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex &child) const override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
	enum class NodeKind
	{
		Folder,  // Plain grouping node, no entry of its own.
		PakFile, // A real top-level pak entry (includes a parsed .wad's own node).
		WadLump, // One entry inside a parsed .wad (a child of a PakFile-kind .wad node).
	};

	struct Node
	{
		QString name;
		Node *parent = nullptr;
		std::vector<std::unique_ptr<Node>> children;
		NodeKind kind = NodeKind::Folder;
		int pakIndex = -1;        // Valid when kind == PakFile or WadLump (pak-backed).
		int narIndex = -1;        // Valid when kind == PakFile or WadLump (nar-backed).
		int entryIndex = -1;      // Valid when kind == PakFile.
		int wadArchiveIndex = -1; // Valid when kind == WadLump, or on a parsed .wad's own PakFile node.
		int wadEntryIndex = -1;   // Valid when kind == WadLump.

		bool IsFolder() const { return kind == NodeKind::Folder; }
		bool IsNar() const { return narIndex >= 0; }
	};

	Node *NodeFromIndex(const QModelIndex &index) const;
	QModelIndex FindEntryRecursive(const Node *node, int pakIndex, int narIndex,
		int entryIndex) const;
	Node *FindOrCreateFolder(Node *parent, const QString &name);
	void SortChildrenRecursive(Node *node);
	static void CollectResolvedRefsRecursive(const Node *node, std::vector<ResolvedRef> &out);
	void AddWadChildrenIfApplicable(Node *fileNode, const cso_pak::PakArchive *archive, int entryIndex);
	void AddWadChildrenIfApplicable(Node *fileNode, const cso_pak::NarArchive *archive, int entryIndex);
	void Rebuild();
	QString SourceNameForPak(int pakIndex) const;
	QString SourceNameForNar(int narIndex) const;
	QString SourceNameForNode(const Node *node) const;

	std::vector<const cso_pak::PakArchive*> archives_;
	std::vector<const cso_pak::NarArchive*> narArchives_;
	std::unique_ptr<Node> root_;
	std::vector<std::unique_ptr<cso_gui::Wad3Archive>> wadArchives_;
	QIcon folderIcon_;
	QIcon fileIcon_;
	QIcon wadIcon_;
};
