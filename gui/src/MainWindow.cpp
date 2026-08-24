#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QByteArrayView>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
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
		PageModel,
		PageSprite,
	};

	// A QLabel whose word-wrapped content gets the height it actually needs.
	//
	// QLabel implements heightForWidth(), but an enclosing QBoxLayout only
	// honors it when *every* item in that layout supports heightForWidth.
	// The model-controls column mixes wrapped labels with combos, buttons,
	// sliders and checkboxes (none of which do), so the layout falls back to
	// each widget's sizeHint().height(). For a wrapped label whose width was
	// squeezed down by a horizontal QSizePolicy::Ignored, that hint still
	// describes the text at its natural (wide) width -- so the label gets
	// far less height than its lines need and long lists get clipped.
	// Whenever the label is resized to its actual width, this pins the
	// minimum height to heightForWidth(width()) so the following layout pass
	// gives it room for every wrapped line (the button and everything below
	// slide down instead of the text vanishing).
	class WrappedFitLabel final : public QLabel
	{
	public:
		using QLabel::QLabel;

	protected:
		void resizeEvent(QResizeEvent *event) override
		{
			QLabel::resizeEvent(event);
			const int needed = wrapHeight(width());
			if (needed != minimumHeight())
				setMinimumHeight(needed);
		}

	private:
		// Real multi-line height of the word-wrapped text at width w.
		//
		// QLabel::heightForWidth() is unusable here: it refuses to report a
		// height smaller than the label's *current* contents height. So if a
		// transiently narrow layout pass ever inflates minimumHeight, the
		// label is stuck tall forever (its own height feeds back into every
		// subsequent heightForWidth call) -- exactly the "big empty band
		// above/below the text" bug. This computes the height from the text
		// metrics instead, which is independent of the label's geometry, so
		// it converges to the real wrapped height from either direction.
		int wrapHeight(int w) const
		{
			const int avail = qMax(1, w - contentsMargins().left() - contentsMargins().right());
			QTextDocument doc;
			doc.setDefaultFont(font());
			QTextOption opt;
			opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
			doc.setDefaultTextOption(opt);
			doc.setPlainText(text());
			doc.setDocumentMargin(0);
			doc.setTextWidth(avail);
			return contentsMargins().top() + contentsMargins().bottom()
				+ int(std::ceil(doc.documentLayout()->documentSize().height()));
		}
	};

	// One-line-per-item list (e.g. the missing-texture names) that elides any
	// line that wouldn't fit the label width instead of wrapping it. Long
	// names (model/*.bmp paths with no breakable characters) can't be split
	// at a word boundary anyway -- QLabel would paint them overflowing the
	// panel edge -- so eliding is the only way they visually fit. The full,
	// unelided text is exposed as a tooltip. The height is simply the line
	// count times the line height, so it never differs from what is painted.
	class ElidedListLabel final : public QLabel
	{
	public:
		using QLabel::QLabel;

		void setListText(const QString &fullText)
		{
			fullText_ = fullText;
			setToolTip(fullText);
			updateText();
		}

	protected:
		void resizeEvent(QResizeEvent *event) override
		{
			QLabel::resizeEvent(event);
			updateText();
		}

	private:
		void updateText()
		{
			if (fullText_.isEmpty())
				return;
			const int avail = qMax(1, width() - contentsMargins().left() - contentsMargins().right());
			const QFontMetrics fm(font());
			QStringList displayed;
			int totalLines = 0;
			const auto srcLines = fullText_.split(QLatin1Char('\n'));
			for (const QString &line : srcLines)
			{
				displayed << fm.elidedText(line, Qt::ElideRight, avail);
				++totalLines;
			}
			setText(displayed.join(QLatin1Char('\n')));
			const int needed = totalLines * fm.height()
				+ contentsMargins().top() + contentsMargins().bottom();
			if (needed != minimumHeight())
				setMinimumHeight(needed);
		}

		QString fullText_;
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

	modelView_ = new cso_gui::ModelViewWidget;
	modelView_->setMinimumSize(300, 300);

	modelControlsContainer_ = new QWidget;
	auto *modelControlsLayout = new QVBoxLayout(modelControlsContainer_);
	modelControlsLayout->setAlignment(Qt::AlignTop);

	auto *modelControlsScroll = new QScrollArea;
	modelControlsScroll->setWidget(modelControlsContainer_);
	modelControlsScroll->setWidgetResizable(true);
	modelControlsScroll->setMinimumWidth(240);
	modelControlsScroll->setMaximumWidth(340);
	modelControlsScroll->setFrameShape(QFrame::NoFrame);

	modelAnimTimer_ = new QTimer(this);
	connect(modelAnimTimer_, &QTimer::timeout, this, &MainWindow::OnModelAnimationTick);

	modelCameraBox_ = new QGroupBox(tr("Camera"));
	auto *cameraLayout = new QVBoxLayout(modelCameraBox_);
	modelCameraCombo_ = new QComboBox;
	modelCameraCombo_->addItem(tr("Orbit"));
	modelCameraCombo_->addItem(tr("First person"));
	cameraLayout->addWidget(modelCameraCombo_);

	auto *fovRow = new QHBoxLayout;
	fovRow->addWidget(new QLabel(tr("FOV")));
	modelFovSpin_ = new QDoubleSpinBox;
	modelFovSpin_->setRange(30.0, 120.0);
	modelFovSpin_->setSuffix(QStringLiteral("\u00b0"));
	modelFovSpin_->setValue(74.0);
	fovRow->addWidget(modelFovSpin_, 1);
	cameraLayout->addLayout(fovRow);

	connect(modelCameraCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int index)
		{
			modelView_->SetCameraMode(index == 1
				? cso_gui::ModelViewWidget::CameraMode::FirstPerson
				: cso_gui::ModelViewWidget::CameraMode::Orbit);
			modelFovSpin_->setEnabled(index == 1);
		});
	connect(modelFovSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
		[this](double value) { modelView_->SetFirstPersonFieldOfView(static_cast<float>(value)); });
	connect(modelView_, &cso_gui::ModelViewWidget::CameraModeChanged, this,
		[this](int mode)
		{
			modelCameraCombo_->blockSignals(true);
			modelCameraCombo_->setCurrentIndex(mode == static_cast<int>(cso_gui::ModelViewWidget::CameraMode::FirstPerson) ? 1 : 0);
			modelCameraCombo_->blockSignals(false);
			modelFovSpin_->setEnabled(mode == static_cast<int>(cso_gui::ModelViewWidget::CameraMode::FirstPerson));
		});
	connect(modelView_, &cso_gui::ModelViewWidget::FirstPersonFieldOfViewChanged, this,
		[this](float fov)
		{
			modelFovSpin_->blockSignals(true);
			modelFovSpin_->setValue(fov);
			modelFovSpin_->blockSignals(false);
		});

	// Reset View is separate from the Lighting box's own Reset (camera vs.
	// light), also persistent for the same reason. Panning with the fix
	// above should no longer lose track of the model, but some multi-piece
	// files (a boss far from its own attack-effect meshes, etc.) are still
	// awkward to frame by hand, so this is a one-click way back to the
	// same auto-framed view SetModel() starts a fresh model at.
	auto *resetViewButton = new QPushButton(tr("Reset View (camera)"));
	connect(resetViewButton, &QPushButton::clicked, this, [this]() { modelView_->ResetView(); });

	// Lighting controls are deliberately a *persistent* panel (built once,
	// here) rather than living inside modelControlsContainer_, which gets
	// torn down and rebuilt from scratch every time a different model
	// loads. A chosen light angle is a viewer preference, not per-model
	// data, so it should survive switching between models.
	auto *lightBox = new QGroupBox(tr("Lighting"));
	auto *lightLayout = new QVBoxLayout(lightBox);

	lightYawSlider_ = new QSlider(Qt::Horizontal);
	lightYawSlider_->setRange(-180, 180);
	lightYawSlider_->setValue(0);
	connect(lightYawSlider_, &QSlider::valueChanged, this, &MainWindow::OnLightAnglesChanged);

	lightPitchSlider_ = new QSlider(Qt::Horizontal);
	lightPitchSlider_->setRange(-90, 90);
	lightPitchSlider_->setValue(0);
	connect(lightPitchSlider_, &QSlider::valueChanged, this, &MainWindow::OnLightAnglesChanged);

	auto *resetLightButton = new QPushButton(tr("Reset"));
	connect(resetLightButton, &QPushButton::clicked, this, &MainWindow::OnResetLightClicked);

	lightLayout->addWidget(new QLabel(tr("Yaw")));
	lightLayout->addWidget(lightYawSlider_);
	lightLayout->addWidget(new QLabel(tr("Pitch")));
	lightLayout->addWidget(lightPitchSlider_);
	lightLayout->addWidget(resetLightButton);

	auto *modelRightPanel = new QWidget;
	auto *modelRightPanelLayout = new QVBoxLayout(modelRightPanel);
	modelRightPanelLayout->setContentsMargins(0, 0, 0, 0);
	modelRightPanelLayout->addWidget(modelCameraBox_);
	modelRightPanelLayout->addWidget(resetViewButton);
	modelRightPanelLayout->addWidget(lightBox);
	modelRightPanelLayout->addWidget(modelControlsScroll, 1);

	auto *modelSplitter = new QSplitter(Qt::Horizontal);
	modelSplitter->addWidget(modelView_);
	modelSplitter->addWidget(modelRightPanel);
	modelSplitter->setStretchFactor(0, 1);
	modelSplitter->setStretchFactor(1, 0);
	modelPageContainer_ = modelSplitter;

	spriteImageLabel_ = new QLabel;
	spriteImageLabel_->setAlignment(Qt::AlignCenter);
	spriteImageLabel_->setBackgroundRole(QPalette::Dark);
	spriteImageLabel_->setAutoFillBackground(true);

	spriteScrollArea_ = new QScrollArea;
	spriteScrollArea_->setWidget(spriteImageLabel_);
	spriteScrollArea_->setWidgetResizable(false);
	spriteScrollArea_->setAlignment(Qt::AlignCenter);

	spriteFitToWindowCheck_ = new QCheckBox(tr("Fit to window"));
	spriteFitToWindowCheck_->setChecked(false); // Sprites are usually tiny; don't blow them up by default.
	connect(spriteFitToWindowCheck_, &QCheckBox::toggled, this, &MainWindow::OnSpriteFitToWindowToggled);

	spritePlayButton_ = new QPushButton(tr("Play"));
	spritePlayButton_->setCheckable(true);
	connect(spritePlayButton_, &QPushButton::toggled, this, &MainWindow::OnSpritePlayToggled);

	spriteFrameSlider_ = new QSlider(Qt::Horizontal);
	spriteFrameSlider_->setRange(0, 0);
	connect(spriteFrameSlider_, &QSlider::valueChanged, this, &MainWindow::OnSpriteFrameSliderMoved);

	spriteFrameLabel_ = new QLabel(tr("Frame 0 / 0"));

	auto *spriteControlsLayout = new QHBoxLayout;
	spriteControlsLayout->addWidget(spriteFitToWindowCheck_);
	spriteControlsLayout->addWidget(spritePlayButton_);
	spriteControlsLayout->addWidget(spriteFrameSlider_, 1);
	spriteControlsLayout->addWidget(spriteFrameLabel_);

	spriteAnimTimer_ = new QTimer(this);
	connect(spriteAnimTimer_, &QTimer::timeout, this, &MainWindow::OnSpriteAnimationTick);

	spritePageContainer_ = new QWidget;
	auto *spritePageLayout = new QVBoxLayout(spritePageContainer_);
	spritePageLayout->setContentsMargins(4, 4, 4, 4);
	spritePageLayout->addWidget(spriteScrollArea_, 1);
	spritePageLayout->addLayout(spriteControlsLayout);

	stack_->addWidget(placeholderPage_);   // PagePlaceholder
	stack_->addWidget(textPage_);          // PageText
	stack_->addWidget(imagePageContainer_);// PageImage
	stack_->addWidget(csvPage_);           // PageCsv
	stack_->addWidget(propertiesPage_);    // PageProperties
	stack_->addWidget(mediaPageContainer_);// PageMedia
	stack_->addWidget(modelPageContainer_);// PageModel
	stack_->addWidget(spritePageContainer_);// PageSprite

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

	recentFilesMenu_ = fileMenu->addMenu(tr("Recent &Files"));
	UpdateRecentFilesMenu();

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
	if (stack_->currentWidget() == spritePageContainer_ && spriteFitToWindowCheck_->isChecked())
		UpdateSpriteFrameDisplay();
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

		AddRecentFile(QFileInfo(path).absoluteFilePath());
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Failed to open archive"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::AddRecentFile(const QString &path)
{
	QSettings settings;
	QStringList files = settings.value(QStringLiteral("recentFiles")).toStringList();
	files.removeAll(path);
	files.prepend(path);
	while (files.size() > kMaxRecentFiles)
		files.removeLast();
	settings.setValue(QStringLiteral("recentFiles"), files);

	UpdateRecentFilesMenu();
}

void MainWindow::UpdateRecentFilesMenu()
{
	if (recentFilesMenu_ == nullptr)
		return;

	recentFilesMenu_->clear();

	const QSettings settings;
	const QStringList files = settings.value(QStringLiteral("recentFiles")).toStringList();

	if (files.isEmpty())
	{
		QAction *emptyAction = recentFilesMenu_->addAction(tr("(no recent files)"));
		emptyAction->setEnabled(false);
		return;
	}

	int number = 1;
	for (const QString &path : files)
	{
		// &1, &2, ... give each of the first 9 a single-key mnemonic, like
		// most apps' recent-files lists.
		QAction *action = recentFilesMenu_->addAction(
			tr("&%1 %2").arg(number).arg(QFileInfo(path).fileName()));
		action->setToolTip(path);
		action->setStatusTip(path);
		connect(action, &QAction::triggered, this, [this, path]()
		{
			if (!QFileInfo::exists(path))
			{
				QMessageBox::warning(this, tr("File not found"),
					tr("%1\n\nThis file no longer exists.").arg(path));
				return;
			}
			LoadPak(path);
		});
		++number;
	}

	recentFilesMenu_->addSeparator();
	QAction *clearAction = recentFilesMenu_->addAction(tr("Clear Recent Files"));
	connect(clearAction, &QAction::triggered, this, [this]()
	{
		QSettings clearSettings;
		clearSettings.remove(QStringLiteral("recentFiles"));
		UpdateRecentFilesMenu();
	});
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
				while (!data.empty() && data.back() == 0)
					data.pop_back();
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

	if (model_->IsWadLump(current))
	{
		ShowWadLumpPreview(model_->WadArchiveIndexForIndex(current), model_->WadEntryIndexForIndex(current));
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
	StopModelPlayback();
	StopSpritePlayback();
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

	// The cipher works in 8-byte blocks, so the original file was very
	// likely zero-padded up to a multiple of 8 before encryption -- those
	// padding bytes decrypt back to real 0x00 bytes at the very end of the
	// buffer. Trim them before the text check: LooksLikeText treats any
	// null byte as a hard "this is binary" signal, so a handful of
	// legitimate trailing padding bytes would otherwise veto an entire
	// table that's actually perfectly readable text.
	while (!data.empty() && data.back() == 0)
		data.pop_back();

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

void MainWindow::ShowModel(const cso_pak::PakArchive::Entry &entry, const std::vector<uint8_t> &data)
{
	auto model = std::make_shared<cso_gui::StudioModel>();
	try
	{
		model->Load(data);
	}
	catch (const std::exception &ex)
	{
		currentModel_.reset();
		externalTextureSources_.clear();
		modelView_->Clear();
		ShowProperties(entry, tr("Failed to parse this .mdl model: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	// Textures the model only *names* (its embedded copy is a small
	// placeholder) are usually shipped as separate image files inside the
	// same pak. Bind them before the 3D view builds its GPU textures, so the
	// model shows real surfaces instead of black.
	ResolveExternalTextures(model);

	const QString fileName = QFileInfo(QString::fromStdU16String(entry.path)).fileName();
	const bool firstPerson = fileName.startsWith(QStringLiteral("v_"), Qt::CaseInsensitive);
	currentModel_ = model;
	modelView_->SetModel(model, firstPerson);
	modelCameraCombo_->blockSignals(true);
	modelCameraCombo_->setCurrentIndex(firstPerson ? 1 : 0);
	modelCameraCombo_->blockSignals(false);
	modelFovSpin_->setEnabled(firstPerson);
	modelCameraBox_->setVisible(firstPerson);
	RebuildModelControls();

	stack_->setCurrentWidget(modelPageContainer_);
}

bool MainWindow::TextureLooksExternal(const cso_gui::StudioModel::Texture &tex)
{
	// CSO marks textures that live outside the .mdl with a leading '#' in the
	// name (e.g. "#256256wondercannonex_p.bmp"). Some archives also store a
	// tiny placeholder (like 4x1) instead of real pixels; anything that small
	// is almost certainly a placeholder too, so treat it as external as well.
	if (!tex.name.empty() && tex.name.front() == '#')
		return true;

	const long long area = static_cast<long long>(tex.width) * tex.height;
	return area > 0 && area <= 64;
}

QImage MainWindow::DecodeImageData(const std::vector<uint8_t> &data, const QString &extension)
{
	if (extension == QLatin1String("tga"))
		return tga::Load(data);

	if (extension == QLatin1String("dds"))
	{
		try
		{
			return cso_gui::LoadDdsImage(data);
		}
		catch (const std::exception &)
		{
			return {};
		}
	}

	QImage image;
	if (image.loadFromData(reinterpret_cast<const uchar *>(data.data()), static_cast<int>(data.size())))
		return image;
	return {};
}

QImage MainWindow::DecodeImageFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	const QByteArray bytes = file.readAll();
	std::vector<uint8_t> data(bytes.begin(), bytes.end());
	return DecodeImageData(data, QFileInfo(path).suffix().toLower());
}

bool MainWindow::FileMatchesTextureName(const QString &path, const std::string &textureName)
{
	const QFileInfo info(path);
	const QString wanted = QString::fromLatin1(textureName.c_str());
	if (info.fileName().compare(wanted, Qt::CaseInsensitive) == 0)
		return true;

	// Some packs ship the same texture under another extension (the .mdl asks
	// for "foo.bmp" but the folder contains "foo.tga"); compare base names.
	return info.completeBaseName().compare(
		QFileInfo(wanted).completeBaseName(), Qt::CaseInsensitive) == 0;
}

const cso_pak::PakArchive::Entry *MainWindow::FindArchiveEntryForTextureName(
	const std::string &textureName) const
{
	if (!archive_)
		return nullptr;

	// Fold to lowercase ASCII so the byte-level match is case-insensitive
	// without depending on Qt's encoding assumptions for model name bytes.
	auto fold = [](std::string value)
		{
			for (char &c : value)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return value;
		};

	auto entryFileName = [](const std::u16string &path) -> std::string
		{
			return QFileInfo(QString::fromStdU16String(path)).fileName().toUtf8().toStdString();
		};

	const std::string wanted = fold(textureName);
	for (const auto &entry : archive_->Entries())
	{
		if (fold(entryFileName(entry.path)) == wanted)
			return &entry;  // exact filename match wins
	}

	// No exact file: retry with the same base name but any image extension,
	// so "foo.bmp" can bind "foo.tga"/"foo.dds" when only those are shipped.
	std::string wantedBase = wanted;
	const size_t dot = wantedBase.find_last_of('.');
	if (dot != std::string::npos)
		wantedBase.resize(dot);

	static const char *const kAltExtensions[] = { "tga", "dds", "bmp", "png", "jpg", "jpeg" };
	for (const char *altExtension : kAltExtensions)
	{
		for (const auto &entry : archive_->Entries())
		{
			const std::string name = fold(entryFileName(entry.path));
			const size_t entryDot = name.find_last_of('.');
			if (entryDot == std::string::npos)
				continue;
			if (name.compare(0, entryDot, wantedBase) == 0 &&
				name.compare(entryDot + 1, std::string::npos, altExtension) == 0)
				return &entry;
		}
	}

	return nullptr;
}

void MainWindow::ResolveExternalTextures(const std::shared_ptr<cso_gui::StudioModel> &model)
{
	externalTextureSources_.clear();
	if (!model || !archive_)
		return;

	const auto &textures = model->Textures();
	externalTextureSources_.resize(textures.size());

	for (size_t i = 0; i < textures.size(); ++i)
	{
		const auto &tex = textures[static_cast<size_t>(i)];
		if (!TextureLooksExternal(tex))
			continue;

		const cso_pak::PakArchive::Entry *found =
			FindArchiveEntryForTextureName(tex.name);
		if (!found)
			continue;

		std::vector<uint8_t> data;
		try
		{
			data = archive_->ExtractEntry(*found);
		}
		catch (const std::exception &)
		{
			continue;
		}

		const QImage image = DecodeImageData(data, ExtensionOf(*found));
		if (image.isNull())
			continue;

		model->SetTextureImage(static_cast<int>(i), image);
		externalTextureSources_[i] = QString::fromStdU16String(found->path);
	}
}

void MainWindow::LoadTexturesFromFolder()
{
	if (!currentModel_)
		return;

	const auto &textures = currentModel_->Textures();
	std::vector<int> unresolved;
	for (size_t i = 0; i < textures.size(); ++i)
	{
		// Only textures ResolveExternalTextures didn't already bind from
		// inside the pak -- reprocessing an already-resolved one would just
		// redo work, and risks overwriting a good match with an unrelated
		// same-named file if the picked folder happens to contain one.
		const bool alreadyResolved = i < externalTextureSources_.size() && !externalTextureSources_[i].isEmpty();
		if (TextureLooksExternal(textures[i]) && !alreadyResolved)
			unresolved.push_back(static_cast<int>(i));
	}
	if (unresolved.empty())
		return;

	const QString dir = QFileDialog::getExistingDirectory(this,
		tr("Choose a folder containing the model textures"));
	if (dir.isEmpty())
		return;

	// Walk the folder once and reuse the listing for every unresolved
	// texture, instead of a fresh recursive scan per texture.
	QStringList files;
	QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
		files << it.next();

	int loaded = 0;
	for (const int i : unresolved)
	{
		const auto &tex = textures[static_cast<size_t>(i)];
		QString foundPath;
		for (const QString &candidate : files)
		{
			if (FileMatchesTextureName(candidate, tex.name))
			{
				foundPath = candidate;
				break;
			}
		}
		if (foundPath.isEmpty())
			continue;

		const QImage image = DecodeImageFile(foundPath);
		if (image.isNull())
			continue;

		currentModel_->SetTextureImage(i, image);
		if (static_cast<size_t>(i) < externalTextureSources_.size())
			externalTextureSources_[static_cast<size_t>(i)] = foundPath;
		++loaded;
	}

	// Refresh the 3D view (its GPU texture cache is keyed off the old
	// placeholder images) and the texture controls.
	if (loaded > 0)
	{
		modelView_->ReloadTextures();
		RebuildModelControls();
	}

	statusBar()->showMessage(loaded > 0
		? tr("Loaded %1 external texture(s) from %2").arg(loaded).arg(dir)
		: tr("No textures matching this .mdl were found in %1").arg(dir), 6000);
}

void MainWindow::StopModelPlayback()
{
	if (modelAnimTimer_ != nullptr)
		modelAnimTimer_->stop();
	if (modelPlayButton_ != nullptr)
		modelPlayButton_->setChecked(false);
}

void MainWindow::RebuildModelControls()
{
	// Tear down the previous model's controls (bodypart/skin/sequence
	// widgets vary per file) and rebuild from scratch for the new one.
	//
	// This used to walk the layout with takeAt()/deleteLater(), but that
	// only unmanages/deletes the *layout* for nested items (like the
	// play-button+slider row added via addLayout()) -- it does NOT delete
	// the widgets that nested layout was managing. Those widgets are
	// parented directly to modelControlsContainer_, not to the layout, so
	// they stayed alive and visible forever, stacking up as duplicate
	// Play buttons/sliders every time a new model with a sequence list
	// loaded. Deleting the whole layout tree and then explicitly deleting
	// every remaining direct-child widget covers both cases.
	delete modelControlsContainer_->layout();
	qDeleteAll(modelControlsContainer_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly));

	modelSkinCombo_ = nullptr;
	modelSequenceCombo_ = nullptr;
	modelTextureCombo_ = nullptr;
	modelTexturePreview_ = nullptr;
	modelTextureInfoLabel_ = nullptr;
	modelForceAdditiveCheck_ = nullptr;
	modelFrameSlider_ = nullptr;
	modelFrameLabel_ = nullptr;
	modelPlayButton_ = nullptr;
	modelWireframeCheck_ = nullptr;

	auto *vLayout = new QVBoxLayout(modelControlsContainer_);
	vLayout->setAlignment(Qt::AlignTop);

	// The controls live inside a thin (fixed 240-340px) scroll column. Any
	// widget whose horizontal size hint is wider than that (a combo whose
	// longest model/texture/sequence name is long, a label with a long
	// single-line string) would otherwise inflate the column past the
	// viewport and make a horizontal scrollbar appear. Horizontal
	// QSizePolicy::Ignored makes the layout shrink these widgets to the
	// available width instead.
	auto makeCaption = [](const QString &text) -> QLabel *
		{
			auto *label = new WrappedFitLabel(text);
			label->setWordWrap(true);
			label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			return label;
		};
	auto makeCombo = []() -> QComboBox *
		{
			auto *combo = new QComboBox;
			combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			return combo;
		};

	if (!currentModel_)
		return;

	// One combo per bodypart that actually has more than one submodel to
	// choose from (a bodypart with just one submodel has nothing to pick).
	const int bodyPartCount = modelView_->BodyPartCount();
	for (int bp = 0; bp < bodyPartCount; ++bp)
	{
		const int subCount = modelView_->SubModelCount(bp);
		if (subCount <= 1)
			continue;

		const auto &bodyPart = currentModel_->BodyParts()[static_cast<size_t>(bp)];
		const QString label = bodyPart.name.empty()
			? tr("Bodypart %1").arg(bp)
		: DecodeModelText(bodyPart.name);

		vLayout->addWidget(makeCaption(label));
		auto *combo = makeCombo();
		for (int s = 0; s < subCount; ++s)
		{
			const auto &subModel = bodyPart.models[static_cast<size_t>(s)];
			combo->addItem(subModel.name.empty()
				? tr("Submodel %1").arg(s)
				: DecodeModelText(subModel.name));
		}
		combo->setCurrentIndex(modelView_->CurrentSubModel(bp));
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this, bp](int subModel) { modelView_->SetSubModel(bp, subModel); });
		vLayout->addWidget(combo);
	}

	const int skinCount = modelView_->SkinFamilyCount();
	if (skinCount > 1)
	{
		vLayout->addWidget(makeCaption(tr("Skin")));
		modelSkinCombo_ = makeCombo();
		for (int i = 0; i < skinCount; ++i)
			modelSkinCombo_->addItem(tr("Skin %1").arg(i));
		connect(modelSkinCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &MainWindow::OnModelSkinChanged);
		vLayout->addWidget(modelSkinCombo_);
	}

	vLayout->addWidget(makeCaption(tr("Texture")));
	const auto &textures = currentModel_->Textures();
	if (!textures.empty())
	{
		modelTextureCombo_ = makeCombo();
		for (size_t i = 0; i < textures.size(); ++i)
		{
			modelTextureCombo_->addItem(textures[i].name.empty()
				? tr("Texture %1").arg(i)
				: DecodeModelText(textures[i].name));
		}

		modelTexturePreview_ = new QLabel;
		modelTexturePreview_->setAlignment(Qt::AlignCenter);
		modelTexturePreview_->setMinimumHeight(140);
		modelTexturePreview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		modelTexturePreview_->setFrameShape(QFrame::Box);
		modelTexturePreview_->setBackgroundRole(QPalette::Dark);
		modelTexturePreview_->setAutoFillBackground(true);
		modelTexturePreview_->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(modelTexturePreview_, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &position)
			{
				QMenu menu(this);
				menu.addAction(tr("Export texture..."), this, &MainWindow::ExportModelTexture);
				menu.exec(modelTexturePreview_->mapToGlobal(position));
			});

		modelTextureInfoLabel_ = new WrappedFitLabel;
		modelTextureInfoLabel_->setWordWrap(true);
		modelTextureInfoLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

		modelForceAdditiveCheck_ = new QCheckBox(tr("Treat as additive (transparent)"));
		modelForceAdditiveCheck_->setToolTip(tr(
			"Reflects whether this texture is currently rendered additive\n"
			"(transparent black background): from the file's own flag, from\n"
			"the \"$0a_\"/\"$0b_\" name-prefix convention, or a manual choice.\n"
			"Toggle it to force either direction for a texture that still\n"
			"looks wrong either way -- there's no fully reliable way to tell\n"
			"an effect's black background apart from a texture that's just\n"
			"legitimately black (dark metal, etc.) automatically."));
		connect(modelForceAdditiveCheck_, &QCheckBox::toggled, this, &MainWindow::OnModelForceAdditiveToggled);

		connect(modelTextureCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &MainWindow::OnModelTextureChanged);

		vLayout->addWidget(modelTextureCombo_);
		vLayout->addWidget(modelTexturePreview_);
		vLayout->addWidget(modelTextureInfoLabel_);
		vLayout->addWidget(modelForceAdditiveCheck_);

		OnModelTextureChanged(0);
	}
	else
	{
		auto *noTextureLabel = new WrappedFitLabel(
			tr("No textures embedded in this .mdl \u2014 it expects external texture files, "
				"which is why it's rendering untextured."));
		noTextureLabel->setWordWrap(true);
		noTextureLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		vLayout->addWidget(noTextureLabel);
	}

	// External textures that no archive entry (or resolution pass) could
	// bind. Nothing was swapped in for these -- the model shows untextured --
	// so offer a manual folder picker as a fallback.
	QStringList missingTextures;
	for (size_t i = 0; i < textures.size(); ++i)
	{
		if (TextureLooksExternal(textures[static_cast<size_t>(i)]) &&
			(static_cast<size_t>(i) >= externalTextureSources_.size() ||
				externalTextureSources_[static_cast<size_t>(i)].isEmpty()))
		{
			missingTextures << tr("\u2022 %1").arg(DecodeModelText(textures[static_cast<size_t>(i)].name));
		}
	}
	if (!missingTextures.isEmpty())
	{
		vLayout->addWidget(makeCaption(tr("These textures weren't found in this archive:")));
		auto *missingLabel = new ElidedListLabel;
		missingLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		missingLabel->setListText(missingTextures.join(QLatin1Char('\n')));
		vLayout->addWidget(missingLabel);

		auto *loadFolderButton = new QPushButton(tr("Load textures from folder\u2026"));
		connect(loadFolderButton, &QPushButton::clicked, this, &MainWindow::LoadTexturesFromFolder);
		vLayout->addWidget(loadFolderButton);
	}

	modelWireframeCheck_ = new QCheckBox(tr("Wireframe"));
	connect(modelWireframeCheck_, &QCheckBox::toggled, this, &MainWindow::OnModelWireframeToggled);
	vLayout->addWidget(modelWireframeCheck_);

	const int seqCount = modelView_->SequenceCount();
	if (seqCount > 0)
	{
		vLayout->addWidget(makeCaption(tr("Sequence")));
		modelSequenceCombo_ = makeCombo();
		modelSequenceCombo_->addItem(tr("(rest pose)"));
		for (int i = 0; i < seqCount; ++i)
			modelSequenceCombo_->addItem(DecodeModelText(modelView_->SequenceLabel(i)));
		modelSequenceCombo_->setCurrentIndex(modelView_->CurrentSequence() + 1);
		connect(modelSequenceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &MainWindow::OnModelSequenceChanged);
		vLayout->addWidget(modelSequenceCombo_);

		auto *frameRow = new QHBoxLayout;
		modelPlayButton_ = new QPushButton(tr("Play"));
		modelPlayButton_->setCheckable(true);
		connect(modelPlayButton_, &QPushButton::toggled, this, &MainWindow::OnModelPlayToggled);
		modelFrameSlider_ = new QSlider(Qt::Horizontal);
		modelFrameSlider_->setRange(0, 0);
		connect(modelFrameSlider_, &QSlider::valueChanged, this, &MainWindow::OnModelFrameSliderMoved);
		frameRow->addWidget(modelPlayButton_);
		frameRow->addWidget(modelFrameSlider_, 1);
		vLayout->addLayout(frameRow);

		modelFrameLabel_ = new QLabel(tr("Frame 0 / 0"));
		modelFrameLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		vLayout->addWidget(modelFrameLabel_);

		// The initial sequence is selected before currentIndexChanged is
		// connected, so initialize the frame controls explicitly. This matters
		// for v_ models, whose idle sequence is selected inside SetModel().
		OnModelSequenceChanged(modelSequenceCombo_->currentIndex());
	}

	vLayout->addStretch(1);
}

void MainWindow::OnModelSkinChanged(int index)
{
	modelView_->SetSkinFamily(index);
}

void MainWindow::OnModelTextureChanged(int index)
{
	if (!currentModel_ || modelTextureCombo_ == nullptr || modelTexturePreview_ == nullptr)
		return;

	const auto &textures = currentModel_->Textures();
	if (index < 0 || index >= static_cast<int>(textures.size()))
		return;

	const auto &tex = textures[static_cast<size_t>(index)];

	// The controls are built before this label has been laid out, so
	// width() here can still report Qt's default widget width (640), not the
	// ~240 the panel actually gives the thumbnail. Scaling the pixmap by that
	// blows the label's sizeHint past the panel edge and pushes every control
	// (combo boxes, buttons) off the right side of the window. Clamp to a
	// width that always fits the control panel instead.
	const int previewWidth = std::clamp(modelTexturePreview_->width() - 8, 120, 220);
	QPixmap pixmap = QPixmap::fromImage(tex.image).scaled(
		previewWidth, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	modelTexturePreview_->setPixmap(pixmap);

	// GoldSource studiomdl STUDIO_NF_* texture flags. Shown mainly so a
	// texture that renders as a solid black plane in this simple viewer
	// (no alpha blending beyond the "masked" cutout the parser already
	// applies) can be told apart from one that's genuinely just black --
	// e.g. a texture flagged "additive" is meant to be blended, not drawn
	// opaquely, which this viewer doesn't do; that's a likely explanation
	// for particle-effect meshes showing as solid black panels.
	QStringList flags;
	if (tex.flags & 0x1) flags << QStringLiteral("flatshade");
	if (tex.flags & 0x2) flags << QStringLiteral("chrome");
	if (tex.flags & 0x4) flags << QStringLiteral("fullbright");
	if (tex.flags & 0x10) flags << QStringLiteral("alpha");
	if (tex.flags & 0x20) flags << QStringLiteral("additive");
	if (tex.flags & 0x40) flags << QStringLiteral("masked");
	QString flagsText = flags.isEmpty() ? tr("none") : flags.join(QStringLiteral(", "));

	// Note when additive rendering is in effect for a reason other than the
	// file's own flag: the "$0a_"/"$0b_" name-prefix heuristic, or a manual
	// override (which can also force additive rendering *off* even when the
	// flag or prefix would otherwise turn it on).
	switch (modelView_->AdditiveSourceFor(index))
	{
	case cso_gui::ModelViewWidget::AdditiveSource::NamePrefix:
		flagsText += tr(" (treated as additive: name prefix)");
		break;
	case cso_gui::ModelViewWidget::AdditiveSource::ManualOverride:
		flagsText += tr(" (additive: manual override)");
		break;
	case cso_gui::ModelViewWidget::AdditiveSource::Flag:
	case cso_gui::ModelViewWidget::AdditiveSource::None:
		if (modelView_->HasManualAdditiveOverride(index))
			flagsText += tr(" (additive forced off: manual override)");
		break;
	}

	// Clarify where this texture's pixels came from: embedded in the .mdl,
	// bound from an external file, or still missing.
	QString origin = tr("embedded in .mdl");
	if (index >= 0 && index < static_cast<int>(externalTextureSources_.size()))
	{
		if (!externalTextureSources_[static_cast<size_t>(index)].isEmpty())
			origin = tr("external file: %1").arg(externalTextureSources_[static_cast<size_t>(index)]);
		else if (TextureLooksExternal(tex))
			origin = tr("external \u2014 not found");
	}

	modelTextureInfoLabel_->setText(tr("%1\u00d7%2 \u2014 flags: %3\n%4")
		.arg(tex.width).arg(tex.height).arg(flagsText).arg(origin));

	if (modelForceAdditiveCheck_ != nullptr)
	{
		modelForceAdditiveCheck_->blockSignals(true);
		modelForceAdditiveCheck_->setChecked(modelView_->IsTextureAdditive(index));
		modelForceAdditiveCheck_->blockSignals(false);
	}
}

void MainWindow::ExportModelTexture()
{
	if (!currentModel_ || modelTextureCombo_ == nullptr)
		return;

	const int index = modelTextureCombo_->currentIndex();
	const auto &textures = currentModel_->Textures();
	if (index < 0 || index >= static_cast<int>(textures.size()))
		return;

	const auto &texture = textures[static_cast<size_t>(index)];
	if (texture.image.isNull())
	{
		statusBar()->showMessage(tr("The selected texture has no image data."), 5000);
		return;
	}

	QString textureName = texture.name.empty()
		? tr("texture_%1").arg(index)
		: DecodeModelText(texture.name);
	// '$' and '#' are meaningful CSO texture-name prefixes (for example
	// $0a_/$0b_ additive textures and #... external textures), and both are
	// valid in Windows file names. Preserve them so exported files can still
	// be matched by their original MDL texture names.
	textureName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._#$-]+")), QStringLiteral("_"));
	if (textureName.isEmpty())
		textureName = tr("texture_%1").arg(index);
	if (QFileInfo(textureName).suffix().isEmpty())
		textureName += QStringLiteral(".bmp");

	QString selectedFilter = tr("Bitmap image (*.bmp)");
	QString path = QFileDialog::getSaveFileName(this, tr("Export texture"), textureName,
		tr("Bitmap image (*.bmp);;PNG image (*.png);;JPEG image (*.jpg *.jpeg)"),
		&selectedFilter);
	if (path.isEmpty())
		return;

	QString extension;
	QByteArray format;
	if (selectedFilter.startsWith(tr("PNG image")))
	{
		extension = QStringLiteral(".png");
		format = "PNG";
	}
	else if (selectedFilter.startsWith(tr("JPEG image")))
	{
		extension = QStringLiteral(".jpg");
		format = "JPG";
	}
	else
	{
		extension = QStringLiteral(".bmp");
		format = "BMP";
	}

	// QFileDialog can leave the original extension in the returned path when
	// the user changes the selected filter. Replace it explicitly so the file
	// name and the actual image codec always agree.
	const QFileInfo selectedPath(path);
	path = QDir(selectedPath.path()).filePath(selectedPath.completeBaseName() + extension);
	if (!texture.image.save(path, format.constData()))
	{
		QMessageBox::warning(this, tr("Export texture"),
			tr("Failed to save the texture to:\n%1").arg(path));
		return;
	}

	statusBar()->showMessage(tr("Texture saved to %1").arg(path), 5000);
}

void MainWindow::OnModelForceAdditiveToggled(bool checked)
{
	if (modelTextureCombo_ == nullptr)
		return;

	modelView_->SetTextureForceAdditive(modelTextureCombo_->currentIndex(), checked);
}

void MainWindow::OnModelWireframeToggled(bool enabled)
{
	modelView_->SetWireframe(enabled);
}

void MainWindow::OnLightAnglesChanged()
{
	modelView_->SetLightAngles(static_cast<float>(lightYawSlider_->value()),
		static_cast<float>(lightPitchSlider_->value()));
}

void MainWindow::OnResetLightClicked()
{
	// blockSignals so we don't fire OnLightAnglesChanged twice (once per
	// slider) mid-reset; apply the actual reset once at the end instead.
	lightYawSlider_->blockSignals(true);
	lightPitchSlider_->blockSignals(true);
	lightYawSlider_->setValue(0);
	lightPitchSlider_->setValue(0);
	lightYawSlider_->blockSignals(false);
	lightPitchSlider_->blockSignals(false);

	modelView_->SetLightAngles(0.0f, 0.0f);
}

void MainWindow::OnModelSequenceChanged(int index)
{
	StopModelPlayback();

	const int sequence = index - 1; // combo index 0 = "(rest pose)" = sequence -1
	modelView_->SetSequence(sequence);

	const int frames = modelView_->CurrentSequenceFrames();
	if (modelFrameSlider_ != nullptr)
	{
		modelFrameSlider_->setRange(0, std::max(frames - 1, 0));
		modelFrameSlider_->setValue(0);
	}
	if (modelFrameLabel_ != nullptr)
		modelFrameLabel_->setText(tr("Frame 0 / %1").arg(frames));
}

void MainWindow::OnModelFrameSliderMoved(int frame)
{
	modelView_->SetSequenceFrame(frame);
	if (modelFrameLabel_ != nullptr)
		modelFrameLabel_->setText(tr("Frame %1 / %2").arg(frame).arg(modelView_->CurrentSequenceFrames()));
}

void MainWindow::OnModelPlayToggled(bool play)
{
	if (!play)
	{
		modelAnimTimer_->stop();
		modelPlayButton_->setText(tr("Play"));
		return;
	}

	// Play back at the sequence's own fps when known, else a sane default.
	float fps = 30.0f;
	const int seqIndex = modelView_->CurrentSequence();
	if (currentModel_ && seqIndex >= 0 && seqIndex < static_cast<int>(currentModel_->Sequences().size()))
		fps = currentModel_->Sequences()[static_cast<size_t>(seqIndex)].fps;
	if (fps <= 0.0f)
		fps = 30.0f;

	modelAnimationStartFrame_ = modelView_->CurrentSequenceFrame();
	modelAnimationClock_.restart();
	modelAnimTimer_->start(16);
	modelPlayButton_->setText(tr("Pause"));
}

void MainWindow::OnModelAnimationTick()
{
	if (modelFrameSlider_ == nullptr)
		return;

	const int frames = modelView_->CurrentSequenceFrames();
	if (frames <= 0 || !currentModel_)
		return;

	const int sequence = modelView_->CurrentSequence();
	if (sequence < 0 || sequence >= static_cast<int>(currentModel_->Sequences().size()))
		return;

	const float fps = std::max(currentModel_->Sequences()[static_cast<size_t>(sequence)].fps, 1.0f);
	// The final Studio frame duplicates the first pose and closes the loop.
	// Match GoldSource/HLAM by wrapping over NumFrames - 1.
	const int cycleFrames = std::max(frames - 1, 1);
	const float elapsedFrame = modelAnimationStartFrame_
		+ static_cast<float>(modelAnimationClock_.elapsed()) * fps / 1000.0f;
	const float frame = std::fmod(elapsedFrame, static_cast<float>(cycleFrames));
	modelView_->SetSequenceFrame(frame);

	const int displayFrame = static_cast<int>(std::floor(frame)) % cycleFrames;
	modelFrameSlider_->blockSignals(true);
	modelFrameSlider_->setValue(displayFrame);
	modelFrameSlider_->blockSignals(false);
	if (modelFrameLabel_ != nullptr)
		modelFrameLabel_->setText(tr("Frame %1 / %2").arg(displayFrame).arg(frames));
}

void MainWindow::ShowSprite(const cso_pak::PakArchive::Entry &entry, const std::vector<uint8_t> &data)
{
	std::shared_ptr<cso_gui::SpriteImage> sprite;
	try
	{
		sprite = cso_gui::LoadSpriteImage(data);
	}
	catch (const std::exception &ex)
	{
		currentSprite_.reset();
		ShowProperties(entry, tr("Failed to parse this .spr sprite: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	currentSprite_ = sprite;

	spriteFrameSlider_->setRange(0, static_cast<int>(sprite->frames.size()) - 1);
	spriteFrameSlider_->setValue(0);
	UpdateSpriteFrameDisplay();

	stack_->setCurrentWidget(spritePageContainer_);

	// Autoplay multi-frame sprites like a GIF; a single-frame sprite is just
	// a static image, so there's nothing to play.
	const bool animated = sprite->frames.size() > 1;
	spritePlayButton_->setEnabled(animated);
	spritePlayButton_->setChecked(animated);
}

void MainWindow::StopSpritePlayback()
{
	if (spriteAnimTimer_ != nullptr)
		spriteAnimTimer_->stop();
	if (spritePlayButton_ != nullptr)
		spritePlayButton_->setChecked(false);
}

void MainWindow::UpdateSpriteFrameDisplay()
{
	if (!currentSprite_ || currentSprite_->frames.empty())
		return;

	const int index = spriteFrameSlider_->value();
	const auto &frame = currentSprite_->frames[static_cast<size_t>(index)];

	QPixmap pixmap = QPixmap::fromImage(frame.image);
	if (spriteFitToWindowCheck_->isChecked())
	{
		const QSize viewportSize = spriteScrollArea_->viewport()->size();
		if (viewportSize.width() > 0 && viewportSize.height() > 0)
			pixmap = pixmap.scaled(viewportSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	spriteImageLabel_->setPixmap(pixmap);
	spriteImageLabel_->resize(pixmap.size());

	spriteFrameLabel_->setText(tr("Frame %1 / %2 (%3\u00d7%4, %5 ms)")
		.arg(index + 1)
		.arg(currentSprite_->frames.size())
		.arg(frame.image.width())
		.arg(frame.image.height())
		.arg(static_cast<int>(frame.interval * 1000.0f)));
}

void MainWindow::OnSpriteFrameSliderMoved(int /*frame*/)
{
	UpdateSpriteFrameDisplay();
}

void MainWindow::OnSpriteFitToWindowToggled(bool /*checked*/)
{
	UpdateSpriteFrameDisplay();
}

void MainWindow::OnSpritePlayToggled(bool play)
{
	if (!play || !currentSprite_ || currentSprite_->frames.size() <= 1)
	{
		spriteAnimTimer_->stop();
		spritePlayButton_->setText(tr("Play"));
		return;
	}

	spritePlayButton_->setText(tr("Pause"));

	// Sprite frames each carry their own display duration (unlike model
	// sequences, which play at one fixed fps), so arm the timer with the
	// CURRENT frame's interval; OnSpriteAnimationTick re-arms it with the
	// next frame's interval each time it fires.
	const int index = spriteFrameSlider_->value();
	const float interval = currentSprite_->frames[static_cast<size_t>(index)].interval;
	spriteAnimTimer_->start(std::max(10, static_cast<int>(interval * 1000.0f)));
}

void MainWindow::OnSpriteAnimationTick()
{
	if (!currentSprite_ || currentSprite_->frames.empty())
		return;

	int next = spriteFrameSlider_->value() + 1;
	if (next >= static_cast<int>(currentSprite_->frames.size()))
		next = 0;

	spriteFrameSlider_->setValue(next); // triggers OnSpriteFrameSliderMoved via valueChanged

	const float interval = currentSprite_->frames[static_cast<size_t>(next)].interval;
	spriteAnimTimer_->start(std::max(10, static_cast<int>(interval * 1000.0f)));
}

void MainWindow::ShowGenericProperties(const QList<QPair<QString, QString>> &rows)
{
	propertiesPage_->setRowCount(0);

	for (const auto &row : rows)
	{
		const int r = propertiesPage_->rowCount();
		propertiesPage_->insertRow(r);
		propertiesPage_->setItem(r, 0, new QTableWidgetItem(row.first));
		propertiesPage_->setItem(r, 1, new QTableWidgetItem(row.second));
	}

	stack_->setCurrentWidget(propertiesPage_);
}

void MainWindow::ShowProperties(const cso_pak::PakArchive::Entry &entry, const QString &note)
{
	QList<QPair<QString, QString>> rows;
	const auto addRow = [&rows](const QString &property, const QString &value)
	{
		rows.append({ property, value });
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

	ShowGenericProperties(rows);
}

void MainWindow::ShowWadLumpProperties(const cso_gui::WadEntry &lump, const QString &note)
{
	QList<QPair<QString, QString>> rows;
	rows.append({ tr("Name"), QString::fromStdString(lump.name) });
	rows.append({ tr("Size"), FormatSize(lump.diskSize) });
	rows.append({ tr("Type byte"), QStringLiteral("0x%1").arg(lump.type, 2, 16, QLatin1Char('0')).toUpper() });
	rows.append({ tr("Compressed"), lump.compressed ? tr("yes (unsupported)") : tr("no") });
	if (!note.isEmpty())
		rows.append({ tr("Note"), note });

	ShowGenericProperties(rows);
}

void MainWindow::ShowWadLumpPreview(int wadArchiveIndex, int wadEntryIndex)
{
	StopMediaPlayback();
	StopModelPlayback();
	StopSpritePlayback();

	const cso_gui::Wad3Archive *wad = model_->WadArchiveAt(wadArchiveIndex);
	if (wad == nullptr || wadEntryIndex < 0 || wadEntryIndex >= static_cast<int>(wad->Entries().size()))
	{
		ShowPlaceholder(tr("Select a file in the tree to preview it."));
		return;
	}

	const auto &lump = wad->Entries()[static_cast<size_t>(wadEntryIndex)];

	std::vector<uint8_t> data;
	try
	{
		data = wad->ExtractEntry(lump);
	}
	catch (const std::exception &ex)
	{
		ShowWadLumpProperties(lump, tr("Failed to extract this WAD entry: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	// There's no reliable type-byte value to gate this on (see the comment
	// on LoadWadMiptex), so just try decoding it as a miptex texture and
	// fall back to properties if the data doesn't have that shape.
	try
	{
		const QImage image = cso_gui::LoadWadMiptex(data, lump.name);
		ShowImage(image);
		return;
	}
	catch (const std::exception &)
	{
	}

	ShowWadLumpProperties(lump, tr("Not a recognized texture (miptex) lump."));
}

void MainWindow::ShowPreviewForEntry(const cso_pak::PakArchive::Entry &entry)
{
	StopMediaPlayback();
	StopModelPlayback();
	StopSpritePlayback();

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

	if (ext == QLatin1String("dds"))
	{
		try
		{
			ShowImage(cso_gui::LoadDdsImage(data));
			return;
		}
		catch (const std::exception &ex)
		{
			ShowProperties(entry, tr("Failed to decode DDS image: %1").arg(QString::fromUtf8(ex.what())));
			return;
		}
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

	if (ext == QLatin1String("mdl"))
	{
		ShowModel(entry, data);
		return;
	}

	if (ext == QLatin1String("spr"))
	{
		ShowSprite(entry, data);
		return;
	}

	if (ext == QLatin1String("wad"))
	{
		try
		{
			const auto wad = cso_gui::Wad3Archive::Load(data);
			QList<QPair<QString, QString>> rows;
			rows.append({ tr("Format"), wad.IsWad3() ? tr("WAD3 (Half-Life/GoldSource)") : tr("WAD2 (Quake)") });
			rows.append({ tr("Entries"), QString::number(wad.Entries().size()) });
			rows.append({ tr("Note"), tr("Expand this file in the tree (arrow on the left) to browse and preview its lumps individually.") });
			ShowGenericProperties(rows);
		}
		catch (const std::exception &ex)
		{
			ShowProperties(entry, tr("Failed to parse this WAD archive: %1").arg(QString::fromUtf8(ex.what())));
		}
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

	// Any other binary format with no dedicated preview: just properties.
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

#ifdef _WIN32
	// Not valid UTF-8: this covers plain .txt/.csv files with Korean text
	// too, not just decrypted .cso tables -- same EUC-KR/CP949 check used
	// there and for model bodypart/sequence names.
	if (cso_gui::LooksLikeEucKr(data))
		return QString::fromStdString(cso_gui::DecodeEucKrToUtf8(data));
#endif

	// Fall back to Latin-1, which never fails, so something is always shown.
	// If the archive's un-BOM'd text turns out to be Windows-1251 (or
	// another 8-bit code page) instead, add the "Qt6 Core5Compat" module
	// and decode with QTextCodec::codecForName("Windows-1251") here.
	return QString::fromLatin1(bytes, static_cast<int>(data.size()));
}

QString MainWindow::DecodeModelText(const std::string &text)
{
	// StudioModel stores bodypart/submodel/sequence names as raw bytes with
	// no encoding decisions of its own -- CSO's own animation/model names
	// are often Korean (EUC-KR/CP949), which isn't valid UTF-8 and shows up
	// as black diamond replacement characters if just handed to QString
	// as-is. Same detect-and-convert approach used for .cso table text.
#ifdef _WIN32
	const std::vector<uint8_t> bytes(text.begin(), text.end());
	if (cso_gui::LooksLikeEucKr(bytes))
		return QString::fromStdString(cso_gui::DecodeEucKrToUtf8(bytes));
#endif

	// Not Korean (or EUC-KR detection isn't available on this platform):
	// most of these names are plain ASCII, but use it if it's valid UTF-8
	// too, and only fall back to Latin-1 as a last resort.
	QStringDecoder utf8Decoder(QStringDecoder::Utf8);
	QString decoded = utf8Decoder(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())));
	if (!utf8Decoder.hasError())
		return decoded;

	return QString::fromLatin1(text.data(), static_cast<int>(text.size()));
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
