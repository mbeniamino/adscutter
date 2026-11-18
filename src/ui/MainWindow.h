#pragma once

#include "media/FrameDecoder.h"
#include "media/IndexJob.h"
#include "media/MediaFile.h"
#include "media/PacketIndex.h"

#include <QMainWindow>
#include <memory>

class QLabel;
class QProgressBar;
class QToolButton;

namespace adscutter {

class StreamPanel;
class VideoView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString &path);

private slots:
    void openFileDialog();
    void showAbout();
    void rebuildIndex();

private:
    void createMenus();
    void closeFile();
    void startIndexing(FormatContextPtr fmt, bool useCache);
    void stopIndexing();
    void indexFinished();
    void indexFailed(const QString &error);

    VideoView *m_videoView;
    StreamPanel *m_streamPanel;
    QLabel *m_statusInfo;
    QProgressBar *m_indexProgress;
    QToolButton *m_indexCancel;
    QAction *m_rebuildIndexAction = nullptr;

    std::unique_ptr<MediaFile> m_file;
    std::unique_ptr<FrameDecoder> m_decoder;
    std::unique_ptr<IndexJob> m_indexJob;
    std::unique_ptr<PacketIndex> m_index;
    // Bumped whenever a job is replaced, so late signals of an old job are ignored.
    unsigned m_indexGeneration = 0;
    QString m_lastDir;
};

} // namespace adscutter
