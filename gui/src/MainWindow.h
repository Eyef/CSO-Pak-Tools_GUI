#pragma once

#include <memory>
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
#include "ModelViewWidget.h"
#include "PakArchive.h"
#include "PakTreeModel.h"
#include "SpriteImage.h"
#include "Wad3Archive.h"
#include "StudioModel.h"

class QMenu;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *event) override;

private slots:
	void OnOpenPak();
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
	void OnModelTextureChanged(int index);
	void OnModelForceAdditiveToggled(bool checked);
	void LoadTexturesFromFolder();
	void OnLightAnglesChanged();
	void OnResetLightClicked();
	void OnModelAnimationTick();
	void OnSpriteFrameSliderMoved(int frame);
	void OnSpritePlayToggled(bool play);
	void OnSpriteAnimationTick();
	void OnSpriteFitToWindowToggled(bool checked);

private:
	void BuildUi();
	void BuildMenusAndToolbar();
	void LoadPak(const QString &path);
	void AddRecentFile(const QString &path);
	void UpdateRecentFilesMenu();

	void ShowPreviewForEntry(const cso_pak::PakArchive::Entry &entry);
	void ShowPlaceholder(const QString &message);
	void ShowText(const QString &text);
	void ShowImage(const QImage &image);
	void ShowCsv(const std::vector<uint8_t> &data);
	void ShowCso(const cso_pak::PakArchive::Entry &entry, std::vector<uint8_t> data);
	void ShowMedia(const std::vector<uint8_t> &data, const QString &displayName, const QString &fileExtension);
	void StopMediaPlayback();
	void ShowModel(const cso_pak::PakArchive::Entry &entry, const std::vector<uint8_t> &data);
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
	const cso_pak::PakArchive::Entry *FindArchiveEntryForTextureName(const std::string &textureName) const;
	void ResolveExternalTextures(const std::shared_ptr<cso_gui::StudioModel> &model);
	void ShowSprite(const cso_pak::PakArchive::Entry &entry, const std::vector<uint8_t> &data);
	void StopSpritePlayback();
	void UpdateSpriteFrameDisplay();
	void ShowWadLumpPreview(int wadArchiveIndex, int wadEntryIndex);
	void ShowWadLumpProperties(const cso_gui::WadEntry &lump, const QString &note);
	void ShowGenericProperties(const QList<QPair<QString, QString>> &rows);
	void ShowProperties(const cso_pak::PakArchive::Entry &entry, const QString &note = QString());
	void UpdateImageDisplay();

	void ExtractOneWithDialog(const cso_pak::PakArchive::Entry &entry);
	void ExtractIndicesToDirectory(const std::vector<int> &entryIndices, const QString &destRoot, bool decodeCso = false);
	std::vector<int> CollectSelectedEntryIndices() const;

	static QString ExtensionOf(const cso_pak::PakArchive::Entry &entry);
	static bool LooksLikeText(const std::vector<uint8_t> &data);
	static QString DecodeText(const std::vector<uint8_t> &data);
	static QString DecodeModelText(const std::string &text);
	static QString FormatSize(uint64_t bytes);
	static QString FormatTypeFlags(uint32_t type);
	static QString FormatDuration(qint64 milliseconds);
	// Returns a cleaned, root-relative path with no ".." components, or an
	// empty string if the entry path can't be made safe.
	static QString SanitizedRelativePath(const std::u16string &entryPath);

	std::unique_ptr<cso_pak::PakArchive> archive_;
	PakTreeModel *model_ = nullptr;

	QTreeView *treeView_ = nullptr;
	QStackedWidget *stack_ = nullptr;

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
};
