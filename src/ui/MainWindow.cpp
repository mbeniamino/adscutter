#include "ui/MainWindow.h"
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

namespace adscutter {

namespace {

QString formatDuration(double seconds)
{
    if (seconds < 0)
        return QStringLiteral("unknown duration");
    return QTime(0, 0).addMSecs(qint64(seconds * 1000.0)).toString(QStringLiteral("HH:mm:ss.zzz"));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_videoView(new VideoView(this))
    , m_streamPanel(new StreamPanel(this))
    , m_statusInfo(new QLabel(this))
    , m_indexProgress(new QProgressBar(this))
    , m_indexCancel(new QToolButton(this))
{
    setWindowTitle(QStringLiteral("AdsCutter"));
    setCentralWidget(m_videoView);

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

    createMenus();
    resize(1024, 768);
}

MainWindow::~MainWindow() = default;

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openFileDialog);
    m_rebuildIndexAction = fileMenu->addAction(tr("&Rebuild Index"), this, &MainWindow::rebuildIndex);
    m_rebuildIndexAction->setEnabled(false);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

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
    stopIndexing();
    m_index.reset();
    m_rebuildIndexAction->setEnabled(false);
    m_decoder.reset();
    m_file.reset();
    m_streamPanel->clear();
    m_videoView->clear(tr("Open a recording with File → Open…"));
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
        QMessageBox::critical(this, tr("Cannot open file"),
                              tr("%1\n\n%2").arg(path, error));
        return false;
    }

    m_file = std::move(file);
    m_streamPanel->setStreams(m_file->streams(), m_file->bestVideoStream());
    setWindowTitle(QStringLiteral("%1 — AdsCutter").arg(QFileInfo(path).fileName()));
    m_statusInfo->setText(QStringLiteral("%1 · %2")
                              .arg(m_file->formatName(), formatDuration(m_file->duration())));

    // The indexer reuses the probed demuxer: it is positioned at the start
    // and already holds the packets read while probing.
    startIndexing(m_file->takeFormatContext(), true);

    const int video = m_file->bestVideoStream();
    if (video < 0) {
        m_videoView->clear(tr("This file has no video stream."));
        return true;
    }

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
    m_decoder = std::move(decoder);
    m_videoView->setFrame(frame->image, frame->displayAspect);
    statusBar()->showMessage(
        tr("First frame: %1x%2%3")
            .arg(frame->image.width())
            .arg(frame->image.height())
            .arg(frame->interlaced ? tr(", interlaced") : QString()),
        5000);
    return true;
}

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
    if (const PacketIndex::Stream *video = m_index->stream(m_file->bestVideoStream())) {
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
                              .arg(m_file->formatName(),
                                   formatDuration(double(m_index->durationUs()) / 1e6), summary));
    statusBar()->showMessage(fromCache ? tr("Index loaded from cache") : tr("Indexing complete"), 5000);
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
    m_index.reset();
    startIndexing(std::move(fmt), false);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About AdsCutter"),
                       tr("<b>AdsCutter %1</b><p>Cut ads out of TV recordings without "
                          "re-encoding.</p><p>Uses FFmpeg %2 and Qt %3.</p>")
                           .arg(QApplication::applicationVersion(),
                                QString::fromUtf8(av_version_info()),
                                QString::fromUtf8(qVersion())));
}

} // namespace adscutter
