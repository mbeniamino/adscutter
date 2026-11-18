#include "ui/MainWindow.h"
#include "ui/NavigationBar.h"
#include "ui/StreamPanel.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
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
    , m_navigation(new NavigationBar(this))
    , m_streamPanel(new StreamPanel(this))
    , m_statusInfo(new QLabel(this))
    , m_indexProgress(new QProgressBar(this))
    , m_indexCancel(new QToolButton(this))
{
    setWindowTitle(QStringLiteral("AdsCutter"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_videoView, 1);
    layout->addWidget(m_navigation);
    setCentralWidget(central);

    auto *dock = new QDockWidget(tr("Streams"), this);
    dock->setObjectName(QStringLiteral("streamsDock"));
    dock->setWidget(m_streamPanel);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

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

    connect(m_navigation, &NavigationBar::seekRequested, this, [this](int pos) { goTo(pos); });

    createActions();
    createMenus();
    resize(1024, 768);
}

MainWindow::~MainWindow()
{
    // Stop the worker threads before the widgets they report to go away.
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
    };

    QList<QAction *> buttons;
    for (const Def &d : defs) {
        auto *action = new QAction(d.text, this);
        action->setShortcut(d.key);
        action->setShortcutContext(Qt::WindowShortcut);
        action->setToolTip(QStringLiteral("%1 (%2)").arg(QString(d.text).remove(QLatin1Char('&')),
                                                         d.key.toString(QKeySequence::NativeText)));
        action->setEnabled(false);
        action->setAutoRepeat(true);
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

    m_deinterlaceAction = new QAction(tr("&Deinterlace"), this);
    m_deinterlaceAction->setCheckable(true);
    m_deinterlaceAction->setChecked(true);
    m_deinterlaceAction->setShortcut(Qt::Key_D);
    m_deinterlaceAction->setToolTip(tr("Deinterlace interlaced frames for display (export is not affected)"));
    connect(m_deinterlaceAction, &QAction::toggled, this, [this](bool on) {
        if (m_frameServer)
            m_frameServer->setDeinterlace(on);
    });
}

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openFileDialog);
    m_rebuildIndexAction = fileMenu->addAction(tr("&Rebuild Index"), this, &MainWindow::rebuildIndex);
    m_rebuildIndexAction->setEnabled(false);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu *goMenu = menuBar()->addMenu(tr("&Go"));
    for (int i = 0; i < m_navigationActions.size(); ++i) {
        goMenu->addAction(m_navigationActions[i]);
        if (i == 0 || i == 3 || i == 7 || i == 10)
            goMenu->addSeparator();
    }

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_deinterlaceAction);
    addAction(m_deinterlaceAction);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About AdsCutter"), this, &MainWindow::showAbout);
}

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

void MainWindow::closeFile()
{
    stopFrameServer();
    stopIndexing();
    m_index.reset();
    m_rebuildIndexAction->setEnabled(false);
    m_previewDecoder.reset();
    m_file.reset();
    m_streamPanel->clear();
    m_videoView->clear(tr("Open a recording with File → Open…"));
    m_navigation->setInfo({});
    m_statusInfo->clear();
    setWindowTitle(QStringLiteral("AdsCutter"));
}

bool MainWindow::openFile(const QString &path)
{
    closeFile();

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
    m_streamPanel->setStreams(m_file->streams(), m_file->bestVideoStream());
    setWindowTitle(QStringLiteral("%1 — AdsCutter").arg(QFileInfo(path).fileName()));
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
    if (!m_file)
        return;
    QString error;
    FormatContextPtr fmt = openFormatContext(m_file->path(), &error);
    if (!fmt) {
        QMessageBox::warning(this, tr("Cannot reopen file"), error);
        return;
    }
    stopFrameServer();
    m_index.reset();
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

    m_navigation->setFrameCount(int(video->displayOrder.size()));
    for (QAction *a : std::as_const(m_navigationActions))
        a->setEnabled(true);
    m_position = 0;
    m_frameServer->request(0);
}

void MainWindow::stopFrameServer()
{
    ++m_serverGeneration;
    m_frameServer.reset(); // joins the worker
    for (QAction *a : std::as_const(m_navigationActions))
        a->setEnabled(false);
    m_navigation->setFrameCount(0);
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
    m_navigation->setPosition(int(pos));
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
        kind += frame.deinterlaced ? tr(" · interlaced (deinterlaced)") : tr(" · interlaced");
    m_navigation->setInfo(QStringLiteral("%1 / %2   frame %3 / %4   %5")
                              .arg(formatTime(frame.timeUs), formatTime(m_index->durationUs()))
                              .arg(frame.pos + 1)
                              .arg(video->displayOrder.size())
                              .arg(kind));
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
