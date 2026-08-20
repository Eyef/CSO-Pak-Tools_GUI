#pragma once

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QIcon>

#include "PakArchive.h"
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

	// Rebuilds the tree from the given archive. The archive must outlive
	// the model (MainWindow keeps it alive for as long as the model is used).
	void SetArchive(const cso_pak::PakArchive *archive);

	// Returns the entry index (into archive->Entries()) for a plain pak file
	// node (this includes a .wad's own node, which is both a normal
	// extractable pak entry AND expandable into its lumps), or -1 for a
	// folder or a WAD-lump node.
	int EntryIndexForIndex(const QModelIndex &index) const;

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

	// Returns the entry index of `index` itself if it's a plain file, or the
	// entry indices of every plain file nested under it if it's a folder
	// (including the whole tree when `index` is invalid, i.e. the root).
	// WAD-lump nodes are skipped -- they aren't real top-level pak entries,
	// so there's nothing here to extract them as (see README).
	std::vector<int> CollectEntryIndices(const QModelIndex &index) const;

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
		int entryIndex = -1;      // Valid when kind == PakFile.
		int wadArchiveIndex = -1; // Valid when kind == WadLump, or on a parsed .wad's own PakFile node.
		int wadEntryIndex = -1;   // Valid when kind == WadLump.

		bool IsFolder() const { return kind == NodeKind::Folder; }
	};

	Node *NodeFromIndex(const QModelIndex &index) const;
	Node *FindOrCreateFolder(Node *parent, const QString &name);
	void SortChildrenRecursive(Node *node);
	static void CollectEntryIndicesRecursive(const Node *node, std::vector<int> &out);
	void AddWadChildrenIfApplicable(Node *fileNode, int entryIndex);

	const cso_pak::PakArchive *archive_ = nullptr;
	std::unique_ptr<Node> root_;
	std::vector<std::unique_ptr<cso_gui::Wad3Archive>> wadArchives_;
	QIcon folderIcon_;
	QIcon fileIcon_;
	QIcon wadIcon_;
};
