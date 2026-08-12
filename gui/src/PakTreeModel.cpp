#include "PakTreeModel.h"

#include <algorithm>
#include <iterator>

#include <QApplication>
#include <QStyle>

PakTreeModel::PakTreeModel(QObject *parent)
	: QAbstractItemModel(parent)
	, root_(std::make_unique<Node>())
{
	folderIcon_ = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
	fileIcon_ = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

void PakTreeModel::SetArchive(const cso_pak::PakArchive *archive)
{
	beginResetModel();

	archive_ = archive;
	root_ = std::make_unique<Node>();

	if (archive_ != nullptr)
	{
		const auto &entries = archive_->Entries();
		for (int i = 0; i < static_cast<int>(entries.size()); ++i)
		{
			const auto &entry = entries[static_cast<size_t>(i)];
			const QString path = QString::fromStdU16String(entry.path);
			const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
			if (parts.isEmpty())
				continue;

			Node *current = root_.get();
			for (int p = 0; p < parts.size() - 1; ++p)
				current = FindOrCreateFolder(current, parts[p]);

			auto fileNode = std::make_unique<Node>();
			fileNode->name = parts.last();
			fileNode->parent = current;
			fileNode->entryIndex = i;
			current->children.push_back(std::move(fileNode));
		}

		SortChildrenRecursive(root_.get());
	}

	endResetModel();
}

PakTreeModel::Node *PakTreeModel::FindOrCreateFolder(Node *parent, const QString &name)
{
	for (auto &child : parent->children)
	{
		if (child->IsFolder() && child->name == name)
			return child.get();
	}

	auto folder = std::make_unique<Node>();
	folder->name = name;
	folder->parent = parent;
	Node *raw = folder.get();
	parent->children.push_back(std::move(folder));
	return raw;
}

void PakTreeModel::SortChildrenRecursive(Node *node)
{
	std::ranges::sort(node->children, [](const auto &left, const auto &right)
	{
		if (left->IsFolder() != right->IsFolder())
			return left->IsFolder(); // Folders before files, like Explorer.

		return QString::compare(left->name, right->name, Qt::CaseInsensitive) < 0;
	});

	for (auto &child : node->children)
	{
		if (child->IsFolder())
			SortChildrenRecursive(child.get());
	}
}

PakTreeModel::Node *PakTreeModel::NodeFromIndex(const QModelIndex &index) const
{
	if (!index.isValid())
		return root_.get();

	return static_cast<Node *>(index.internalPointer());
}

int PakTreeModel::EntryIndexForIndex(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	if (node == nullptr || node == root_.get())
		return -1;

	return node->entryIndex;
}

bool PakTreeModel::IsFolder(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	return node != nullptr && node->IsFolder();
}

void PakTreeModel::CollectEntryIndicesRecursive(const Node *node, std::vector<int> &out)
{
	if (!node->IsFolder())
	{
		out.push_back(node->entryIndex);
		return;
	}

	for (const auto &child : node->children)
		CollectEntryIndicesRecursive(child.get(), out);
}

std::vector<int> PakTreeModel::CollectEntryIndices(const QModelIndex &index) const
{
	std::vector<int> result;
	const Node *node = NodeFromIndex(index);
	if (node != nullptr)
		CollectEntryIndicesRecursive(node, result);

	return result;
}

QModelIndex PakTreeModel::index(int row, int column, const QModelIndex &parent) const
{
	if (row < 0 || column != 0)
		return QModelIndex();

	const Node *parentNode = NodeFromIndex(parent);
	if (parentNode == nullptr || row >= static_cast<int>(parentNode->children.size()))
		return QModelIndex();

	return createIndex(row, column, parentNode->children[static_cast<size_t>(row)].get());
}

QModelIndex PakTreeModel::parent(const QModelIndex &child) const
{
	if (!child.isValid())
		return QModelIndex();

	Node *node = NodeFromIndex(child);
	Node *parentNode = node->parent;
	if (parentNode == nullptr || parentNode == root_.get())
		return QModelIndex();

	Node *grandparent = parentNode->parent;
	const auto &siblings = grandparent->children;
	const auto it = std::find_if(siblings.begin(), siblings.end(),
		[parentNode](const auto &candidate) { return candidate.get() == parentNode; });
	const int row = static_cast<int>(std::distance(siblings.begin(), it));

	return createIndex(row, 0, parentNode);
}

int PakTreeModel::rowCount(const QModelIndex &parent) const
{
	if (parent.column() > 0)
		return 0;

	const Node *parentNode = NodeFromIndex(parent);
	return parentNode == nullptr ? 0 : static_cast<int>(parentNode->children.size());
}

int PakTreeModel::columnCount(const QModelIndex & /*parent*/) const
{
	return 1;
}

QVariant PakTreeModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid())
		return QVariant();

	const Node *node = NodeFromIndex(index);
	if (node == nullptr)
		return QVariant();

	if (role == Qt::DisplayRole)
		return node->name;

	if (role == Qt::DecorationRole)
		return node->IsFolder() ? folderIcon_ : fileIcon_;

	return QVariant();
}

QVariant PakTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0)
		return tr("Name");

	return QVariant();
}
