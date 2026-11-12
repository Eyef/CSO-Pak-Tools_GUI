#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>

#include <QLabel>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QByteArrayView>
#include <QMatrix4x4>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
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
	// CSO marks "load this from outside the .mdl" textures with a leading '#'
	// (e.g. "#256256hand_p.bmp"), but the file itself is stored without it.
	// Strip the marker before comparing names, on both sides, so pak entries
	// and folder files match regardless of which form they use.
	std::string StripTextureNamePrefix(std::string name)
	{
		while (!name.empty() && name.front() == '#')
			name.erase(name.begin());
		return name;
	}

	// Optional "textures" folder shipped next to the executable (e.g. with
	// hand textures extracted from elsewhere). Used automatically as a last
	// fallback, so bundled textures work out of the box with no setup.
	QString BundledTextureFolder()
	{
		const QString dir =
			QCoreApplication::applicationDirPath() + QStringLiteral("/textures");
		return QDir(dir).exists() ? dir : QString();
	}

	// Last directory used in an open-file/folder dialog, persisted across
	// restarts so the next launch starts where the user left off instead
	// of at the program folder.
	QString LastOpenDirectory()
	{
		const QSettings settings;
		return settings.value(QStringLiteral("lastOpenDir")).toString();
	}

	void SaveLastOpenDirectory(const QString &dir)
	{
		if (dir.isEmpty())
			return;
		QSettings settings;
		settings.setValue(QStringLiteral("lastOpenDir"), dir);
	}

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
		PageBsp,
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

	// Restore the remembered texture folder, if it still exists.
	const QSettings settings;
	const QString savedFolder = settings.value(QStringLiteral("textureFolder")).toString();
	if (!savedFolder.isEmpty() && QDir(savedFolder).exists())
		textureFolder_ = savedFolder;

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

	searchEdit_ = new QLineEdit;
	searchEdit_->setPlaceholderText(tr("Find file in all loaded archives (Ctrl+F)"));
	searchEdit_->setClearButtonEnabled(true);
	searchEdit_->setMinimumWidth(280);
	searchEdit_->setToolTip(tr(
		"Case-insensitive substring search over the full internal path of "
		"every entry in every loaded .pak and .nar archive (including "
		"auto-loaded NARs hidden from the tree). Use * and ? as wildcards. "
		"Press Enter to search."));
	connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::RunSearch);

	searchResults_ = new QTableWidget(0, 4);
	searchResults_->setHorizontalHeaderLabels(
		{ tr("Name"), tr("Path"), tr("Archive"), tr("Type") });
	searchResults_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	searchResults_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	searchResults_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	searchResults_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	searchResults_->verticalHeader()->setVisible(false);
	searchResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	searchResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
	searchResults_->setSelectionMode(QAbstractItemView::SingleSelection);
	searchResults_->setAlternatingRowColors(true);
	searchResults_->setWordWrap(false);
	connect(searchResults_, &QTableWidget::currentCellChanged,
		this, &MainWindow::OnSearchResultChanged);
	connect(searchResults_, &QTableWidget::cellDoubleClicked,
		this, &MainWindow::OnSearchResultDoubleClicked);

	searchDock_ = new QDockWidget(tr("Search results"), this);
	searchDock_->setObjectName(QStringLiteral("searchResultsDock"));
	searchDock_->setAllowedAreas(Qt::BottomDockWidgetArea);
	searchDock_->setWidget(searchResults_);
	addDockWidget(Qt::BottomDockWidgetArea, searchDock_);
	searchDock_->hide();

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

	// BSP map page: 3D view on the left, small info panel on the right.
	// Unlike the model page, the right side is static (built once) -- map
	// stats just update their text per file.
	bspView_ = new cso_gui::BspViewWidget;
	bspView_->setMinimumSize(300, 300);

	bspInfoLabel_ = new QLabel;
	bspInfoLabel_->setWordWrap(true);
	bspInfoLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	bspInfoLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	bspWireframeCheck_ = new QCheckBox(tr("Wireframe"));
	auto *bspPulseCheck = new QCheckBox(tr("Pulse (renderfx)"));
	bspPulseCheck->setToolTip(tr("Pulsing brightness of glowing signs (renderfx 1-4)"));
	bspPulseCheck->setChecked(false);
	connect(bspPulseCheck, &QCheckBox::toggled, this,
		[this](bool enabled) { bspView_->SetPulseEnabled(enabled); });
	connect(bspWireframeCheck_, &QCheckBox::toggled, this, &MainWindow::OnBspWireframeToggled);

	auto *bspResetViewButton = new QPushButton(tr("Reset View (camera)"));
	connect(bspResetViewButton, &QPushButton::clicked, this, [this]() { bspView_->ResetView(); });

	auto *bspCameraCombo = new QComboBox;
	bspCameraCombo->addItem(tr("Orbit"));
	bspCameraCombo->addItem(tr("Fly (WASD)"));
	connect(bspCameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int index)
		{
			bspView_->SetCameraMode(index == 1
				? cso_gui::BspViewWidget::CameraMode::Fly
				: cso_gui::BspViewWidget::CameraMode::Orbit);
		});

	auto *bspLightLabel = new QLabel(tr("Lighting:"));
	auto *bspLightCombo = new QComboBox;
	bspLightCombo->addItem(tr("Game-fitted (default)"));
	bspLightCombo->addItem(tr("Plain (like goldsrc-bsp-viewer)"));
	connect(bspLightCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int index) { bspView_->SetLightingMode(index); });

	auto *bspBrightLabel = new QLabel(tr("Map brightness: %1%").arg(100));
	auto *bspBrightSlider = new QSlider(Qt::Horizontal);
	bspBrightSlider->setRange(30, 300);
	bspBrightSlider->setValue(100);
	connect(bspBrightSlider, &QSlider::valueChanged, this,
		[this, bspBrightLabel](int value)
		{
			bspBrightLabel->setText(tr("Map brightness: %1%").arg(value));
			bspView_->SetBrightness(value / 100.0f);
		});

	auto *bspFillLabel = new QLabel(tr("Model light boost: %1%").arg(50));
	auto *bspFillSlider = new QSlider(Qt::Horizontal);
	bspFillSlider->setRange(0, 100);
	bspFillSlider->setValue(50);
	connect(bspFillSlider, &QSlider::valueChanged, this,
		[this, bspFillLabel](int value)
		{
			bspFillLabel->setText(tr("Model light boost: %1%").arg(value));
			bspView_->SetModelFill(value / 100.0f);
		});

	auto *bspFovLabel = new QLabel(tr("FOV: %1°").arg(60));
	auto *bspFovSlider = new QSlider(Qt::Horizontal);
	bspFovSlider->setRange(30, 120);
	bspFovSlider->setValue(60);
	connect(bspFovSlider, &QSlider::valueChanged, this,
		[this, bspFovLabel](int value)
		{
			bspFovLabel->setText(tr("FOV: %1°").arg(value));
			bspView_->SetFieldOfView(static_cast<float>(value));
		});

	auto *bspFlyHint = new QLabel(tr("Fly: drag to look, W/A/S/D to move (any keyboard layout), Q/E or C/Space down/up, wheel dolly, Shift = fast."));
	bspFlyHint->setWordWrap(true);
	bspFlyHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	auto *bspRightPanel = new QWidget;
	auto *bspRightPanelLayout = new QVBoxLayout(bspRightPanel);
	bspRightPanelLayout->setContentsMargins(0, 0, 0, 0);
	bspRightPanelLayout->addWidget(bspResetViewButton);
	bspRightPanelLayout->addWidget(bspCameraCombo);
	// The "Plain (like goldsrc-bsp-viewer)" switch is disabled: the
	// "Model light boost" slider reproduces it. To bring it back, set
	// kShowLightingModeSwitch to true (the shader still supports it).
	constexpr bool kShowLightingModeSwitch = false;
	if (kShowLightingModeSwitch)
	{
		bspRightPanelLayout->addWidget(bspLightLabel);
		bspRightPanelLayout->addWidget(bspLightCombo);
	}
	else
	{
		delete bspLightLabel;
		delete bspLightCombo;
	}
	bspRightPanelLayout->addWidget(bspBrightLabel);
	bspRightPanelLayout->addWidget(bspBrightSlider);
	bspRightPanelLayout->addWidget(bspFillLabel);
	bspRightPanelLayout->addWidget(bspFillSlider);
	bspRightPanelLayout->addWidget(bspFovLabel);
	bspRightPanelLayout->addWidget(bspFovSlider);
	bspRightPanelLayout->addWidget(bspFlyHint);
	bspRightPanelLayout->addWidget(bspWireframeCheck_);
	bspRightPanelLayout->addWidget(bspPulseCheck);
	bspRightPanelLayout->addWidget(bspInfoLabel_, 1);

	auto *bspControlsScroll = new QScrollArea;
	bspControlsScroll->setWidget(bspRightPanel);
	bspControlsScroll->setWidgetResizable(true);
	bspControlsScroll->setMinimumWidth(240);
	bspControlsScroll->setMaximumWidth(340);
	bspControlsScroll->setFrameShape(QFrame::NoFrame);

	auto *bspSplitter = new QSplitter(Qt::Horizontal);
	bspSplitter->addWidget(bspView_);
	bspSplitter->addWidget(bspControlsScroll);
	bspSplitter->setStretchFactor(0, 1);
	bspSplitter->setStretchFactor(1, 0);
	bspPageContainer_ = bspSplitter;

	stack_->addWidget(placeholderPage_);   // PagePlaceholder
	stack_->addWidget(textPage_);          // PageText
	stack_->addWidget(imagePageContainer_);// PageImage
	stack_->addWidget(csvPage_);           // PageCsv
	stack_->addWidget(propertiesPage_);    // PageProperties
	stack_->addWidget(mediaPageContainer_);// PageMedia
	stack_->addWidget(modelPageContainer_);// PageModel
	stack_->addWidget(spritePageContainer_);// PageSprite
	stack_->addWidget(bspPageContainer_);  // PageBsp

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

	auto *openAction = fileMenu->addAction(tr("&Open Archive..."), this, &MainWindow::OnOpenPak);
	openAction->setShortcut(QKeySequence::Open);

	fileMenu->addAction(tr("Open &Multiple Archives..."), this, &MainWindow::OnOpenPaks);
	fileMenu->addAction(tr("Open Pa&ks + All Nars..."), this, &MainWindow::OnOpenPaksWithNars);
	fileMenu->addAction(tr("Open Archives &Folder..."), this, &MainWindow::OnOpenPakFolder);

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
	fileMenu->addAction(tr("Pack Directory into New &NAR..."), this, &MainWindow::OnPackNar);
	fileMenu->addAction(tr("Patch NA&R with Replacements..."), this, &MainWindow::OnPatchNar);
	fileMenu->addAction(tr("&Unpack NAR to Directory..."), this, &MainWindow::OnUnpackNar);
	verifyNarAction_ = fileMenu->addAction(tr("Verify NAR Archive &Integrity..."),
		this, &MainWindow::OnVerifyNarIntegrity);
	verifyNarAction_->setEnabled(false);

	fileMenu->addSeparator();
	fileMenu->addAction(tr("E&xit"), this, &QWidget::close);

	auto *viewMenu = menuBar()->addMenu(tr("&View"));
	auto *findAction = viewMenu->addAction(tr("&Find File..."), this, &MainWindow::FocusSearch);
	findAction->setShortcut(QKeySequence::Find);
	findAction->setToolTip(tr(
		"Search every loaded .pak/.nar archive by file name or internal path "
		"(Ctrl+F)."));
	viewMenu->addSeparator();
	showAutoNarAction_ = viewMenu->addAction(tr("Show auto-loaded NAR archives"));
	showAutoNarAction_->setCheckable(true);
	showAutoNarAction_->setToolTip(tr(
		"When .pak files are opened, their neighbouring .nar archives are "
		"loaded silently so models and textures resolve. Check this to also "
		"show those archives in the tree."));
	{
		const QSettings settings;
		showAutoNar_ = settings.value(QStringLiteral("showAutoNar"), false).toBool();
	}
	showAutoNarAction_->setChecked(showAutoNar_);
	connect(showAutoNarAction_, &QAction::toggled, this, &MainWindow::OnShowAutoNarToggled);

	searchNeighborSeqAction_ = viewMenu->addAction(
		tr("Search neighbouring archives for external animations (.seq)"));
	searchNeighborSeqAction_->setCheckable(true);
	searchNeighborSeqAction_->setToolTip(tr(
		"Some animations of a model are stored outside the .mdl, in files "
		"named <model><N>.seq that may lie in another .pak/.nar than the model. "
		"On: the .pak/.nar files next to the loaded ones are opened once and "
		"searched too (slow with hundreds of archives). Off (default): only the "
		"archives loaded right now are searched. "
		"Applies to the next model you open."));
	{
		const QSettings settings;
		searchNeighborSeq_ = settings.value(QStringLiteral("searchNeighborSeq"), false).toBool();
	}
	searchNeighborSeqAction_->setChecked(searchNeighborSeq_);
	connect(searchNeighborSeqAction_, &QAction::toggled, this,
		&MainWindow::OnSearchNeighborSeqToggled);

	// BSP viewer: skybox on/off. Off by default -- the sky faces enclose
	// parts of the level (rooftops, grates, side walls) and hide the map when
	// browsing it from outside; the sky is mainly useful for a game-like look.
	auto *showSkyAction = viewMenu->addAction(tr("Show skybox in BSP viewer"));
	showSkyAction->setCheckable(true);
	showSkyAction->setChecked(false);
	showSkyAction->setToolTip(tr(
		"Draw the map's skybox through its \"sky\" faces, as the game does. "
		"Off by default: those faces can cover parts of the map when viewed "
		"from outside."));
	connect(showSkyAction, &QAction::toggled, this,
		[this](bool on) { bspView_->SetSkyVisible(on); });

	// BSP viewer: game-like visibility of placed models/sprites. Off by
	// default (browsing wants to see everything).
	auto *pvsAction = viewMenu->addAction(tr("Hide map parts and models outside the PVS (as in game)"));
	pvsAction->setCheckable(true);
	pvsAction->setChecked(false);
	pvsAction->setToolTip(tr(
		"Draw map faces, placed models and sprites only when the game would "
		"send them to a player standing at the camera (their leaf is in the "
		"camera leaf's PVS). Behind a grate of a sealed room nothing is drawn, "
		"as in game. Works with the fly camera inside the map; with the camera "
		"outside everything stays visible."));
	connect(pvsAction, &QAction::toggled, this,
		[this](bool on) { bspView_->SetPvsCulling(on); });
	// Permanent status-bar label: showMessage() text is wiped by every menu
	// hover (QMenu sends an empty status tip), so the camera/PVS line used to
	// vanish as soon as the mouse touched File or View.
	auto *pvsStatusLabel = new QLabel(this);
	pvsStatusLabel->setObjectName(QStringLiteral("pvsStatusLabel"));
	pvsStatusLabel->hide();
	statusBar()->addPermanentWidget(pvsStatusLabel);
	connect(bspView_, &cso_gui::BspViewWidget::PvsStatus, this,
		[pvsStatusLabel](const QString &text)
		{
			// Empty = PVS option off / no map: show nothing at all.
			pvsStatusLabel->setText(text);
			pvsStatusLabel->setVisible(!text.isEmpty());
		});

	// BSP viewer: GoldSrc draws every brush face from its front side only.
	// On by default; turn off to see both sides of every face.
	auto *backfaceAction = viewMenu->addAction(tr("Cull back faces in BSP viewer (as in game)"));
	backfaceAction->setCheckable(true);
	backfaceAction->setChecked(bspView_->BackfaceCulling());
	backfaceAction->setToolTip(tr(
		"Draw map faces only from their front side, like the game engine. "
		"Turn off to see both sides of every face (shows one-sided faces that "
		"hang in the air, or helps if a face looks wrongly missing)."));
	connect(backfaceAction, &QAction::toggled, this,
		[this](bool on) { bspView_->SetBackfaceCulling(on); });

	// BSP viewer: maps with an env_fog entity (zs_lastride2) fade distant
	// geometry into the fog colour in game.
	auto *fogAction = viewMenu->addAction(tr("Apply env_fog in BSP viewer (as in game)"));
	fogAction->setCheckable(true);
	fogAction->setChecked(bspView_->FogEnabled());  // default off
	fogAction->setToolTip(tr(
		"Maps with an env_fog entity darken distant geometry in game. Turn off "
		"to see the map without fog. Has no effect on maps without env_fog."));
	connect(fogAction, &QAction::toggled, this,
		[this](bool on) { bspView_->SetFogEnabled(on); });

	// CSO model switchers (zs_obstacle1: intact antennas -> burnt ones, the boss
	// that appears with the cutscene). Default = state at map start.
	auto *afterSwitchAction = viewMenu->addAction(
		tr("Show map state after the scripted switch (cutscene, destroyed props)"));
	afterSwitchAction->setCheckable(true);
	afterSwitchAction->setChecked(false);
	afterSwitchAction->setToolTip(tr(
		"CSO maps swap some models with a trigger (cycler_mdl _mdl0 -> _mdl1): "
		"intact antennas become burnt ones, a hidden boss appears. Off shows the "
		"map as it is at the round start; on shows the models after the switch. "
		"Reloads the current map (the camera resets)."));
	connect(afterSwitchAction, &QAction::toggled, this,
		[this](bool on)
		{
			setProperty("bspAfterSwitch", on);
			// Re-preview the selected entry so ShowBsp rebuilds the placed models.
			if (currentBsp_ && treeView_->currentIndex().isValid())
				OnCurrentChanged(treeView_->currentIndex(), QModelIndex());
		});

	auto *toolbar = addToolBar(tr("Main"));
	toolbar->setMovable(false);
	toolbar->addAction(openAction);
	toolbar->addAction(extractAllAction_);
	toolbar->addAction(extractSelectedAction_);
	toolbar->addSeparator();
	toolbar->addWidget(searchEdit_);
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
	const QString path = QFileDialog::getOpenFileName(this, tr("Open Archive (.pak / .nar)"),
		LastOpenDirectory(), tr("Game Archives (*.pak *.nar);;All Files (*)"));
	if (path.isEmpty())
		return;

	SaveLastOpenDirectory(QFileInfo(path).absolutePath());
	LoadPak(path);
}

void MainWindow::OnOpenPaks()
{
	const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open Multiple Archives (.pak / .nar)"),
		LastOpenDirectory(), tr("Game Archives (*.pak *.nar);;All Files (*)"));
	if (paths.isEmpty())
		return;

	SaveLastOpenDirectory(QFileInfo(paths.front()).absolutePath());
	// Free choice: exactly what was selected, no silent additions.
	LoadPaks(paths, false);
}

void MainWindow::OnOpenPaksWithNars()
{
	const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open Paks (all NARs auto-load)"),
		LastOpenDirectory(), tr("Game Archives (*.pak *.nar);;All Files (*)"));
	if (paths.isEmpty())
		return;

	SaveLastOpenDirectory(QFileInfo(paths.front()).absolutePath());
	// Same as above, but neighbouring .nar archives are pulled in and
	// shown: the one-click setup for BSP maps with placed models.
	showAutoNar_ = true;
	if (showAutoNarAction_ != nullptr)
	{
		showAutoNarAction_->blockSignals(true);
		showAutoNarAction_->setChecked(true);
		showAutoNarAction_->blockSignals(false);
	}
	QSettings settings;
	settings.setValue(QStringLiteral("showAutoNar"), true);
	LoadPaks(paths, true);
}

void MainWindow::OnOpenPakFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(this,
		tr("Open Folder Containing Game Archives"), LastOpenDirectory());
	if (dir.isEmpty())
		return;

	SaveLastOpenDirectory(dir);
	QDir pakDir(dir);
	const QStringList names = pakDir.entryList(
		QStringList() << QStringLiteral("*.pak") << QStringLiteral("*.nar"),
		QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
	if (names.isEmpty())
	{
		QMessageBox::information(this, tr("No Game Archives"),
			tr("No .pak/.nar files were found in:\n%1").arg(dir));
		return;
	}

	QStringList paths;
	paths.reserve(names.size());
	std::set<QString> seenCanonical;
	for (const QString &name : names)
	{
		const QString full = pakDir.absoluteFilePath(name);
		if (seenCanonical.insert(QFileInfo(full).canonicalFilePath()).second)
			paths << full;
	}
	{
		const QDir parent = QFileInfo(dir).absoluteDir();
		bool dirHasNar = false;
		for (const QString &name : names)
		{
			if (QFileInfo(name).suffix().compare(
				QStringLiteral("nar"), Qt::CaseInsensitive) == 0)
			{
				dirHasNar = true;
				break;
			}
		}
		if (!dirHasNar && parent.absolutePath() != pakDir.absolutePath())
		{
			const QStringList narNames = parent.entryList(
				QStringList() << QStringLiteral("*.nar"),
				QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
			for (const QString &name : narNames)
			{
				const QString full = parent.absoluteFilePath(name);
				if (seenCanonical.insert(QFileInfo(full).canonicalFilePath()).second)
					paths << full;
			}
		}
	}

	LoadPaks(paths, true);
}

void MainWindow::LoadPak(const QString &path)
{
	// A single .pak still pulls neighbouring .nars (hidden) so its models
	// resolve; a single .nar loads exactly.
	const bool autoNar = QFileInfo(path).suffix().compare(
		QStringLiteral("nar"), Qt::CaseInsensitive) != 0;
	LoadPaks({ path }, autoNar);
	AddRecentFile(QFileInfo(path).absoluteFilePath());
}

void MainWindow::LoadPaks(const QStringList &paths, bool autoNar)
{
	if (paths.isEmpty())
		return;

	// Indexing hundreds of archives means gigabytes of sequential disk
	// reads, so show progress and allow cancelling partway through.
	QProgressDialog progress(tr("Loading game archives..."), tr("Cancel"), 0,
		paths.size(), this);
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(400);

	std::vector<std::unique_ptr<cso_pak::PakArchive>> loaded;
	loaded.reserve(static_cast<size_t>(paths.size()));
	std::vector<std::unique_ptr<cso_pak::NarArchive>> loadedNar;
	QStringList failed;

	for (int i = 0; i < paths.size(); ++i)
	{
		progress.setValue(i);
		progress.setLabelText(tr("Loading %1 (%2 of %3)...")
			.arg(QFileInfo(paths[i]).fileName()).arg(i + 1).arg(paths.size()));
		QApplication::processEvents();
		if (progress.wasCanceled())
			break;

		const bool isNar = QFileInfo(paths[i]).suffix().compare(
			QStringLiteral("nar"), Qt::CaseInsensitive) == 0;
		try
		{
			if (isNar)
			{
				loadedNar.push_back(std::make_unique<cso_pak::NarArchive>(
					cso_pak::NarArchive::Load(
						std::filesystem::path(paths[static_cast<int>(i)].toStdU16String()))));
			}
			else
			{
				loaded.push_back(std::make_unique<cso_pak::PakArchive>(
					cso_pak::PakArchive::Load(
						std::filesystem::path(paths[static_cast<int>(i)].toStdU16String()))));
			}
		}
		catch (const std::exception &ex)
		{
			failed << tr("%1: %2").arg(QFileInfo(paths[i]).fileName(),
				QString::fromUtf8(ex.what()));
		}
	}
	progress.setValue(paths.size());

	if (loaded.empty() && loadedNar.empty())
	{
		QMessageBox::critical(this, tr("Failed to open archives"),
			failed.isEmpty()
				? tr("No archives were loaded.")
				: tr("None of the selected archives could be loaded:\n%1").arg(failed.join(QLatin1Char('\n'))));
		return;
	}

	findCache_.clear();
	// Search hits point into the archives that are about to be replaced.
	searchHits_.clear();
	if (searchResults_ != nullptr)
	{
		const QSignalBlocker blocker(searchResults_);
		searchResults_->setRowCount(0);
	}
	if (searchDock_ != nullptr)
		searchDock_->hide();
	archives_ = std::move(loaded);
	narArchives_ = std::move(loadedNar);
	autoNarPaths_.clear();
	// CSO keeps Nexon .nar archives next to Packer/ (Data/*.nar) while maps
	// and most browser workflows open .pak files: most placed map models
	// live in .nar, so when asked, pull sibling .nar files in too (same
	// folder and its parent). They resolve textures/models but stay hidden
	// from the tree unless shown (see showAutoNar_). Loading a .nar only
	// parses its directory, file bytes stay on disk.
	if (autoNar && !archives_.empty())
	{
		std::set<QString> known;
		for (const auto &a : archives_)
			known.insert(QFileInfo(QString::fromStdU16String(
				a->SourcePath().generic_u16string())).canonicalFilePath());
		for (const auto &a : narArchives_)
			known.insert(QFileInfo(QString::fromStdString(
				a->SourcePath().generic_string())).canonicalFilePath());
		std::set<QString> narDirs;
		for (const auto &a : archives_)
		{
			const QString dir = QFileInfo(QString::fromStdU16String(
				a->SourcePath().generic_u16string())).absolutePath();
			narDirs.insert(dir);
			const QString parent = QFileInfo(dir).absoluteDir().absolutePath();
			if (parent != dir)
				narDirs.insert(parent);
		}
		for (const auto &a : narArchives_)
		{
			const QString dir = QFileInfo(QString::fromStdString(
				a->SourcePath().generic_string())).absolutePath();
			narDirs.insert(dir);
		}
		for (const QString &dir : narDirs)
		{
			const QStringList names = QDir(dir).entryList(
				QStringList() << QStringLiteral("*.nar"),
				QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
			for (const QString &name : names)
			{
				const QString full = QDir(dir).absoluteFilePath(name);
				if (!known.insert(QFileInfo(full).canonicalFilePath()).second)
					continue;
				try
				{
					narArchives_.push_back(std::make_unique<cso_pak::NarArchive>(
						cso_pak::NarArchive::Load(
							std::filesystem::path(full.toStdU16String()))));
					findCache_.clear();
					autoNarPaths_.insert(QFileInfo(full).canonicalFilePath());
					statusBar()->showMessage(tr("Loading %1...").arg(
						QFileInfo(full).fileName()));
					QApplication::processEvents();
				}
				catch (const std::exception &ex)
				{
					failed << tr("%1: %2").arg(QFileInfo(full).fileName(),
						QString::fromUtf8(ex.what()));
				}
			}
		}
	}
	if (!failed.isEmpty() && (archives_.empty() && narArchives_.empty()))
	{
		QMessageBox::critical(this, tr("Failed to open archives"),
			tr("None of the selected archives could be loaded:\n%1").arg(failed.join(QLatin1Char('\n'))));
		return;
	}
	currentModelPakIndex_ = -1;
	currentModelNarIndex_ = -1;
	// A new archive set invalidates everything resolved from elsewhere.
	findCache_.clear();
	neighborArchives_.clear();
	neighborNarArchives_.clear();
	neighborArchivesScanned_ = false;
	externalSeqInfo_.clear();
	externalSeqBase_.clear();
	wadImageCache_.clear();
	placedModelCache_.clear();

	RebuildTreeViews();
	treeView_->expandToDepth(0);

	extractAllAction_->setEnabled(true);
	extractSelectedAction_->setEnabled(true);
	extractSelectedDecodedAction_->setEnabled(true);
	verifyNarAction_->setEnabled(!narArchives_.empty());

	ShowPlaceholder(tr("Select a file in the tree to preview it."));

	const size_t totalEntries = TotalEntryCount();
	const size_t narCount = narArchives_.size();
	if (archives_.size() == 1 && narCount == 0)
	{
		const QString name = QFileInfo(QString::fromStdU16String(archives_.front()->SourcePath().generic_u16string())).fileName();
		setWindowTitle(tr("%1 \u2014 CS Online Pak Browser").arg(name));
		statusBar()->showMessage(tr("Loaded %1 (%2 entries)").arg(name).arg(totalEntries));
	}
	else if (archives_.empty())
	{
		setWindowTitle(tr("%1 nars \u2014 CS Online Pak Browser").arg(narCount));
		statusBar()->showMessage(tr("Loaded %1 nars (%2 entries total)").arg(narCount).arg(totalEntries));
	}
	else if (narCount == 0)
	{
		setWindowTitle(tr("%1 paks \u2014 CS Online Pak Browser").arg(archives_.size()));
		statusBar()->showMessage(tr("Loaded %1 paks (%2 entries total)").arg(archives_.size()).arg(totalEntries));
	}
	else
	{
		setWindowTitle(tr("%1 paks + %2 nars \u2014 CS Online Pak Browser").arg(archives_.size()).arg(narCount));
		statusBar()->showMessage(tr("Loaded %1 paks + %2 nars (%3 entries total)").arg(archives_.size()).arg(narCount).arg(totalEntries));
	}

	if (!failed.isEmpty())
		QMessageBox::warning(this, tr("Some archives failed to load"), failed.join(QLatin1Char('\n')));
}

void MainWindow::RebuildTreeViews()
{
	std::vector<const cso_pak::PakArchive*> views;
	views.reserve(archives_.size());
	for (const auto &archive : archives_)
		views.push_back(archive.get());
	std::vector<const cso_pak::NarArchive*> narViews;
	narViews.reserve(narArchives_.size());
	for (const auto &archive : narArchives_)
	{
		// Auto-loaded helpers stay out of the tree unless shown: opening
		// one .pak must not flood the view with .nar contents, while
		// resolution still uses every loaded archive either way.
		// Explicitly chosen .nar archives are always shown.
		if (showAutoNar_)
		{
			narViews.push_back(archive.get());
			continue;
		}
		if (autoNarPaths_.find(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).canonicalFilePath()) ==
			autoNarPaths_.end())
			narViews.push_back(archive.get());
	}
	model_->SetAllArchives(views, narViews);
}

void MainWindow::OnShowAutoNarToggled(bool show)
{
	showAutoNar_ = show;
	QSettings settings;
	settings.setValue(QStringLiteral("showAutoNar"), show);
	RebuildTreeViews();
	treeView_->expandToDepth(0);
}

void MainWindow::OnSearchNeighborSeqToggled(bool on)
{
	searchNeighborSeq_ = on;
	QSettings settings;
	settings.setValue(QStringLiteral("searchNeighborSeq"), on);
	statusBar()->showMessage(on
		? tr("External animations (.seq) will also be searched in neighbouring archives "
			"(applies to the next model you open).")
		: tr("External animations (.seq) will only be searched in the loaded archives "
			"(applies to the next model you open)."), 6000);
}

size_t MainWindow::TotalEntryCount() const
{
	size_t total = 0;
	for (const auto &archive : archives_)
		total += archive->Entries().size();
	for (const auto &archive : narArchives_)
		total += archive->Entries().size();
	return total;
}

namespace
{
	// (wad archive index, lump index) as PakTreeModel reports them.
	using WadLumpRef = std::pair<int, int>;

	QString WadTr(const char *text)
	{
		return QCoreApplication::translate("MainWindow", text);
	}

	// Windows-safe file name from a WAD lump name. WAD texture names contain
	// characters such as '*' (water), '+' (animations) or '{' (cut-outs).
	QString SafeLumpFileName(const std::string &name)
	{
		QString out = QString::fromLatin1(name.c_str());
		out.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
		out = out.trimmed();
		while (out.endsWith(QLatin1Char('.')))
			out.chop(1);
		if (out.isEmpty())
			out = QStringLiteral("texture");
		return out;
	}

	// Decodes a WAD texture for viewing/saving. Map textures are cut out only
	// when their name starts with '{', but spray/decal WADs (for example
	// decals_userdef.wad) keep plain names and still use palette slot 255 as the
	// transparent background, painted pure blue. If a lump without '{' has pure
	// blue in slot 255, decode it as masked too, so the preview shows the
	// background as transparent instead of a solid blue box.
	QImage LoadWadLumpImageForView(const std::vector<uint8_t> &data, const std::string &name)
	{
		bool treatAsMasked = false;
		if (!name.empty() && name.front() != '{' && data.size() > 40)
		{
			const auto u32 = [&](size_t at) -> uint32_t
			{
				return at + 4 <= data.size()
					? (data[at] | (data[at + 1] << 8) | (data[at + 2] << 16) |
						(static_cast<uint32_t>(data[at + 3]) << 24))
					: 0u;
			};
			const uint32_t w = u32(16), h = u32(20), mip3 = u32(36);
			if (w >= 8 && h >= 8 && w <= 4096 && h <= 4096)
			{
				const size_t pal = static_cast<size_t>(mip3) + static_cast<size_t>(w / 8) * (h / 8) + 2;
				if (pal + 768 <= data.size() && data[pal + 765] == 0 &&
					data[pal + 766] == 0 && data[pal + 767] == 255)
					treatAsMasked = true;
			}
		}
		return cso_gui::LoadWadMiptex(data, treatAsMasked ? "{" + name : name);
	}

	// Decodes one WAD lump as a miptex texture; null image on failure.
	QImage DecodeWadLump(const cso_gui::Wad3Archive &wad, const cso_gui::WadEntry &lump,
		QString *error)
	{
		try
		{
			return LoadWadLumpImageForView(wad.ExtractEntry(lump), lump.name);
		}
		catch (const std::exception &ex)
		{
			if (error != nullptr)
				*error = QString::fromUtf8(ex.what());
			return QImage();
		}
	}

	// Saves WAD textures as image files: one lump asks for a file name (PNG /
	// BMP / JPEG), several ask for a folder and are written as PNG (keeps the
	// transparency of '{' cut-out textures). Returns a status line for the
	// status bar; reports problems itself.
	QString SaveWadLumpsAsImages(QWidget *parent, const PakTreeModel *model,
		const std::vector<WadLumpRef> &lumps)
	{
		if (lumps.empty())
			return QString();

		const auto lumpOf = [&](const WadLumpRef &ref, const cso_gui::Wad3Archive *&wad)
			-> const cso_gui::WadEntry *
		{
			wad = model->WadArchiveAt(ref.first);
			if (wad == nullptr || ref.second < 0 ||
				ref.second >= static_cast<int>(wad->Entries().size()))
				return nullptr;
			return &wad->Entries()[static_cast<size_t>(ref.second)];
		};

		if (lumps.size() == 1)
		{
			const cso_gui::Wad3Archive *wad = nullptr;
			const cso_gui::WadEntry *lump = lumpOf(lumps.front(), wad);
			if (lump == nullptr)
				return QString();
			QString error;
			const QImage image = DecodeWadLump(*wad, *lump, &error);
			if (image.isNull())
			{
				QMessageBox::warning(parent, WadTr("Save texture"),
					WadTr("This WAD entry is not a texture that can be decoded.")
					+ (error.isEmpty() ? QString() : QStringLiteral("\n\n") + error));
				return QString();
			}

			QString selectedFilter = WadTr("PNG image (*.png)");
			QString path = QFileDialog::getSaveFileName(parent, WadTr("Save texture"),
				SafeLumpFileName(lump->name) + QStringLiteral(".png"),
				WadTr("PNG image (*.png);;Bitmap image (*.bmp);;JPEG image (*.jpg *.jpeg)"),
				&selectedFilter);
			if (path.isEmpty())
				return QString();

			QString extension = QStringLiteral(".png");
			QByteArray format = "PNG";
			if (selectedFilter.startsWith(WadTr("Bitmap image")))
			{
				extension = QStringLiteral(".bmp");
				format = "BMP";
			}
			else if (selectedFilter.startsWith(WadTr("JPEG image")))
			{
				extension = QStringLiteral(".jpg");
				format = "JPG";
			}
			// Keep the file name and the codec in agreement (the dialog can
			// leave the old extension when the filter is switched).
			const QFileInfo chosen(path);
			path = QDir(chosen.path()).filePath(chosen.completeBaseName() + extension);
			QImage out = image;
			if (format == "JPG")  // No alpha in JPEG: flatten cut-outs onto black.
				out = image.convertToFormat(QImage::Format_RGB32);
			if (!out.save(path, format.constData()))
			{
				QMessageBox::warning(parent, WadTr("Save texture"),
					WadTr("Failed to save the texture to:\n%1").arg(path));
				return QString();
			}
			return WadTr("Texture saved to %1").arg(path);
		}

		const QString dir = QFileDialog::getExistingDirectory(parent,
			WadTr("Choose output folder for the textures (saved as PNG)"));
		if (dir.isEmpty())
			return QString();

		int saved = 0;
		QStringList problems;
		std::set<QString> used;
		for (const WadLumpRef &ref : lumps)
		{
			const cso_gui::Wad3Archive *wad = nullptr;
			const cso_gui::WadEntry *lump = lumpOf(ref, wad);
			if (lump == nullptr)
				continue;
			QString error;
			const QImage image = DecodeWadLump(*wad, *lump, &error);
			if (image.isNull())
			{
				problems << QString::fromLatin1(lump->name.c_str()) + QStringLiteral(": ")
					+ (error.isEmpty() ? WadTr("not a texture (miptex) lump") : error);
				continue;
			}
			// Same name in different WADs (or after sanitising): keep both.
			const QString base = SafeLumpFileName(lump->name);
			QString file = base + QStringLiteral(".png");
			for (int n = 2; !used.insert(file.toLower()).second; ++n)
				file = base + QStringLiteral("_") + QString::number(n) + QStringLiteral(".png");
			if (image.save(QDir(dir).filePath(file), "PNG"))
				++saved;
			else
				problems << file + QStringLiteral(": ") + WadTr("failed to write the file");
		}

		QString summary = WadTr("Saved %1 of %2 texture(s) to %3.")
			.arg(saved).arg(lumps.size()).arg(dir);
		if (!problems.isEmpty())
			summary += QStringLiteral("\n\n") + WadTr("Skipped:") + QStringLiteral("\n")
				+ problems.join(QLatin1Char('\n'));
		QMessageBox::information(parent, WadTr("Save textures"), summary);
		return WadTr("Saved %1 texture(s) to %2").arg(saved).arg(dir);
	}
}

MainWindow::FoundEntry MainWindow::ResolveFoundEntry(const QModelIndex &index) const
{
	FoundEntry found;
	if (model_->IsWadLump(index))
		return found;
	const int entryIndex = model_->EntryIndexForIndex(index);
	if (entryIndex < 0)
		return found;
	if (model_->IsNarFile(index))
	{
		const int narIndex = model_->NarIndexForIndex(index);
		if (narIndex < 0 || narIndex >= static_cast<int>(narArchives_.size()))
			return found;
		const auto *archive = narArchives_[static_cast<size_t>(narIndex)].get();
		if (entryIndex >= static_cast<int>(archive->Entries().size()))
			return found;
		found.nar = archive;
		found.narEntry = &archive->Entries()[static_cast<size_t>(entryIndex)];
		return found;
	}
	const int pakIndex = model_->PakIndexForIndex(index);
	if (pakIndex < 0 || pakIndex >= static_cast<int>(archives_.size()))
		return found;
	const auto *archive = archives_[static_cast<size_t>(pakIndex)].get();
	if (entryIndex >= static_cast<int>(archive->Entries().size()))
		return found;
	found.pak = archive;
	found.pakEntry = &archive->Entries()[static_cast<size_t>(entryIndex)];
	return found;
}

void MainWindow::FocusSearch()
{
	if (searchEdit_ == nullptr)
		return;

	searchEdit_->setFocus(Qt::ShortcutFocusReason);
	searchEdit_->selectAll();
	if (!HasArchives())
		statusBar()->showMessage(
			tr("Open .pak/.nar archives first; the search covers every loaded archive."), 5000);
}

void MainWindow::RunSearch()
{
	if (searchEdit_ == nullptr)
		return;

	const QString query = searchEdit_->text().trimmed();
	if (query.isEmpty())
	{
		statusBar()->showMessage(tr("Type part of a file name or internal path to search for."), 5000);
		searchEdit_->setFocus();
		return;
	}
	if (!HasArchives())
	{
		statusBar()->showMessage(
			tr("Open .pak/.nar archives first; the search covers every loaded archive."), 5000);
		return;
	}

	// '*' and '?' make the query a wildcard pattern; otherwise it's a plain
	// case-insensitive substring match over the whole internal path.
	const bool wildcard = query.contains(QLatin1Char('*')) || query.contains(QLatin1Char('?'));
	QRegularExpression pattern;
	if (wildcard)
	{
		pattern.setPattern(QRegularExpression::wildcardToRegularExpression(
			query, QRegularExpression::UnanchoredWildcardConversion));
		pattern.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
		if (!pattern.isValid())
		{
			QMessageBox::warning(this, tr("Find File"),
				tr("Invalid wildcard pattern:\n%1").arg(query));
			return;
		}
	}
	const auto matches = [&](const QString &path)
	{
		return wildcard ? pattern.match(path).hasMatch()
			: path.contains(query, Qt::CaseInsensitive);
	};

	// Guard against a query so broad ("mdl") that filling the table would
	// freeze the UI for ages; the first matches are still useful.
	constexpr int kMaxHits = 20000;
	bool truncated = false;

	QApplication::setOverrideCursor(Qt::WaitCursor);
	searchHits_.clear();

	const auto appendHit = [&](FoundEntry entry, int pakIndex, int narIndex,
		int entryIndex, const QString &path)
	{
		if (searchHits_.size() >= static_cast<size_t>(kMaxHits))
		{
			truncated = true;
			return;
		}
		SearchHit hit;
		hit.entry = entry;
		hit.pakIndex = pakIndex;
		hit.narIndex = narIndex;
		hit.entryIndex = entryIndex;
		hit.path = path;
		hit.name = path.section(QLatin1Char('/'), -1);
		hit.archiveName = entry.archiveName();
		searchHits_.push_back(std::move(hit));
	};

	for (size_t ai = 0; ai < archives_.size() && !truncated; ++ai)
	{
		const auto &entries = archives_[ai]->Entries();
		for (size_t ei = 0; ei < entries.size() && !truncated; ++ei)
		{
			const QString path = QString::fromStdU16String(entries[ei].path);
			if (!matches(path))
				continue;
			FoundEntry entry;
			entry.pak = archives_[ai].get();
			entry.pakEntry = &entries[ei];
			appendHit(entry, static_cast<int>(ai), -1, static_cast<int>(ei), path);
		}
	}
	for (size_t ai = 0; ai < narArchives_.size() && !truncated; ++ai)
	{
		const auto &entries = narArchives_[ai]->Entries();
		for (size_t ei = 0; ei < entries.size() && !truncated; ++ei)
		{
			const QString path = QString::fromStdString(entries[ei].path);
			if (!matches(path))
				continue;
			FoundEntry entry;
			entry.nar = narArchives_[ai].get();
			entry.narEntry = &entries[ei];
			appendHit(entry, -1, static_cast<int>(ai), static_cast<int>(ei), path);
		}
	}
	QApplication::restoreOverrideCursor();

	// Same file name in several archives is common (and important to see),
	// so sort by name, then path, then archive instead of hiding duplicates.
	std::ranges::sort(searchHits_, [](const SearchHit &left, const SearchHit &right)
	{
		int order = QString::compare(left.name, right.name, Qt::CaseInsensitive);
		if (order != 0)
			return order < 0;
		order = QString::compare(left.path, right.path, Qt::CaseInsensitive);
		if (order != 0)
			return order < 0;
		return QString::compare(left.archiveName, right.archiveName, Qt::CaseInsensitive) < 0;
	});

	{
		const QSignalBlocker blocker(searchResults_);
		searchResults_->clearContents();
		searchResults_->setRowCount(static_cast<int>(searchHits_.size()));
		for (int row = 0; row < static_cast<int>(searchHits_.size()); ++row)
		{
			const SearchHit &hit = searchHits_[static_cast<size_t>(row)];
			auto *nameItem = new QTableWidgetItem(hit.name);
			nameItem->setToolTip(hit.path);
			searchResults_->setItem(row, 0, nameItem);

			auto *pathItem = new QTableWidgetItem(hit.path);
			pathItem->setToolTip(hit.path);
			searchResults_->setItem(row, 1, pathItem);

			auto *archiveItem = new QTableWidgetItem(hit.archiveName);
			archiveItem->setToolTip(tr("Archive file: %1").arg(hit.archiveName));
			searchResults_->setItem(row, 2, archiveItem);

			searchResults_->setItem(row, 3, new QTableWidgetItem(
				hit.entry.pakEntry != nullptr ? QStringLiteral("pak") : QStringLiteral("nar")));
		}
	}

	const int count = static_cast<int>(searchHits_.size());
	searchDock_->setWindowTitle(count > 0
		? tr("Search results: %1").arg(count)
		: tr("Search results"));
	searchDock_->show();

	QString summary = count > 0
		? tr("Found %1 file(s) matching \"%2\".").arg(count).arg(query)
		: tr("No files match \"%1\".").arg(query);
	if (truncated)
		summary += QLatin1Char(' ') + tr("Only the first %1 matches are shown.").arg(kMaxHits);

	// Preview the first hit right away so the result list is immediately
	// useful; selecting another row updates the preview. Show the summary
	// afterwards, since the preview writes its own status line.
	if (count > 0)
		searchResults_->setCurrentCell(0, 0);
	statusBar()->showMessage(summary, 8000);
}

void MainWindow::ShowSearchHit(int row)
{
	if (row < 0 || row >= static_cast<int>(searchHits_.size()))
		return;

	const SearchHit &hit = searchHits_[static_cast<size_t>(row)];
	statusBar()->showMessage(tr("%1  —  %2").arg(hit.path, hit.archiveName));
	ShowPreviewForFoundEntry(hit.entry);
}

void MainWindow::RevealEntry(int pakIndex, int narIndex, int entryIndex)
{
	// Search hits index MainWindow's full .nar list; the tree may hold a
	// filtered subset (auto-loaded NARs are hidden by default), so
	// translate before asking the model for the node.
	int treeNarIndex = narIndex;
	if (narIndex >= 0)
	{
		if (narIndex >= static_cast<int>(narArchives_.size()))
			return;
		treeNarIndex = model_->NarIndexForArchive(
			narArchives_[static_cast<size_t>(narIndex)].get());
		if (treeNarIndex < 0)
		{
			statusBar()->showMessage(tr(
				"This file is not shown in the tree. For NAR archives, enable "
				"View > Show auto-loaded NAR archives."), 8000);
			return;
		}
	}

	const QModelIndex index = model_->FindEntry(pakIndex, treeNarIndex, entryIndex);
	if (!index.isValid())
	{
		statusBar()->showMessage(tr(
			"This file is not shown in the tree. For NAR archives, enable "
			"View > Show auto-loaded NAR archives."), 8000);
		return;
	}

	// Expand every ancestor from the root down, then select the node.
	QModelIndexList ancestors;
	for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent())
		ancestors.prepend(parent);
	for (const QModelIndex &parent : ancestors)
		treeView_->expand(parent);

	treeView_->setCurrentIndex(index);
	treeView_->scrollTo(index, QAbstractItemView::PositionAtCenter);
	treeView_->setFocus();
}

void MainWindow::RevealSearchHit(int row)
{
	if (row < 0 || row >= static_cast<int>(searchHits_.size()))
		return;

	const SearchHit &hit = searchHits_[static_cast<size_t>(row)];
	RevealEntry(hit.pakIndex, hit.narIndex, hit.entryIndex);
}

void MainWindow::OnSearchResultChanged(int currentRow, int currentColumn,
	int previousRow, int previousColumn)
{
	Q_UNUSED(currentColumn);
	Q_UNUSED(previousRow);
	Q_UNUSED(previousColumn);
	ShowSearchHit(currentRow);
}

void MainWindow::OnSearchResultDoubleClicked(int row, int column)
{
	Q_UNUSED(column);
	RevealSearchHit(row);
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
	if (!HasArchives())
		return;

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	// Extract the whole merged tree. Duplicate virtual paths from several
	// archives are disambiguated with a pak suffix instead of silently
	// overwriting each other.
	ExtractIndicesToDirectory(model_->CollectResolvedRefs(QModelIndex()), dir);
}

std::vector<PakTreeModel::ResolvedRef> MainWindow::CollectSelectedRefs() const
{
	std::set<std::tuple<bool, int, int>> uniqueRefs;
	const auto selected = treeView_->selectionModel()->selectedIndexes();
	for (const auto &index : selected)
	{
		if (index.column() != 0)
			continue;

		for (const auto &ref : model_->CollectResolvedRefs(index))
			uniqueRefs.emplace(ref.isNar, ref.isNar ? ref.narIndex : ref.pakIndex, ref.entryIndex);
	}

	std::vector<PakTreeModel::ResolvedRef> result;
	result.reserve(uniqueRefs.size());
	for (const auto &[isNar, archiveIndex, entryIndex] : uniqueRefs)
	{
		PakTreeModel::ResolvedRef ref;
		ref.entryIndex = entryIndex;
		ref.isNar = isNar;
		if (isNar)
			ref.narIndex = archiveIndex;
		else
			ref.pakIndex = archiveIndex;
		result.push_back(ref);
	}
	return result;
}

void MainWindow::OnExtractSelected()
{
	if (!HasArchives())
		return;

	const auto refs = CollectSelectedRefs();
	if (refs.empty())
	{
		QMessageBox::information(this, tr("Nothing selected"),
			tr("Select one or more files or folders in the tree first."));
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	ExtractIndicesToDirectory(refs, dir);
}

void MainWindow::OnExtractSelectedDecoded()
{
	if (!HasArchives())
		return;

	const auto refs = CollectSelectedRefs();
	if (refs.empty())
	{
		QMessageBox::information(this, tr("Nothing selected"),
			tr("Select one or more files or folders in the tree first."));
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose output folder"));
	if (dir.isEmpty())
		return;

	ExtractIndicesToDirectory(refs, dir, /*decodeCso=*/true);
}

void MainWindow::ExtractIndicesToDirectory(const std::vector<PakTreeModel::ResolvedRef> &refs,
	const QString &destRoot, bool decodeCso)
{
	int succeeded = 0;
	QStringList errors;
	// Tracks destination paths already written during this run so duplicate
	// virtual paths from different archives don't silently overwrite.
	std::set<QString> usedPaths;

	for (const auto &ref : refs)
	{
		std::vector<uint8_t> data;
		QString relative;
		QString sourceBase;
		if (ref.isNar)
		{
			if (ref.narIndex < 0 || ref.narIndex >= static_cast<int>(narArchives_.size()))
				continue;
			const auto *archive = narArchives_[static_cast<size_t>(ref.narIndex)].get();
			if (ref.entryIndex < 0 || ref.entryIndex >= static_cast<int>(archive->Entries().size()))
				continue;
			const auto &entry = archive->Entries()[static_cast<size_t>(ref.entryIndex)];
			relative = SanitizedRelativePath(entry.path);
			sourceBase = QFileInfo(QString::fromStdString(
				archive->SourcePath().generic_string())).completeBaseName();
			try
			{
				data = archive->ExtractEntry(entry);
			}
			catch (const std::exception &ex)
			{
				errors << tr("%1: %2").arg(relative.isEmpty() ? tr("(skipped, unsafe path)") : relative,
					QString::fromUtf8(ex.what()));
				continue;
			}
		}
		else
		{
			if (ref.pakIndex < 0 || ref.pakIndex >= static_cast<int>(archives_.size()))
				continue;
			const auto *archive = archives_[static_cast<size_t>(ref.pakIndex)].get();
			if (ref.entryIndex < 0 || ref.entryIndex >= static_cast<int>(archive->Entries().size()))
				continue;

			const auto &entry = archive->Entries()[static_cast<size_t>(ref.entryIndex)];
			relative = SanitizedRelativePath(entry.path);
			sourceBase = QFileInfo(
				QString::fromStdU16String(archive->SourcePath().generic_u16string())).completeBaseName();
			if (relative.isEmpty())
			{
				errors << tr("(skipped, unsafe path)");
				continue;
			}

			try
			{
				data = archive->ExtractEntry(entry);
			}
			catch (const std::exception &ex)
			{
				errors << tr("%1: %2").arg(relative, QString::fromUtf8(ex.what()));
				continue;
			}
		}
		if (relative.isEmpty())
		{
			errors << tr("(skipped, unsafe path)");
			continue;
		}

		try
		{
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

			// Several archives can contain the same virtual path. Keep every
			// variant: the first keeps its plain name, later ones get an
			// archive suffix so nothing is lost to overwriting.
			QString destRelative = relative;
			if (!usedPaths.insert(destRelative).second)
			{
				const QFileInfo clash(destRelative);
				destRelative = clash.path() + QLatin1Char('/') + clash.completeBaseName()
					+ QStringLiteral("__from_") + sourceBase;
				if (!clash.suffix().isEmpty())
					destRelative += QLatin1Char('.') + clash.suffix();
				int counter = 2;
				while (!usedPaths.insert(destRelative).second)
					destRelative = clash.path() + QLatin1Char('/') + clash.completeBaseName()
						+ QStringLiteral("__from_") + sourceBase + QString::number(counter++);
			}

			const QString destPath = QDir(destRoot).filePath(destRelative);
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

	QString summary = tr("Extracted %1 of %2 file(s).").arg(succeeded).arg(refs.size());
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
	// The PVS/camera readout belongs to the BSP view only.
	if (auto *pvsLabel = statusBar()->findChild<QLabel *>(QStringLiteral("pvsStatusLabel")))
	{
		pvsLabel->clear();
		pvsLabel->hide();
	}
	if (!HasArchives() || !current.isValid())
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

	const FoundEntry ref = ResolveFoundEntry(current);
	if (!ref.valid())
	{
		ShowPlaceholder(tr("Select a file in the tree to preview it."));
		return;
	}

	// With several archives open the same virtual path can exist in many
	// of them -- always show which concrete file is being previewed.
	statusBar()->showMessage(tr("%1  —  %2")
		.arg(ref.displayPath(), ref.archiveName()));
	ShowPreviewForFoundEntry(ref);
}

void MainWindow::OnTreeDoubleClicked(const QModelIndex &index)
{
	if (!HasArchives() || model_->IsFolder(index))
		return;

	// A texture inside a .wad: save it as an image instead of doing nothing.
	if (model_->IsWadLump(index))
	{
		const QString status = SaveWadLumpsAsImages(this, model_,
			{ { model_->WadArchiveIndexForIndex(index), model_->WadEntryIndexForIndex(index) } });
		if (!status.isEmpty())
			statusBar()->showMessage(status, 5000);
		return;
	}

	const FoundEntry ref = ResolveFoundEntry(index);
	if (!ref.valid())
		return;

	ExtractOneWithDialog(ref);
}

void MainWindow::ExtractOneWithDialog(const FoundEntry &ref)
{
	const QString suggestedName = ref.fileName();
	const QString dest = QFileDialog::getSaveFileName(this, tr("Save extracted file"), suggestedName);
	if (dest.isEmpty())
		return;

	try
	{
		const auto data = ref.extract();
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
	if (!HasArchives())
		return;

	// Textures inside a .wad are not pak entries (they live inside the .wad's
	// own bytes), so the plain "Extract Selected" can't write them; they get
	// their own "save as image" action. Selecting the .wad node itself offers
	// all of its textures at once.
	std::set<std::pair<int, int>> lumpSet;
	QModelIndexList selected = treeView_->selectionModel()->selectedIndexes();
	if (selected.isEmpty())
	{
		const QModelIndex under = treeView_->indexAt(pos);
		if (under.isValid())
			selected.append(under);
	}
	for (const auto &index : selected)
	{
		if (index.column() != 0)
			continue;
		if (model_->IsWadLump(index))
		{
			lumpSet.emplace(model_->WadArchiveIndexForIndex(index),
				model_->WadEntryIndexForIndex(index));
		}
		else if (!model_->IsFolder(index))
		{
			const int rows = model_->rowCount(index);
			for (int r = 0; r < rows; ++r)
			{
				const QModelIndex child = model_->index(r, 0, index);
				if (model_->IsWadLump(child))
					lumpSet.emplace(model_->WadArchiveIndexForIndex(child),
						model_->WadEntryIndexForIndex(child));
			}
		}
	}
	const std::vector<std::pair<int, int>> lumps(lumpSet.begin(), lumpSet.end());

	// Real archive files in the selection (WAD lumps are not among them).
	const auto refs = CollectSelectedRefs();
	bool hasCso = false;
	for (const auto &ref : refs)
	{
		QString path;
		if (ref.isNar)
		{
			if (ref.narIndex >= 0 && ref.narIndex < static_cast<int>(narArchives_.size()))
			{
				const auto &entries = narArchives_[static_cast<size_t>(ref.narIndex)]->Entries();
				if (ref.entryIndex >= 0 && ref.entryIndex < static_cast<int>(entries.size()))
					path = SanitizedRelativePath(entries[static_cast<size_t>(ref.entryIndex)].path);
			}
		}
		else if (ref.pakIndex >= 0 && ref.pakIndex < static_cast<int>(archives_.size()))
		{
			const auto &entries = archives_[static_cast<size_t>(ref.pakIndex)]->Entries();
			if (ref.entryIndex >= 0 && ref.entryIndex < static_cast<int>(entries.size()))
				path = SanitizedRelativePath(entries[static_cast<size_t>(ref.entryIndex)].path);
		}
		if (path.endsWith(QLatin1String(".cso"), Qt::CaseInsensitive))
		{
			hasCso = true;
			break;
		}
	}

	if (lumps.empty() && refs.empty())
		return;

	QMenu menu(this);
	if (!lumps.empty())
	{
		const QString text = lumps.size() == 1
			? tr("Save Texture As Image...")
			: tr("Save %1 Textures As PNG...").arg(lumps.size());
		menu.addAction(text, this, [this, lumps]()
		{
			const QString status = SaveWadLumpsAsImages(this, model_, lumps);
			if (!status.isEmpty())
				statusBar()->showMessage(status, 5000);
		});
	}
	if (!refs.empty())
	{
		menu.addAction(tr("Extract Selected..."), this, &MainWindow::OnExtractSelected);
		// The .cso decoder only matters when the selection contains .cso tables.
		if (hasCso)
			menu.addAction(tr("Extract Selected (Decode .cso to CSV)..."),
				this, &MainWindow::OnExtractSelectedDecoded);
	}
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

void MainWindow::ShowCso(const FoundEntry &ref, std::vector<uint8_t> data)
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

	ShowEntryProperties(ref, tr("Decrypted this .cso entry, but the result doesn't look like readable text."));
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

void MainWindow::ShowModel(const FoundEntry &ref, const std::vector<uint8_t> &data)
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
		externalSeqInfo_.clear();
		externalSeqBase_.clear();
		modelView_->Clear();
		ShowEntryProperties(ref, tr("Failed to parse this .mdl model: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	// Animations stored outside the .mdl (<model><N>.seq companion files)
	// join the model's own sequence list before the 3D view and the
	// sequence combo are built from it.
	LoadExternalSequences(model, ref);

	// Split world models carry geometry with an empty texture table and
	// keep the textures in a *t.mdl twin (w_50ae_big.mdl + w_50ae_bigt.mdl).
	// Import the twin's table first so everything below (external resolve,
	// texture combo, GPU upload) sees real textures instead of nothing.
	if (model->IsValid() && model->Textures().empty())
	{
		if (auto twin = LoadTwinTextureModel(ref.displayPath()))
			model->ImportTexturesFrom(*twin);
	}

	// Textures the model only *names* (its embedded copy is a small
	// placeholder) are usually shipped as separate image files inside the
	// same pak. Bind them before the 3D view builds its GPU textures, so the
	// model shows real surfaces instead of black.
	ResolveExternalTextures(model);
	// Whatever the archives couldn't provide, try the remembered texture
	// folder (if any) so it doesn't have to be re-picked for every model.
	ApplyRememberedTextureFolder(model);

	const QString fileName = ref.fileName();
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

namespace
{
	// A Windows .ico can carry several sizes/bit depths of the same icon, and
	// Qt exposes each as a separate "frame". Retail CSO icons usually store a
	// small (16x16) or 8-bit variant first, so QImage::loadFromData would show
	// the blurriest copy. Walk the frames and keep the largest one instead;
	// on equal areas the later frame wins, which is the more colourful one in
	// the ICOs shipped with the game.
	QImage DecodeIcoBestFrame(const std::vector<uint8_t> &data)
	{
		QBuffer buffer;
		buffer.setData(reinterpret_cast<const char *>(data.data()), static_cast<qint64>(data.size()));
		if (!buffer.open(QIODevice::ReadOnly))
			return {};

		QImageReader reader(&buffer, "ico");
		const int frameCount = reader.imageCount();
		QImage best;
		int bestArea = 0;
		for (int frame = 0; frame < frameCount; ++frame)
		{
			if (!reader.jumpToImage(frame))
				continue;
			const QImage image = reader.read();
			if (image.isNull())
				continue;

			const int area = image.width() * image.height();
			if (area >= bestArea)
			{
				bestArea = area;
				best = image;
			}
		}

		// imageCount() can be 0/unknown for unusual files; the first frame is
		// still better than nothing.
		if (best.isNull())
			best.loadFromData(reinterpret_cast<const uchar *>(data.data()), static_cast<int>(data.size()));
		return best;
	}
}

QImage MainWindow::DecodeImageData(const std::vector<uint8_t> &data, const QString &extension)
{
	if (extension == QLatin1String("tga"))
		return tga::Load(data);

	if (extension == QLatin1String("ico"))
		return DecodeIcoBestFrame(data);

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
	QString fileName = info.fileName();
	while (fileName.startsWith(QLatin1Char('#')))
		fileName.remove(0, 1);
	const QString wanted = QString::fromLatin1(StripTextureNamePrefix(textureName).c_str());
	if (fileName.compare(wanted, Qt::CaseInsensitive) == 0)
		return true;

	// Some packs ship the same texture under another extension (the .mdl asks
	// for "foo.bmp" but the folder contains "foo.tga"); compare base names.
	return QFileInfo(fileName).completeBaseName().compare(
		QFileInfo(wanted).completeBaseName(), Qt::CaseInsensitive) == 0;
}

std::vector<uint8_t> MainWindow::FoundEntry::extract() const
{
	if (pakEntry != nullptr)
		return pak->ExtractEntry(*pakEntry);
	if (narEntry != nullptr)
		return nar->ExtractEntry(*narEntry);
	throw std::runtime_error("no archive entry");
}

QString MainWindow::FoundEntry::sourceName() const
{
	if (pakEntry != nullptr)
	{
		return QString::fromStdU16String(pak->SourceFileName()) +
			QStringLiteral(":") +
			QString::fromStdU16String(pakEntry->path);
	}
	if (narEntry != nullptr)
	{
		return QString::fromStdString(nar->SourcePath().filename().string()) +
			QStringLiteral(":") +
			QString::fromStdString(narEntry->path);
	}
	return {};
}

QString MainWindow::FoundEntry::archiveName() const
{
	if (pakEntry != nullptr)
		return QString::fromStdU16String(pak->SourceFileName());
	if (narEntry != nullptr)
		return QString::fromStdString(nar->SourcePath().filename().string());
	return {};
}

QString MainWindow::FoundEntry::extension() const
{
	if (pakEntry != nullptr)
		return ExtensionOf(*pakEntry);
	if (narEntry != nullptr)
		return QFileInfo(QString::fromStdString(narEntry->path)).suffix().toLower();
	return {};
}

QString MainWindow::FoundEntry::fileName() const
{
	if (pakEntry != nullptr)
		return QFileInfo(QString::fromStdU16String(pakEntry->path)).fileName();
	if (narEntry != nullptr)
		return QFileInfo(QString::fromStdString(narEntry->path)).fileName();
	return {};
}

QString MainWindow::FoundEntry::displayPath() const
{
	if (pakEntry != nullptr)
		return QString::fromStdU16String(pakEntry->path);
	if (narEntry != nullptr)
		return QString::fromStdString(narEntry->path);
	return {};
}

MainWindow::FoundEntry MainWindow::FindArchiveEntryByPath(
	const QString &internalPath) const
{
	const QString cacheKey = internalPath.toLower();
	const auto hit = findCache_.find(cacheKey);
	if (hit != findCache_.end())
		return hit->second;
	const FoundEntry result = FindArchiveEntryByPathUncached(internalPath);
	findCache_.emplace(cacheKey, result);
	return result;
}

MainWindow::FoundEntry MainWindow::FindArchiveEntryByPathUncached(
	const QString &internalPath) const
{
	FoundEntry none;
	if (!HasArchives() && narArchives_.empty())
		return none;

	QString wanted = internalPath;
	wanted.replace(QLatin1Char('\\'), QLatin1Char('/'));
	wanted = wanted.toLower();

	const auto matchesU16 = [&](const std::u16string &raw) -> bool
	{
		QString path = QString::fromStdU16String(raw);
		path.replace(QLatin1Char('\\'), QLatin1Char('/'));
		path = path.toLower();
		if (path == wanted)
			return true;
		// BSP entity paths are usually root-relative ("models/x.mdl") while
		// archive entries may carry a game-dir prefix ("cstrike/models/x.mdl").
		return path.endsWith(QLatin1Char('/') + wanted);
	};
	const auto matchesNar = [&](const std::string &raw) -> bool
	{
		QString path = QString::fromStdString(raw);
		path.replace(QLatin1Char('\\'), QLatin1Char('/'));
		path = path.toLower();
		if (path == wanted)
			return true;
		return path.endsWith(QLatin1Char('/') + wanted);
	};

	for (const auto &archive : archives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (matchesU16(entry.path))
			{
				FoundEntry found;
				found.pak = archive.get();
				found.pakEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : narArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (matchesNar(entry.path))
			{
				FoundEntry found;
				found.nar = archive.get();
				found.narEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : neighborArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (matchesU16(entry.path))
			{
				FoundEntry found;
				found.pak = archive.get();
				found.pakEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : neighborNarArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (matchesNar(entry.path))
			{
				FoundEntry found;
				found.nar = archive.get();
				found.narEntry = &entry;
				return found;
			}
		}
	}
	return none;
}

MainWindow::FoundEntry MainWindow::FindTextureFile(
	const std::string &textureName) const
{
	FoundEntry none;

	auto fold = [](std::string value)
	{
		for (char &c : value)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return value;
	};
	auto pakFileName = [](const std::u16string &path) -> std::string
	{
		const std::string raw =
			QFileInfo(QString::fromStdU16String(path)).fileName().toUtf8().toStdString();
		return StripTextureNamePrefix(raw);
	};
	auto narFileName = [](const std::string &path) -> std::string
	{
		return StripTextureNamePrefix(cso_pak::NarArchive::FileNameOf(path));
	};

	const std::string wanted = fold(StripTextureNamePrefix(textureName));

	// Search order: the model's own archive first (a same-named texture in
	// the same archive is the most likely intended one), then all other
	// loaded archives (.pak, then .nar), then session neighbours. This is
	// what makes models work when their textures were split into a
	// neighbouring archive by the game update layout.
	std::vector<int> pakOrder;
	pakOrder.reserve(archives_.size());
	if (currentModelPakIndex_ >= 0 && currentModelPakIndex_ < static_cast<int>(archives_.size()))
		pakOrder.push_back(currentModelPakIndex_);
	for (int i = 0; i < static_cast<int>(archives_.size()); ++i)
	{
		if (i != currentModelPakIndex_)
			pakOrder.push_back(i);
	}
	std::vector<int> narOrder;
	narOrder.reserve(narArchives_.size());
	if (currentModelNarIndex_ >= 0 && currentModelNarIndex_ < static_cast<int>(narArchives_.size()))
		narOrder.push_back(currentModelNarIndex_);
	for (int i = 0; i < static_cast<int>(narArchives_.size()); ++i)
	{
		if (i != currentModelNarIndex_)
			narOrder.push_back(i);
	}

	// Exact filename match first, .pak archives then .nar (session
	// neighbours included).
	for (const int pakIndex : pakOrder)
	{
		const auto *archive = archives_[static_cast<size_t>(pakIndex)].get();
		for (const auto &entry : archive->Entries())
		{
			if (fold(pakFileName(entry.path)) == wanted)
			{
				FoundEntry found;
				found.pak = archive;
				found.pakEntry = &entry;
				return found;
			}
		}
	}
	for (const int narIndex : narOrder)
	{
		const auto *archive = narArchives_[static_cast<size_t>(narIndex)].get();
		for (const auto &entry : archive->Entries())
		{
			if (fold(narFileName(entry.path)) == wanted)
			{
				FoundEntry found;
				found.nar = archive;
				found.narEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : neighborArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (fold(pakFileName(entry.path)) == wanted)
			{
				FoundEntry found;
				found.pak = archive.get();
				found.pakEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : neighborNarArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (fold(narFileName(entry.path)) == wanted)
			{
				FoundEntry found;
				found.nar = archive.get();
				found.narEntry = &entry;
				return found;
			}
		}
	}

	// Same base name with any image extension.
	std::string wantedBase = wanted;
	const size_t dot = wantedBase.find_last_of('.');
	if (dot != std::string::npos)
		wantedBase.resize(dot);

	static const char *const kAltExtensions[] = { "tga", "dds", "bmp", "png", "jpg", "jpeg" };
	const auto baseExtMatch = [&](const std::string &name) -> bool
	{
		const size_t entryDot = name.find_last_of('.');
		if (entryDot == std::string::npos)
			return false;
		if (name.compare(0, entryDot, wantedBase) != 0)
			return false;
		const std::string ext = name.substr(entryDot + 1);
		for (const char *alt : kAltExtensions)
		{
			if (ext == alt)
				return true;
		}
		return false;
	};
	for (const auto &archive : archives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (baseExtMatch(fold(pakFileName(entry.path))))
			{
				FoundEntry found;
				found.pak = archive.get();
				found.pakEntry = &entry;
				return found;
			}
		}
	}
	for (const auto &archive : narArchives_)
	{
		for (const auto &entry : archive->Entries())
		{
			if (baseExtMatch(fold(narFileName(entry.path))))
			{
				FoundEntry found;
				found.nar = archive.get();
				found.narEntry = &entry;
				return found;
			}
		}
	}
	return none;
}

std::shared_ptr<cso_gui::StudioModel> MainWindow::LoadTwinTextureModel(
	const QString &modelPath) const
{
	// CSO splits some world models: w_50ae_big.mdl carries geometry with an
	// empty texture table, w_50ae_bigt.mdl carries the textures (no
	// geometry) in matching order. The twin rule is a 't' before ".mdl".
	QString twinPath = modelPath;
	twinPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
	if (!twinPath.endsWith(QStringLiteral(".mdl"), Qt::CaseInsensitive))
		return nullptr;
	twinPath.insert(twinPath.size() - 4, QLatin1Char('t'));

	const auto loadBytes = [](const std::vector<uint8_t> &bytes)
		-> std::shared_ptr<cso_gui::StudioModel>
	{
		auto twin = std::make_shared<cso_gui::StudioModel>();
		try
		{
			twin->Load(bytes);
		}
		catch (const std::exception &)
		{
			return nullptr;
		}
		if (!twin->IsValid() || twin->Textures().empty())
			return nullptr;
		return twin;
	};

	// Same archives the placed-file lookups use.
	const FoundEntry found = FindArchiveEntryByPath(twinPath);
	if (found.valid())
	{
		try
		{
			if (auto twin = loadBytes(found.extract()))
				return twin;
		}
		catch (const std::exception &)
		{
		}
	}

	// Loose twin next to extracted files (remembered + bundled folders).
	const QString twinFileName = QFileInfo(twinPath).fileName();
	QStringList twinDirs;
	if (!textureFolder_.isEmpty() && QDir(textureFolder_).exists())
		twinDirs << textureFolder_;
	const QString bundled = BundledTextureFolder();
	if (!bundled.isEmpty() && !twinDirs.contains(bundled, Qt::CaseInsensitive))
		twinDirs << bundled;
	for (const QString &dir : twinDirs)
	{
		QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			const QString candidate = it.next();
			if (QFileInfo(candidate).fileName().compare(
				twinFileName, Qt::CaseInsensitive) != 0)
				continue;
			QFile file(candidate);
			if (!file.open(QIODevice::ReadOnly))
				continue;
			const QByteArray bytes = file.readAll();
			if (auto twin = loadBytes(std::vector<uint8_t>(
				bytes.begin(), bytes.end())))
				return twin;
		}
	}
	return nullptr;
}

void MainWindow::EnsureNeighborArchivesLoaded()
{
	if (neighborArchivesScanned_)
		return;
	// Set up front: a scan the user cancelled is not restarted for every
	// model they open afterwards.
	neighborArchivesScanned_ = true;

	std::set<QString> knownPaths;
	for (const auto &archive : archives_)
		knownPaths.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).canonicalFilePath());
	for (const auto &archive : neighborArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).canonicalFilePath());
	for (const auto &archive : narArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).canonicalFilePath());
	for (const auto &archive : neighborNarArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).canonicalFilePath());

	std::set<QString> sourceDirs;
	for (const auto &archive : archives_)
		sourceDirs.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).absolutePath());
	for (const auto &archive : narArchives_)
		sourceDirs.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).absolutePath());
	// Same CSO layout reasoning as OnOpenPakFolder: .nar files usually sit
	// in the parent of Packer/, so look there too.
	{
		std::set<QString> withParents = sourceDirs;
		for (const QString &d : sourceDirs)
		{
			const QString parent = QFileInfo(d).absoluteDir().absolutePath();
			if (parent != d)
				withParents.insert(parent);
		}
		sourceDirs = std::move(withParents);
	}

	QStringList neighbourPaks;
	for (const QString &dir : sourceDirs)
	{
		const QStringList names = QDir(dir).entryList(
			QStringList() << QStringLiteral("*.pak") << QStringLiteral("*.nar"),
			QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
		for (const QString &name : names)
		{
			const QString full = QDir(dir).absoluteFilePath(name);
			if (knownPaths.insert(QFileInfo(full).canonicalFilePath()).second)
				neighbourPaks << full;
		}
	}
	if (neighbourPaks.isEmpty())
		return;

	QProgressDialog progress(
		tr("Looking for external animations (.seq) in neighbouring archives..."),
		tr("Cancel"), 0, neighbourPaks.size(), this);
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(400);

	for (int i = 0; i < neighbourPaks.size(); ++i)
	{
		progress.setValue(i);
		progress.setLabelText(tr("Opening %1 (%2 of %3)...")
			.arg(QFileInfo(neighbourPaks[i]).fileName()).arg(i + 1).arg(neighbourPaks.size()));
		QApplication::processEvents();
		if (progress.wasCanceled())
			break;

		const std::filesystem::path path(neighbourPaks[i].toStdU16String());
		const bool isNar = QFileInfo(neighbourPaks[i]).suffix().compare(
			QStringLiteral("nar"), Qt::CaseInsensitive) == 0;
		try
		{
			// Only the entry table is read; the file bytes stay on disk.
			if (isNar)
				neighborNarArchives_.push_back(std::make_unique<cso_pak::NarArchive>(
					cso_pak::NarArchive::Load(path)));
			else
				neighborArchives_.push_back(std::make_unique<cso_pak::PakArchive>(
					cso_pak::PakArchive::Load(path)));
		}
		catch (const std::exception &)
		{
			// Not a readable archive: nothing to search in it.
		}
	}
	progress.setValue(neighbourPaks.size());

	// Earlier misses may be found in the newly opened archives.
	findCache_.clear();
}

void MainWindow::LoadExternalSequences(const std::shared_ptr<cso_gui::StudioModel> &model,
	const FoundEntry &modelRef)
{
	externalSeqInfo_.clear();
	externalSeqBase_.clear();
	if (!model || !model->IsValid() || model->Bones().empty() || !modelRef.valid())
		return;

	QString modelPath = modelRef.displayPath();
	modelPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
	modelPath = modelPath.toLower();
	if (!modelPath.endsWith(QStringLiteral(".mdl")))
		return;
	const int modelSlash = modelPath.lastIndexOf(QLatin1Char('/'));
	const QString modelDir = modelSlash >= 0 ? modelPath.left(modelSlash) : QString();
	const QString modelBase = modelPath.mid(modelSlash + 1,
		modelPath.size() - modelSlash - 1 - 4);
	if (modelBase.isEmpty())
		return;
	externalSeqBase_ = modelRef.fileName().chopped(4);

	// Keep "path  —  archive" visible while the (possibly long) neighbour
	// scan runs, and after it: every later status message for this model
	// is built on top of it instead of replacing it.
	const QString entryStatus = tr("%1  —  %2")
		.arg(modelRef.displayPath(), modelRef.archiveName());
	statusBar()->showMessage(entryStatus);

	if (searchNeighborSeq_)
		EnsureNeighborArchivesLoaded();

	// group number N -> the "<model>N.seq" entry to use. The first hit wins,
	// and archives are visited in priority order: the model's own archive,
	// the other loaded ones, then (if enabled) the neighbours.
	std::map<int, FoundEntry> found;

	const auto hasSeqExtension = [](const auto &path) -> bool
	{
		const size_t n = path.size();
		return n >= 4 && path[n - 4] == '.' && (path[n - 3] | 0x20) == 's' &&
			(path[n - 2] | 0x20) == 'e' && (path[n - 1] | 0x20) == 'q';
	};
	// Group number if `rawPath` is "<same dir as the model>/<model name><digits>.seq".
	const auto groupOf = [&](QString rawPath) -> int
	{
		rawPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
		rawPath = rawPath.toLower();
		const int slash = rawPath.lastIndexOf(QLatin1Char('/'));
		const QString name = rawPath.mid(slash + 1);
		const QString stem = name.left(name.size() - 4);
		if (stem.size() <= modelBase.size() || !stem.startsWith(modelBase))
			return -1;
		const QString digits = stem.mid(modelBase.size());
		if (digits.size() > 4)
			return -1;
		for (const QChar ch : digits)
		{
			if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
				return -1;
		}
		// Entry paths may carry a game-dir prefix the model's own path lacks
		// (or the reverse), as in FindArchiveEntryByPath.
		const QString entryDir = slash >= 0 ? rawPath.left(slash) : QString();
		if (entryDir != modelDir &&
			!entryDir.endsWith(QLatin1Char('/') + modelDir) &&
			!modelDir.endsWith(QLatin1Char('/') + entryDir))
			return -1;
		const int group = digits.toInt();
		return group > 0 ? group : -1;
	};
	const auto scanPak = [&](const cso_pak::PakArchive *archive)
	{
		if (archive == nullptr)
			return;
		for (const auto &entry : archive->Entries())
		{
			if (!hasSeqExtension(entry.path))
				continue;
			const int group = groupOf(QString::fromStdU16String(entry.path));
			if (group < 0 || found.count(group) != 0)
				continue;
			FoundEntry hit;
			hit.pak = archive;
			hit.pakEntry = &entry;
			found.emplace(group, hit);
		}
	};
	const auto scanNar = [&](const cso_pak::NarArchive *archive)
	{
		if (archive == nullptr)
			return;
		for (const auto &entry : archive->Entries())
		{
			if (!hasSeqExtension(entry.path))
				continue;
			const int group = groupOf(QString::fromStdString(entry.path));
			if (group < 0 || found.count(group) != 0)
				continue;
			FoundEntry hit;
			hit.nar = archive;
			hit.narEntry = &entry;
			found.emplace(group, hit);
		}
	};

	scanPak(modelRef.pak);
	scanNar(modelRef.nar);
	for (const auto &archive : archives_)
		scanPak(archive.get());
	for (const auto &archive : narArchives_)
		scanNar(archive.get());
	if (searchNeighborSeq_)
	{
		for (const auto &archive : neighborArchives_)
			scanPak(archive.get());
		for (const auto &archive : neighborNarArchives_)
			scanNar(archive.get());
	}
	if (found.empty())
		return;

	std::vector<int> appliedGroups;
	int appliedSequences = 0;
	QStringList problems;
	std::set<int> failedGroups;
	for (const auto &[group, hit] : found)
	{
		try
		{
			appliedSequences += model->AppendExternalSequences(hit.extract(), group);
			appliedGroups.push_back(group);
		}
		catch (const std::exception &ex)
		{
			failedGroups.insert(group);
			problems << tr("%1%2.seq skipped: %3")
				.arg(externalSeqBase_).arg(group).arg(QString::fromUtf8(ex.what()));
		}
	}

	// "1-13", "2-13", "1-3, 5" ...
	const auto formatGroups = [](const std::vector<int> &groups) -> QString
	{
		QStringList parts;
		for (size_t i = 0; i < groups.size();)
		{
			size_t j = i;
			while (j + 1 < groups.size() && groups[j + 1] == groups[j] + 1)
				++j;
			parts << (j > i ? QStringLiteral("%1–%2").arg(groups[i]).arg(groups[j])
				: QString::number(groups[i]));
			i = j + 1;
		}
		return parts.join(QStringLiteral(", "));
	};

	// Numbers below the highest one found that no archive provided: the
	// model probably has more animation files than the loaded archives hold.
	std::vector<int> missingGroups;
	for (int g = 1; g < found.rbegin()->first; ++g)
	{
		if (found.count(g) == 0)
			missingGroups.push_back(g);
	}

	QStringList lines;
	if (appliedSequences > 0)
		lines << tr("External animations: %1 sequence(s) from %2 .seq file(s) (%3).")
			.arg(appliedSequences).arg(appliedGroups.size()).arg(formatGroups(appliedGroups));
	if (!missingGroups.empty())
		lines << tr("Not found in the searched archives: %1%2.seq.")
			.arg(externalSeqBase_).arg(formatGroups(missingGroups));
	lines << problems;
	externalSeqInfo_ = lines.join(QLatin1Char('\n'));

	// No timeout: a temporary message would expire into an empty status bar
	// and the path/archive of the model would be lost.
	statusBar()->showMessage(appliedSequences > 0
		? tr("%1  —  +%2 external animation(s) from %3 .seq file(s)")
			.arg(entryStatus).arg(appliedSequences).arg(appliedGroups.size())
		: entryStatus);
}

void MainWindow::ResolveExternalTextures(const std::shared_ptr<cso_gui::StudioModel> &model)
{
	externalTextureSources_.clear();
	if (!model || !HasArchives())
		return;

	const auto &textures = model->Textures();
	externalTextureSources_.resize(textures.size());

	for (size_t i = 0; i < textures.size(); ++i)
	{
		const auto &tex = textures[static_cast<size_t>(i)];
		if (!TextureLooksExternal(tex))
			continue;

		const FoundEntry found = FindTextureFile(tex.name);
		if (!found.valid())
			continue;

		std::vector<uint8_t> data;
		try
		{
			data = found.extract();
		}
		catch (const std::exception &)
		{
			continue;
		}

		const QImage image = DecodeImageData(data, found.extension());
		if (image.isNull())
			continue;

		model->SetTextureImage(static_cast<int>(i), image);
		// Record which archive the texture actually came from -- with duplicates
		// across updates this is the only way to tell sources apart.
		externalTextureSources_[i] = found.sourceName();
	}
}

void MainWindow::LoadTexturesFromFolder()
{
	if (!currentModel_)
		return;

	const QString dir = QFileDialog::getExistingDirectory(this,
		tr("Choose a folder containing the model textures"));
	if (dir.isEmpty())
		return;

	// Remember the folder (persisted across restarts) and reuse it
	// automatically for every subsequently opened model.
	textureFolder_ = dir;
	QSettings settings;
	settings.setValue(QStringLiteral("textureFolder"), dir);

	// Walk the folder once and reuse the listing for every unresolved
	// texture, instead of a fresh recursive scan per texture.
	QStringList files;
	QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
		files << it.next();

	const int loaded = ResolveTexturesFromFolderFiles(currentModel_, files);

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

void MainWindow::ApplyRememberedTextureFolder(
	const std::shared_ptr<cso_gui::StudioModel> &model)
{
	if (!model)
		return;

	auto hasUnresolved = [&]()
		{
			const auto &textures = model->Textures();
			for (size_t i = 0; i < textures.size(); ++i)
			{
				const bool alreadyResolved =
					i < externalTextureSources_.size() && !externalTextureSources_[i].isEmpty();
				if (TextureLooksExternal(textures[i]) && !alreadyResolved)
					return true;
			}
			return false;
		};

	auto scanDir = [&](const QString &dir)
		{
			QStringList files;
			QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
				files << it.next();
			return ResolveTexturesFromFolderFiles(model, files);
		};

	// Candidate folders, in priority order: the remembered folder first,
	// then the bundled "textures" folder next to the executable.
	QStringList candidates;
	if (!textureFolder_.isEmpty() && QDir(textureFolder_).exists())
		candidates << textureFolder_;
	const QString bundled = BundledTextureFolder();
	if (!bundled.isEmpty() && !candidates.contains(bundled, Qt::CaseInsensitive))
		candidates << bundled;

	int loaded = 0;
	QStringList usedDirs;
	for (const QString &dir : candidates)
	{
		// Walk a folder only when something is still unresolved.
		if (!hasUnresolved())
			break;
		const int count = scanDir(dir);
		if (count > 0)
		{
			loaded += count;
			usedDirs << dir;
		}
	}

	if (loaded > 0)
		statusBar()->showMessage(tr("Loaded %1 external texture(s) from %2")
			.arg(loaded).arg(usedDirs.join(QStringLiteral(", "))), 6000);
}

int MainWindow::ResolveTexturesFromFolderFiles(
	const std::shared_ptr<cso_gui::StudioModel> &model, const QStringList &files)
{
	if (!model)
		return 0;

	const auto &textures = model->Textures();
	int loaded = 0;
	for (size_t i = 0; i < textures.size(); ++i)
	{
		// Only textures that aren't already bound -- reprocessing a resolved
		// one would just redo work, and risks overwriting a good match with
		// an unrelated same-named file.
		const bool alreadyResolved =
			i < externalTextureSources_.size() && !externalTextureSources_[i].isEmpty();
		if (!TextureLooksExternal(textures[i]) || alreadyResolved)
			continue;

		const auto &tex = textures[i];
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

		model->SetTextureImage(static_cast<int>(i), image);
		if (static_cast<size_t>(i) < externalTextureSources_.size())
			externalTextureSources_[static_cast<size_t>(i)] = foundPath;
		++loaded;
	}

	return loaded;
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
			"the \"$0a_\"/\"$0b_\"/\"ef_\" name-prefix convention, or a manual choice.\n"
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

	// Texture-library files (CSO *t.mdl twins: a texture table but no
	// geometry) leave the 3D viewport empty, which looks broken. Say so.
	{
		size_t totalTris = 0;
		for (const auto &bp : currentModel_->BodyParts())
		{
			for (const auto &sub : bp.models)
			{
				for (const auto &mesh : sub.meshes)
					totalTris += mesh.triangles.size();
			}
		}
		if (totalTris == 0)
		{
			auto *geoNote = new WrappedFitLabel(
				tr("No geometry in this file (texture library?) \u2014 the 3D view stays "
					"empty; its textures are listed above."));
			geoNote->setWordWrap(true);
			geoNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			vLayout->addWidget(geoNote);
		}
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
		const auto &modelSequences = currentModel_->Sequences();
		for (int i = 0; i < seqCount; ++i)
		{
			modelSequenceCombo_->addItem(DecodeModelText(modelView_->SequenceLabel(i)));
			// Animations merged in from <model><N>.seq files say where they came from.
			if (static_cast<size_t>(i) < modelSequences.size() &&
				modelSequences[static_cast<size_t>(i)].externalGroup > 0)
			{
				modelSequenceCombo_->setItemData(i + 1,
					tr("External animation: %1%2.seq")
						.arg(externalSeqBase_)
						.arg(modelSequences[static_cast<size_t>(i)].externalGroup),
					Qt::ToolTipRole);
			}
		}
		modelSequenceCombo_->setCurrentIndex(modelView_->CurrentSequence() + 1);
		connect(modelSequenceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &MainWindow::OnModelSequenceChanged);
		vLayout->addWidget(modelSequenceCombo_);

		if (!externalSeqInfo_.isEmpty())
			vLayout->addWidget(makeCaption(externalSeqInfo_));

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

namespace
{
	// Rebuilds an 8-bit paletted image from a decoded texture, the way GoldSrc
	// textures are stored: up to 256 colours, and for masked textures
	// (flag 0x40) palette slot 255 is the transparent one. Returns a null image
	// when the picture has more colours than a palette can hold, so the caller
	// can fall back to a plain save.
	QImage BuildPalettedImage(const QImage &source, bool masked)
	{
		const QImage image = source.convertToFormat(QImage::Format_ARGB32);
		const int maxColors = masked ? 255 : 256;

		std::map<QRgb, int> lookup;
		QList<QRgb> palette;
		QImage out(image.width(), image.height(), QImage::Format_Indexed8);
		for (int y = 0; y < image.height(); ++y)
		{
			const QRgb *src = reinterpret_cast<const QRgb *>(image.constScanLine(y));
			uchar *dst = out.scanLine(y);
			for (int x = 0; x < image.width(); ++x)
			{
				const QRgb px = src[x];
				if (qAlpha(px) < 128)
				{
					if (!masked)
						return QImage();  // Real alpha: a palette BMP can't hold it.
					dst[x] = 255;
					continue;
				}
				const QRgb rgb = px | 0xFF000000u;
				auto it = lookup.find(rgb);
				if (it == lookup.end())
				{
					if (static_cast<int>(palette.size()) >= maxColors)
						return QImage();
					it = lookup.emplace(rgb, static_cast<int>(palette.size())).first;
					palette.push_back(rgb);
				}
				dst[x] = static_cast<uchar>(it->second);
			}
		}

		while (palette.size() < 256)
			palette.push_back(0xFF000000u);
		if (masked)
			palette[255] = 0xFF0000FFu;  // The conventional "transparent" blue.
		out.setColorTable(palette);
		return out;
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
	// Game textures are 8-bit paletted BMPs. Saving the decoded ARGB image
	// would turn them into 24/32-bit files and lose palette slot 255 (the
	// transparent one), so rebuild a palette image when the colours fit.
	QImage toSave = texture.image;
	if (format == "BMP")
	{
		const QImage paletted = BuildPalettedImage(texture.image, (texture.flags & 0x40) != 0);
		if (!paletted.isNull())
			toSave = paletted;
	}
	if (!toSave.save(path, format.constData()))
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

void MainWindow::OnBspWireframeToggled(bool enabled)
{
	bspView_->SetWireframe(enabled);
}

namespace
{
	// Decodes every texture lump of an already-loaded .wad into
	// lowercase-name -> image pairs. Undecodable lumps are skipped.
	std::map<QString, QImage> DecodeWadImages(const cso_gui::Wad3Archive &wad)
	{
		std::map<QString, QImage> images;
		for (const auto &lump : wad.Entries())
		{
			const QString key = QString::fromStdString(lump.name).toLower();
			if (images.find(key) != images.end())
				continue;
			try
			{
				const QImage image = cso_gui::LoadWadMiptex(
					wad.ExtractEntry(lump), lump.name);
				if (!image.isNull())
					images.emplace(key, image);
			}
			catch (const std::exception &)
			{
				continue;  // Not a decodable texture lump, skip it.
			}
		}
		return images;
	}
}

std::map<QString, QImage> MainWindow::ResolveWadFile(const QString &wadFileName)
{
	const QString cacheKey = wadFileName.toLower();
	const auto cached = wadImageCache_.find(cacheKey);
	if (cached != wadImageCache_.end())
		return cached->second;

	std::map<QString, QImage> images;

	// 1. Every explicitly loaded archive (.pak, then .nar).
	for (const auto &archive : archives_)
	{
		for (const auto &candidate : archive->Entries())
		{
			if (QFileInfo(QString::fromStdU16String(candidate.path)).fileName()
				.compare(wadFileName, Qt::CaseInsensitive) != 0)
				continue;
			try
			{
				auto wad = cso_gui::Wad3Archive::Load(archive->ExtractEntry(candidate));
				images = DecodeWadImages(wad);
			}
			catch (const std::exception &)
			{
				continue;  // Unparseable .wad, keep looking.
			}
			if (!images.empty())
			{
				wadImageCache_.emplace(cacheKey, images);
				return images;
			}
		}
	}
	for (const auto &archive : narArchives_)
	{
		for (const auto &candidate : archive->Entries())
		{
			if (QString::fromStdString(
				cso_pak::NarArchive::FileNameOf(candidate.path))
				.compare(wadFileName, Qt::CaseInsensitive) != 0)
				continue;
			try
			{
				auto wad = cso_gui::Wad3Archive::Load(archive->ExtractEntry(candidate));
				images = DecodeWadImages(wad);
			}
			catch (const std::exception &)
			{
				continue;  // Unparseable .wad, keep looking.
			}
			if (!images.empty())
			{
				wadImageCache_.emplace(cacheKey, images);
				return images;
			}
		}
	}

	// 2. Loose .wad files in the remembered texture folder and the bundled
	// textures/ folder (same convention as model textures).
	QStringList wadSearchDirs;
	if (!textureFolder_.isEmpty() && QDir(textureFolder_).exists())
		wadSearchDirs << textureFolder_;
	const QString bundled = BundledTextureFolder();
	if (!bundled.isEmpty() && !wadSearchDirs.contains(bundled, Qt::CaseInsensitive))
		wadSearchDirs << bundled;
	for (const QString &dir : wadSearchDirs)
	{
		QDirIterator it(dir, QStringList() << QStringLiteral("*.wad"),
			QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			const QString path = it.next();
			if (QFileInfo(path).fileName().compare(wadFileName, Qt::CaseInsensitive) != 0)
				continue;
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
				continue;
			const QByteArray bytes = file.readAll();
			try
			{
				std::vector<uint8_t> data(bytes.begin(), bytes.end());
				auto wad = cso_gui::Wad3Archive::Load(std::move(data));
				images = DecodeWadImages(wad);
			}
			catch (const std::exception &)
			{
				continue;
			}
			if (!images.empty())
			{
				wadImageCache_.emplace(cacheKey, images);
				return images;
			}
		}
	}

	// 3. Neighbouring .pak files next to the loaded archives. This is what
	// makes single-pak mode work when the map and its .wad were split
	// across archives. Loaded neighbours are cached for the session, so a
	// whole Packer folder is scanned at most once per missing .wad.
	//
	// NOTE: neighbours cached while resolving *other* .wad names were never
	// searched for THIS name (they are excluded from neighbourPaks below),
	// so search them first -- otherwise a pak cached by an earlier scan can
	// hide the very .wad we are looking for.
	for (const auto &archive : neighborArchives_)
	{
		for (const auto &candidate : archive->Entries())
		{
			if (QFileInfo(QString::fromStdU16String(candidate.path)).fileName()
				.compare(wadFileName, Qt::CaseInsensitive) != 0)
				continue;
			try
			{
				auto wad = cso_gui::Wad3Archive::Load(archive->ExtractEntry(candidate));
				images = DecodeWadImages(wad);
			}
			catch (const std::exception &)
			{
				continue;
			}
			if (!images.empty())
			{
				wadImageCache_.emplace(cacheKey, images);
				return images;
			}
		}
	}
	for (const auto &archive : neighborNarArchives_)
	{
		for (const auto &candidate : archive->Entries())
		{
			if (QString::fromStdString(
				cso_pak::NarArchive::FileNameOf(candidate.path))
				.compare(wadFileName, Qt::CaseInsensitive) != 0)
				continue;
			try
			{
				auto wad = cso_gui::Wad3Archive::Load(archive->ExtractEntry(candidate));
				images = DecodeWadImages(wad);
			}
			catch (const std::exception &)
			{
				continue;
			}
			if (!images.empty())
			{
				wadImageCache_.emplace(cacheKey, images);
				return images;
			}
		}
	}

	std::set<QString> knownPaths;
	for (const auto &archive : archives_)
		knownPaths.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).canonicalFilePath());
	for (const auto &archive : neighborArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).canonicalFilePath());
	for (const auto &archive : narArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).canonicalFilePath());
	for (const auto &archive : neighborNarArchives_)
		knownPaths.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).canonicalFilePath());

	std::set<QString> sourceDirs;
	for (const auto &archive : archives_)
	{
		sourceDirs.insert(QFileInfo(QString::fromStdU16String(
			archive->SourcePath().generic_u16string())).absolutePath());
	}
	for (const auto &archive : narArchives_)
	{
		sourceDirs.insert(QFileInfo(QString::fromStdString(
			archive->SourcePath().generic_string())).absolutePath());
	}
	// Same CSO layout reasoning as OnOpenPakFolder: .nar files usually sit
	// in the parent of Packer/, so let the neighbour scan look there too.
	{
		std::set<QString> withParents = sourceDirs;
		for (const QString &d : sourceDirs)
		{
			const QString parent = QFileInfo(d).absoluteDir().absolutePath();
			if (parent != d)
				withParents.insert(parent);
		}
		sourceDirs = std::move(withParents);
	}

	QStringList neighbourPaks;
	for (const QString &dir : std::vector<QString>(sourceDirs.begin(), sourceDirs.end()))
	{
		const QStringList names = QDir(dir).entryList(
			QStringList() << QStringLiteral("*.pak") << QStringLiteral("*.nar"),
			QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
		for (const QString &name : names)
		{
			const QString full = QDir(dir).absoluteFilePath(name);
			if (knownPaths.insert(QFileInfo(full).canonicalFilePath()).second)
				neighbourPaks << full;
		}
	}

	if (!neighbourPaks.isEmpty())
	{
		QProgressDialog progress(tr("Looking for %1 in neighbouring archives...")
			.arg(wadFileName), tr("Cancel"), 0, neighbourPaks.size(), this);
		progress.setWindowModality(Qt::WindowModal);
		progress.setMinimumDuration(400);

		for (int i = 0; i < neighbourPaks.size(); ++i)
		{
			progress.setValue(i);
			QApplication::processEvents();
			if (progress.wasCanceled())
				break;

			const bool isNar = QFileInfo(neighbourPaks[i]).suffix().compare(
				QStringLiteral("nar"), Qt::CaseInsensitive) == 0;
			if (isNar)
			{
				std::unique_ptr<cso_pak::NarArchive> neighbour;
				try
				{
					neighbour = std::make_unique<cso_pak::NarArchive>(
						cso_pak::NarArchive::Load(
							std::filesystem::path(neighbourPaks[i].toStdU16String())));
				}
				catch (const std::exception &)
				{
					continue;
				}

				for (const auto &candidate : neighbour->Entries())
				{
					if (QString::fromStdString(
						cso_pak::NarArchive::FileNameOf(candidate.path))
						.compare(wadFileName, Qt::CaseInsensitive) != 0)
						continue;
					try
					{
						auto wad = cso_gui::Wad3Archive::Load(neighbour->ExtractEntry(candidate));
						images = DecodeWadImages(wad);
					}
					catch (const std::exception &)
					{
						continue;
					}
					if (!images.empty())
					{
						findCache_.clear();
neighborNarArchives_.push_back(std::move(neighbour));
						wadImageCache_.emplace(cacheKey, images);
						progress.setValue(neighbourPaks.size());
						return images;
					}
				}
				// Keep the indexed archive even on a miss -- later maps and
				// .wad names reuse it without re-reading the file.
				findCache_.clear();
neighborNarArchives_.push_back(std::move(neighbour));
				continue;
			}

			std::unique_ptr<cso_pak::PakArchive> neighbour;
			try
			{
				neighbour = std::make_unique<cso_pak::PakArchive>(
					cso_pak::PakArchive::Load(
						std::filesystem::path(neighbourPaks[i].toStdU16String())));
			}
			catch (const std::exception &)
			{
				continue;
			}

			for (const auto &candidate : neighbour->Entries())
			{
				if (QFileInfo(QString::fromStdU16String(candidate.path)).fileName()
					.compare(wadFileName, Qt::CaseInsensitive) != 0)
					continue;
				try
				{
					auto wad = cso_gui::Wad3Archive::Load(neighbour->ExtractEntry(candidate));
					images = DecodeWadImages(wad);
				}
				catch (const std::exception &)
				{
					continue;
				}
				if (!images.empty())
				{
					findCache_.clear();
neighborArchives_.push_back(std::move(neighbour));
					wadImageCache_.emplace(cacheKey, images);
					progress.setValue(neighbourPaks.size());
					return images;
				}
			}
			// Keep the indexed archive even on a miss -- later maps and
			// .wad names reuse it without re-reading the file.
			findCache_.clear();
neighborArchives_.push_back(std::move(neighbour));
		}
		progress.setValue(neighbourPaks.size());
	}

	wadImageCache_.emplace(cacheKey, images);
	return images;
}

void MainWindow::ShowBsp(const FoundEntry &ref, const std::vector<uint8_t> &data)
{
	auto map = std::make_shared<cso_gui::BspMap>();
	try
	{
		map->Load(data);
		map->SetPlacedState(property("bspAfterSwitch").toBool());
	}
	catch (const std::exception &ex)
	{
		currentBsp_.reset();
		bspView_->Clear();
		ShowEntryProperties(ref, tr("Failed to parse this .bsp map: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	// Texture images aligned with map->Textures(): embedded ones straight
	// from the parser, external ones resolved below via .wad files.
	std::vector<QImage> images(map->Textures().size());
	int embeddedCount = 0;
	for (size_t i = 0; i < map->Textures().size(); ++i)
	{
		if (!map->Textures()[i].image.isNull())
		{
			images[i] = map->Textures()[i].image;
			++embeddedCount;
		}
	}

	// External textures live in .wad files named by the worldspawn "wad"
	// key (semicolon-separated, often with full Windows paths -- compare by
	// file name only). Each .wad is resolved through every source.
	std::map<QString, QImage> wadImages;  // lowercase texture name -> image
	QStringList usedWads;
	QStringList missingWads;
	{
		// Collect every "wad" value from the entities text (usually just
		// worldspawn's, but custom maps vary).
		QStringList wadNames;
		const QRegularExpression wadKey(QStringLiteral("\"wad\"\\s+\"([^\"]*)\""));
		auto it = wadKey.globalMatch(QString::fromLatin1(
			map->EntitiesText().c_str(), static_cast<int>(map->EntitiesText().size())));
		while (it.hasNext())
		{
			for (const QString &part : it.next().captured(1).split(QLatin1Char(';')))
			{
				const QString base = QFileInfo(part.trimmed()).fileName();
				if (!base.isEmpty() && !wadNames.contains(base, Qt::CaseInsensitive))
					wadNames << base;
			}
		}

		for (const QString &wadName : wadNames)
		{
			const std::map<QString, QImage> resolved = ResolveWadFile(wadName);
			if (resolved.empty())
			{
				missingWads << wadName;
				continue;
			}
			for (const auto &[key, image] : resolved)
			{
				if (wadImages.find(key) == wadImages.end())
					wadImages.emplace(key, image);
			}
			usedWads << wadName;
		}
	}

	int wadCount = 0;
	int looseCount = 0;
	int missingCount = 0;
	QStringList missingNames;

	// Fallback for stale map "wad" keys (custom maps, or stock maps whose
	// wads CSO doesn't ship, like halflife.wad): look for the still-missing
	// texture names inside every other .wad of the already-loaded archives.
	// Only name-matching lumps are pixel-decoded, so this is one I/O pass
	// over wad bytes with no decode work for the rest. Stops at the first
	// archive set that covers everything; cancellable when it gets long.
	{
		std::set<QString> stillMissing;
		for (size_t i = 0; i < map->Textures().size(); ++i)
		{
			if (!images[i].isNull() || map->Textures()[i].name.empty())
				continue;
			const QString key =
				QString::fromStdString(map->Textures()[i].name).toLower();
			if (wadImages.find(key) == wadImages.end())
				stillMissing.insert(key);
		}
		if (!stillMissing.empty())
		{
		struct WadRef
		{
			const cso_pak::PakArchive *pak = nullptr;
			const cso_pak::NarArchive *nar = nullptr;
			size_t entryIndex = 0;
		};
		std::vector<WadRef> wadRefs;
		std::set<QString> seenWads;
		for (const QString &w : usedWads)
			seenWads.insert(w.toLower());
		std::vector<const cso_pak::PakArchive *> sources;
		for (const auto &a : archives_)
			sources.push_back(a.get());
		for (const auto &a : neighborArchives_)
			sources.push_back(a.get());
		for (const cso_pak::PakArchive *archive : sources)
		{
			const auto &entries = archive->Entries();
			for (size_t ei = 0; ei < entries.size(); ++ei)
			{
				const QString base = QFileInfo(QString::fromStdU16String(
					entries[ei].path)).fileName();
				if (!base.endsWith(QStringLiteral(".wad"), Qt::CaseInsensitive))
					continue;
				if (!seenWads.insert(base.toLower()).second)
					continue;
				WadRef ref;
				ref.pak = archive;
				ref.entryIndex = ei;
				wadRefs.push_back(ref);
			}
		}
		std::vector<const cso_pak::NarArchive *> narSources;
		for (const auto &a : narArchives_)
			narSources.push_back(a.get());
		for (const auto &a : neighborNarArchives_)
			narSources.push_back(a.get());
		for (const cso_pak::NarArchive *archive : narSources)
		{
			const auto &entries = archive->Entries();
			for (size_t ei = 0; ei < entries.size(); ++ei)
			{
				const QString base = QString::fromStdString(
					cso_pak::NarArchive::FileNameOf(entries[ei].path));
				if (!base.endsWith(QStringLiteral(".wad"), Qt::CaseInsensitive))
					continue;
				if (!seenWads.insert(base.toLower()).second)
					continue;
				WadRef ref;
				ref.nar = archive;
				ref.entryIndex = ei;
				wadRefs.push_back(ref);
			}
		}
			if (!wadRefs.empty())
			{
				QProgressDialog progress(tr("Searching other .wad files for %1 missing textures...")
					.arg(stillMissing.size()), tr("Cancel"), 0,
					static_cast<int>(wadRefs.size()), this);
				progress.setWindowModality(Qt::WindowModal);
				progress.setMinimumDuration(400);
				for (size_t wi = 0; wi < wadRefs.size(); ++wi)
				{
					if (stillMissing.empty())
						break;
					progress.setValue(static_cast<int>(wi));
					QApplication::processEvents();
					if (progress.wasCanceled())
						break;
				const auto &ref = wadRefs[wi];
				try
				{
					std::vector<uint8_t> wadBytes;
					if (ref.pak != nullptr)
					{
						wadBytes = ref.pak->ExtractEntry(
							ref.pak->Entries()[ref.entryIndex]);
					}
					else
					{
						wadBytes = ref.nar->ExtractEntry(
							ref.nar->Entries()[ref.entryIndex]);
					}
					cso_gui::Wad3Archive wad = cso_gui::Wad3Archive::Load(
						std::move(wadBytes));
						for (const auto &lump : wad.Entries())
						{
							const QString key =
								QString::fromStdString(lump.name).toLower();
							const auto need = stillMissing.find(key);
							if (need == stillMissing.end() ||
								wadImages.find(key) != wadImages.end())
								continue;
							try
							{
								const QImage image = cso_gui::LoadWadMiptex(
									wad.ExtractEntry(lump), lump.name);
								if (image.isNull())
									continue;
								wadImages.emplace(key, image);
								stillMissing.erase(need);
								if (stillMissing.empty())
									break;
							}
							catch (const std::exception &)
							{
								continue;
							}
						}
					}
					catch (const std::exception &)
					{
						continue;  // Unparseable .wad, keep looking.
					}
				}
				progress.setValue(static_cast<int>(wadRefs.size()));
			}
		}
	}

	// Loose image files (remembered texture folder + bundled textures/)
	// as a last resort, e.g. for hand-extracted map textures.
	QStringList looseFiles;
	{
		QStringList looseDirs;
		if (!textureFolder_.isEmpty() && QDir(textureFolder_).exists())
			looseDirs << textureFolder_;
		const QString bundled = BundledTextureFolder();
		if (!bundled.isEmpty() && !looseDirs.contains(bundled, Qt::CaseInsensitive))
			looseDirs << bundled;
		for (const QString &dir : looseDirs)
		{
			QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
				looseFiles << it.next();
		}
	}

	for (size_t i = 0; i < map->Textures().size(); ++i)
	{
		if (!images[i].isNull())
			continue;
		const std::string &slotName = map->Textures()[i].name;
		if (slotName.empty())
			continue;
		const QString key = QString::fromStdString(slotName).toLower();
		const auto it = wadImages.find(key);
		if (it != wadImages.end())
		{
			images[i] = it->second;
			++wadCount;
			continue;
		}
		bool looseFound = false;
		for (const QString &candidate : looseFiles)
		{
			if (!FileMatchesTextureName(candidate, slotName))
				continue;
			const QImage image = DecodeImageFile(candidate);
			if (image.isNull())
				continue;
			images[i] = image;
			++looseCount;
			looseFound = true;
			break;
		}
		if (!looseFound)
		{
			++missingCount;
			if (missingNames.size() < 10)
				missingNames << QString::fromStdString(slotName);
		}
	}

	currentBsp_ = map;
	bspWireframeCheck_->setChecked(false);
	bspView_->SetMap(map, images);

	// Skybox: worldspawn "skyname" -> gfx/env/<sky>{ft,bk,lf,rt,up,dn}.tga|.bmp
	// (CSO ships sets for cstrike/dstrike/estrike/valve; some use "<sky>_ft").
	// Sky faces themselves are TEX_SPECIAL and never drawn, so without this the
	// background is just the clear color.
	{
		QString skyName;
		const std::string &ents = map->EntitiesText();
		const size_t keyPos = ents.find("\"skyname\"");
		if (keyPos != std::string::npos)
		{
			const size_t q1 = ents.find('"', keyPos + 9);
			const size_t q2 = q1 == std::string::npos
				? std::string::npos : ents.find('"', q1 + 1);
			if (q2 != std::string::npos)
				skyName = QString::fromStdString(ents.substr(q1 + 1, q2 - q1 - 1)).trimmed();
		}
		if (!skyName.isEmpty())
		{
			static const char *const kSuffixes[6] = { "ft", "bk", "lf", "rt", "up", "dn" };
			std::array<QImage, 6> faces;
			int found = 0;
			for (int i = 0; i < 6; ++i)
			{
				const QString suffix = QString::fromLatin1(kSuffixes[i]);
				const QStringList candidates = {
					QStringLiteral("gfx/env/%1%2.tga").arg(skyName, suffix),
					QStringLiteral("gfx/env/%1_%2.tga").arg(skyName, suffix),
					QStringLiteral("gfx/env/%1%2.bmp").arg(skyName, suffix),
					QStringLiteral("gfx/env/%1_%2.bmp").arg(skyName, suffix) };
				for (const QString &path : candidates)
				{
					const FoundEntry hit = FindArchiveEntryByPath(path);
					if (!hit.valid())
						continue;
					try
					{
						faces[static_cast<size_t>(i)] =
							DecodeImageData(hit.extract(), hit.extension());
					}
					catch (const std::exception &)
					{
					}
					if (!faces[static_cast<size_t>(i)].isNull())
					{
						++found;
						break;
					}
				}
			}
			if (found > 0)
				bspView_->SetSkybox(faces);
		}
	}

	// Placed models and sprites from point entities (cycler_mdl /
	// cycler_sprite .mdl props, env_sprite .spr glows, supply boxes...).
	// The web reference viewer draws none of these; CSO maps rely on them
	// for cars, trees, fences, grass and glows, so resolve and bake them
	// here with the same StudioModel machinery as the .mdl preview.
	// Models are lit by the lightmap texel under them (engine R_LightPoint):
	// the same atlas the viewer builds, so UVs from this sampler address it.
	const cso_gui::BspLightmapData placedLightmaps = cso_gui::BuildBspLightmaps(*map);
	const cso_gui::BspLightSampler placedLightSampler(*map, placedLightmaps);
	int placedModelsShown = 0;
	int placedSpritesShown = 0;
	int placedModelsMissing = 0;  // instances (one file can repeat per copy)
	int placedMissingFiles = 0;   // distinct files
	QStringList placedMissingNames;
	{
		const std::vector<cso_gui::BspPlacedEntity> &placed = map->PlacedEntities();
		if (!placed.empty())
		{
			QProgressDialog progress(tr("Placing %1 models on the map...")
				.arg(placed.size()), tr("Cancel"), 0,
				static_cast<int>(placed.size()), this);
			progress.setWindowModality(Qt::WindowModal);
			progress.setMinimumDuration(400);

			std::vector<QImage> placedTextures;
			std::vector<cso_gui::BspCpuMesh> placedMeshes;
			// Sprites are collected and uploaded in one batch at the end:
			// every Add* call resets the GPU state, so adding them one by
			// one would rebuild the whole scene per sprite on next paint.
			std::vector<cso_gui::BspViewWidget::PlacedSprite> placedSprites;
			// Decoded first frame per .spr path (null = undecodable), so a
			// sprite repeated hundreds of times is decoded once and all
			// instances share one image (and one GL texture).
			struct SpriteData
			{
				QImage image;
				int type = 2;
				int originX = 0;
				int originY = 0;
			};
			std::map<QString, SpriteData> spriteFrames;

			// Per model file: parsed model (cached for the session) + base
			// texture slot in placedTextures.
			struct PlacedFile
			{
				std::shared_ptr<cso_gui::StudioModel> model;
				int textureBase = 0;
			};
			std::map<QString, PlacedFile> files;

			// External MDL textures (CSO "#" convention or placeholders):
			// same search as the standalone .mdl preview (other paks and
			// .nar archives, then the remembered/bundled loose files
			// already listed above).
			const auto resolveModelTextures =
				[&](const std::shared_ptr<cso_gui::StudioModel> &model)
			{
				for (size_t ti = 0; ti < model->Textures().size(); ++ti)
				{
					const auto &tex = model->Textures()[ti];
					if (!TextureLooksExternal(tex))
						continue;
					const FoundEntry found = FindTextureFile(tex.name);
					bool bound = false;
					if (found.valid())
					{
						try
						{
							const QImage image = DecodeImageData(
								found.extract(), found.extension());
							if (!image.isNull())
							{
								model->SetTextureImage(static_cast<int>(ti), image);
								bound = true;
							}
						}
						catch (const std::exception &)
						{
						}
					}
					if (bound)
						continue;
					for (const QString &candidate : looseFiles)
					{
						if (!FileMatchesTextureName(candidate, tex.name))
							continue;
						const QImage image = DecodeImageFile(candidate);
						if (image.isNull())
							continue;
						model->SetTextureImage(static_cast<int>(ti), image);
						break;
					}
				}
			};

			for (size_t pi = 0; pi < placed.size(); ++pi)
			{
				progress.setValue(static_cast<int>(pi));
				QApplication::processEvents();
				if (progress.wasCanceled())
					break;
				const cso_gui::BspPlacedEntity &pe = placed[pi];
				if (pe.model.empty() || pe.scale <= 0.0f)
					continue;
				// Particle emitters are logic, not static visuals (only
				// their spawned particles show in-game); skip the sprite.
				if (pe.classname == "particle_emitter")
					continue;
				// env_sprite starts invisible until triggered unless its
				// "Start on" spawnflag (bit 1) is set -- event smokes and
				// ignite-fires must not show on the untouched map. (env_glow
				// has no such flag and always renders.)
				if (pe.classname == "env_sprite" && !(pe.spawnFlags & 1))
					continue;

				const QString key = QString::fromStdString(pe.model).toLower();

				cso_gui::BspRenderClass rc = cso_gui::BspRenderClass::Opaque;
				float opacity = 1.0f;
				if (pe.renderMode != 0)
				{
					if (pe.renderAmt <= 0)
						continue;
					// 5 = additive, 3 = glow (env_glow): both add light.
					if (pe.renderMode == 5 || pe.renderMode == 3)
						rc = cso_gui::BspRenderClass::Additive;
					else if (pe.renderMode == 2 && pe.renderAmt < 255)
						rc = cso_gui::BspRenderClass::Blend;
					// rendermode 2 "texture" at renderamt 255 is fully opaque
					// (zs_crack1's osprey): drawing it as a blend mesh (no
					// depth write, no sorting) shows the inner parts of the
					// model through its hull, so keep it in the opaque pass.
					// rendermode 4 "solid" is opaque + alpha test: blending
					// it (no depth write) shows a model's inner faces
					// through its outer ones.
					if (pe.renderMode != 4)
						opacity = pe.renderAmt / 255.0f;
				}

				if (pe.isSprite)
				{
					// env_sprite glow: camera-facing quad, first frame.
					const FoundEntry found = FindArchiveEntryByPath(key);
					if (!found.valid())
					{
						++placedModelsMissing;
						if (!placedMissingNames.contains(key, Qt::CaseInsensitive))
						{
							++placedMissingFiles;
							if (placedMissingNames.size() < 10)
								placedMissingNames << key;
						}
						continue;
					}
					auto sit = spriteFrames.find(key);
					if (sit == spriteFrames.end())
					{
						SpriteData decoded;
						try
						{
							const auto sprite =
								cso_gui::LoadSpriteImage(found.extract());
							if (sprite && !sprite->frames.empty())
							{
								decoded.image = sprite->frames[0].image;
								decoded.type = sprite->type;
								decoded.originX = sprite->frames[0].originX;
								decoded.originY = sprite->frames[0].originY;
							}
						}
						catch (const std::exception &)
						{
						}
						sit = spriteFrames.emplace(key, decoded).first;
					}
					if (sit->second.image.isNull())
						continue;
					const QImage &frame = sit->second.image;
					// HL sprite scale: world size ~= frame pixels * scale.
					const float h = frame.height() * pe.scale;
					const float w = frame.width() * pe.scale;
					cso_gui::BspViewWidget::PlacedSprite placed;
					placed.image = frame;
					placed.origin = QVector3D(pe.origin[0], pe.origin[1], pe.origin[2]);
					placed.widthUnits = w;
					placed.heightUnits = h;
					placed.orientation = sit->second.type;
					// Frame origin: x = left edge, y = top edge, in pixels
					// from the entity origin (centered sprites: -w/2, +h/2).
					if (sit->second.originX != 0 || sit->second.originY != 0)
					{
						placed.left = sit->second.originX * pe.scale;
						placed.right = placed.left + w;
						placed.up = sit->second.originY * pe.scale;
						placed.down = placed.up - h;
					}
					placed.angles[0] = pe.angles[0];
					placed.angles[1] = pe.angles[1];
					placed.angles[2] = pe.angles[2];
					placed.renderClass = rc;
					placed.opacity = opacity;
						// Normal/Texture sprites (grass, bushes) are lit by the
						// lightmap under the entity, like placed models; glows
						// (additive/glow rendermodes) stay self-luminous.
						if (rc != cso_gui::BspRenderClass::Additive &&
							!placedLightmaps.atlas.isNull())
						{
							float spriteUv[2] = {
								0.5f / placedLightmaps.atlas.width(),
								0.5f / placedLightmaps.atlas.height() };  // white texel
							// Map has baked light but nothing lit below: black
							// texel (minimum ambient), as for models.
							if (!map->Lighting().empty() &&
								placedLightmaps.atlas.height() >= 2)
								spriteUv[1] = 1.5f / placedLightmaps.atlas.height();
							float found[2];
							// Probe from the entity origin (a ray goes straight
							// down to the floor under the sprite).
							if (placedLightSampler.SampleNear(pe.origin[0], pe.origin[1],
								pe.origin[2], 1024.0f, found))
							{
								spriteUv[0] = found[0];
								spriteUv[1] = found[1];
							}
							placed.lit = true;
							placed.lightUv[0] = spriteUv[0];
							placed.lightUv[1] = spriteUv[1];
						}
					placedSprites.push_back(std::move(placed));
					++placedSpritesShown;
					continue;
				}

				auto fit = files.find(key);
				if (fit == files.end())
				{
					const FoundEntry found = FindArchiveEntryByPath(key);
					if (!found.valid())
					{
						++placedModelsMissing;
						if (!placedMissingNames.contains(key, Qt::CaseInsensitive))
						{
							++placedMissingFiles;
							if (placedMissingNames.size() < 10)
								placedMissingNames << key;
						}
						continue;
					}
					std::shared_ptr<cso_gui::StudioModel> model;
					const QString cacheKey = found.sourceName().toLower();
					const auto cit = placedModelCache_.find(cacheKey);
					if (cit != placedModelCache_.end())
					{
						model = cit->second;
					}
					else
					{
						try
						{
							model = std::make_shared<cso_gui::StudioModel>();
							model->Load(found.extract());
						}
						catch (const std::exception &)
						{
							continue;
						}
						if (!model->IsValid())
							continue;
						if (model->Textures().empty())
						{
							if (auto twin = LoadTwinTextureModel(
								QString::fromStdString(pe.model)))
								model->ImportTexturesFrom(*twin);
						}
						resolveModelTextures(model);
						placedModelCache_.emplace(cacheKey, model);
					}
					PlacedFile pf;
					pf.model = model;
					pf.textureBase = static_cast<int>(placedTextures.size());
					for (const auto &tex : model->Textures())
						placedTextures.push_back(tex.image);
					fit = files.emplace(key, std::move(pf)).first;
				}
				const std::shared_ptr<cso_gui::StudioModel> &model = fit->second.model;
				// Pose: entity sequence frame, else rest pose. Applies onto
				// the cached model, baked out immediately below.
				try
				{
					// The engine resets an out-of-range "sequence" to 0 (e.g.
					// the zs_electron_guard models have only "idle" but the
					// entities say sequence 5); the rest pose is not that.
					int sequence = pe.sequence;
					if (sequence < 0 ||
						sequence >= static_cast<int>(model->Sequences().size()))
						sequence = 0;
					// A dying player model placed as a prop (zs_decoy's dead
					// soldier by the radio, sequence "death"): the game has
					// played the animation to its end, so show the last frame
					// (the body on the ground), not the standing first one.
					float frame = 0.0f;
					{
						const auto &seq = model->Sequences()[static_cast<size_t>(sequence)];
						std::string label = seq.label;
						for (char &ch : label)
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						const bool isPlayerModel = pe.model.find("player/") != std::string::npos;
						const bool isHostage = pe.model.find("hostage") != std::string::npos;
						// CS player models end their sequence list with the
						// non-looping dying animations: death1..3, crouch_die and
						// the hit-direction ones (head, gutshot, left, right,
						// back, forward).
						const bool dying =
							label.find("death") != std::string::npos ||
							label.find("die") == 0 || label.find("_die") != std::string::npos ||
							label == "head" || label == "gutshot" || label == "left" ||
							label == "right" || label == "back" || label == "forward";
						if ((isPlayerModel || isHostage) && seq.numframes > 1 && dying)
							frame = static_cast<float>(seq.numframes - 1);
						// Sequence-only model switch (zs_victor2 jeeps exploding in the cut
						// scene): the animation is over, show its last frame (the wreck).
						if (pe.holdLastFrame && seq.numframes > 1)
							frame = static_cast<float>(seq.numframes - 1);
					}
					model->ApplyFrame(sequence, frame);
				}
				catch (const std::exception &)
				{
					continue;
				}
				// Skin family -> texture index (mirror of the .mdl viewer).
				const int numSkins = model->SkinFamilyCount();
				const auto &skinRefs = model->SkinRefs();
				const int perFamily = skinRefs.empty() ? 0 :
					static_cast<int>(skinRefs.size()) / std::max(numSkins, 1);
				const int family = numSkins <= 0 ? 0 : std::clamp(pe.skin, 0, numSkins - 1);
				const auto skinToTex = [&](int skinref) -> int
				{
					if (numSkins <= 0 || perFamily <= 0)
						return skinref;
					const int idx = family * perFamily + skinref;
					if (idx < 0 || idx >= static_cast<int>(skinRefs.size()))
						return -1;
					const int ref = skinRefs[static_cast<size_t>(idx)];
					if (ref < 0 || ref >= static_cast<int>(model->Textures().size()))
						return -1;
					return ref;
				};
				// CSO renderfx 22 = light multiplier: the model's lighting is
				// scaled by rendercolor/255 (boss and ruin props on zs_electron2
				// use ~100/255 and are much darker in game than the lightmap
				// alone gives). "0 0 0" means no multiplier.
				constexpr int kRenderFxLightMultiplier = 22;
				const bool lightMulOn = pe.renderFx == kRenderFxLightMultiplier &&
					(pe.renderColor[0] > 0.0f || pe.renderColor[1] > 0.0f ||
						pe.renderColor[2] > 0.0f);
				// Light under the model's feet (a little above the origin so
				// a model standing exactly on the floor still finds it).
				float modelLightUv[2] = { 0.5f, 0.5f };
				if (!placedLightmaps.atlas.isNull())
				{
					modelLightUv[0] = 0.5f / placedLightmaps.atlas.width();
					modelLightUv[1] = 0.5f / placedLightmaps.atlas.height();  // white texel
					// The map HAS baked light but nothing lit lies below this
					// model (e.g. zs_electron2: the arena floor is unlit, so
					// almost every prop). The engine's light probe then finds
					// no light and the model only gets its minimum ambient,
					// not full brightness: default to the black texel (row 1)
					// and let a found sample override it below.
					if (!map->Lighting().empty() && placedLightmaps.atlas.height() >= 2)
						modelLightUv[1] = 1.5f / placedLightmaps.atlas.height();
					float uv[2];
					// Nothing under the origin (the map compiler drops faces
					// buried in solid void, so towers/props on the map
					// boundary have no floor): borrow the nearest lit floor
					// instead of falling back to the fullbright white texel,
					// which made exactly those props glow.
					if (placedLightSampler.SampleNear(pe.origin[0], pe.origin[1],
						pe.origin[2] + 16.0f, 1024.0f, uv))
					{
						modelLightUv[0] = uv[0];
						modelLightUv[1] = uv[1];
					}
				}
				// HLSDK AngleMatrix order: yaw about Z, then pitch about Y,
				// then roll about X (model frame matches world frame).
				QMatrix4x4 m;
				m.translate(pe.origin[0], pe.origin[1], pe.origin[2]);
				m.rotate(pe.angles[1], 0.0f, 0.0f, 1.0f);
				// The engine negates pitch for studio entities (Xash3D
				// R_StudioSetUpTransform: "stupid Valve bug"). Set
				// kInvertStudioPitch to false to undo it.
				constexpr bool kInvertStudioPitch = true;
				m.rotate(kInvertStudioPitch ? -pe.angles[0] : pe.angles[0],
					0.0f, 1.0f, 0.0f);
				m.rotate(pe.angles[2], 1.0f, 0.0f, 0.0f);
				m.scale(pe.scale);
				// Bake all bodyparts at submodel 0 ("body" groups need the
				// header base values the parser doesn't expose; map props
				// use body 0).
				for (const auto &bp : model->BodyParts())
				{
					if (bp.models.empty())
						continue;
					// "body" selects the submodel of each bodypart.
					const auto &sub = bp.models[static_cast<size_t>(
						(pe.body / std::max(bp.base, 1)) %
						static_cast<int>(bp.models.size()))];
					for (const auto &mesh : sub.meshes)
					{
						if (mesh.triangles.empty())
							continue;
						const int texIndex = skinToTex(mesh.skinref);
						float uScale = 1.0f, vScale = 1.0f;
						bool texAdditive = false;
						bool texChrome = false;
						if (texIndex >= 0 &&
							texIndex < static_cast<int>(model->Textures().size()))
						{
							const auto &t = model->Textures()[static_cast<size_t>(texIndex)];
							if (t.width > 0)
								uScale = 1.0f / t.width;
							if (t.height > 0)
								vScale = 1.0f / t.height;
							texChrome = (t.flags & 0x02) != 0;  // env-mapped
							// Additive rule mirror (.mdl viewer): flag 0x20
							// or a $0a_/$0b_/ef_ name prefix.
							if (t.flags & 0x20)
								texAdditive = true;
							else
							{
								const QString nm = QString::fromStdString(t.name);
								if (nm.startsWith(QStringLiteral("$0a"), Qt::CaseInsensitive) ||
									nm.startsWith(QStringLiteral("$0b"), Qt::CaseInsensitive) ||
										nm.startsWith(QStringLiteral("ef_"), Qt::CaseInsensitive))
									texAdditive = true;
							}
						}
						cso_gui::BspCpuMesh cpu;
						cpu.texture = texIndex >= 0 ?
							fit->second.textureBase + texIndex : -1;
						// An additive texture (flag / $0a / $0b / ef_ prefix) is
						// additive whatever the entity's rendermode says (the
						// wind vortices on zs_biolabs1 are rendermode 2).
						cpu.renderClass = texAdditive
							? cso_gui::BspRenderClass::Additive
							: rc;
						cpu.opacity = opacity;
						cpu.studio = true;
						cpu.chrome = texChrome;
						cpu.instance = static_cast<int>(pi);
						if (lightMulOn)
							for (int k = 0; k < 3; ++k)
								cpu.lightMul[k] = pe.renderColor[k] / 255.0f;
						cpu.verts.reserve(mesh.triangles.size() * 3 * 8);
						for (const auto &tri : mesh.triangles)
						{
							const cso_gui::StudioModel::Vec3 *p[3] =
								{ &tri.a, &tri.b, &tri.c };
							const cso_gui::StudioModel::Vec3 *n[3] =
								{ &tri.na, &tri.nb, &tri.nc };
							const float s[3] = { tri.sa, tri.sb, tri.sc };
							const float t[3] = { tri.ta, tri.tb, tri.tc };
							for (int i = 0; i < 3; ++i)
							{
								const QVector3D wp = m.map(QVector3D(
									p[i]->x, p[i]->y, p[i]->z));
								QVector3D wn = m.mapVector(QVector3D(
									n[i]->x, n[i]->y, n[i]->z));
								if (wn.lengthSquared() > 1e-12f)
									wn.normalize();
								cpu.verts.push_back(wp.x());
								cpu.verts.push_back(wp.y());
								cpu.verts.push_back(wp.z());
								cpu.verts.push_back(wn.x());
								cpu.verts.push_back(wn.y());
								cpu.verts.push_back(wn.z());
								cpu.verts.push_back(s[i] * uScale);
								cpu.verts.push_back(t[i] * vScale);
							}
						}
						cpu.lightUv.reserve(cpu.verts.size() / 8 * 2);
						for (size_t vi = 0; vi < cpu.verts.size() / 8; ++vi)
						{
							cpu.lightUv.push_back(modelLightUv[0]);
							cpu.lightUv.push_back(modelLightUv[1]);
						}
						placedMeshes.push_back(std::move(cpu));
					}
				}
				++placedModelsShown;
			}
			progress.setValue(static_cast<int>(placed.size()));
			if (!placedTextures.empty() || !placedMeshes.empty())
				bspView_->AddPlacedModels(std::move(placedTextures),
					std::move(placedMeshes));
			if (!placedSprites.empty())
				bspView_->AddPlacedSprites(std::move(placedSprites));
		}
	}

	QString info;
	// Count entities by classname occurrences (same quick metric as tests).
	size_t entityCount = 0;
	for (size_t pos = 0;
		(pos = map->EntitiesText().find("\"classname\"", pos)) != std::string::npos;
		++pos, ++entityCount)
	{
	}
	info = tr("Faces: %1\nModels: %2\nEntities: %3\nTextures: %4 embedded, %5 from .wad, %6 loose files, %7 missing")
		.arg(map->Faces().size()).arg(map->Models().size()).arg(entityCount)
		.arg(embeddedCount).arg(wadCount).arg(looseCount).arg(missingCount);
	if (!usedWads.isEmpty())
		info += tr("\nWADs: %1").arg(usedWads.join(QStringLiteral(", ")));
	if (!missingWads.isEmpty())
		info += tr("\nWADs not found: %1").arg(missingWads.join(QStringLiteral(", ")));
	if (placedModelsShown > 0 || placedSpritesShown > 0 || placedModelsMissing > 0)
	{
		info += tr("\nPlaced: %1 models, %2 sprites, %3 files missing (%4 instances)")
			.arg(placedModelsShown).arg(placedSpritesShown)
			.arg(placedMissingFiles).arg(placedModelsMissing);
		if (!placedMissingNames.isEmpty())
			info += tr("\nMissing models: %1%2").arg(placedMissingNames.join(QStringLiteral(", ")),
				placedMissingFiles > placedMissingNames.size() ? QStringLiteral(", ...") : QString());
		if (placedMissingFiles > 0 && narArchives_.empty())
			info += tr("\nTip: most map models live in .nar archives \u2014 reopen with \"Open Paks + All Nars...\"");
	}
	if (!missingNames.isEmpty())
		info += tr("\nMissing: %1%2").arg(missingNames.join(QStringLiteral(", ")),
			missingCount > missingNames.size() ? QStringLiteral(", ...") : QString());
	bspInfoLabel_->setText(info);

	stack_->setCurrentWidget(bspPageContainer_);
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

void MainWindow::ShowSprite(const FoundEntry &ref, const std::vector<uint8_t> &data)
{
	std::shared_ptr<cso_gui::SpriteImage> sprite;
	try
	{
		sprite = cso_gui::LoadSpriteImage(data);
	}
	catch (const std::exception &ex)
	{
		currentSprite_.reset();
		ShowEntryProperties(ref, tr("Failed to parse this .spr sprite: %1").arg(QString::fromUtf8(ex.what())));
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

void MainWindow::ShowNarProperties(const cso_pak::NarArchive::Entry &entry, const QString &note)
{
	QList<QPair<QString, QString>> rows;
	const auto addRow = [&rows](const QString &property, const QString &value)
	{
		rows.append({ property, value });
	};

	const QString path = QString::fromStdString(entry.path);

	addRow(tr("Path"), path);
	addRow(tr("Extension"), ExtensionOf(entry).isEmpty() ? tr("(none)") : ExtensionOf(entry));
	addRow(tr("Extracted size"), FormatSize(entry.extractedSize));
	addRow(tr("Stored size"), FormatSize(entry.storedSize));
	addRow(tr("Data block offset"), QString::number(entry.offset));
	const char *typeName = "?";
	switch (entry.type)
	{
	case 0: typeName = "raw"; break;
	case 1: typeName = "encoded (XOR)"; break;
	case 2: typeName = "encoded + compressed (XOR + LZSS)"; break;
	default: break;
	}
	addRow(tr("Storage"), tr("%1 (type %2)").arg(QLatin1String(typeName)).arg(entry.type));
	addRow(tr("Entry checksum"), QStringLiteral("0x%1").arg(entry.checksum, 8, 16, QLatin1Char('0')).toUpper());

	if (!note.isEmpty())
		addRow(tr("Note"), note);

	ShowGenericProperties(rows);
}

void MainWindow::ShowEntryProperties(const FoundEntry &ref, const QString &note)
{
	if (ref.narEntry != nullptr)
	{
		ShowNarProperties(*ref.narEntry, note);
		return;
	}
	if (ref.pakEntry != nullptr && ref.pak != nullptr)
	{
		ShowProperties(*ref.pakEntry, note);
		return;
	}
	ShowPlaceholder(tr("Select a file in the tree to preview it."));
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
		const QImage image = LoadWadLumpImageForView(data, lump.name);
		ShowImage(image);
		return;
	}
	catch (const std::exception &)
	{
	}

	ShowWadLumpProperties(lump, tr("Not a recognized texture (miptex) lump."));
}

void MainWindow::ShowPreviewForFoundEntry(const FoundEntry &ref)
{
	StopMediaPlayback();
	StopModelPlayback();
	StopSpritePlayback();

	if (ref.pakEntry != nullptr &&
		(ref.pakEntry->type & cso_pak::EntryTypeCompressed) != 0)
	{
		ShowEntryProperties(ref, tr("This entry is stored compressed; content preview isn't supported yet."));
		return;
	}

	std::vector<uint8_t> data;
	try
	{
		data = ref.extract();
	}
	catch (const std::exception &ex)
	{
		ShowEntryProperties(ref, tr("Failed to decrypt this entry: %1").arg(QString::fromUtf8(ex.what())));
		return;
	}

	const QString ext = ref.extension();

	if (ext == QLatin1String("tga"))
	{
		QString error;
		const QImage image = tga::Load(data, &error);
		if (!image.isNull())
		{
			ShowImage(image);
			return;
		}

		ShowEntryProperties(ref, tr("Failed to decode TGA image: %1").arg(error));
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
			ShowEntryProperties(ref, tr("Failed to decode DDS image: %1").arg(QString::fromUtf8(ex.what())));
			return;
		}
	}

	static const QStringList nativeImageExtensions = {
		QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
		QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("ppm"),
		QStringLiteral("ico"),
	};
	if (nativeImageExtensions.contains(ext))
	{
		const QImage image = DecodeImageData(data, ext);
		if (!image.isNull())
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
		ShowCso(ref, std::move(data));
		return;
	}

	if (ext == QLatin1String("wav") || ext == QLatin1String("webm"))
	{
		ShowMedia(data, ref.fileName(), ext);
		return;
	}

	if (ext == QLatin1String("mdl"))
	{
		// Remember which archive this model came from so texture lookup
		// can prefer the same archive before searching the neighbours.
		currentModelPakIndex_ = -1;
		currentModelNarIndex_ = -1;
		for (int i = 0; i < static_cast<int>(archives_.size()); ++i)
		{
			if (archives_[static_cast<size_t>(i)].get() == ref.pak)
			{
				currentModelPakIndex_ = i;
				break;
			}
		}
		for (int i = 0; i < static_cast<int>(narArchives_.size()); ++i)
		{
			if (narArchives_[static_cast<size_t>(i)].get() == ref.nar)
			{
				currentModelNarIndex_ = i;
				break;
			}
		}
		ShowModel(ref, data);
		return;
	}

	if (ext == QLatin1String("spr"))
	{
		ShowSprite(ref, data);
		return;
	}

	if (ext == QLatin1String("bsp"))
	{
		ShowBsp(ref, data);
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
			ShowEntryProperties(ref, tr("Failed to parse this WAD archive: %1").arg(QString::fromUtf8(ex.what())));
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
	ShowEntryProperties(ref);
}

QString MainWindow::ExtensionOf(const cso_pak::PakArchive::Entry &entry)
{
	const QString name = QFileInfo(QString::fromStdU16String(entry.path)).fileName();
	const int dot = name.lastIndexOf(QLatin1Char('.'));
	if (dot <= 0 || dot == name.size() - 1)
		return QString();

	return name.mid(dot + 1).toLower();
}

QString MainWindow::ExtensionOf(const cso_pak::NarArchive::Entry &entry)
{
	const QString name = QFileInfo(QString::fromStdString(entry.path)).fileName();
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

QString MainWindow::SanitizedRelativePath(const std::string &entryPath)
{
	// .nar paths are byte strings (EUC-KR); the game's own paths are ASCII,
	// anything else falls back to '?' just like the tree display does.
	std::u16string wide;
	wide.reserve(entryPath.size());
	for (const unsigned char c : entryPath)
		wide.push_back(c < 128 ? static_cast<char16_t>(c) : u'?');
	return SanitizedRelativePath(wide);
}
