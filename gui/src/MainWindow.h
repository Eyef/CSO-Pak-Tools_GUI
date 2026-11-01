#pragma once
#include <memory>

#include <map>

#include <set>

#include <vector>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QTreeView>
#include <QVideoWidget>

#include "CsoDecoder.h"
#include "DdsImage.h"
#include "BspMap.h"
#include "BspViewWidget.h"
#include "ModelViewWidget.h"
#include "PakArchive.h"
#include "NarArchive.h"
#include "PakTreeModel.h"
#include "SpriteImage.h"
#include "Wad3Archive.h"
#include "StudioModel.h"

class QMenu;
class QDockWidget;
class QLineEdit;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *event) override;

private slots:
	void OnOpenPak();
	void OnOpenPaks();
	void OnOpenPaksWithNars();
	void OnOpenPakFolder();
	void OnShowAutoNarToggled(bool show);
	void OnExtractAll();
	void OnExtractSelected();
	void OnExtractSelectedDecoded();
	void OnPackDirectory();
	void OnPatchArchive();
	void OnCurrentChanged(const QModelIndex &current, const QModelIndex &previous);
	void OnTreeDoubleClicked(const QModelIndex &index);
	void OnTreeContextMenu(const QPoint &pos);
	void OnFitToWindowToggled(bool checked);
	void OnPlayPauseClicked();
	void OnMediaSliderMoved(int valueMs);
	void OnMediaPositionChanged(qint64 positionMs);
	void OnMediaDurationChanged(qint64 durationMs);
	void OnMediaPlaybackStateChanged(QMediaPlayer::PlaybackState state);
	void OnModelSkinChanged(int index);
	void OnModelSequenceChanged(int index);
	void OnModelFrameSliderMoved(int frame);
	void OnModelPlayToggled(bool play);
	void OnModelWireframeToggled(bool enabled);
	void OnBspWireframeToggled(bool enabled);
	void OnModelTextureChanged(int index);
	void ExportModelTexture();
	void OnModelForceAdditiveToggled(bool checked);
	void LoadTexturesFromFolder();
	void OnLightAnglesChanged();
	void OnResetLightClicked();
	void OnModelAnimationTick();
	void OnSpriteFrameSliderMoved(int frame);
	void OnSpritePlayToggled(bool play);
	void OnSpriteAnimationTick();
	void OnSpriteFitToWindowToggled(bool checked);
	void OnSearchResultChanged(int currentRow, int currentColumn, int previousRow, int previousColumn);
	void OnSearchResultDoubleClicked(int row, int column);

private:
	void BuildUi();
	void BuildMenusAndToolbar();
	void LoadPak(const QString &path);
	// autoNar: also pull sibling/parent .nar archives in (CSO keeps models
	// there). Auto-loaded .nars resolve textures/models but stay hidden
	// from the tree unless shown (see showAutoNar_).
	void LoadPaks(const QStringList &paths, bool autoNar);
	// Rebuilds the tree from loaded archives, honouring showAutoNar_.
	void RebuildTreeViews();
	bool HasArchives() const { return !archives_.empty() || !narArchives_.empty(); }
	size_t TotalEntryCount() const;
	void AddRecentFile(const QString &path);
	void UpdateRecentFilesMenu();

	// One concrete file inside a loaded archive of either kind (.pak or
	// .nar). .nar files hold most CSO models, so every BSP-placed-file
	// lookup (models, sprites, wad textures) goes through this.
	struct FoundEntry
	{
		const cso_pak::PakArchive *pak = nullptr;
		const cso_pak::PakArchive::Entry *pakEntry = nullptr;
		const cso_pak::NarArchive *nar = nullptr;
		const cso_pak::NarArchive::Entry *narEntry = nullptr;

		bool valid() const { return pakEntry != nullptr || narEntry != nullptr; }
		// Reads + decodes the entry (throws on failure).
		std::vector<uint8_t> extract() const;
		// Lowercase file extension without the dot (for image decoders).
		QString extension() const;
		// File name (last path component) for display and v_ detection.
		QString fileName() const;
		// Full internal path for display.
		QString displayPath() const;
		// "archive-file:entry-path" for messages and cache keys.
		QString sourceName() const;
		// Just the archive file name (no entry path).
		QString archiveName() const;
	};

	// One row of the file-name search: the concrete entry plus the indices
	// needed to reveal the same file in the tree.
	struct SearchHit
	{
		FoundEntry entry;
		int pakIndex = -1;
		int narIndex = -1;
		int entryIndex = -1;
		QString name;
		QString path;
		QString archiveName;
	};

	// Same node resolved as a .pak-or-.nar entry (for tree nodes of either
	// kind); invalid when the node isn't a plain file.
	FoundEntry ResolveFoundEntry(const QModelIndex &index) const;

	// File-name search across every loaded archive, including auto-loaded
	// .nars that are hidden from the tree. Results go to the search dock;
	// selecting a row previews the file, double-clicking reveals it in the
	// tree when that archive is shown there.
	void FocusSearch();
	void RunSearch();
	void ShowSearchHit(int row);
	void RevealSearchHit(int row);
	void RevealEntry(int pakIndex, int narIndex, int entryIndex);

	void ShowPreviewForFoundEntry(const FoundEntry &ref);
	void ShowPlaceholder(const QString &message);
	void ShowText(const QString &text);
	void ShowImage(const QImage &image);
	void ShowCsv(const std::vector<uint8_t> &data);
	void ShowCso(const FoundEntry &ref, std::vector<uint8_t> data);
	void ShowMedia(const std::vector<uint8_t> &data, const QString &displayName, const QString &fileExtension);
	void StopMediaPlayback();
	void ShowModel(const FoundEntry &ref, const std::vector<uint8_t> &data);
	void ShowBsp(const FoundEntry &ref, const std::vector<uint8_t> &data);
	// Resolves one .wad file to its textures (lowercase name -> image),
	// searching loaded archives, texture folders, then neighbouring .pak
	// files. Results (including misses) are cached for the session.
	std::map<QString, QImage> ResolveWadFile(const QString &wadFileName);
	void StopModelPlayback();
	void RebuildModelControls();

	// Resolves any texture a model only *references* by name (its embedded
	// copy is a tiny placeholder, or CSO's own "#"-prefixed external-texture
	// naming convention) against other entries in the currently open pak,
	// and swaps in the decoded image via StudioModel::SetTextureImage.
	static bool TextureLooksExternal(const cso_gui::StudioModel::Texture &tex);
	static QImage DecodeImageData(const std::vector<uint8_t> &data, const QString &extension);
	static QImage DecodeImageFile(const QString &path);
	static bool FileMatchesTextureName(const QString &path, const std::string &textureName);
	// Same filename search as above, but across .pak AND .nar archives
	// (session neighbours included). For BSP-placed content, whose files
	// mostly live in .nar.
	FoundEntry FindTextureFile(const std::string &textureName) const;
	// Loads the texture-twin of a textureless world model (w_50ae_big.mdl
	// pairs with w_50ae_bigt.mdl, which holds the texture table the
	// geometry file lacks). Searches loaded archives, then the remembered
	// and bundled loose folders. Returns null when there is no twin.
	// modelPath is the model's own internal path (any separators/case).
	std::shared_ptr<cso_gui::StudioModel> LoadTwinTextureModel(
		const QString &modelPath) const;
	// Finds an archive entry by its internal path (slashes normalized,
	// case-insensitive), searching loaded .pak/.nar archives then session
	// neighbours. Used for map-placed .mdl/.spr files named by BSP point
	// entities.
	FoundEntry FindArchiveEntryByPath(const QString &internalPath) const;
	FoundEntry FindArchiveEntryByPathUncached(const QString &internalPath) const;
	// Lookup results (misses included) keyed by lowercase path; must be
	// cleared whenever any archive list changes.
	mutable std::map<QString, FoundEntry> findCache_;
	void ResolveExternalTextures(const std::shared_ptr<cso_gui::StudioModel> &model);
	// Applies textures from the remembered folder (if any) to textures that
	// are still unresolved. Called automatically for every opened model so
	// the folder doesn't have to be re-picked per model.
	void ApplyRememberedTextureFolder(const std::shared_ptr<cso_gui::StudioModel> &model);
	// Binds unresolved external textures of `model` from a pre-scanned file
	// listing. Returns how many textures were loaded.
	int ResolveTexturesFromFolderFiles(const std::shared_ptr<cso_gui::StudioModel> &model,
		const QStringList &files);
	void ShowSprite(const FoundEntry &ref, const std::vector<uint8_t> &data);
	void StopSpritePlayback();
	void UpdateSpriteFrameDisplay();
	void ShowWadLumpPreview(int wadArchiveIndex, int wadEntryIndex);
	void ShowWadLumpProperties(const cso_gui::WadEntry &lump, const QString &note);
	void ShowGenericProperties(const QList<QPair<QString, QString>> &rows);
	void ShowProperties(const cso_pak::PakArchive::Entry &entry, const QString &note = QString());
	void ShowNarProperties(const cso_pak::NarArchive::Entry &entry, const QString &note = QString());
	void ShowEntryProperties(const FoundEntry &ref, const QString &note = QString());
	void UpdateImageDisplay();

	void ExtractOneWithDialog(const FoundEntry &ref);
	void ExtractIndicesToDirectory(const std::vector<PakTreeModel::ResolvedRef> &refs,
		const QString &destRoot, bool decodeCso = false);
	std::vector<PakTreeModel::ResolvedRef> CollectSelectedRefs() const;

	static QString ExtensionOf(const cso_pak::PakArchive::Entry &entry);
	static QString ExtensionOf(const cso_pak::NarArchive::Entry &entry);
	static bool LooksLikeText(const std::vector<uint8_t> &data);
	static QString DecodeText(const std::vector<uint8_t> &data);
	static QString DecodeModelText(const std::string &text);
	static QString FormatSize(uint64_t bytes);
	static QString FormatTypeFlags(uint32_t type);
	static QString FormatDuration(qint64 milliseconds);
	// Returns a cleaned, root-relative path with no ".." components, or an
	// empty string if the entry path can't be made safe.
	static QString SanitizedRelativePath(const std::u16string &entryPath);
	static QString SanitizedRelativePath(const std::string &entryPath);

	std::vector<std::unique_ptr<cso_pak::PakArchive>> archives_;
	// Archive index (into archives_) the currently previewed model came
	// from. Texture lookup prefers this archive first, then the rest.
	int currentModelPakIndex_ = -1;
	// Same for .nar archives (see currentModelPakIndex_).
	int currentModelNarIndex_ = -1;
	// Texture folder picked via "Load textures from folder". Remembered
	// (including across restarts via QSettings) and auto-applied to every
	// subsequently opened model with unresolved textures.
	QString textureFolder_;
	PakTreeModel *model_ = nullptr;

	QTreeView *treeView_ = nullptr;
	QStackedWidget *stack_ = nullptr;

	// File-name search: toolbar edit + flat results dock. Hits store the
	// concrete archive entry (and its tree indices) so a result can be
	// previewed without depending on the tree's hierarchy.
	QLineEdit *searchEdit_ = nullptr;
	QDockWidget *searchDock_ = nullptr;
	QTableWidget *searchResults_ = nullptr;
	std::vector<SearchHit> searchHits_;

	QLabel *placeholderPage_ = nullptr;

	QPlainTextEdit *textPage_ = nullptr;

	QWidget *imagePageContainer_ = nullptr;
	QScrollArea *imageScrollArea_ = nullptr;
	QLabel *imageLabel_ = nullptr;
	QCheckBox *fitToWindowCheck_ = nullptr;
	QImage currentImage_;

	QTableWidget *csvPage_ = nullptr;
	QTableWidget *propertiesPage_ = nullptr;

	QWidget *mediaPageContainer_ = nullptr;
	QVideoWidget *videoWidget_ = nullptr;
	QMediaPlayer *mediaPlayer_ = nullptr;
	QAudioOutput *audioOutput_ = nullptr;
	QPushButton *playPauseButton_ = nullptr;
	QSlider *mediaPositionSlider_ = nullptr;
	QLabel *mediaTimeLabel_ = nullptr;
	QLabel *mediaFileLabel_ = nullptr;
	std::unique_ptr<QTemporaryFile> tempMediaFile_;
	bool mediaSliderBeingDragged_ = false;

	QAction *extractAllAction_ = nullptr;
	QMenu *recentFilesMenu_ = nullptr;
	static constexpr int kMaxRecentFiles = 10;
	QAction *extractSelectedAction_ = nullptr;
	QAction *extractSelectedDecodedAction_ = nullptr;

	QWidget *modelPageContainer_ = nullptr;
	cso_gui::ModelViewWidget *modelView_ = nullptr;
	QWidget *modelControlsContainer_ = nullptr;  // rebuilt on every model load
	QWidget *modelCameraBox_ = nullptr;
	QComboBox *modelCameraCombo_ = nullptr;
	QDoubleSpinBox *modelFovSpin_ = nullptr;
	std::shared_ptr<cso_gui::StudioModel> currentModel_;
	// Parallel to currentModel_->Textures(): archive path a texture was
	// resolved from (empty = still embedded/unresolved). Rebuilt fresh by
	// ResolveExternalTextures every time a new .mdl loads.
	std::vector<QString> externalTextureSources_;
	QComboBox *modelSkinCombo_ = nullptr;
	QComboBox *modelSequenceCombo_ = nullptr;
	QComboBox *modelTextureCombo_ = nullptr;
	QLabel *modelTexturePreview_ = nullptr;
	QLabel *modelTextureInfoLabel_ = nullptr;
	QCheckBox *modelForceAdditiveCheck_ = nullptr;
	QSlider *modelFrameSlider_ = nullptr;
	QLabel *modelFrameLabel_ = nullptr;
	QPushButton *modelPlayButton_ = nullptr;
	QCheckBox *modelWireframeCheck_ = nullptr;
		QTimer *modelAnimTimer_ = nullptr;
		QElapsedTimer modelAnimationClock_;
		float modelAnimationStartFrame_ = 0.0f;
	QSlider *lightYawSlider_ = nullptr;
	QSlider *lightPitchSlider_ = nullptr;

	QWidget *spritePageContainer_ = nullptr;
	QLabel *spriteImageLabel_ = nullptr;
	QScrollArea *spriteScrollArea_ = nullptr;
	QCheckBox *spriteFitToWindowCheck_ = nullptr;
	QSlider *spriteFrameSlider_ = nullptr;
	QLabel *spriteFrameLabel_ = nullptr;
	QPushButton *spritePlayButton_ = nullptr;
	QTimer *spriteAnimTimer_ = nullptr;
	std::shared_ptr<cso_gui::SpriteImage> currentSprite_;

	QWidget *bspPageContainer_ = nullptr;
	cso_gui::BspViewWidget *bspView_ = nullptr;
	QLabel *bspInfoLabel_ = nullptr;
	QCheckBox *bspWireframeCheck_ = nullptr;
	std::shared_ptr<cso_gui::BspMap> currentBsp_;
	// Neighbouring .pak files auto-opened while resolving BSP .wad textures
	// in single-pak mode, plus parsed .wad images. Session caches, cleared
	// whenever a new archive set is loaded.
	std::vector<std::unique_ptr<cso_pak::PakArchive>> neighborArchives_;
	std::map<QString, std::map<QString, QImage>> wadImageCache_;
	// Loaded .nar archives (Nexon archives: most CSO models live here, see
	// NarArchive) plus session neighbours. Searched by every BSP-placed
	// file lookup alongside the .pak lists above; the tree view still
	// shows .pak contents only.
	std::vector<std::unique_ptr<cso_pak::NarArchive>> narArchives_;
	std::vector<std::unique_ptr<cso_pak::NarArchive>> neighborNarArchives_;
	// Parsed .mdl files placed on the current BSP map (keyed by lowercase
	// internal path), with external textures already resolved. Session
	// cache, cleared together with the .wad caches.
	std::map<QString, std::shared_ptr<cso_gui::StudioModel>> placedModelCache_;
	// Whether auto-loaded (not explicitly chosen) .nar archives are shown
	// in the tree. Off by default so opening paks doesn't flood the tree
	// with nar contents; resolution always uses every loaded archive either
	// way. Persisted across restarts.
	bool showAutoNar_ = false;
	// Canonical file paths of the auto-loaded (hidden unless shown) .nars.
	std::set<QString> autoNarPaths_;
	QAction *showAutoNarAction_ = nullptr;
};
