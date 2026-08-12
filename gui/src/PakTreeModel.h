#pragma once

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QIcon>

#include "PakArchive.h"

// Presents a loaded cso_pak::PakArchive as a folder tree, the way Explorer
// shows a directory. Entry paths inside the pak use '/' as separator; each
// path component becomes a folder node except the last, which is a file
// node referencing the entry it came from.
class PakTreeModel : public QAbstractItemModel
{
	Q_OBJECT

public:
	explicit PakTreeModel(QObject *parent = nullptr);

	// Rebuilds the tree from the given archive. The archive must outlive
	// the model (MainWindow keeps it alive for as long as the model is used).
	void SetArchive(const cso_pak::PakArchive *archive);

	// Returns the entry index (into archive->Entries()) for a file node, or
	// -1 if the index refers to a folder or is invalid.
	int EntryIndexForIndex(const QModelIndex &index) const;

	bool IsFolder(const QModelIndex &index) const;

	// Returns the entry index of `index` itself if it's a file, or the entry
	// indices of every file nested under it if it's a folder (including the
	// whole tree when `index` is invalid, i.e. the root).
	std::vector<int> CollectEntryIndices(const QModelIndex &index) const;

	QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex &child) const override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
	struct Node
	{
		QString name;
		Node *parent = nullptr;
		std::vector<std::unique_ptr<Node>> children;
		int entryIndex = -1; // -1 for folders

		bool IsFolder() const { return entryIndex < 0; }
	};

	Node *NodeFromIndex(const QModelIndex &index) const;
	Node *FindOrCreateFolder(Node *parent, const QString &name);
	void SortChildrenRecursive(Node *node);
	static void CollectEntryIndicesRecursive(const Node *node, std::vector<int> &out);

	const cso_pak::PakArchive *archive_ = nullptr;
	std::unique_ptr<Node> root_;
	QIcon folderIcon_;
	QIcon fileIcon_;
};
