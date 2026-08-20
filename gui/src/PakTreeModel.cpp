#include "PakTreeModel.h"

#include <algorithm>
#include <iterator>

#include <QApplication>
#include <QFileInfo>
#include <QStyle>

PakTreeModel::PakTreeModel(QObject *parent)
	: QAbstractItemModel(parent)
	, root_(std::make_unique<Node>())
{
	folderIcon_ = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
	fileIcon_ = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
	// Visually distinct from a plain folder, for a .wad that parsed
	// successfully and is expandable into its own lumps.
	wadIcon_ = QApplication::style()->standardIcon(QStyle::SP_DriveCDIcon);
}

void PakTreeModel::SetArchive(const cso_pak::PakArchive *archive)
{
	beginResetModel();

	archive_ = archive;
	root_ = std::make_unique<Node>();
	wadArchives_.clear();

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
			fileNode->kind = NodeKind::PakFile;
			fileNode->entryIndex = i;
			Node *fileNodeRaw = fileNode.get();
			current->children.push_back(std::move(fileNode));

			AddWadChildrenIfApplicable(fileNodeRaw, i);
		}

		SortChildrenRecursive(root_.get());
	}

	endResetModel();
}

void PakTreeModel::AddWadChildrenIfApplicable(Node *fileNode, int entryIndex)
{
	if (archive_ == nullptr)
		return;

	const QString extension = QFileInfo(fileNode->name).suffix().toLower();
	if (extension != QLatin1String("wad"))
		return;

	// A .wad that fails to parse (corrupt, or just not actually a WAD
	// despite the extension) simply stays a normal, non-expandable file
	// node -- same "fall back gracefully" approach used for every other
	// format in this app.
	try
	{
		const auto &entry = archive_->Entries()[static_cast<size_t>(entryIndex)];
		auto data = archive_->ExtractEntry(entry);
		auto wad = std::make_unique<cso_gui::Wad3Archive>(cso_gui::Wad3Archive::Load(std::move(data)));

		const int wadArchiveIndex = static_cast<int>(wadArchives_.size());
		fileNode->wadArchiveIndex = wadArchiveIndex;

		for (int i = 0; i < static_cast<int>(wad->Entries().size()); ++i)
		{
			const auto &lump = wad->Entries()[static_cast<size_t>(i)];

			auto lumpNode = std::make_unique<Node>();
			lumpNode->name = lump.name.empty()
				? QStringLiteral("(unnamed lump %1)").arg(i)
				: QString::fromStdString(lump.name);
			lumpNode->parent = fileNode;
			lumpNode->kind = NodeKind::WadLump;
			lumpNode->wadArchiveIndex = wadArchiveIndex;
			lumpNode->wadEntryIndex = i;
			fileNode->children.push_back(std::move(lumpNode));
		}

		wadArchives_.push_back(std::move(wad));
	}
	catch (const std::exception &)
	{
		fileNode->wadArchiveIndex = -1;
		fileNode->children.clear();
	}
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
	// Folders/files alphabetical, as before -- but WAD-lump children of a
	// .wad node are left in their original on-disk order rather than
	// resorted, since that's usually more meaningful for a lump directory
	// than alphabetical, and they were never mixed with folders/files to
	// begin with (a .wad node's children are ALL WadLump).
	if (node->children.empty() || node->children.front()->kind != NodeKind::WadLump)
	{
		std::ranges::sort(node->children, [](const auto &left, const auto &right)
		{
			if (left->IsFolder() != right->IsFolder())
				return left->IsFolder(); // Folders before files, like Explorer.

			return QString::compare(left->name, right->name, Qt::CaseInsensitive) < 0;
		});
	}

	for (auto &child : node->children)
	{
		if (child->kind != NodeKind::WadLump)
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
	if (node == nullptr || node == root_.get() || node->kind != NodeKind::PakFile)
		return -1;

	return node->entryIndex;
}

bool PakTreeModel::IsFolder(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	return node != nullptr && node->IsFolder();
}

bool PakTreeModel::IsWadLump(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	return node != nullptr && node->kind == NodeKind::WadLump;
}

int PakTreeModel::WadArchiveIndexForIndex(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	if (node == nullptr || node->kind != NodeKind::WadLump)
		return -1;

	return node->wadArchiveIndex;
}

int PakTreeModel::WadEntryIndexForIndex(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	if (node == nullptr || node->kind != NodeKind::WadLump)
		return -1;

	return node->wadEntryIndex;
}

const cso_gui::Wad3Archive *PakTreeModel::WadArchiveAt(int wadArchiveIndex) const
{
	if (wadArchiveIndex < 0 || wadArchiveIndex >= static_cast<int>(wadArchives_.size()))
		return nullptr;

	return wadArchives_[static_cast<size_t>(wadArchiveIndex)].get();
}

void PakTreeModel::CollectEntryIndicesRecursive(const Node *node, std::vector<int> &out)
{
	if (node->kind == NodeKind::WadLump)
		return; // Not a real top-level pak entry; nothing to extract it as.

	if (node->kind == NodeKind::PakFile)
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
	{
		if (node->IsFolder())
			return folderIcon_;
		if (node->kind == NodeKind::PakFile && node->wadArchiveIndex >= 0)
			return wadIcon_; // Successfully parsed .wad: expandable, distinct icon.
		return fileIcon_;
	}

	return QVariant();
}

QVariant PakTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0)
		return tr("Name");

	return QVariant();
}
