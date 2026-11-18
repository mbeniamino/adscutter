#pragma once

#include "media/FrameDecoder.h"
#include "media/FrameServer.h"
#include "media/IndexJob.h"
#include "media/MediaFile.h"
#include "media/PacketIndex.h"

#include <QMainWindow>
#include <memory>

class QLabel;
class QProgressBar;
class QToolButton;

namespace adscutter {

class NavigationBar;
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
    enum class Step {
        PreviousFrame, NextFrame,
        PreviousKeyframe, NextKeyframe,
        Back1s, Forward1s,
        Back10s, Forward10s,
        Back1min, Forward1min,
        Start, End,
    };

    void createActions();
    void createMenus();
    void closeFile();
    void startIndexing(FormatContextPtr fmt, bool useCache);
    void stopIndexing();
    void indexFinished();
    void indexFailed(const QString &error);

    void startFrameServer();
    void stopFrameServer();
    void navigate(Step step);
    void goTo(int64_t pos);
    void showFrame(const DisplayFrame &frame);
    const PacketIndex::Stream *videoIndex() const;

    VideoView *m_videoView;
    NavigationBar *m_navigation;
    StreamPanel *m_streamPanel;
    QLabel *m_statusInfo;
    QProgressBar *m_indexProgress;
    QToolButton *m_indexCancel;
    QAction *m_rebuildIndexAction = nullptr;
    QAction *m_deinterlaceAction = nullptr;
    QList<QAction *> m_navigationActions;

    std::unique_ptr<MediaFile> m_file;
    std::unique_ptr<FrameDecoder> m_previewDecoder; // first frame, before the index is ready
    std::unique_ptr<IndexJob> m_indexJob;
    std::shared_ptr<const PacketIndex> m_index;
    std::unique_ptr<FrameServer> m_frameServer;
    // Bumped whenever a job is replaced, so late signals of an old job are ignored.
    unsigned m_indexGeneration = 0;
    unsigned m_serverGeneration = 0;
    uint32_t m_position = 0; // last requested frame (display order position)
    QString m_lastDir;
};

} // namespace adscutter
