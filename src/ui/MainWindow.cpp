#include "ui/MainWindow.h"
#include "ui/NavigationBar.h"
#include "ui/SegmentPanel.h"
#include "ui/StreamPanel.h"
#include "ui/Timeline.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QCloseEvent>
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
    statusBar()->addPermanentWidget(m_indexProgress);
    statusBar()->addPermanentWidget(m_indexCancel);
    statusBar()->addPermanentWidget(m_statusInfo);
    m_lastDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);

    connect(m_timeline, &Timeline::seekRequested, this, [this](int pos) { goTo(pos); });
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
}

MainWindow::~MainWindow()
{
    // Stop the worker threads before the widgets they report to go away.
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
    for (int i = 0; i < m_navigationActions.size(); ++i) {
        goMenu->addAction(m_navigationActions[i]);
        if (i == 0 || i == 3 || i == 7 || i == 10 || i == 11)
            goMenu->addSeparator();
    }

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_deinterlaceAction);
    viewMenu->addSeparator();
    QAction *zoomIn = viewMenu->addAction(tr("Zoom &In Timeline"), QKeySequence::ZoomIn, m_timeline, &Timeline::zoomIn);
    QAction *zoomOut = viewMenu->addAction(tr("Zoom &Out Timeline"), QKeySequence::ZoomOut, m_timeline, &Timeline::zoomOut);
    QAction *zoomFit = viewMenu->addAction(tr("&Fit Timeline"), QKeySequence(Qt::CTRL | Qt::Key_0), m_timeline, &Timeline::zoomFit);
    for (QAction *a : {zoomIn, zoomOut, zoomFit}) {
        a->setEnabled(false);
        m_editActions << a;
    }

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
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

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_exportJob
        && QMessageBox::question(this, tr("Export running"), tr("An export is running. Cancel it and quit?"))
               != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (maybeSave())
        event->accept();
    else
        event->ignore();
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
    if (!video)
        return;

    auto server = std::make_unique<FrameServer>();
    QString error;
    if (!server->open(m_file->path(), video->index, m_index, &error)) {
        QMessageBox::warning(this, tr("Cannot decode video"), error);
        return;
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
    m_previewDecoder.reset();

    // Cut list constraints come from the index; load the saved list if any.
    m_cutList.setFrames(uint32_t(video->displayOrder.size()), video->keyframes);
    m_timeline->setSource(m_index, video->index);
    if (QFileInfo::exists(m_cutListPath))
        loadCutList(m_cutListPath, true);

    for (QAction *a : std::as_const(m_navigationActions))
        a->setEnabled(true);
    for (QAction *a : std::as_const(m_editActions))
        a->setEnabled(true);
    cutListChanged();
    m_position = 0;
    m_timeline->setPlayhead(0);
    m_frameServer->request(0);
}

void MainWindow::stopFrameServer()
{
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
    pos = std::clamp<int64_t>(pos, 0, int64_t(video->displayOrder.size()) - 1);
    m_position = uint32_t(pos);
    m_timeline->setPlayhead(m_position);
    m_frameServer->request(m_position);
}

void MainWindow::showFrame(const DisplayFrame &frame)
{
    const PacketIndex::Stream *video = videoIndex();
    if (!video)
        return;
    m_videoView->setFrame(frame.image, frame.displayAspect);

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

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About AdsCutter"),
                       tr("<b>AdsCutter %1</b><p>Cut ads out of TV recordings without "
                          "re-encoding.</p><p>Uses FFmpeg %2 and Qt %3.</p>")
                           .arg(QApplication::applicationVersion(), QString::fromUtf8(av_version_info()),
                                QString::fromUtf8(qVersion())));
}

} // namespace adscutter
