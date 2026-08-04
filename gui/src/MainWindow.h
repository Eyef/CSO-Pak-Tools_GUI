#pragma once

#include <memory>
#include <vector>

#include <QCheckBox>
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
#include <QTreeView>
#include <QVideoWidget>

#include "CsoDecoder.h"
#include "PakArchive.h"
#include "PakTreeModel.h"

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

private:
	void BuildUi();
	void BuildMenusAndToolbar();
	void LoadPak(const QString &path);

	void ShowPreviewForEntry(const cso_pak::PakArchive::Entry &entry);
	void ShowPlaceholder(const QString &message);
	void ShowText(const QString &text);
	void ShowImage(const QImage &image);
	void ShowCsv(const std::vector<uint8_t> &data);
	void ShowCso(const cso_pak::PakArchive::Entry &entry, std::vector<uint8_t> data);
	void ShowMedia(const std::vector<uint8_t> &data, const QString &displayName, const QString &fileExtension);
	void StopMediaPlayback();
	void ShowProperties(const cso_pak::PakArchive::Entry &entry, const QString &note = QString());
	void UpdateImageDisplay();

	void ExtractOneWithDialog(const cso_pak::PakArchive::Entry &entry);
	void ExtractIndicesToDirectory(const std::vector<int> &entryIndices, const QString &destRoot, bool decodeCso = false);
	std::vector<int> CollectSelectedEntryIndices() const;

	static QString ExtensionOf(const cso_pak::PakArchive::Entry &entry);
	static bool LooksLikeText(const std::vector<uint8_t> &data);
	static QString DecodeText(const std::vector<uint8_t> &data);
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
	QAction *extractSelectedAction_ = nullptr;
	QAction *extractSelectedDecodedAction_ = nullptr;
};
