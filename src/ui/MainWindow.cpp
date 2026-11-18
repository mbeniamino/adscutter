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
#include <QStandardPaths>
#include <QStatusBar>
#include <QTime>

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
{
    setWindowTitle(QStringLiteral("AdsCutter"));
    setCentralWidget(m_videoView);

    auto *dock = new QDockWidget(tr("Streams"), this);
    dock->setObjectName(QStringLiteral("streamsDock"));
    dock->setWidget(m_streamPanel);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

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
