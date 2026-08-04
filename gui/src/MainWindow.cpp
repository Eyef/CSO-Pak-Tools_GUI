#include "MainWindow.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include <QAction>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QResizeEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QStringDecoder>
#include <QStyle>
#include <QTableWidgetItem>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "TgaImage.h"

#ifdef _WIN32
#include "EucKrCodec.h"
#endif

namespace
{
	// Preview pages, in the order they are added to the QStackedWidget.
	enum PreviewPage
	{
		PagePlaceholder = 0,
		PageText,
		PageImage,
		PageCsv,
		PageProperties,
		PageMedia,
	};
}

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
{
	BuildUi();
	BuildMenusAndToolbar();

	setWindowTitle(tr("CS Online Pak Browser"));
	resize(1200, 800);
	statusBar()->showMessage(tr("Open a .pak archive to get started."));
}

void MainWindow::BuildUi()
{
	model_ = new PakTreeModel(this);

	treeView_ = new QTreeView;
	treeView_->setModel(model_);
	treeView_->setHeaderHidden(false);
	treeView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
	treeView_->setUniformRowHeights(true);
	connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged,
		this, &MainWindow::OnCurrentChanged);
	connect(treeView_, &QTreeView::doubleClicked, this, &MainWindow::OnTreeDoubleClicked);
	connect(treeView_, &QTreeView::customContextMenuRequested, this, &MainWindow::OnTreeContextMenu);

	stack_ = new QStackedWidget;

	placeholderPage_ = new QLabel(tr("Open a .pak archive, then select a file in the tree to preview it."));
	placeholderPage_->setAlignment(Qt::AlignCenter);
	placeholderPage_->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 13px;"));
	placeholderPage_->setWordWrap(true);

	textPage_ = new QPlainTextEdit;
	textPage_->setReadOnly(true);
	textPage_->setLineWrapMode(QPlainTextEdit::NoWrap);
	QFont monoFont(QStringLiteral("Consolas"));
	monoFont.setStyleHint(QFont::Monospace);
	monoFont.setPointSize(10);
	textPage_->setFont(monoFont);

	imageLabel_ = new QLabel;
	imageLabel_->setAlignment(Qt::AlignCenter);
	imageLabel_->setBackgroundRole(QPalette::Dark);
	imageLabel_->setAutoFillBackground(true);

	imageScrollArea_ = new QScrollArea;
	imageScrollArea_->setWidget(imageLabel_);
	imageScrollArea_->setWidgetResizable(false);
	imageScrollArea_->setAlignment(Qt::AlignCenter);

	fitToWindowCheck_ = new QCheckBox(tr("Fit to window"));
	fitToWindowCheck_->setChecked(false);
	connect(fitToWindowCheck_, &QCheckBox::toggled, this, &MainWindow::OnFitToWindowToggled);

	imagePageContainer_ = new QWidget;
	auto *imageLayout = new QVBoxLayout(imagePageContainer_);
	imageLayout->setContentsMargins(4, 4, 4, 4);
	imageLayout->addWidget(fitToWindowCheck_);
	imageLayout->addWidget(imageScrollArea_, 1);

	csvPage_ = new QTableWidget;
	csvPage_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	csvPage_->setAlternatingRowColors(true);

	propertiesPage_ = new QTableWidget(0, 2);
	propertiesPage_->setHorizontalHeaderLabels({ tr("Property"), tr("Value") });
	propertiesPage_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	propertiesPage_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	propertiesPage_->verticalHeader()->setVisible(false);
	propertiesPage_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	propertiesPage_->setSelectionMode(QAbstractItemView::NoSelection);
	propertiesPage_->setAlternatingRowColors(true);

	mediaPlayer_ = new QMediaPlayer(this);
	audioOutput_ = new QAudioOutput(this);
	mediaPlayer_->setAudioOutput(audioOutput_);
	connect(mediaPlayer_, &QMediaPlayer::positionChanged, this, &MainWindow::OnMediaPositionChanged);
	connect(mediaPlayer_, &QMediaPlayer::durationChanged, this, &MainWindow::OnMediaDurationChanged);
	connect(mediaPlayer_, &QMediaPlayer::playbackStateChanged, this, &MainWindow::OnMediaPlaybackStateChanged);

	videoWidget_ = new QVideoWidget;
	videoWidget_->setMinimumHeight(240);
	videoWidget_->setVisible(false); // Hidden until a file that actually has a video track is loaded.
	mediaPlayer_->setVideoOutput(videoWidget_);
	// Audio-only files (like .wav) never fire hasVideoChanged(true), so the
	// video area just stays collapsed and only the transport controls show.
	connect(mediaPlayer_, &QMediaPlayer::hasVideoChanged, videoWidget_, &QWidget::setVisible);

	mediaFileLabel_ = new QLabel;
	mediaFileLabel_->setAlignment(Qt::AlignCenter);
	mediaFileLabel_->setWordWrap(true);
	QFont fileNameFont = mediaFileLabel_->font();
	fileNameFont.setBold(true);
	fileNameFont.setPointSize(fileNameFont.pointSize() + 1);
	mediaFileLabel_->setFont(fileNameFont);

	playPauseButton_ = new QPushButton(tr("Play"));
	playPauseButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
	connect(playPauseButton_, &QPushButton::clicked, this, &MainWindow::OnPlayPauseClicked);

	mediaPositionSlider_ = new QSlider(Qt::Horizontal);
	mediaPositionSlider_->setRange(0, 0);
	connect(mediaPositionSlider_, &QSlider::sliderPressed, this, [this] { mediaSliderBeingDragged_ = true; });
	connect(mediaPositionSlider_, &QSlider::sliderReleased, this, [this]
	{
		mediaSliderBeingDragged_ = false;
		mediaPlayer_->setPosition(mediaPositionSlider_->value());
	});
	connect(mediaPositionSlider_, &QSlider::sliderMoved, this, &MainWindow::OnMediaSliderMoved);

	mediaTimeLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"));

	auto *mediaControlsLayout = new QHBoxLayout;
	mediaControlsLayout->addWidget(playPauseButton_);
	mediaControlsLayout->addWidget(mediaPositionSlider_, 1);
	mediaControlsLayout->addWidget(mediaTimeLabel_);

	mediaPageContainer_ = new QWidget;
	auto *mediaLayout = new QVBoxLayout(mediaPageContainer_);
	mediaLayout->setContentsMargins(24, 24, 24, 24);
	mediaLayout->addWidget(videoWidget_, 1);
	mediaLayout->addWidget(mediaFileLabel_);
	mediaLayout->addSpacing(12);
	mediaLayout->addLayout(mediaControlsLayout);

	stack_->addWidget(placeholderPage_);   // PagePlaceholder
	stack_->addWidget(textPage_);          // PageText
	stack_->addWidget(imagePageContainer_);// PageImage
	stack_->addWidget(csvPage_);           // PageCsv
	stack_->addWidget(propertiesPage_);    // PageProperties
	stack_->addWidget(mediaPageContainer_);// PageMedia

	auto *splitter = new QSplitter(Qt::Horizontal);
	splitter->addWidget(treeView_);
	splitter->addWidget(stack_);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({ 320, 880 });

	setCentralWidget(splitter);
}

void MainWindow::BuildMenusAndToolbar()
{
	auto *fileMenu = menuBar()->addMenu(tr("&File"));

	auto *openAction = fileMenu->addAction(tr("&Open Pak..."), this, &MainWindow::OnOpenPak);
	openAction->setShortcut(QKeySequence::Open);

	extractAllAction_ = fileMenu->addAction(tr("Extract &All..."), this, &MainWindow::OnExtractAll);
	extractAllAction_->setEnabled(false);

	extractSelectedAction_ = fileMenu->addAction(tr("Extract &Selected..."), this, &MainWindow::OnExtractSelected);
	extractSelectedAction_->setEnabled(false);

	extractSelectedDecodedAction_ = fileMenu->addAction(tr("Extract Selected (&Decode .cso to CSV)..."),
		this, &MainWindow::OnExtractSelectedDecoded);
	extractSelectedDecodedAction_->setEnabled(false);

	fileMenu->addSeparator();

	fileMenu->addAction(tr("&Pack Directory into New Pak..."), this, &MainWindow::OnPackDirectory);
	fileMenu->addAction(tr("Patch &Archive with Replacements..."), this, &MainWindow::OnPatchArchive);

	fileMenu->addSeparator();
	fileMenu->addAction(tr("E&xit"), this, &QWidget::close);

	auto *toolbar = addToolBar(tr("Main"));
	toolbar->setMovable(false);
	toolbar->addAction(openAction);
	toolbar->addAction(extractAllAction_);
	toolbar->addAction(extractSelectedAction_);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);
	if (stack_->currentWidget() == imagePageContainer_ && fitToWindowCheck_->isChecked())
		UpdateImageDisplay();
}

void MainWindow::OnOpenPak()
{
	const QString path = QFileDialog::getOpenFileName(this, tr("Open Pak Archive"),
		QString(), tr("Pak Archives (*.pak);;All Files (*)"));
	if (path.isEmpty())
		return;

	LoadPak(path);
}

void MainWindow::LoadPak(const QString &path)
{
	try
	{
		const std::filesystem::path fsPath(path.toStdU16String());
		auto archive = std::make_unique<cso_pak::PakArchive>(cso_pak::PakArchive::Load(fsPath));
		archive_ = std::move(archive);
		model_->SetArchive(archive_.get());
		treeView_->expandToDepth(0);

		extractAllAction_->setEnabled(true);
		extractSelectedAction_->setEnabled(true);
		extractSelectedDecodedAction_->setEnabled(true);

		ShowPlaceholder(tr("Select a file in the tree to preview it."));
		setWindowTitle(tr("%1 \u2014 CS Online Pak Browser").arg(QFileInfo(path).fileName()));
		statusBar()->showMessage(tr("Loaded %1 (%2 entries)")
			.arg(QFileInfo(path).fileName())
			.arg(archive_->Entries().size()));
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Failed to open archive"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnExtractAll()
{
	if (!archive_)
		return;

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	try
	{
		const auto stats = archive_->UnpackToDirectory(std::filesystem::path(dir.toStdU16String()));
		QMessageBox::information(this, tr("Extraction complete"),
			tr("Extracted %1 of %2 entries.").arg(stats.writtenEntries).arg(stats.totalEntries));
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Extraction failed"), QString::fromUtf8(ex.what()));
	}
}

std::vector<int> MainWindow::CollectSelectedEntryIndices() const
{
	std::set<int> uniqueIndices;
	const auto selected = treeView_->selectionModel()->selectedIndexes();
	for (const auto &index : selected)
	{
		if (index.column() != 0)
			continue;

		for (const int entryIndex : model_->CollectEntryIndices(index))
			uniqueIndices.insert(entryIndex);
	}

	return { uniqueIndices.begin(), uniqueIndices.end() };
}

void MainWindow::OnExtractSelected()
{
	if (!archive_)
		return;

	const auto indices = CollectSelectedEntryIndices();
	if (indices.empty())
	{
		QMessageBox::information(this, tr("Nothing selected"),
			tr("Select one or more files or folders in the tree first."));
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	ExtractIndicesToDirectory(indices, dir);
}

void MainWindow::OnExtractSelectedDecoded()
{
	if (!archive_)
		return;

	const auto indices = CollectSelectedEntryIndices();
	if (indices.empty())
	{
		QMessageBox::information(this, tr("Nothing selected"),
			tr("Select one or more files or folders in the tree first."));
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	ExtractIndicesToDirectory(indices, dir, /*decodeCso=*/true);
}

void MainWindow::ExtractIndicesToDirectory(const std::vector<int> &entryIndices, const QString &destRoot, bool decodeCso)
{
	int succeeded = 0;
	QStringList errors;

	for (const int index : entryIndices)
	{
		const auto &entry = archive_->Entries()[static_cast<size_t>(index)];
		QString relative = SanitizedRelativePath(entry.path);
		if (relative.isEmpty())
		{
			errors << tr("(skipped, unsafe path)");
			continue;
		}

		try
		{
			auto data = archive_->ExtractEntry(entry);

			const bool isCso = relative.endsWith(QLatin1String(".cso"), Qt::CaseInsensitive);
			if (decodeCso && isCso)
			{
				// Same two-layer decode ShowCso uses for preview: TEA-decrypt,
				// then convert from EUC-KR to UTF-8 if that's what it is.
				cso_pak::CsoDecoder::Decrypt(data);
#ifdef _WIN32
				if (cso_gui::LooksLikeEucKr(data))
				{
					const std::string utf8 = cso_gui::DecodeEucKrToUtf8(data);
					data.assign(utf8.begin(), utf8.end());
				}
#endif
				relative.chop(4); // drop ".cso"
				relative += QStringLiteral(".csv");
			}

			const QString destPath = QDir(destRoot).filePath(relative);
			const QFileInfo info(destPath);
			QDir().mkpath(info.absolutePath());

			QFile file(destPath);
			if (!file.open(QIODevice::WriteOnly))
				throw std::runtime_error("failed to open output file");

			file.write(reinterpret_cast<const char *>(data.data()), static_cast<qint64>(data.size()));
			++succeeded;
		}
		catch (const std::exception &ex)
		{
			errors << tr("%1: %2").arg(relative, QString::fromUtf8(ex.what()));
		}
	}

	QString summary = tr("Extracted %1 of %2 file(s).").arg(succeeded).arg(entryIndices.size());
	if (!errors.isEmpty())
		summary += QStringLiteral("\n\n") + tr("Errors:") + QStringLiteral("\n") + errors.join(QStringLiteral("\n"));

	QMessageBox::information(this, tr("Extraction complete"), summary);
}

void MainWindow::OnPackDirectory()
{
	const QString inputDir = QFileDialog::getExistingDirectory(this,
		tr("Choose directory to pack"));
	if (inputDir.isEmpty())
		return;

	const QString outputPak = QFileDialog::getSaveFileName(this, tr("Save new Pak Archive"),
		QString(), tr("Pak Archives (*.pak)"));
	if (outputPak.isEmpty())
		return;

	try
	{
		const auto stats = cso_pak::PakArchive::PackDirectory(
			std::filesystem::path(inputDir.toStdU16String()),
			std::filesystem::path(outputPak.toStdU16String()));

		const QString summary = tr("Packed %1 of %2 file(s) into %3.")
			.arg(stats.packedEntries).arg(stats.totalEntries).arg(QFileInfo(outputPak).fileName());

		if (QMessageBox::question(this, tr("Pack complete"),
				summary + QStringLiteral("\n\n") + tr("Open the new archive now?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
		{
			LoadPak(outputPak);
		}
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Pack failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnPatchArchive()
{
	// Patching works from a chosen source .pak, independent of whatever is
	// currently open in the tree (mirrors `cso-pak-cli patch <src> <dir> <out>`).
	const QString sourcePak = QFileDialog::getOpenFileName(this, tr("Choose source Pak Archive"),
		QString(), tr("Pak Archives (*.pak);;All Files (*)"));
	if (sourcePak.isEmpty())
		return;

	const QString replacementDir = QFileDialog::getExistingDirectory(this,
		tr("Choose replacement files directory"));
	if (replacementDir.isEmpty())
		return;

	const QString outputPak = QFileDialog::getSaveFileName(this, tr("Save patched Pak Archive"),
		QString(), tr("Pak Archives (*.pak)"));
	if (outputPak.isEmpty())
		return;

	try
	{
		const auto sourceArchive = cso_pak::PakArchive::Load(
			std::filesystem::path(sourcePak.toStdU16String()));
		const auto stats = sourceArchive.PatchFromDirectory(
			std::filesystem::path(replacementDir.toStdU16String()),
			std::filesystem::path(outputPak.toStdU16String()));

		const QString summary = tr("Patched %1 entries: %2 replaced, %3 preserved unchanged.\nWritten to %4.")
			.arg(stats.totalEntries).arg(stats.replacedEntries).arg(stats.preservedEntries)
			.arg(QFileInfo(outputPak).fileName());

		if (QMessageBox::question(this, tr("Patch complete"),
				summary + QStringLiteral("\n\n") + tr("Open the new archive now?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
		{
			LoadPak(outputPak);
		}
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Patch failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnCurrentChanged(const QModelIndex &current, const QModelIndex & /*previous*/)
{
	if (!archive_ || !current.isValid())
	{
		ShowPlaceholder(tr("Select a file in the tree to preview it."));
		return;
	}

	if (model_->IsFolder(current))
	{
		ShowPlaceholder(tr("Folder selected \u2014 pick a file to preview it."));
		return;
	}

	const int entryIndex = model_->EntryIndexForIndex(current);
	if (entryIndex < 0)
	{
		ShowPlaceholder(tr("Select a file in the tree to preview it."));
		return;
	}

	ShowPreviewForEntry(archive_->Entries()[static_cast<size_t>(entryIndex)]);
}

void MainWindow::OnTreeDoubleClicked(const QModelIndex &index)
{
	if (!archive_ || model_->IsFolder(index))
		return;

	const int entryIndex = model_->EntryIndexForIndex(index);
	if (entryIndex < 0)
		return;

	ExtractOneWithDialog(archive_->Entries()[static_cast<size_t>(entryIndex)]);
}

void MainWindow::ExtractOneWithDialog(const cso_pak::PakArchive::Entry &entry)
{
	const QString suggestedName = QFileInfo(QString::fromStdU16String(entry.path)).fileName();
	const QString dest = QFileDialog::getSaveFileName(this, tr("Save extracted file"), suggestedName);
	if (dest.isEmpty())
		return;

	try
	{
		const auto data = archive_->ExtractEntry(entry);
		QFile file(dest);
		if (!file.open(QIODevice::WriteOnly))
			throw std::runtime_error("failed to open output file");

		file.write(reinterpret_cast<const char *>(data.data()), static_cast<qint64>(data.size()));
		statusBar()->showMessage(tr("Saved %1").arg(dest), 5000);
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Extraction failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnTreeContextMenu(const QPoint &pos)
{
	if (!archive_)
		return;

	QMenu menu(this);
	menu.addAction(tr("Extract Selected..."), this, &MainWindow::OnExtractSelected);
	menu.addAction(tr("Extract Selected (Decode .cso to CSV)..."), this, &MainWindow::OnExtractSelectedDecoded);
	menu.exec(treeView_->viewport()->mapToGlobal(pos));
}

void MainWindow::OnFitToWindowToggled(bool /*checked*/)
{
	UpdateImageDisplay();
}

void MainWindow::ShowPlaceholder(const QString &message)
{
	StopMediaPlayback();
	placeholderPage_->setText(message);
	stack_->setCurrentWidget(placeholderPage_);
}

void MainWindow::ShowText(const QString &text)
{
	textPage_->setPlainText(text);
	stack_->setCurrentWidget(textPage_);
}

void MainWindow::ShowImage(const QImage &image)
{
	currentImage_ = image;
	UpdateImageDisplay();
	stack_->setCurrentWidget(imagePageContainer_);
}

void MainWindow::UpdateImageDisplay()
{
	if (currentImage_.isNull())
		return;

	QPixmap pixmap = QPixmap::fromImage(currentImage_);
	if (fitToWindowCheck_->isChecked())
	{
		const QSize viewportSize = imageScrollArea_->viewport()->size();
		if (viewportSize.width() > 0 && viewportSize.height() > 0)
			pixmap = pixmap.scaled(viewportSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	imageLabel_->setPixmap(pixmap);
	imageLabel_->resize(pixmap.size());
}

void MainWindow::ShowCsv(const std::vector<uint8_t> &data)
{
	const QString text = DecodeText(data);
	QStringList lines = text.split(QLatin1Char('\n'));
	while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
		lines.removeLast();

	for (QString &line : lines)
	{
		if (line.endsWith(QLatin1Char('\r')))
			line.chop(1);
	}

	// Pick whichever delimiter looks more common in the first line; this is
	// a simple heuristic (no quoted-field handling) good enough for preview.
	QChar delimiter(QLatin1Char(','));
	if (!lines.isEmpty() && lines.first().count(QLatin1Char(';')) > lines.first().count(QLatin1Char(',')))
		delimiter = QLatin1Char(';');

	constexpr qsizetype MaxRows = 5000;
	const int rowCount = static_cast<int>(std::min(lines.size(), MaxRows));

	int columnCount = 1;
	for (int i = 0; i < rowCount; ++i)
		columnCount = std::max(columnCount, static_cast<int>(lines[i].count(delimiter)) + 1);

	csvPage_->clear();
	csvPage_->setRowCount(rowCount);
	csvPage_->setColumnCount(columnCount);

	for (int row = 0; row < rowCount; ++row)
	{
		const QStringList fields = lines[row].split(delimiter);
		for (int col = 0; col < fields.size(); ++col)
			csvPage_->setItem(row, col, new QTableWidgetItem(fields[col]));
	}

	csvPage_->resizeColumnsToContents();
	stack_->setCurrentWidget(csvPage_);

	if (lines.size() > MaxRows)
		statusBar()->showMessage(tr("CSV preview truncated to first %1 rows.").arg(MaxRows), 5000);
}

void MainWindow::ShowCso(const cso_pak::PakArchive::Entry &entry, std::vector<uint8_t> data)
{
	// .cso entries carry a second layer of encryption on top of the normal
	// pak/SnowCipher one (already removed by ExtractEntry before we get
	// here) -- a fixed-key TEA-style block cipher. Once that's peeled off,
	// the payload is table/CSV-like text, so it reuses the CSV view.
	cso_pak::CsoDecoder::Decrypt(data);

#ifdef _WIN32
	// The game's own text tables are Korean (EUC-KR/CP949), which isn't
	// valid UTF-8 and would otherwise show as mojibake. Detect and convert
	// to UTF-8 before handing off to the CSV view, which expects UTF-8/BOM.
	if (cso_gui::LooksLikeEucKr(data))
	{
		const std::string utf8 = cso_gui::DecodeEucKrToUtf8(data);
		ShowCsv(std::vector<uint8_t>(utf8.begin(), utf8.end()));
		return;
	}
#endif

	if (LooksLikeText(data))
	{
		ShowCsv(data);
		return;
	}

	ShowProperties(entry, tr("Decrypted this .cso entry, but the result doesn't look like readable text."));
}

void MainWindow::StopMediaPlayback()
{
	if (mediaPlayer_ != nullptr)
	{
		mediaPlayer_->stop();
		mediaPlayer_->setSource(QUrl()); // Release the file handle before we try to delete it.
	}

	if (videoWidget_ != nullptr)
		videoWidget_->setVisible(false);

	// Best-effort cleanup: on Windows the media backend can hold the file
	// open briefly after stop(), so deletion can occasionally fail here.
	// That just leaves a harmless leftover in the OS temp folder rather
	// than causing a crash.
	tempMediaFile_.reset();
}

void MainWindow::ShowMedia(const std::vector<uint8_t> &data, const QString &displayName, const QString &fileExtension)
{
	StopMediaPlayback();

	// Name the temp file with the real extension: some backends use it as a
	// hint for picking the right demuxer instead of sniffing the content.
	const QString suffix = fileExtension.isEmpty() ? QStringLiteral("bin") : fileExtension;
	auto tempFile = std::make_unique<QTemporaryFile>(
		QDir::temp().filePath(QStringLiteral("cso-pak-preview-XXXXXX.") + suffix));
	tempFile->setAutoRemove(true);
	if (!tempFile->open())
	{
		ShowPlaceholder(tr("Could not create a temporary file to play this media."));
		return;
	}

	tempFile->write(reinterpret_cast<const char *>(data.data()), static_cast<qint64>(data.size()));
	tempFile->flush();
	tempFile->close();
	tempMediaFile_ = std::move(tempFile);

	mediaFileLabel_->setText(displayName);
	mediaPositionSlider_->setRange(0, 0);
	mediaTimeLabel_->setText(QStringLiteral("00:00 / 00:00"));

	mediaPlayer_->setSource(QUrl::fromLocalFile(tempMediaFile_->fileName()));
	mediaPlayer_->play();

	stack_->setCurrentWidget(mediaPageContainer_);
}

void MainWindow::OnPlayPauseClicked()
{
	if (mediaPlayer_->playbackState() == QMediaPlayer::PlayingState)
		mediaPlayer_->pause();
	else
		mediaPlayer_->play();
}

void MainWindow::OnMediaSliderMoved(int valueMs)
{
	mediaPlayer_->setPosition(valueMs);
}

void MainWindow::OnMediaPositionChanged(qint64 positionMs)
{
	if (!mediaSliderBeingDragged_)
		mediaPositionSlider_->setValue(static_cast<int>(positionMs));

	mediaTimeLabel_->setText(tr("%1 / %2")
		.arg(FormatDuration(positionMs), FormatDuration(mediaPlayer_->duration())));
}

void MainWindow::OnMediaDurationChanged(qint64 durationMs)
{
	mediaPositionSlider_->setRange(0, static_cast<int>(durationMs));
}

void MainWindow::OnMediaPlaybackStateChanged(QMediaPlayer::PlaybackState state)
{
	const bool playing = state == QMediaPlayer::PlayingState;
	playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
	playPauseButton_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void MainWindow::ShowProperties(const cso_pak::PakArchive::Entry &entry, const QString &note)
{
	propertiesPage_->setRowCount(0);

	const auto addRow = [this](const QString &property, const QString &value)
	{
		const int row = propertiesPage_->rowCount();
		propertiesPage_->insertRow(row);
		propertiesPage_->setItem(row, 0, new QTableWidgetItem(property));
		propertiesPage_->setItem(row, 1, new QTableWidgetItem(value));
	};

	const QString path = QString::fromStdU16String(entry.path);
	const QString extension = ExtensionOf(entry);

	addRow(tr("Path"), path);
	addRow(tr("Extension"), extension.isEmpty() ? tr("(none)") : extension);
	addRow(tr("Real size"), FormatSize(entry.realSize));
	addRow(tr("Packed size"), FormatSize(entry.packedSize));
	addRow(tr("Data block offset"), QString::number(entry.fileOffset));
	addRow(tr("Type flags"), FormatTypeFlags(entry.type));

	QStringList baseKeyWords;
	for (const auto word : entry.baseKey)
		baseKeyWords << QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0')).toUpper();
	addRow(tr("Base key"), baseKeyWords.join(QLatin1Char(' ')));

	addRow(tr("Entry checksum"), QStringLiteral("0x%1").arg(entry.entryChecksum, 8, 16, QLatin1Char('0')).toUpper());

	if (!note.isEmpty())
		addRow(tr("Note"), note);

	stack_->setCurrentWidget(propertiesPage_);
}

void MainWindow::ShowPreviewForEntry(const cso_pak::PakArchive::Entry &entry)
{
	StopMediaPlayback();

	if ((entry.type & cso_pak::EntryTypeCompressed) != 0)
	{
		ShowProperties(entry, tr("This entry is stored compressed; content preview isn't supported yet."));
		return;
	}

	std::vector<uint8_t> data;
	try
	{
		data = archive_->ExtractEntry(entry);
	}
	catch (const std::exception &ex)
	{
		ShowProperties(entry, tr("Failed to decrypt this entry: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	const QString ext = ExtensionOf(entry);

	if (ext == QLatin1String("tga"))
	{
		QString error;
		const QImage image = tga::Load(data, &error);
		if (!image.isNull())
		{
			ShowImage(image);
			return;
		}

		ShowProperties(entry, tr("Failed to decode TGA image: %1").arg(error));
		return;
	}

	static const QStringList nativeImageExtensions = {
		QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
		QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("ppm"),
	};
	if (nativeImageExtensions.contains(ext))
	{
		QImage image;
		if (image.loadFromData(reinterpret_cast<const uchar *>(data.data()), static_cast<int>(data.size())))
		{
			ShowImage(image);
			return;
		}
	}

	if (ext == QLatin1String("csv"))
	{
		ShowCsv(data);
		return;
	}

	if (ext == QLatin1String("cso"))
	{
		ShowCso(entry, std::move(data));
		return;
	}

	if (ext == QLatin1String("wav") || ext == QLatin1String("webm"))
	{
		ShowMedia(data, QFileInfo(QString::fromStdU16String(entry.path)).fileName(), ext);
		return;
	}

	static const QStringList textExtensions = {
		QStringLiteral("txt"), QStringLiteral("ini"), QStringLiteral("cfg"),
		QStringLiteral("log"), QStringLiteral("xml"), QStringLiteral("json"),
		QStringLiteral("lst"), QStringLiteral("def"), QStringLiteral("vdf"),
		QStringLiteral("inf"),
	};
	if (textExtensions.contains(ext) || LooksLikeText(data))
	{
		ShowText(DecodeText(data));
		return;
	}

	// Includes .webm and any other binary format: no preview, just properties.
	ShowProperties(entry);
}

QString MainWindow::ExtensionOf(const cso_pak::PakArchive::Entry &entry)
{
	const QString name = QFileInfo(QString::fromStdU16String(entry.path)).fileName();
	const int dot = name.lastIndexOf(QLatin1Char('.'));
	if (dot <= 0 || dot == name.size() - 1)
		return QString();

	return name.mid(dot + 1).toLower();
}

bool MainWindow::LooksLikeText(const std::vector<uint8_t> &data)
{
	if (data.empty())
		return true;

	// A Unicode byte-order mark is an unambiguous "this is text" signal,
	// even though UTF-16/UTF-32 content is naturally full of null bytes
	// (which the heuristic below would otherwise read as binary).
	if (data.size() >= 2 &&
		((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
		return true;
	if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
		return true;

	const size_t sampleSize = std::min<size_t>(data.size(), 4096);
	size_t suspicious = 0;

	for (size_t i = 0; i < sampleSize; ++i)
	{
		const uint8_t byte = data[i];
		if (byte == 0)
			return false; // Null bytes are a strong binary signal.

		const bool isControl = byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r';
		if (isControl || byte == 0x7F)
			++suspicious;
	}

	return (static_cast<double>(suspicious) / static_cast<double>(sampleSize)) < 0.02;
}

QString MainWindow::DecodeText(const std::vector<uint8_t> &data)
{
	const auto bytes = reinterpret_cast<const char *>(data.data());
	const auto size = static_cast<qsizetype>(data.size());

	// Respect a byte-order mark if present. Source-engine style locale
	// files (like the CS Online resource/*.txt ones) are commonly saved as
	// UTF-16LE with BOM by their original editing tools.
	if (data.size() >= 4 && data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00)
	{
		QStringDecoder decoder(QStringDecoder::Utf32); // BOM-aware: picks LE/BE from the mark.
		return decoder(QByteArrayView(bytes, size));
	}
	if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF)
	{
		QStringDecoder decoder(QStringDecoder::Utf32);
		return decoder(QByteArrayView(bytes, size));
	}
	if (data.size() >= 2 &&
		((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
	{
		QStringDecoder decoder(QStringDecoder::Utf16); // BOM-aware: picks LE/BE from the mark.
		return decoder(QByteArrayView(bytes, size));
	}

	// UTF-8 BOM: skip it, then decode as plain UTF-8.
	const qsizetype utf8Offset = (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) ? 3 : 0;

	QStringDecoder utf8Decoder(QStringDecoder::Utf8);
	QString text = utf8Decoder(QByteArrayView(bytes + utf8Offset, size - utf8Offset));
	if (!utf8Decoder.hasError())
		return text;

	// Fall back to Latin-1, which never fails, so something is always shown.
	// If the archive's un-BOM'd text turns out to be Windows-1251 (or
	// another 8-bit code page) instead, add the "Qt6 Core5Compat" module
	// and decode with QTextCodec::codecForName("Windows-1251") here.
	return QString::fromLatin1(bytes, static_cast<int>(data.size()));
}

QString MainWindow::FormatSize(uint64_t bytes)
{
	constexpr double Kb = 1024.0;
	constexpr double Mb = Kb * 1024.0;

	if (bytes < static_cast<uint64_t>(Kb))
		return tr("%1 B").arg(bytes);

	if (bytes < static_cast<uint64_t>(Mb))
		return tr("%1 KB").arg(bytes / Kb, 0, 'f', 1);

	return tr("%1 MB").arg(bytes / Mb, 0, 'f', 2);
}

QString MainWindow::FormatDuration(qint64 milliseconds)
{
	if (milliseconds < 0)
		milliseconds = 0;

	const qint64 totalSeconds = milliseconds / 1000;
	return QStringLiteral("%1:%2")
		.arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
		.arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

QString MainWindow::FormatTypeFlags(uint32_t type)
{
	QStringList flags;
	if ((type & cso_pak::EntryTypeCompressed) != 0)
		flags << tr("Compressed");
	if ((type & cso_pak::EntryTypeEncrypted) != 0)
		flags << tr("Encrypted");
	if ((type & cso_pak::EntryTypeEncryptedAgain) != 0)
		flags << tr("Encrypted (double)");
	if (flags.isEmpty())
		flags << tr("None");

	return QStringLiteral("%1 (0x%2)").arg(flags.join(QStringLiteral(", ")))
		.arg(type, 0, 16);
}

QString MainWindow::SanitizedRelativePath(const std::u16string &entryPath)
{
	QString path = QString::fromStdU16String(entryPath);
	path.replace(QLatin1Char('\\'), QLatin1Char('/'));

	const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	QStringList cleaned;
	for (const auto &part : parts)
	{
		if (part == QLatin1String(".."))
			return QString(); // Refuse to write outside the destination folder.
		if (part == QLatin1String("."))
			continue;

		cleaned << part;
	}

	if (cleaned.isEmpty())
		return QString();

	return cleaned.join(QLatin1Char('/'));
}
