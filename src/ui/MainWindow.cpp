#include "ui/MainWindow.h"
#include "media/RandomAccessDecoder.h"
#include "ui/NavigationBar.h"
#include "ui/Playback.h"
#include "ui/RegionPanel.h"
#include "ui/SegmentPanel.h"
#include "ui/StreamPanel.h"
#include "ui/Timeline.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QMimeData>
#include <QSettings>
#include <QTreeWidget>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTime>
#include <QToolButton>
#include <QVBoxLayout>

namespace adscutter {

namespace {

constexpr int64_t kPreviewUs = 3'000'000; // smart play: before and after each cue or cut
constexpr int kMarkerMs = 1000;           // how long a preview marker stays on the video

// Preview marker colours.
const QColor kProgrammeEndsColor(0xf0, 0x50, 0x50);
const QColor kProgrammeResumesColor(0x50, 0xd0, 0x70);
const QColor kCutColor(0xf0, 0xc0, 0x30);

QString formatTime(int64_t us)
{
    if (us < 0)
        return QStringLiteral("--:--:--.---");
    return QTime(0, 0).addMSecs(us / 1000).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString formatDuration(double seconds)
{
    if (seconds < 0)
        return QStringLiteral("unknown duration");
    return formatTime(int64_t(seconds * 1e6));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_videoView(new VideoView(this))
    , m_timeline(new Timeline(this))
    , m_navigation(new NavigationBar(this))
    , m_streamPanel(new StreamPanel(this))
    , m_segmentPanel(new SegmentPanel(this))
    , m_statusInfo(new QLabel(this))
    , m_decoderInfo(new QLabel(this))
    , m_indexProgress(new QProgressBar(this))
    , m_indexCancel(new QToolButton(this))
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_videoView, 1);
    layout->addWidget(m_timeline);
    layout->addWidget(m_navigation);
    setCentralWidget(central);
    m_timeline->setCutList(&m_cutList);

    auto *streamsDock = new QDockWidget(tr("Streams"), this);
    streamsDock->setObjectName(QStringLiteral("streamsDock"));
    streamsDock->setWidget(m_streamPanel);
    streamsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, streamsDock);

    auto *segmentsDock = new QDockWidget(tr("Segments"), this);
    segmentsDock->setObjectName(QStringLiteral("segmentsDock"));
    segmentsDock->setWidget(m_segmentPanel);
    segmentsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, segmentsDock);
    splitDockWidget(streamsDock, segmentsDock, Qt::Horizontal);

    m_regionPanel = new RegionPanel(this);
    m_regionsDock = new QDockWidget(tr("Regions"), this);
    m_regionsDock->setObjectName(QStringLiteral("regionsDock"));
    m_regionsDock->setWidget(m_regionPanel);
    m_regionsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    tabifyDockWidget(streamsDock, m_regionsDock);
    streamsDock->raise();
    connect(m_regionsDock, &QDockWidget::visibilityChanged, this, [this] { updateRegionViews(); });

    m_indexProgress->setRange(0, 1000);
    m_indexProgress->setMaximumWidth(200);
    m_indexProgress->setFormat(tr("Indexing %p%"));
    m_indexProgress->setVisible(false);
    m_indexCancel->setText(tr("Cancel"));
    m_indexCancel->setToolTip(tr("Stop indexing"));
    m_indexCancel->setVisible(false);
    connect(m_indexCancel, &QToolButton::clicked, this, [this] {
        stopIndexing();
        statusBar()->showMessage(tr("Indexing canceled"), 5000);
    });
    m_analysisProgress = new QProgressBar(this);
    m_analysisProgress->setRange(0, 1000);
    m_analysisProgress->setMaximumWidth(200);
    m_analysisProgress->setFormat(tr("Analyzing %p%"));
    m_analysisProgress->setVisible(false);
    m_analysisCancel = new QToolButton(this);
    m_analysisCancel->setText(tr("Cancel"));
    m_analysisCancel->setToolTip(tr("Stop the analysis"));
    m_analysisCancel->setVisible(false);
    connect(m_analysisCancel, &QToolButton::clicked, this, [this] {
        stopAnalysis();
        stopRegionScan();
        statusBar()->showMessage(tr("Analysis canceled"), 5000);
    });
    statusBar()->addPermanentWidget(m_analysisProgress);
    statusBar()->addPermanentWidget(m_analysisCancel);
    statusBar()->addPermanentWidget(m_indexProgress);
    statusBar()->addPermanentWidget(m_indexCancel);
    statusBar()->addPermanentWidget(m_statusInfo);
    statusBar()->addPermanentWidget(m_decoderInfo);
    m_decoderInfo->setToolTip(tr("Video decoder used for display (View → Hardware Decoding)"));
    m_lastDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);

    QSettings settings;
    m_hwPreference = hwPreferenceFromString(settings.value(QStringLiteral("hardwareDecoding"), QStringLiteral("auto")).toString());
    m_recentFiles = settings.value(QStringLiteral("recentFiles")).toStringList();
    m_lastDir = settings.value(QStringLiteral("lastDirectory"), m_lastDir).toString();
    setAcceptDrops(true);
    m_regions = regionsFromJson(QJsonDocument::fromJson(settings.value(QStringLiteral("regions")).toByteArray()).array());
    m_regionPanel->setMinChangeSeconds(settings.value(QStringLiteral("regionMinChangeSeconds"), 30).toInt());
    m_regionPanel->setRegions(m_regions);
    m_regionPanel->setPresetNames(regionPresetNames());
    connect(m_regionPanel, &RegionPanel::drawRequested, this, &MainWindow::drawRegion);
    connect(m_regionPanel, &RegionPanel::findRequested, this, [this] { startRegionScan(); });
    connect(m_regionPanel, &RegionPanel::removeRequested, this, [this](int index) {
        if (index >= 0 && size_t(index) < m_regions.size()) {
            m_regions.erase(m_regions.begin() + index);
            regionsChanged(false);
        }
    });
    connect(m_regionPanel, &RegionPanel::kindToggled, this, [this](int index) {
        if (index < 0 || size_t(index) >= m_regions.size())
            return;
        WatchRegion &r = m_regions[size_t(index)];
        r.kind = r.kind == WatchRegion::Kind::VisibleInAds ? WatchRegion::Kind::VisibleInProgramme
                                                           : WatchRegion::Kind::VisibleInAds;
        regionsChanged(true); // where it changes stays the same; what it means flips
    });
    connect(m_regionPanel, &RegionPanel::presetSaveRequested, this, &MainWindow::saveRegionPreset);
    connect(m_regionPanel, &RegionPanel::presetLoadRequested, this, &MainWindow::loadRegionPreset);
    connect(m_regionPanel, &RegionPanel::presetDeleteRequested, this, &MainWindow::deleteRegionPreset);
    connect(m_regionPanel, &RegionPanel::minChangeChanged, this,
            [](int seconds) { QSettings().setValue(QStringLiteral("regionMinChangeSeconds"), seconds); });

    m_playback = new PlaybackController(this);
    connect(m_playback, &PlaybackController::positionChanged, this, [this](quint32 pos) { showPosition(pos); });
    connect(m_playback, &PlaybackController::message, this, [this](const QString &text) {
        statusBar()->showMessage(text, 5000);
    });
    connect(m_playback, &PlaybackController::rangeStarted, this, [this](int index, int count, const QString &label) {
        statusBar()->showMessage(tr("Preview %1 of %2: %3 · Enter: stop there · Space: stop")
                                     .arg(index + 1).arg(count).arg(label));
    });
    m_markerTimer = new QTimer(this);
    m_markerTimer->setSingleShot(true);
    m_markerTimer->setInterval(kMarkerMs);
    connect(m_markerTimer, &QTimer::timeout, m_videoView, &VideoView::clearMarker);
    connect(m_playback, &PlaybackController::stateChanged, this, [this](bool playing) {
        if (m_frameServer)
            m_frameServer->setPlaying(playing);
        if (!playing)
            m_previewMarks.clear(); // the marker shown, if any, still times out
        m_playAction->setText(playing ? tr("&Pause") : tr("&Play"));
    });

    connect(m_timeline, &Timeline::seekRequested, this, [this](int pos) { goTo(pos); });
    connect(m_videoView, &VideoView::rectangleDrawn, this, &MainWindow::regionDrawn);
    connect(m_videoView, &VideoView::drawingCanceled, this,
            [this] { statusBar()->showMessage(tr("Drawing canceled"), 3000); });
    connect(m_timeline, &Timeline::edgeDragStarted, this, [this](int, bool) { m_cutList.checkpoint(); });
    connect(m_timeline, &Timeline::edgeDragged, this, [this](int segment, bool isIn, int pos) {
        const uint32_t applied = isIn ? m_cutList.moveIn(size_t(segment), uint32_t(pos))
                                      : m_cutList.moveOut(size_t(segment), uint32_t(pos));
        goTo(applied); // show the frame the edge is on
        cutListChanged();
    });
    connect(m_timeline, &Timeline::edgeDragFinished, this, [this] { cutListChanged(); });
    connect(m_segmentPanel, &SegmentPanel::segmentActivated, this, [this](int index) {
        if (index >= 0 && size_t(index) < m_cutList.segments().size())
            goTo(m_cutList.segments()[size_t(index)].in);
    });

    createActions();
    createMenus();
    updateTitle();
    resize(1100, 800);
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
}

MainWindow::~MainWindow()
{
    // Qt hides the docks after this object's members are gone: their
    // signals must not reach it any more.
    m_regionsDock->disconnect(this);
    m_regionPanel->disconnect(this);
    // Stop the worker threads before the widgets they report to go away.
    stopRegionScan();
    stopAnalysis();
    stopExport();
    stopFrameServer();
    stopIndexing();
}

void MainWindow::createActions()
{
    struct Def {
        Step step;
        QString text;
        QString button; // short label for the navigation bar ({} = menu only)
        QKeySequence key;
    };
    const Def defs[] = {
        {Step::Start, tr("Go to &Start"), QStringLiteral("|◀"), Qt::Key_Home},
        {Step::Back1min, tr("Back 1 Minute"), {}, Qt::Key_PageUp},
        {Step::Back10s, tr("Back 10 Seconds"), QStringLiteral("◀◀"), QKeySequence(Qt::SHIFT | Qt::Key_Left)},
        {Step::Back1s, tr("Back 1 Second"), {}, QKeySequence(Qt::CTRL | Qt::Key_Left)},
        {Step::PreviousKeyframe, tr("Previous &Cut Point"), QStringLiteral("◀ Key"), Qt::Key_Down},
        {Step::PreviousFrame, tr("&Previous Frame"), QStringLiteral("◀ Frame"), Qt::Key_Left},
        {Step::NextFrame, tr("&Next Frame"), QStringLiteral("Frame ▶"), Qt::Key_Right},
        {Step::NextKeyframe, tr("Next C&ut Point"), QStringLiteral("Key ▶"), Qt::Key_Up},
        {Step::Forward1s, tr("Forward 1 Second"), {}, QKeySequence(Qt::CTRL | Qt::Key_Right)},
        {Step::Forward10s, tr("Forward 10 Seconds"), QStringLiteral("▶▶"), QKeySequence(Qt::SHIFT | Qt::Key_Right)},
        {Step::Forward1min, tr("Forward 1 Minute"), {}, Qt::Key_PageDown},
        {Step::End, tr("Go to &End"), QStringLiteral("▶|"), Qt::Key_End},
        {Step::PreviousBoundary, tr("Previous Segment &Boundary"), {}, QKeySequence(Qt::CTRL | Qt::Key_Down)},
        {Step::NextBoundary, tr("Next Segment Bou&ndary"), {}, QKeySequence(Qt::CTRL | Qt::Key_Up)},
    };

    QList<QAction *> buttons;
    for (const Def &d : defs) {
        auto *action = new QAction(d.text, this);
        action->setShortcut(d.key);
        action->setToolTip(QStringLiteral("%1 (%2)").arg(QString(d.text).remove(QLatin1Char('&')),
                                                         d.key.toString(QKeySequence::NativeText)));
        action->setEnabled(false);
        const Step step = d.step;
        connect(action, &QAction::triggered, this, [this, step] { navigate(step); });
        m_navigationActions << action;
        addAction(action); // shortcuts work even when the menu is closed

        if (!d.button.isEmpty()) {
            auto *button = new QAction(d.button, this);
            button->setToolTip(action->toolTip());
            button->setEnabled(false);
            connect(action, &QAction::changed, button, [button, action] { button->setEnabled(action->isEnabled()); });
            connect(button, &QAction::triggered, action, &QAction::trigger);
            buttons << button;
        }
    }
    m_navigation->addActions(buttons);

    auto editAction = [this](const QString &text, const QKeySequence &key, auto slot) {
        auto *action = new QAction(text, this);
        action->setShortcut(key);
        action->setEnabled(false);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
        m_editActions << action;
        return action;
    };
    QAction *in = editAction(tr("Mark &In (start of a part to keep)"), Qt::Key_I, &MainWindow::markIn);
    QAction *out = editAction(tr("Mark &Out (end of a part to keep)"), Qt::Key_O, &MainWindow::markOut);
    editAction(tr("&Delete Segment at Playhead"), Qt::Key_Delete, &MainWindow::deleteSegment);
    editAction(tr("&Clear Marks"), Qt::Key_Escape, [this] {
        if (m_videoView->isDrawing()) { // Esc first cancels drawing a region
            m_videoView->setDrawing(false);
            statusBar()->showMessage(tr("Drawing canceled"), 3000);
            return;
        }
        m_cutList.clearMarks();
        cutListChanged();
    });
    m_navigation->addSeparator();
    auto *inButton = new QAction(tr("[ In"), this);
    auto *outButton = new QAction(tr("Out ]"), this);
    for (auto [button, action] : {std::pair{inButton, in}, std::pair{outButton, out}}) {
        button->setToolTip(QStringLiteral("%1 (%2)").arg(QString(action->text()).remove(QLatin1Char('&')),
                                                         action->shortcut().toString(QKeySequence::NativeText)));
        button->setEnabled(false);
        connect(action, &QAction::changed, button, [button, action] { button->setEnabled(action->isEnabled()); });
        connect(button, &QAction::triggered, action, &QAction::trigger);
    }
    m_navigation->addActions({inButton, outButton});

    m_playAction = new QAction(tr("&Play"), this);
    m_playAction->setShortcut(Qt::Key_Space);
    m_playAction->setToolTip(tr("Play / pause (Space)"));
    m_playAction->setEnabled(false);
    connect(m_playAction, &QAction::triggered, this, &MainWindow::togglePlayback);
    addAction(m_playAction);
    m_editActions << m_playAction;
    auto *playButton = new QAction(QStringLiteral("▶ ❚❚"), this);
    playButton->setToolTip(m_playAction->toolTip());
    playButton->setEnabled(false);
    connect(m_playAction, &QAction::changed, playButton, [playButton, this] { playButton->setEnabled(m_playAction->isEnabled()); });
    connect(playButton, &QAction::triggered, m_playAction, &QAction::trigger);
    m_navigation->addSeparator();
    m_navigation->addActions({playButton});

    m_keptOnlyAction = new QAction(tr("Play &Kept Parts Only"), this);
    m_keptOnlyAction->setCheckable(true);
    m_keptOnlyAction->setShortcut(Qt::Key_K);
    m_keptOnlyAction->setToolTip(tr("Preview the result: playback skips the removed parts"));
    connect(m_keptOnlyAction, &QAction::toggled, this, [this](bool) {
        if (m_playback->isPlaying()) { // apply right away
            m_playback->stop();
            togglePlayback();
        }
    });
    addAction(m_keptOnlyAction);

    auto *previewCuesAction = new QAction(tr("Preview C&ues"), this);
    previewCuesAction->setShortcut(Qt::Key_P);
    previewCuesAction->setToolTip(tr("Play 3 s before and after each cue, skipping the rest"));
    connect(previewCuesAction, &QAction::triggered, this, &MainWindow::previewCues);
    auto *previewCutsAction = new QAction(tr("Preview C&uts"), this);
    previewCutsAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_P));
    previewCutsAction->setToolTip(tr("Play each cut as the exported file will show it: the last 3 s of a part, "
                                     "then the first 3 s of the next one"));
    connect(previewCutsAction, &QAction::triggered, this, &MainWindow::previewCuts);
    auto *stopAtAction = new QAction(tr("Stop at the Previewed Cue or Cut"), this);
    stopAtAction->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    connect(stopAtAction, &QAction::triggered, this, &MainWindow::stopAtPreview);
    for (QAction *a : {previewCuesAction, previewCutsAction, stopAtAction}) {
        a->setEnabled(false);
        m_editActions << a;
        addAction(a);
    }
    m_previewActions = {previewCuesAction, previewCutsAction, stopAtAction};

    m_muteAction = new QAction(tr("&Mute"), this);
    m_muteAction->setCheckable(true);
    m_muteAction->setShortcut(Qt::Key_M);
    connect(m_muteAction, &QAction::toggled, m_playback, &PlaybackController::setMuted);
    addAction(m_muteAction);

    m_undoAction = new QAction(tr("&Undo"), this);
    m_undoAction->setShortcuts(QKeySequence::Undo); // all platform bindings, not just the first
    m_undoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);
    m_redoAction = new QAction(tr("&Redo"), this);
    m_redoAction->setShortcuts(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);
    addAction(m_undoAction);
    addAction(m_redoAction);

    m_deinterlaceAction = new QAction(tr("&Deinterlace"), this);
    m_deinterlaceAction->setCheckable(true);
    m_deinterlaceAction->setChecked(true);
    m_deinterlaceAction->setShortcut(Qt::Key_D);
    m_deinterlaceAction->setToolTip(tr("Deinterlace interlaced frames for display (export is not affected)"));
    connect(m_deinterlaceAction, &QAction::toggled, this, [this](bool on) {
        if (m_frameServer)
            m_frameServer->setDeinterlace(on);
    });
    addAction(m_deinterlaceAction);
}

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open Recording…"), QKeySequence::Open, this, &MainWindow::openFileDialog);
    m_recentMenu = fileMenu->addMenu(tr("Open &Recent"));
    updateRecentMenu();
    m_rebuildIndexAction = fileMenu->addAction(tr("Re&build Index"), this, &MainWindow::rebuildIndex);
    m_rebuildIndexAction->setEnabled(false);
    fileMenu->addSeparator();
    QAction *openList = fileMenu->addAction(tr("Open &Cut List…"), this, &MainWindow::openCutListDialog);
    QAction *save = fileMenu->addAction(tr("&Save Cut List"), QKeySequence::Save, this, [this] { saveCutList(false); });
    QAction *saveAs = fileMenu->addAction(tr("Save Cut List &As…"), QKeySequence::SaveAs, this, [this] { saveCutList(true); });
    fileMenu->addSeparator();
    QAction *exportAction = fileMenu->addAction(tr("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
                                                &MainWindow::exportDialog);
    for (QAction *a : {openList, save, saveAs, exportAction}) {
        a->setEnabled(false);
        m_editActions << a;
    }
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    for (int i = 0; i < 4; ++i) // in, out, delete, clear marks
        editMenu->addAction(m_editActions[i]);

    QMenu *goMenu = menuBar()->addMenu(tr("&Go"));
    goMenu->addAction(m_playAction);
    goMenu->addAction(m_keptOnlyAction);
    for (QAction *a : std::as_const(m_previewActions))
        goMenu->addAction(a);
    goMenu->addAction(m_muteAction);
    goMenu->addSeparator();
    for (int i = 0; i < m_navigationActions.size(); ++i) {
        goMenu->addAction(m_navigationActions[i]);
        if (i == 0 || i == 3 || i == 7 || i == 10 || i == 11)
            goMenu->addSeparator();
    }

    QMenu *analysisMenu = menuBar()->addMenu(tr("&Analysis"));
    QAction *analyze = analysisMenu->addAction(tr("Find Breaks by &Fades"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
                                               this, [this] { startAnalysis(); });
    analyze->setToolTip(tr("Find fades to black, silences and ad breaks (takes a few minutes for a film)"));
    QAction *findRegions = analysisMenu->addAction(tr("Find Changes in &Regions"),
                                                   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                                                   [this] { startRegionScan(); });
    findRegions->setToolTip(tr("Find where the watched regions (Regions panel) change"));
    QAction *draw = analysisMenu->addAction(tr("&Draw Region…"), this, &MainWindow::drawRegion);
    analysisMenu->addSeparator();
    QAction *propose = analysisMenu->addAction(tr("&Propose Cut List"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                                               this, &MainWindow::proposeCutList);
    propose->setToolTip(tr("Keep what every method found to be programme (replaces the cut list; Ctrl+Z undoes it)"));
    QMenu *startMenu = analysisMenu->addMenu(tr("&Start Parts at"));
    auto *startGroup = new QActionGroup(this);
    m_startAt = QSettings().value(QStringLiteral("proposalStartAt")).toString() == QStringLiteral("after")
                    ? ProposalSettings::StartAt::CutPointAfter
                    : ProposalSettings::StartAt::CutPointBefore;
    const std::pair<ProposalSettings::StartAt, QString> starts[] = {
        {ProposalSettings::StartAt::CutPointBefore,
         tr("Cut Point &Before the Programme Resumes (may show the end of the break)")},
        {ProposalSettings::StartAt::CutPointAfter,
         tr("Cut Point &After the Programme Resumes (may lose its first second)")},
    };
    for (const auto &[value, text] : starts) {
        QAction *a = startMenu->addAction(text);
        a->setCheckable(true);
        a->setChecked(value == m_startAt);
        startGroup->addAction(a);
        const auto choice = value;
        connect(a, &QAction::triggered, this, [this, choice] {
            m_startAt = choice;
            QSettings().setValue(QStringLiteral("proposalStartAt"),
                                 choice == ProposalSettings::StartAt::CutPointAfter ? QStringLiteral("after")
                                                                                     : QStringLiteral("before"));
        });
    }
    analysisMenu->addSeparator();
    QAction *nextBreak = analysisMenu->addAction(tr("&Next Cue"), QKeySequence(Qt::SHIFT | Qt::Key_Up),
                                                 this, [this] { jumpToBreak(true, false); });
    QAction *previousBreak = analysisMenu->addAction(tr("&Previous Cue"), QKeySequence(Qt::SHIFT | Qt::Key_Down),
                                                     this, [this] { jumpToBreak(false, false); });
    QAction *nextSeparator = analysisMenu->addAction(tr("Next &Separator"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up),
                                                     this, [this] { jumpToBreak(true, true); });
    QAction *previousSeparator = analysisMenu->addAction(tr("Previous S&eparator"),
                                                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down),
                                                         this, [this] { jumpToBreak(false, true); });
    for (QAction *a : {analyze, findRegions, draw, propose, nextBreak, previousBreak, nextSeparator, previousSeparator}) {
        a->setEnabled(false);
        m_editActions << a;
        addAction(a);
    }

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_deinterlaceAction);
    QMenu *hwMenu = viewMenu->addMenu(tr("&Hardware Decoding"));
    auto *hwGroup = new QActionGroup(this);
    const std::pair<HwPreference, QString> hwChoices[] = {
        {HwPreference::Auto, tr("&Automatic (Vulkan, then VAAPI)")},
        {HwPreference::Vulkan, tr("&Vulkan")},
        {HwPreference::Vaapi, tr("V&AAPI")},
        {HwPreference::Off, tr("&Off (software)")},
    };
    for (const auto &[preference, text] : hwChoices) {
        QAction *a = hwMenu->addAction(text);
        a->setCheckable(true);
        a->setChecked(preference == m_hwPreference);
        hwGroup->addAction(a);
        const HwPreference choice = preference;
        connect(a, &QAction::triggered, this, [this, choice] { setHwPreference(choice); });
    }
    if (qEnvironmentVariableIsSet("ADSCUTTER_HWDEC")) {
        hwMenu->setEnabled(false);
        hwMenu->setToolTip(tr("Set by the ADSCUTTER_HWDEC environment variable"));
    }
    viewMenu->addSeparator();
    QAction *zoomIn = viewMenu->addAction(tr("Zoom &In Timeline"), QKeySequence::ZoomIn, m_timeline, &Timeline::zoomIn);
    QAction *zoomOut = viewMenu->addAction(tr("Zoom &Out Timeline"), QKeySequence::ZoomOut, m_timeline, &Timeline::zoomOut);
    QAction *zoomFit = viewMenu->addAction(tr("&Fit Timeline"), QKeySequence(Qt::CTRL | Qt::Key_0), m_timeline, &Timeline::zoomFit);
    for (QAction *a : {zoomIn, zoomOut, zoomFit}) {
        a->setEnabled(false);
        m_editActions << a;
    }

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&Keyboard Shortcuts"), QKeySequence::HelpContents, this, &MainWindow::showShortcuts);
    helpMenu->addAction(tr("&About AdsCutter"), this, &MainWindow::showAbout);
}

// --- Files ---------------------------------------------------------------------

void MainWindow::openFileDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open recording"), m_lastDir,
        tr("TV recordings (*.ts *.m2ts *.mts *.trp *.mkv);;All files (*)"));
    if (path.isEmpty())
        return;
    m_lastDir = QFileInfo(path).absolutePath();
    openFile(path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return;
    const QString path = urls.first().toLocalFile();
    event->acceptProposedAction();
    // A cut list dropped on an open recording is loaded; anything else is
    // opened as a recording.
    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) && videoIndex())
        loadCutList(path, false);
    else
        openFile(path);
}

void MainWindow::addRecentFile(const QString &path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    m_recentFiles.removeAll(absolute);
    m_recentFiles.prepend(absolute);
    while (m_recentFiles.size() > 10)
        m_recentFiles.removeLast();
    QSettings settings;
    settings.setValue(QStringLiteral("recentFiles"), m_recentFiles);
    settings.setValue(QStringLiteral("lastDirectory"), m_lastDir);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    for (int i = 0; i < m_recentFiles.size(); ++i) {
        const QString path = m_recentFiles[i];
        QAction *a = m_recentMenu->addAction(QStringLiteral("&%1 %2").arg((i + 1) % 10).arg(QFileInfo(path).fileName()),
                                             this, [this, path] { openFile(path); });
        a->setToolTip(path);
        a->setEnabled(QFileInfo::exists(path));
    }
    if (!m_recentFiles.isEmpty()) {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction(tr("&Clear List"), this, [this] {
            m_recentFiles.clear();
            QSettings().setValue(QStringLiteral("recentFiles"), m_recentFiles);
            updateRecentMenu();
        });
    }
    m_recentMenu->setEnabled(!m_recentFiles.isEmpty());
}

void MainWindow::showShortcuts()
{
    // Built from the menus, so it always matches the actual key bindings.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Keyboard Shortcuts"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *tree = new QTreeWidget(&dialog);
    tree->setObjectName(QStringLiteral("shortcutList"));
    tree->setHeaderLabels({tr("Action"), tr("Shortcut")});
    tree->setRootIsDecorated(false);
    std::function<void(QMenu *, QTreeWidgetItem *)> addMenu = [&](QMenu *menu, QTreeWidgetItem *group) {
        for (QAction *a : menu->actions()) {
            if (a->menu()) {
                addMenu(a->menu(), group);
            } else if (!a->shortcut().isEmpty()) {
                auto *item = new QTreeWidgetItem(group);
                item->setText(0, QString(a->text()).remove(QLatin1Char('&')));
                QStringList keys;
                for (const QKeySequence &k : a->shortcuts())
                    keys << k.toString(QKeySequence::NativeText);
                item->setText(1, keys.join(QStringLiteral(", ")));
            }
        }
    };
    for (QAction *top : menuBar()->actions()) {
        if (!top->menu())
            continue;
        auto *group = new QTreeWidgetItem(tree);
        group->setText(0, QString(top->text()).remove(QLatin1Char('&')));
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        group->setFirstColumnSpanned(true);
        addMenu(top->menu(), group);
        if (group->childCount() == 0)
            delete group;
    }
    tree->expandAll();
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(tree);
    auto *mouse = new QLabel(tr("<b>Timeline</b>: click or drag to move · drag a segment edge to adjust it · "
                                "wheel to zoom · Shift+wheel to scroll · the bottom strip shows the whole "
                                "recording.<br><b>Files</b>: drop a recording on the window to open it, or a "
                                "cut list to load it."),
                             &dialog);
    mouse->setWordWrap(true);
    layout->addWidget(mouse);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, 640);
    dialog.exec();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_exportJob
        && QMessageBox::question(this, tr("Export running"), tr("An export is running. Cancel it and quit?"))
               != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (maybeSave()) {
        QSettings settings;
        settings.setValue(QStringLiteral("geometry"), saveGeometry());
        settings.setValue(QStringLiteral("windowState"), saveState());
        event->accept();
    } else {
        event->ignore();
    }
}

bool MainWindow::closeFile()
{
    if (!maybeSave())
        return false;
    if (m_exportJob
        && QMessageBox::question(this, tr("Export running"), tr("An export is running. Cancel it?"))
               != QMessageBox::Yes)
        return false;
    stopExport();
    stopAnalysis();
    setAnalysis(nullptr);
    stopRegionScan();
    m_regionResults.clear();
    updateRegionViews();
    stopFrameServer();
    stopIndexing();
    m_index.reset();
    m_timeline->clearSource();
    m_cutList.setFrames(0, {});
    m_cutListPath.clear();
    m_rebuildIndexAction->setEnabled(false);
    m_previewDecoder.reset();
    m_file.reset();
    m_streamPanel->clear();
    m_videoView->clear(tr("Open a recording with File → Open…"));
    m_navigation->setInfo({});
    m_statusInfo->clear();
    m_decoderInfo->clear();
    cutListChanged();
    return true;
}

bool MainWindow::openFile(const QString &path)
{
    if (!closeFile())
        return false;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto file = std::make_unique<MediaFile>();
    QString error;
    const bool ok = file->open(path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::critical(this, tr("Cannot open file"), tr("%1\n\n%2").arg(path, error));
        return false;
    }

    m_file = std::move(file);
    m_cutListPath = defaultCutListPath(path);
    addRecentFile(path);
    m_streamPanel->setStreams(m_file->streams(), m_file->bestVideoStream());
    updateTitle();
    m_statusInfo->setText(QStringLiteral("%1 · %2").arg(m_file->formatName(), formatDuration(m_file->duration())));

    // The indexer reuses the probed demuxer: it is positioned at the start
    // and already holds the packets read while probing.
    startIndexing(m_file->takeFormatContext(), true);

    const int video = m_file->bestVideoStream();
    if (video < 0) {
        m_videoView->clear(tr("This file has no video stream."));
        return true;
    }

    // Show a preview while indexing; navigation starts once the index is ready.
    if (!m_index) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto decoder = std::make_unique<FrameDecoder>();
        std::optional<DecodedFrame> frame;
        if (decoder->open(*m_file, video, &error))
            frame = decoder->decodeNextFrame(&error);
        QApplication::restoreOverrideCursor();
        if (!frame) {
            m_videoView->clear(tr("Cannot decode video: %1").arg(error));
            return true;
        }
        m_previewDecoder = std::move(decoder);
        m_videoView->setFrame(frame->image, frame->displayAspect);
        m_navigation->setInfo(tr("Indexing…"));
    }
    return true;
}

void MainWindow::updateTitle()
{
    if (!m_file) {
        setWindowTitle(QStringLiteral("AdsCutter"));
        return;
    }
    setWindowTitle(QStringLiteral("%1%2 — AdsCutter")
                       .arg(QFileInfo(m_file->path()).fileName(), m_cutList.isModified() ? QStringLiteral("*") : QString()));
}

// --- Indexing ----------------------------------------------------------------

void MainWindow::startIndexing(FormatContextPtr fmt, bool useCache)
{
    stopIndexing();
    if (!fmt || !m_file)
        return;

    const unsigned generation = ++m_indexGeneration;
    m_indexJob = std::make_unique<IndexJob>();
    connect(m_indexJob.get(), &IndexJob::progress, this, [this, generation](double fraction) {
        if (generation == m_indexGeneration)
            m_indexProgress->setValue(int(fraction * 1000));
    });
    connect(m_indexJob.get(), &IndexJob::finished, this, [this, generation] {
        if (generation == m_indexGeneration)
            indexFinished();
    });
    connect(m_indexJob.get(), &IndexJob::failed, this, [this, generation](const QString &error) {
        if (generation == m_indexGeneration)
            indexFailed(error);
    });

    m_indexProgress->setValue(0);
    m_indexProgress->setVisible(true);
    m_indexCancel->setVisible(true);
    m_rebuildIndexAction->setEnabled(false);
    m_indexJob->start(std::move(fmt), m_file->path(), {}, useCache);
}

void MainWindow::stopIndexing()
{
    ++m_indexGeneration;
    m_indexJob.reset(); // cancels and waits
    m_indexProgress->setVisible(false);
    m_indexCancel->setVisible(false);
    if (m_file)
        m_rebuildIndexAction->setEnabled(true);
}

void MainWindow::indexFinished()
{
    const bool fromCache = m_indexJob->loadedFromCache();
    m_index = m_indexJob->takeResult();
    stopIndexing();
    if (!m_index)
        return;

    QString summary;
    if (const PacketIndex::Stream *video = videoIndex()) {
        summary = tr("%1 frames, %2 cut points%3")
                      .arg(video->displayOrder.size())
                      .arg(video->keyframes.size())
                      .arg(video->intraFallback ? tr(" (I-frames)") : QString());
    } else {
        summary = tr("no video");
    }
    if (const size_t d = m_index->discontinuities())
        summary += tr(", %n timestamp discontinuities", nullptr, int(d));

    m_statusInfo->setText(QStringLiteral("%1 · %2 · %3")
                              .arg(m_file->formatName(), formatDuration(double(m_index->durationUs()) / 1e6),
                                   summary));
    statusBar()->showMessage(fromCache ? tr("Index loaded from cache") : tr("Indexing complete"), 5000);
    startFrameServer();
}

void MainWindow::indexFailed(const QString &error)
{
    stopIndexing();
    QMessageBox::warning(this, tr("Indexing failed"), error);
}

void MainWindow::rebuildIndex()
{
    if (!m_file || !maybeSave())
        return;
    QString error;
    FormatContextPtr fmt = openFormatContext(m_file->path(), &error);
    if (!fmt) {
        QMessageBox::warning(this, tr("Cannot reopen file"), error);
        return;
    }
    stopAnalysis();
    setAnalysis(nullptr);
    stopRegionScan();
    m_regionResults.clear();
    updateRegionViews();
    stopFrameServer();
    m_index.reset();
    m_timeline->clearSource();
    startIndexing(std::move(fmt), false);
}

// --- Navigation --------------------------------------------------------------

const PacketIndex::Stream *MainWindow::videoIndex() const
{
    if (!m_index || !m_file)
        return nullptr;
    const PacketIndex::Stream *s = m_index->stream(m_file->bestVideoStream());
    return s && !s->displayOrder.empty() ? s : nullptr;
}

int64_t MainWindow::frameDurationUs() const
{
    if (m_file && m_file->bestVideoStream() >= 0) {
        const AVRational fr = m_file->streams()[size_t(m_file->bestVideoStream())].frameRate;
        if (fr.num > 0 && fr.den > 0)
            return av_rescale(AV_TIME_BASE, fr.den, fr.num);
    }
    return 40000;
}

void MainWindow::startFrameServer()
{
    stopFrameServer();
    const PacketIndex::Stream *video = videoIndex();
    if (!video || !openFrameServer())
        return;
    m_previewDecoder.reset();

    // Cut list constraints come from the index; load the saved list if any.
    m_cutList.setFrames(uint32_t(video->displayOrder.size()), video->keyframes);
    m_timeline->setSource(m_index, video->index);
    m_playback->setSource(m_file->path(), m_index, video->index);
    if (QFileInfo::exists(m_cutListPath))
        loadCutList(m_cutListPath, true);
    // A previous analysis of this recording is shown right away.
    QString ignored;
    if (auto cached = AnalysisData::load(AnalysisData::cachePath(m_file->path()), FileKey::of(m_file->path()), &ignored);
        cached && cached->videoStream == video->index && cached->frames.size() == video->displayOrder.size())
        setAnalysis(std::move(cached));

    for (QAction *a : std::as_const(m_navigationActions))
        a->setEnabled(true);
    for (QAction *a : std::as_const(m_editActions))
        a->setEnabled(true);
    m_regionPanel->setEnabledForRecording(true);
    updateRegionViews();
    cutListChanged();
    m_position = 0;
    m_timeline->setPlayhead(0);
    m_frameServer->request(0);
}

void MainWindow::setHwPreference(HwPreference preference)
{
    if (preference == m_hwPreference)
        return;
    m_hwPreference = preference;
    QSettings().setValue(QStringLiteral("hardwareDecoding"), hwPreferenceName(preference));
    restartFrameServer();
}

void MainWindow::restartFrameServer()
{
    if (!m_frameServer)
        return;
    m_playback->stop();
    ++m_serverGeneration;
    m_frameServer.reset();
    if (openFrameServer())
        m_frameServer->request(m_position);
}

bool MainWindow::openFrameServer()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return false;
    auto server = std::make_unique<FrameServer>();
    QString error;
    if (!server->open(m_file->path(), video->index, m_index, &error, effectiveHwPreference(m_hwPreference))) {
        QMessageBox::warning(this, tr("Cannot decode video"), error);
        return false;
    }
    const unsigned generation = ++m_serverGeneration;
    connect(server.get(), &FrameServer::frameReady, this, [this, generation](const DisplayFrame &frame) {
        if (generation == m_serverGeneration)
            showFrame(frame);
    });
    connect(server.get(), &FrameServer::frameFailed, this, [this, generation](quint32 pos, const QString &error) {
        if (generation == m_serverGeneration)
            statusBar()->showMessage(tr("Frame %1: %2").arg(pos).arg(error), 5000);
    });
    server->setDeinterlace(m_deinterlaceAction->isChecked());
    m_frameServer = std::move(server);
    m_decoderInfo->clear();
    return true;
}

void MainWindow::stopFrameServer()
{
    m_playback->clear();
    if (m_regionPanel)
        m_regionPanel->setEnabledForRecording(false);
    ++m_serverGeneration;
    m_frameServer.reset(); // joins the worker
    for (QAction *a : std::as_const(m_navigationActions))
        a->setEnabled(false);
    for (QAction *a : std::as_const(m_editActions))
        a->setEnabled(false);
    if (m_undoAction) {
        m_undoAction->setEnabled(false);
        m_redoAction->setEnabled(false);
    }
}

void MainWindow::navigate(Step step)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || !m_frameServer)
        return;
    const int64_t last = int64_t(video->displayOrder.size()) - 1;
    const int64_t now = m_index->frameTime(*video, m_position);
    auto byTime = [&](int64_t deltaUs) -> int64_t {
        const int64_t target = now + deltaUs;
        if (target <= 0)
            return 0;
        const uint32_t pos = m_index->positionAtTime(*video, target);
        // Never get stuck: always move at least one frame.
        if (deltaUs > 0 && pos <= m_position)
            return int64_t(m_position) + 1;
        return pos;
    };
    auto boundary = [&](bool forward) -> int64_t {
        const std::vector<uint32_t> b = m_cutList.boundaries();
        if (forward) {
            auto it = std::upper_bound(b.begin(), b.end(), m_position);
            return it == b.end() ? m_position : *it;
        }
        auto it = std::lower_bound(b.begin(), b.end(), m_position);
        return it == b.begin() ? m_position : *(it - 1);
    };

    int64_t pos = m_position;
    switch (step) {
    case Step::PreviousFrame: pos = int64_t(m_position) - 1; break;
    case Step::NextFrame: pos = int64_t(m_position) + 1; break;
    case Step::PreviousKeyframe: pos = PacketIndex::previousKeyframe(*video, m_position); break;
    case Step::NextKeyframe: pos = PacketIndex::nextKeyframe(*video, m_position); break;
    case Step::Back1s: pos = byTime(-1'000'000); break;
    case Step::Forward1s: pos = byTime(1'000'000); break;
    case Step::Back10s: pos = byTime(-10'000'000); break;
    case Step::Forward10s: pos = byTime(10'000'000); break;
    case Step::Back1min: pos = byTime(-60'000'000); break;
    case Step::Forward1min: pos = byTime(60'000'000); break;
    case Step::Start: pos = 0; break;
    case Step::End: pos = last; break;
    case Step::PreviousBoundary: pos = boundary(false); break;
    case Step::NextBoundary: pos = boundary(true); break;
    }
    goTo(std::clamp<int64_t>(pos, 0, last));
}

void MainWindow::goTo(int64_t pos)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || !m_frameServer)
        return;
    m_playback->stop(); // any manual move pauses playback
    pos = std::clamp<int64_t>(pos, 0, int64_t(video->displayOrder.size()) - 1);
    showPosition(uint32_t(pos));
}

void MainWindow::showPosition(uint32_t pos)
{
    if (!m_frameServer)
        return;
    m_position = pos;
    m_timeline->setPlayhead(m_position);
    m_frameServer->request(m_position);
}

int MainWindow::playbackAudioStream() const
{
    // The first audio stream selected for export, else the first one.
    if (!m_file)
        return -1;
    const auto &streams = m_file->streams();
    for (int s : m_streamPanel->selectedStreams())
        if (streams[size_t(s)].type == AVMEDIA_TYPE_AUDIO)
            return s;
    for (const StreamInfo &s : streams)
        if (s.type == AVMEDIA_TYPE_AUDIO)
            return s.index;
    return -1;
}

void MainWindow::previewCues()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || !m_frameServer)
        return;
    const std::vector<Cue> all = cues();
    if (all.empty()) {
        statusBar()->showMessage(tr("No cues to preview: find breaks by fades, or changes in regions (Analysis menu)"),
                                 5000);
        return;
    }
    const auto last = uint32_t(video->displayOrder.size()) - 1;
    std::vector<PlaybackController::Range> ranges;
    std::vector<PreviewMark> marks;
    for (const Cue &c : all) {
        const int64_t t = m_index->frameTime(*video, c.frame);
        const uint32_t first = m_index->positionAtTime(*video, t - kPreviewUs);
        const uint32_t end = std::min(last, m_index->positionAtTime(*video, t + kPreviewUs));
        const QString label = (c.programmeEnds ? tr("programme ends, %1") : tr("programme resumes, %1")).arg(c.source);
        // Cues closer than the preview length share one stretch.
        if (!ranges.empty() && first <= ranges.back().last + 1) {
            ranges.back().last = std::max(ranges.back().last, end);
            ranges.back().label += QStringLiteral("; ") + label;
        } else {
            ranges.push_back({first, end, c.frame, label});
        }
        marks.push_back({c.frame, (c.programmeEnds ? tr("▼ Programme ends · %1") : tr("▲ Programme resumes · %1")).arg(c.source),
                         c.programmeEnds ? kProgrammeEndsColor : kProgrammeResumesColor});
    }
    startPreview(std::move(ranges), std::move(marks));
}

void MainWindow::previewCuts()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || !m_frameServer)
        return;
    const auto &segs = m_cutList.segments();
    if (segs.empty()) {
        statusBar()->showMessage(tr("No cuts to preview: mark the parts to keep first (I and O)"), 5000);
        return;
    }
    // What the exported file will show around each cut: the start of the
    // first part, each join (the end of a part, then the start of the next)
    // and the end of the last part.
    auto startOf = [&](const Segment &s) {
        return std::min(s.out, m_index->positionAtTime(*video, m_index->frameTime(*video, s.in) + kPreviewUs));
    };
    auto endOf = [&](const Segment &s) {
        return std::max(s.in, m_index->positionAtTime(*video, m_index->frameTime(*video, s.out) - kPreviewUs));
    };
    std::vector<PlaybackController::Range> ranges;
    std::vector<PreviewMark> marks;
    const auto n = segs.size();
    for (size_t i = 0; i < n; ++i) {
        // The first and last frame of each part; at a join the end of one
        // part plays straight into the start of the next.
        marks.push_back({segs[i].in, i == 0 ? tr("✂ Start of part 1") : tr("✂ Cut %1 · start of part %2").arg(i).arg(i + 1),
                         kCutColor});
        marks.push_back({segs[i].out,
                         i + 1 == n ? tr("✂ End of part %1").arg(n) : tr("✂ Cut %1 · end of part %1").arg(i + 1),
                         kCutColor});
        ranges.push_back({segs[i].in, startOf(segs[i]), segs[i].in,
                          i == 0 ? tr("start of part 1 of %1").arg(n) : tr("cut %1 of %2: start of part %3").arg(i).arg(n - 1).arg(i + 1)});
        ranges.push_back({endOf(segs[i]), segs[i].out, segs[i].out,
                          i + 1 == n ? tr("end of part %1 of %1").arg(n) : tr("cut %1 of %2: end of part %3").arg(i + 1).arg(n - 1).arg(i + 1)});
    }
    // Within a short part both stretches meet: play it once.
    std::vector<PlaybackController::Range> merged;
    for (auto &r : ranges) {
        if (!merged.empty() && r.first <= merged.back().last + 1) {
            merged.back().last = std::max(merged.back().last, r.last);
            merged.back().label += QStringLiteral("; ") + r.label;
        } else {
            merged.push_back(std::move(r));
        }
    }
    startPreview(std::move(merged), std::move(marks));
}

void MainWindow::startPreview(std::vector<PlaybackController::Range> ranges, std::vector<PreviewMark> marks)
{
    std::stable_sort(marks.begin(), marks.end(),
                     [](const PreviewMark &a, const PreviewMark &b) { return a.frame < b.frame; });
    m_playback->setAudioStream(playbackAudioStream());
    m_playback->playRanges(std::move(ranges), m_position);
    // playRanges moved to where the preview starts: marks before it are not
    // shown (the frames arrive later, from the decoder thread).
    m_previewMarks = std::move(marks);
    m_marksShownUpTo = int64_t(m_position) - 1;
    m_lastMark = -1;
}

void MainWindow::showPreviewMarks(uint32_t shownPos)
{
    // Frames still in flight from before the preview, or from a position
    // the playback has left, are not part of it.
    if (m_previewMarks.empty() || !m_playback->isPlayingRanges() || shownPos > m_position
        || int64_t(shownPos) <= m_marksShownUpTo)
        return;
    // Every mark passed since the last frame shown, so that one on a frame
    // skipped (decoding late) still shows; the latest one wins.
    int hit = -1;
    for (size_t i = 0; i < m_previewMarks.size(); ++i) {
        const int64_t f = m_previewMarks[i].frame;
        if (f > m_marksShownUpTo && f <= int64_t(shownPos))
            hit = int(i);
    }
    m_marksShownUpTo = shownPos;
    if (hit < 0)
        return;
    m_lastMark = hit;
    m_videoView->setMarker(m_previewMarks[size_t(hit)].text, m_previewMarks[size_t(hit)].color);
    m_markerTimer->start();
}

void MainWindow::proposeCutList()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return;
    const auto frames = uint32_t(video->displayOrder.size());

    // Regions decide where the programme is (every region must agree);
    // fades only make the boundaries frame exact. Without regions, the
    // fades decide.
    FrameRanges breaks;
    for (const BreakRegion &b : m_breaks)
        breaks.emplace_back(b.first, b.last);
    FrameRanges programme;
    QString from;
    if (m_regionResults.size() == m_regions.size() && !m_regions.empty()) {
        std::vector<FrameRanges> sources;
        for (size_t r = 0; r < m_regions.size(); ++r) {
            const FrameRanges &shown = m_regionResults[r].visible;
            sources.push_back(m_regions[r].kind == WatchRegion::Kind::VisibleInProgramme ? shown
                                                                                         : complementOf(shown, frames));
        }
        programme = intersectAll(sources, frames);
        from = tr("%n region(s)", nullptr, int(m_regions.size()));
        if (m_analysis) {
            const auto tolerance = uint32_t(3'000'000 / frameDurationUs()); // 3 s
            programme = snapToBreaks(programme, breaks, tolerance);
            from += tr(", edges refined by fades");
        }
    } else if (m_analysis) {
        programme = complementOf(breaks, frames);
        from = tr("fades");
    } else {
        statusBar()->showMessage(tr("Nothing to propose from: find breaks by fades, or changes in regions first"), 6000);
        return;
    }
    const QStringList names{from};

    ProposalSettings settings;
    settings.startAt = m_startAt;
    // Pieces shorter than 5 s are slivers where the sources disagree slightly.
    settings.minFrames = uint32_t(std::max<int64_t>(1, 5'000'000 / frameDurationUs()));
    const Proposal proposal = proposeSegments(programme, video->keyframes, frames, settings);
    if (proposal.segments.empty()) {
        statusBar()->showMessage(tr("No programme found by %1").arg(names.join(tr(" and "))), 6000);
        return;
    }

    m_cutList.checkpoint();
    m_cutList.clearMarks();
    m_cutList.setSegments(proposal.segments);
    cutListChanged();

    const uint32_t worst = *std::max_element(proposal.startOffsets.begin(), proposal.startOffsets.end());
    const double worstSeconds = double(worst) * double(frameDurationUs()) / 1e6;
    const QString starts = m_startAt == ProposalSettings::StartAt::CutPointBefore
                               ? tr("parts start up to %1 s before the programme resumes").arg(worstSeconds, 0, 'f', 1)
                               : tr("parts start up to %1 s after the programme resumes").arg(worstSeconds, 0, 'f', 1);
    statusBar()->showMessage(tr("Proposed %n part(s) from %1; %2 (Shift+P to check the cuts, Ctrl+Z to undo)", nullptr,
                                int(proposal.segments.size()))
                                 .arg(names.join(tr(" and ")), starts),
                             10000);
}

void MainWindow::stopAtPreview()
{
    if (!m_playback->isPlayingRanges())
        return;
    const PlaybackController::Range range = m_playback->currentRange();
    // In a stretch shared by several cues or cuts: the one flashed last.
    if (m_lastMark >= 0) {
        const PreviewMark mark = m_previewMarks[size_t(m_lastMark)];
        if (mark.frame >= range.first && mark.frame <= range.last) {
            goTo(mark.frame); // stops playing
            m_videoView->setMarker(mark.text, mark.color);
            m_markerTimer->start();
            statusBar()->showMessage(tr("Stopped at: %1").arg(mark.text), 6000);
            return;
        }
    }
    goTo(range.anchor); // stops playing
    statusBar()->showMessage(tr("Stopped at: %1").arg(range.label), 6000);
}

void MainWindow::togglePlayback()
{
    if (!m_frameServer)
        return;
    if (m_playback->isPlaying()) {
        m_playback->stop();
        return;
    }
    m_playback->setAudioStream(playbackAudioStream());
    m_playback->setKeptOnly(m_keptOnlyAction->isChecked(), &m_cutList);
    m_playback->play(m_position);
}

void MainWindow::showFrame(const DisplayFrame &frame)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return;
    m_videoView->setFrame(frame.image, frame.displayAspect);
    showPreviewMarks(frame.pos);
    m_decoderInfo->setText(tr("Video: %1").arg(frame.decoder));

    QString kind = QString(QLatin1Char(frame.pictureType));
    if (frame.cutPoint)
        kind += tr(" · cut point");
    if (frame.interlaced)
        kind += frame.deinterlaced ? tr(" · deinterlaced") : tr(" · interlaced");
    const int segment = m_cutList.segmentAt(frame.pos);
    kind += segment >= 0 ? tr(" · segment %1").arg(segment + 1) : tr(" · removed");
    m_navigation->setInfo(QStringLiteral("%1   frame %2 / %3   %4")
                              .arg(formatTime(frame.timeUs))
                              .arg(frame.pos + 1)
                              .arg(video->displayOrder.size())
                              .arg(kind));
    m_segmentPanel->setCurrentSegment(segment);
}

// --- Cut list ------------------------------------------------------------------

void MainWindow::markIn()
{
    const uint32_t snapped = m_cutList.snapIn(m_position);
    const auto result = m_cutList.markIn(m_position);
    QString msg;
    switch (result) {
    case CutList::MarkResult::Pending:
        msg = tr("Start set at frame %1").arg(snapped + 1);
        if (snapped != m_position)
            msg += tr(" (moved back to the cut point before frame %1)").arg(m_position + 1);
        msg += tr("; now mark the end with O");
        break;
    case CutList::MarkResult::Added:
        msg = tr("Segment added");
        if (snapped != m_position)
            msg += tr(" (start moved back to the cut point at frame %1)").arg(snapped + 1);
        break;
    case CutList::MarkResult::Invalid:
        msg = tr("Start is after the pending end: end mark cleared");
        break;
    }
    statusBar()->showMessage(msg, 6000);
    cutListChanged();
}

void MainWindow::markOut()
{
    const auto result = m_cutList.markOut(m_position);
    QString msg;
    switch (result) {
    case CutList::MarkResult::Pending: msg = tr("End set at frame %1; now mark the start with I").arg(m_position + 1); break;
    case CutList::MarkResult::Added: msg = tr("Segment added"); break;
    case CutList::MarkResult::Invalid: msg = tr("End is before the pending start: start mark cleared"); break;
    }
    statusBar()->showMessage(msg, 6000);
    cutListChanged();
}

void MainWindow::deleteSegment()
{
    const int segment = m_cutList.segmentAt(m_position);
    if (segment < 0) {
        statusBar()->showMessage(tr("No segment at the playhead"), 4000);
        return;
    }
    m_cutList.checkpoint();
    m_cutList.removeSegment(size_t(segment));
    statusBar()->showMessage(tr("Segment %1 deleted").arg(segment + 1), 4000);
    cutListChanged();
}

void MainWindow::undo()
{
    if (m_cutList.undo())
        cutListChanged();
}

void MainWindow::redo()
{
    if (m_cutList.redo())
        cutListChanged();
}

void MainWindow::cutListChanged()
{
    m_timeline->update();
    const PacketIndex::Stream *video = videoIndex();
    if (video) {
        m_segmentPanel->refresh(
            &m_cutList, [this, video](uint32_t pos) { return m_index->frameTime(*video, pos); }, frameDurationUs(),
            m_index->durationUs());
        m_segmentPanel->setCurrentSegment(m_cutList.segmentAt(m_position));
    } else {
        m_segmentPanel->refresh(nullptr, {}, 0, 0);
    }
    const bool editable = video && m_frameServer;
    m_undoAction->setEnabled(editable && m_cutList.canUndo());
    m_redoAction->setEnabled(editable && m_cutList.canRedo());
    updateTitle();
}

bool MainWindow::saveCutList(bool chooseFile)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return false;
    QString path = m_cutListPath;
    if (chooseFile || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save cut list"), path,
                                            tr("AdsCutter cut lists (*.adscutter.json);;JSON files (*.json)"));
        if (path.isEmpty())
            return false;
    }

    CutListDocument doc;
    doc.mediaFile = QFileInfo(m_file->path()).fileName();
    doc.mediaSize = QFileInfo(m_file->path()).size();
    doc.videoStream = video->index;
    doc.frameCount = uint32_t(video->displayOrder.size());
    doc.selectedStreams = m_streamPanel->selectedStreams();
    for (const Segment &s : m_cutList.segments())
        doc.segments.push_back({s, m_index->frameTime(*video, s.in), m_index->frameTime(*video, s.out)});

    QString error;
    if (!saveCutListDocument(path, doc, &error)) {
        QMessageBox::warning(this, tr("Cannot save cut list"), tr("%1\n\n%2").arg(path, error));
        return false;
    }
    m_cutListPath = path;
    m_cutList.markSaved();
    statusBar()->showMessage(tr("Cut list saved to %1").arg(path), 5000);
    cutListChanged();
    return true;
}

void MainWindow::openCutListDialog()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open cut list"), QFileInfo(m_cutListPath).absolutePath(),
                                                      tr("AdsCutter cut lists (*.adscutter.json *.json)"));
    if (!path.isEmpty())
        loadCutList(path, false);
}

bool MainWindow::loadCutList(const QString &path, bool quiet)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return false;
    QString error;
    const auto doc = loadCutListDocument(path, &error);
    if (!doc) {
        QMessageBox::warning(this, tr("Cannot load cut list"), tr("%1\n\n%2").arg(path, error));
        return false;
    }

    // Frame numbers are exact when the cut list was made on the same index;
    // otherwise fall back to the stored times.
    const bool sameFrames = doc->videoStream == video->index && doc->frameCount == video->displayOrder.size();
    std::vector<Segment> segments;
    for (const auto &e : doc->segments) {
        if (sameFrames) {
            segments.push_back(e.frames);
        } else {
            segments.push_back({m_index->positionAtTime(*video, e.inTimeUs),
                                m_index->positionAtTime(*video, e.outTimeUs)});
        }
    }

    if (!quiet)
        m_cutList.checkpoint();
    m_cutList.clearMarks();
    m_cutList.setSegments(segments);
    if (!doc->selectedStreams.empty())
        m_streamPanel->setSelectedStreams(doc->selectedStreams);
    m_cutListPath = path;
    if (sameFrames)
        m_cutList.markSaved();
    cutListChanged();

    if (!sameFrames) {
        QMessageBox::information(this, tr("Cut list adapted"),
                                 tr("The cut list was made for a different version of this recording "
                                    "or index. Its segments were placed by time; please check them."));
    } else {
        statusBar()->showMessage(tr("Cut list loaded: %n segment(s)", nullptr, int(segments.size())), 5000);
    }
    return true;
}

bool MainWindow::maybeSave()
{
    if (!m_cutList.isModified() || !videoIndex())
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Unsaved cut list"), tr("The cut list has unsaved changes. Save them?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveCutList(false);
    return answer == QMessageBox::Discard;
}

// --- Export --------------------------------------------------------------------

void MainWindow::exportDialog()
{
    if (!m_file)
        return;
    const QFileInfo input(m_file->path());
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export"), input.absolutePath() + QLatin1Char('/') + input.completeBaseName() + QStringLiteral(".cut.mkv"),
        tr("Matroska video (*.mkv)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive))
        path += QStringLiteral(".mkv");
    startExport(path);
}

bool MainWindow::startExport(const QString &outputPath)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || m_exportJob)
        return false;
    if (m_cutList.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to export"),
                                 tr("Mark the parts to keep first: I at the start, O at the end of each part."));
        return false;
    }
    ExportSettings settings;
    settings.outputPath = outputPath;
    settings.videoStream = video->index;
    settings.streams = m_streamPanel->selectedStreams();
    settings.segments = m_cutList.segments();
    settings.frameDurationUs = frameDurationUs();
    if (settings.streams.empty()) {
        QMessageBox::information(this, tr("Nothing to export"), tr("Select at least one stream in the Streams panel."));
        return false;
    }
    if (QFileInfo(outputPath).absoluteFilePath() == QFileInfo(m_file->path()).absoluteFilePath()) {
        QMessageBox::warning(this, tr("Cannot export"), tr("The output file cannot be the recording itself."));
        return false;
    }

    m_exportJob = std::make_unique<ExportJob>();
    m_exportProgress = new QProgressDialog(tr("Exporting to %1…").arg(QFileInfo(outputPath).fileName()), tr("Cancel"),
                                           0, 1000, this);
    m_exportProgress->setWindowTitle(tr("Export"));
    m_exportProgress->setWindowModality(Qt::WindowModal);
    m_exportProgress->setMinimumDuration(0);
    m_exportProgress->setAutoClose(false);
    m_exportProgress->setAutoReset(false);
    m_exportProgress->setValue(0);
    connect(m_exportProgress, &QProgressDialog::canceled, m_exportJob.get(), &ExportJob::cancel);
    connect(m_exportJob.get(), &ExportJob::progress, m_exportProgress,
            [dialog = m_exportProgress](double fraction) { dialog->setValue(int(fraction * 1000)); });
    connect(m_exportJob.get(), &ExportJob::finished, this, [this, outputPath] { exportFinished(outputPath); });
    m_exportJob->start(m_file->path(), m_index, settings);
    return true;
}

void MainWindow::stopExport()
{
    if (m_exportProgress) {
        m_exportProgress->disconnect(this);
        m_exportProgress->deleteLater();
        m_exportProgress = nullptr;
    }
    m_exportJob.reset(); // cancels and waits; a canceled export leaves no file
}

void MainWindow::exportFinished(const QString &outputPath)
{
    if (!m_exportJob)
        return;
    const bool ok = m_exportJob->succeeded();
    const bool canceled = m_exportJob->canceled();
    const QString error = m_exportJob->error();
    const ExportReport report = m_exportJob->report();
    stopExport();

    if (canceled) {
        statusBar()->showMessage(tr("Export canceled"), 5000);
        return;
    }
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setObjectName(QStringLiteral("exportResult"));
    if (!ok) {
        box->setIcon(QMessageBox::Warning);
        box->setWindowTitle(tr("Export failed"));
        box->setText(error);
    } else {
        const double mb = QFileInfo(outputPath).size() / 1e6;
        QString text = tr("Exported %n segment(s), %1 in total, to %2 (%3 MB).", nullptr, int(report.segments.size()))
                           .arg(formatTime(report.durationUs), QFileInfo(outputPath).fileName())
                           .arg(mb, 0, 'f', 1);
        for (const QString &w : report.warnings)
            text += QStringLiteral("\n\n") + w;
        box->setIcon(report.warnings.isEmpty() ? QMessageBox::Information : QMessageBox::Warning);
        box->setWindowTitle(tr("Export complete"));
        box->setText(text);
        statusBar()->showMessage(tr("Exported to %1").arg(outputPath), 8000);
    }
    box->open();
}

// --- Analysis -------------------------------------------------------------------

bool MainWindow::startAnalysis()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video || m_analysisJob)
        return false;
    m_analysisJob = std::make_unique<AnalysisJob>();
    connect(m_analysisJob.get(), &AnalysisJob::progress, m_analysisProgress,
            [this](double fraction) { m_analysisProgress->setValue(int(fraction * 1000)); });
    connect(m_analysisJob.get(), &AnalysisJob::finished, this, &MainWindow::analysisFinished);
    m_analysisProgress->setValue(0);
    m_analysisProgress->setVisible(true);
    m_analysisCancel->setVisible(true);
    m_analysisJob->start(m_file->path(), m_index, video->index, playbackAudioStream());
    return true;
}

void MainWindow::stopAnalysis()
{
    m_analysisJob.reset(); // cancels and waits
    if (m_analysisProgress) {
        m_analysisProgress->setVisible(false);
        m_analysisCancel->setVisible(false);
    }
}

void MainWindow::analysisFinished()
{
    if (!m_analysisJob)
        return;
    auto result = m_analysisJob->takeResult();
    const QString error = m_analysisJob->error();
    const bool canceled = m_analysisJob->canceled();
    // The job emitted from its thread, which is finishing: release it later.
    m_analysisJob.release()->deleteLater();
    m_analysisProgress->setVisible(false);
    m_analysisCancel->setVisible(false);
    if (!result) {
        if (!canceled)
            QMessageBox::warning(this, tr("Analysis failed"), error);
        return;
    }
    setAnalysis(std::move(result));
    statusBar()->showMessage(tr("Analysis complete: %n break(s) found (Shift+Up/Down to go through the cues)", nullptr,
                                int(m_breaks.size())),
                             8000);
}

void MainWindow::setAnalysis(std::unique_ptr<AnalysisData> analysis)
{
    m_analysis = std::move(analysis);
    m_candidates.clear();
    m_breaks.clear();
    if (m_analysis && m_index) {
        m_candidates = findBreakCandidates(*m_analysis, *m_index);
        m_breaks = findBreaks(m_candidates, *m_index, m_analysis->videoStream);
    }
    m_timeline->setAnalysis(m_analysis.get(), m_candidates, m_breaks);
}

std::vector<MainWindow::Cue> MainWindow::cues() const
{
    std::vector<Cue> out;
    for (size_t i = 0; i < m_breaks.size(); ++i) {
        const BreakRegion &b = m_breaks[i];
        const QString what = tr("break %1 of %2 (fades)").arg(i + 1).arg(m_breaks.size());
        out.push_back({b.first > 0 ? b.first - 1 : 0, true, what});
        out.push_back({b.last + 1, false, what});
    }
    if (m_regionResults.size() == m_regions.size()) {
        for (size_t r = 0; r < m_regions.size(); ++r) {
            const RegionResult &res = m_regionResults[r];
            const bool programmeKind = m_regions[r].kind == WatchRegion::Kind::VisibleInProgramme;
            for (uint32_t c : res.changes) {
                const bool becomesShown = std::any_of(res.visible.begin(), res.visible.end(),
                                                      [c](const auto &range) { return range.first == c; });
                const bool programmeStarts = programmeKind == becomesShown;
                const QString what = tr("region \"%1\"").arg(m_regions[r].name);
                if (programmeStarts)
                    out.push_back({c, false, what});
                else
                    out.push_back({c > 0 ? c - 1 : 0, true, what});
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Cue &a, const Cue &b) { return a.frame < b.frame; });
    // Methods agreeing on a frame make one cue.
    std::vector<Cue> merged;
    for (Cue &c : out) {
        if (!merged.empty() && merged.back().frame == c.frame && merged.back().programmeEnds == c.programmeEnds)
            merged.back().source += QStringLiteral(" + ") + c.source;
        else
            merged.push_back(std::move(c));
    }
    return merged;
}

void MainWindow::jumpToBreak(bool forward, bool separators)
{
    if (separators) {
        if (!m_analysis) {
            statusBar()->showMessage(tr("Find breaks by fades first (Analysis menu)"), 5000);
            return;
        }
        std::vector<uint32_t> targets;
        for (const BreakCandidate &c : m_candidates)
            if (c.silent)
                targets.push_back(c.first);
        auto it = forward ? std::upper_bound(targets.begin(), targets.end(), m_position)
                          : std::lower_bound(targets.begin(), targets.end(), m_position);
        if (forward ? it == targets.end() : it == targets.begin()) {
            statusBar()->showMessage(forward ? tr("No more separators") : tr("No earlier separators"), 3000);
            return;
        }
        goTo(forward ? *it : *(it - 1));
        return;
    }

    // Cues: where the programme ends (last frame before a break: mark O) or
    // resumes (first frame after it: mark I), from every method used.
    const std::vector<Cue> all = cues();
    if (all.empty()) {
        statusBar()->showMessage(tr("No cues yet: find breaks by fades, or changes in regions (Analysis menu)"), 5000);
        return;
    }
    const Cue *target = nullptr;
    if (forward) {
        for (const Cue &c : all)
            if (c.frame > m_position) {
                target = &c;
                break;
            }
    } else {
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (it->frame < m_position) {
                target = &*it;
                break;
            }
    }
    if (!target) {
        statusBar()->showMessage(forward ? tr("No more cues") : tr("No earlier cues"), 3000);
        return;
    }
    goTo(target->frame);
    const size_t number = size_t(target - all.data()) + 1;
    statusBar()->showMessage(
        (target->programmeEnds ? tr("Cue %1 of %2, %3: programme ends here (last frame): mark the end of a part with O")
                               : tr("Cue %1 of %2, %3: programme resumes here (first frame): mark the start of a part with I"))
            .arg(number)
            .arg(all.size())
            .arg(target->source),
        8000);
}

// --- Regions (method 2) -----------------------------------------------------------

void MainWindow::drawRegion()
{
    if (!m_frameServer) {
        statusBar()->showMessage(tr("Open a recording first"), 3000);
        return;
    }
    m_playback->stop();
    m_regionsDock->show();
    m_regionsDock->raise();
    m_videoView->setDrawing(true);
    statusBar()->showMessage(tr("Drag a rectangle around a text or logo shown on this frame (Esc to cancel)"), 8000);
}

void MainWindow::regionDrawn(const QRect &rect)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return;
    // Reference: this frame and others over 4 s around it (one per second),
    // so that only the steady overlay remains, not the picture behind it.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    RandomAccessDecoder decoder;
    QString error;
    std::vector<FramePtr> frames;
    if (decoder.open(m_file->path(), video->index, m_index, &error)) {
        const int64_t now = m_index->frameTime(*video, m_position);
        for (int k = -4; k <= 4; ++k) {
            const uint32_t pos = k == 0 ? m_position : m_index->positionAtTime(*video, now + int64_t(k) * 1'000'000);
            if (FramePtr f = decoder.frameAt(pos, &error))
                frames.push_back(std::move(f));
        }
    }
    std::vector<const AVFrame *> raw;
    for (const FramePtr &f : frames)
        raw.push_back(f.get());
    WatchRegion region;
    region.rect = rect;
    const bool ok = !raw.empty() && captureTemplate(raw, region);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::information(this, tr("Nothing to watch"),
                                 tr("No steady text or logo was found in that rectangle around this frame. Draw it "
                                    "tightly around a text or logo, on a frame where it is clearly shown."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Region"));
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(tr("Region %1").arg(m_regions.size() + 1), &dialog);
    name->setObjectName(QStringLiteral("regionName"));
    auto *kind = new QComboBox(&dialog);
    kind->setObjectName(QStringLiteral("regionKind"));
    kind->addItem(tr("Shown during the programme"));
    kind->addItem(tr("Shown during ads"));
    kind->setCurrentIndex(1);
    form->addRow(tr("Name:"), name);
    form->addRow(tr("What it shows here:"), kind);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    region.name = name->text().trimmed().isEmpty() ? tr("Region %1").arg(m_regions.size() + 1) : name->text().trimmed();
    region.kind = kind->currentIndex() == 0 ? WatchRegion::Kind::VisibleInProgramme : WatchRegion::Kind::VisibleInAds;
    m_regions.push_back(std::move(region));
    regionsChanged(false);
    statusBar()->showMessage(tr("Region added: Find Changes (Ctrl+Shift+R) to look for it in the whole recording"), 6000);
}

void MainWindow::regionsChanged(bool keepResults)
{
    QSettings().setValue(QStringLiteral("regions"), QJsonDocument(regionsToJson(m_regions)).toJson(QJsonDocument::Compact));
    if (!keepResults)
        m_regionResults.clear();
    m_regionPanel->setRegions(m_regions);
    updateRegionViews();
}

void MainWindow::updateRegionViews()
{
    const QColor programme(76, 160, 92), ads(240, 150, 30);
    // Rectangles on the picture while the Regions panel is on screen (a
    // tabbed dock behind another tab still counts as "visible").
    QList<VideoView::Overlay> overlays;
    if (m_regionPanel && !m_regionPanel->visibleRegion().isEmpty()) {
        for (const WatchRegion &r : m_regions) {
            const QSize picture = r.pictureSize;
            WatchRegion shown = r;
            if (m_file && m_file->bestVideoStream() >= 0) {
                const StreamInfo &v = m_file->streams()[size_t(m_file->bestVideoStream())];
                if (v.width > 0 && QSize(v.width, v.height) != picture)
                    shown = r.scaledTo(QSize(v.width, v.height));
            }
            overlays.append({shown.rect, r.kind == WatchRegion::Kind::VisibleInProgramme ? programme : ads, r.name});
        }
    }
    m_videoView->setOverlays(overlays);

    // Timeline lanes and change counts.
    std::vector<Timeline::RegionLane> lanes;
    std::vector<int> counts(m_regions.size(), -1);
    if (m_regionResults.size() == m_regions.size()) {
        for (size_t r = 0; r < m_regions.size(); ++r) {
            QColor c = m_regions[r].kind == WatchRegion::Kind::VisibleInProgramme ? programme : ads;
            c.setAlpha(170);
            lanes.push_back({m_regions[r].name, c, m_regionResults[r].visible, m_regionResults[r].changes});
            counts[r] = int(m_regionResults[r].changes.size());
        }
    }
    m_timeline->setRegionLanes(std::move(lanes));
    m_regionPanel->setResults(counts);
}

bool MainWindow::startRegionScan()
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return false;
    if (m_regions.empty()) {
        statusBar()->showMessage(tr("Draw a region first (Analysis → Draw Region)"), 5000);
        return false;
    }
    if (m_regionJob || m_analysisJob) {
        statusBar()->showMessage(tr("Wait for the running analysis to finish"), 4000);
        return false;
    }
    RegionScanSettings settings;
    settings.minRunUs = int64_t(m_regionPanel->minChangeSeconds()) * 1'000'000;
    m_regionJob = std::make_unique<RegionJob>();
    connect(m_regionJob.get(), &RegionJob::progress, m_analysisProgress,
            [this](double fraction) { m_analysisProgress->setValue(int(fraction * 1000)); });
    connect(m_regionJob.get(), &RegionJob::finished, this, &MainWindow::regionScanFinished);
    m_analysisProgress->setFormat(tr("Finding changes %p%"));
    m_analysisProgress->setValue(0);
    m_analysisProgress->setVisible(true);
    m_analysisCancel->setVisible(true);
    m_regionJob->start(m_file->path(), m_index, video->index, m_regions, settings);
    return true;
}

void MainWindow::stopRegionScan()
{
    m_regionJob.reset(); // cancels and waits
    if (m_analysisProgress && !m_analysisJob) {
        m_analysisProgress->setVisible(false);
        m_analysisCancel->setVisible(false);
    }
}

void MainWindow::regionScanFinished()
{
    if (!m_regionJob)
        return;
    const bool ok = m_regionJob->succeeded(), canceled = m_regionJob->canceled();
    const QString error = m_regionJob->error();
    std::vector<RegionResult> results = m_regionJob->results();
    m_regionJob.release()->deleteLater();
    m_analysisProgress->setVisible(false);
    m_analysisCancel->setVisible(false);
    m_analysisProgress->setFormat(tr("Analyzing %p%"));
    if (!ok) {
        if (!canceled)
            QMessageBox::warning(this, tr("Finding changes failed"), error);
        return;
    }
    m_regionResults = std::move(results);
    updateRegionViews();
    size_t changes = 0;
    for (const RegionResult &r : m_regionResults)
        changes += r.changes.size();
    statusBar()->showMessage(tr("%n change(s) found in the regions (Shift+Up/Down to go through the cues)", nullptr,
                                int(changes)),
                             8000);
}

QStringList MainWindow::regionPresetNames() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("regionPresets"));
    QStringList names = settings.childKeys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

void MainWindow::saveRegionPreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Regions"), tr("Preset name (e.g. the channel):"),
                                               QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("regionPresets"));
    settings.setValue(name, QJsonDocument(regionsToJson(m_regions)).toJson(QJsonDocument::Compact));
    settings.endGroup();
    m_regionPanel->setPresetNames(regionPresetNames(), name);
}

void MainWindow::loadRegionPreset(const QString &name)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("regionPresets"));
    m_regions = regionsFromJson(QJsonDocument::fromJson(settings.value(name).toByteArray()).array());
    settings.endGroup();
    regionsChanged(false);
    statusBar()->showMessage(tr("Regions of \"%1\" loaded").arg(name), 4000);
}

void MainWindow::deleteRegionPreset(const QString &name)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("regionPresets"));
    settings.remove(name);
    settings.endGroup();
    m_regionPanel->setPresetNames(regionPresetNames());
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About AdsCutter"),
                       tr("<b>AdsCutter %1</b><p>Cut ads out of TV recordings without "
                          "re-encoding.</p><p>Uses FFmpeg %2 and Qt %3.</p>")
                           .arg(QApplication::applicationVersion(), QString::fromUtf8(av_version_info()),
                                QString::fromUtf8(qVersion())));
}

} // namespace adscutter
