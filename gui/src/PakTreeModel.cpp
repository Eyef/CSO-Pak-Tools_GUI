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
	if (archive != nullptr)
		SetArchives({ archive });
	else
		SetArchives({});
}

void PakTreeModel::SetArchives(const std::vector<const cso_pak::PakArchive*> &archives)
{
	SetAllArchives(archives, {});
}

void PakTreeModel::SetNarArchives(const std::vector<const cso_pak::NarArchive*> &narArchives)
{
	SetAllArchives({}, narArchives);
}

void PakTreeModel::SetAllArchives(const std::vector<const cso_pak::PakArchive*> &archives,
	const std::vector<const cso_pak::NarArchive*> &narArchives)
{
	beginResetModel();

	archives_ = archives;
	narArchives_ = narArchives;
	Rebuild();

	endResetModel();
}

void PakTreeModel::Rebuild()
{
	root_ = std::make_unique<Node>();
	wadArchives_.clear();

	const auto addFileNode = [&](const QString &path, int pakIndex, int narIndex, int entryIndex)
	{
		const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (parts.isEmpty())
			return;

		Node *current = root_.get();
		for (int p = 0; p < parts.size() - 1; ++p)
			current = FindOrCreateFolder(current, parts[p]);

		auto fileNode = std::make_unique<Node>();
		fileNode->name = parts.last();
		fileNode->parent = current;
		fileNode->kind = NodeKind::PakFile;
		fileNode->pakIndex = pakIndex;
		fileNode->narIndex = narIndex;
		fileNode->entryIndex = entryIndex;
		Node *fileNodeRaw = fileNode.get();
		current->children.push_back(std::move(fileNode));

		if (narIndex >= 0)
		{
			const auto *archive = (narIndex >= 0 &&
				narIndex < static_cast<int>(narArchives_.size()))
				? narArchives_[static_cast<size_t>(narIndex)]
				: nullptr;
			AddWadChildrenIfApplicable(fileNodeRaw, archive, entryIndex);
		}
		else
		{
			const auto *archive = (pakIndex >= 0 &&
				pakIndex < static_cast<int>(archives_.size()))
				? archives_[static_cast<size_t>(pakIndex)]
				: nullptr;
			AddWadChildrenIfApplicable(fileNodeRaw, archive, entryIndex);
		}
	};

	for (int pakIndex = 0; pakIndex < static_cast<int>(archives_.size()); ++pakIndex)
	{
		const auto *archive = archives_[static_cast<size_t>(pakIndex)];
		if (archive == nullptr)
			continue;

		const auto &entries = archive->Entries();
		for (int i = 0; i < static_cast<int>(entries.size()); ++i)
			addFileNode(QString::fromStdU16String(entries[static_cast<size_t>(i)].path), pakIndex, -1, i);
	}

	for (int narIndex = 0; narIndex < static_cast<int>(narArchives_.size()); ++narIndex)
	{
		const auto *archive = narArchives_[static_cast<size_t>(narIndex)];
		if (archive == nullptr)
			continue;

		const auto &entries = archive->Entries();
		for (int i = 0; i < static_cast<int>(entries.size()); ++i)
			addFileNode(QString::fromStdString(entries[static_cast<size_t>(i)].path), -1, narIndex, i);
	}

	SortChildrenRecursive(root_.get());
}

void PakTreeModel::AddWadChildrenIfApplicable(Node *fileNode, const cso_pak::PakArchive *archive, int entryIndex)
{
	if (archive == nullptr)
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
		const auto &entry = archive->Entries()[static_cast<size_t>(entryIndex)];
		auto data = archive->ExtractEntry(entry);
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
			lumpNode->pakIndex = fileNode->pakIndex;
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

void PakTreeModel::AddWadChildrenIfApplicable(Node *fileNode, const cso_pak::NarArchive *archive, int entryIndex)
{
	if (archive == nullptr)
		return;

	const QString extension = QFileInfo(fileNode->name).suffix().toLower();
	if (extension != QLatin1String("wad"))
		return;

	// Same graceful fallback as for .pak entries: an unparseable .wad stays
	// a normal, non-expandable file node.
	try
	{
		const auto &entry = archive->Entries()[static_cast<size_t>(entryIndex)];
		auto data = archive->ExtractEntry(entry);
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
			lumpNode->pakIndex = fileNode->pakIndex;
			lumpNode->narIndex = fileNode->narIndex;
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
		std::ranges::sort(node->children, [this](const auto &left, const auto &right)
		{
			if (left->IsFolder() != right->IsFolder())
				return left->IsFolder(); // Folders before files, like Explorer.

			const int nameOrder = QString::compare(left->name, right->name, Qt::CaseInsensitive);
			if (nameOrder != 0)
				return nameOrder < 0;

			// Same virtual name from several archives: keep a stable order
			// by source archive so duplicates don't shuffle between reloads
			// (.pak sources sort before .nar ones).
			const int leftSource = left->IsNar() ? 1000000 + left->narIndex : left->pakIndex;
			const int rightSource = right->IsNar() ? 1000000 + right->narIndex : right->pakIndex;
			return leftSource < rightSource;
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

int PakTreeModel::PakIndexForIndex(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	if (node == nullptr || node == root_.get() || node->IsFolder())
		return -1;

	return node->pakIndex;
}

bool PakTreeModel::IsNarFile(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	return node != nullptr && node != root_.get() && !node->IsFolder() && node->IsNar();
}

int PakTreeModel::NarIndexForIndex(const QModelIndex &index) const
{
	const Node *node = NodeFromIndex(index);
	if (node == nullptr || node == root_.get() || node->IsFolder())
		return -1;

	return node->narIndex;
}

const cso_pak::NarArchive *PakTreeModel::NarArchiveForIndex(const QModelIndex &index) const
{
	const int narIndex = NarIndexForIndex(index);
	if (narIndex < 0 || narIndex >= static_cast<int>(narArchives_.size()))
		return nullptr;

	return narArchives_[static_cast<size_t>(narIndex)];
}

const cso_pak::PakArchive *PakTreeModel::ArchiveForIndex(const QModelIndex &index) const
{
	const int pakIndex = PakIndexForIndex(index);
	if (pakIndex < 0 || pakIndex >= static_cast<int>(archives_.size()))
		return nullptr;

	return archives_[static_cast<size_t>(pakIndex)];
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

QModelIndex PakTreeModel::FindEntryRecursive(const Node *node, int pakIndex,
	int narIndex, int entryIndex) const
{
	for (int row = 0; row < static_cast<int>(node->children.size()); ++row)
	{
		const Node *child = node->children[static_cast<size_t>(row)].get();
		if (child->kind == NodeKind::PakFile &&
			child->entryIndex == entryIndex &&
			child->pakIndex == pakIndex &&
			child->narIndex == narIndex)
		{
			return createIndex(row, 0, const_cast<Node *>(child));
		}

		const QModelIndex found = FindEntryRecursive(child, pakIndex, narIndex, entryIndex);
		if (found.isValid())
			return found;
	}

	return QModelIndex();
}

QModelIndex PakTreeModel::FindEntry(int pakIndex, int narIndex, int entryIndex) const
{
	// Exactly one of the two archive indices is valid; -1 entry index is
	// never a real file (folders and WAD lumps report -1 too).
	if ((pakIndex < 0) == (narIndex < 0) || entryIndex < 0)
		return QModelIndex();

	return FindEntryRecursive(root_.get(), pakIndex, narIndex, entryIndex);
}

int PakTreeModel::NarIndexForArchive(const cso_pak::NarArchive *archive) const
{
	if (archive == nullptr)
		return -1;

	for (int i = 0; i < static_cast<int>(narArchives_.size()); ++i)
	{
		if (narArchives_[static_cast<size_t>(i)] == archive)
			return i;
	}

	return -1;
}

void PakTreeModel::CollectResolvedRefsRecursive(const Node *node, std::vector<ResolvedRef> &out)
{
	if (node->kind == NodeKind::WadLump)
		return; // Not a real top-level pak entry; nothing to extract it as.

	if (node->kind == NodeKind::PakFile)
	{
		ResolvedRef ref;
		ref.pakIndex = node->pakIndex;
		ref.narIndex = node->narIndex;
		ref.entryIndex = node->entryIndex;
		ref.isNar = node->IsNar();
		out.push_back(ref);
		return;
	}

	for (const auto &child : node->children)
		CollectResolvedRefsRecursive(child.get(), out);
}

std::vector<PakTreeModel::ResolvedRef> PakTreeModel::CollectResolvedRefs(const QModelIndex &index) const
{
	std::vector<ResolvedRef> result;
	const Node *node = NodeFromIndex(index);
	if (node != nullptr)
		CollectResolvedRefsRecursive(node, result);

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
	return 1; // Name only; the source pak is shown via tooltip and status bar.
}

QString PakTreeModel::SourceNameForPak(int pakIndex) const
{
	if (pakIndex < 0 || pakIndex >= static_cast<int>(archives_.size()))
		return QString();

	const auto *archive = archives_[static_cast<size_t>(pakIndex)];
	if (archive == nullptr)
		return QString();

	return QString::fromStdU16String(archive->SourceFileName());
}

QString PakTreeModel::SourceNameForNar(int narIndex) const
{
	if (narIndex < 0 || narIndex >= static_cast<int>(narArchives_.size()))
		return QString();

	const auto *archive = narArchives_[static_cast<size_t>(narIndex)];
	if (archive == nullptr)
		return QString();

	return QString::fromStdString(archive->SourcePath().filename().string());
}

QString PakTreeModel::SourceNameForNode(const Node *node) const
{
	if (node == nullptr)
		return QString();
	if (node->IsNar())
		return SourceNameForNar(node->narIndex);
	return SourceNameForPak(node->pakIndex);
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

	if (role == Qt::ToolTipRole && !node->IsFolder())
	{
		const QString source = SourceNameForNode(node);
		if (source.isEmpty())
			return node->name;
		return tr("%1\nSource: %2").arg(node->name, source);
	}

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
