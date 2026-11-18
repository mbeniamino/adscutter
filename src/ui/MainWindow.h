#pragma once

#include "media/ExportJob.h"
#include "media/FrameDecoder.h"
#include "media/FrameServer.h"
#include "media/HwDecode.h"
#include "media/IndexJob.h"
#include "media/MediaFile.h"
#include "media/PacketIndex.h"
#include "model/CutList.h"

#include <QMainWindow>
#include <memory>

class QLabel;
class QProgressBar;
class QProgressDialog;
class QToolButton;

namespace adscutter {

class NavigationBar;
class PlaybackController;
class SegmentPanel;
class StreamPanel;
class Timeline;
class VideoView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString &path);
    const CutList &cutList() const { return m_cutList; }
    QString cutListPath() const { return m_cutListPath; }
    // Exports the kept segments of the selected streams (in the background).
    bool startExport(const QString &outputPath);
    bool isExporting() const { return m_exportJob != nullptr; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

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
        PreviousBoundary, NextBoundary,
    };

    void createActions();
    void createMenus();
    bool closeFile(); // false if the user canceled (unsaved changes)
    void startIndexing(FormatContextPtr fmt, bool useCache);
    void stopIndexing();
    void indexFinished();
    void indexFailed(const QString &error);

    void startFrameServer();
    bool openFrameServer(); // just the decoder thread
    void restartFrameServer();
    void stopFrameServer();
    void setHwPreference(HwPreference preference);
    void addRecentFile(const QString &path);
    void updateRecentMenu();
    void showShortcuts();
    void navigate(Step step);
    void goTo(int64_t pos);
    void showPosition(uint32_t pos); // move without stopping playback
    void togglePlayback();
    int playbackAudioStream() const;
    void showFrame(const DisplayFrame &frame);
    const PacketIndex::Stream *videoIndex() const;

    // Cut list editing
    void markIn();
    void markOut();
    void deleteSegment();
    void undo();
    void redo();
    void cutListChanged();
    bool saveCutList(bool chooseFile);
    void openCutListDialog();
    bool loadCutList(const QString &path, bool quiet);
    bool maybeSave();
    void exportDialog();
    void exportFinished(const QString &outputPath);
    void stopExport();
    void updateTitle();
    int64_t frameDurationUs() const;

    VideoView *m_videoView;
    Timeline *m_timeline;
    NavigationBar *m_navigation;
    StreamPanel *m_streamPanel;
    SegmentPanel *m_segmentPanel;
    QLabel *m_statusInfo;
    QLabel *m_decoderInfo;
    QMenu *m_recentMenu = nullptr;
    HwPreference m_hwPreference = HwPreference::Auto;
    QStringList m_recentFiles;
    QProgressBar *m_indexProgress;
    QToolButton *m_indexCancel;
    QAction *m_rebuildIndexAction = nullptr;
    QAction *m_deinterlaceAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_keptOnlyAction = nullptr;
    QAction *m_muteAction = nullptr;
    PlaybackController *m_playback = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QList<QAction *> m_navigationActions;
    QList<QAction *> m_editActions; // enabled once the index is ready

    std::unique_ptr<MediaFile> m_file;
    std::unique_ptr<FrameDecoder> m_previewDecoder; // first frame, before the index is ready
    std::unique_ptr<IndexJob> m_indexJob;
    std::shared_ptr<const PacketIndex> m_index;
    std::unique_ptr<FrameServer> m_frameServer;
    std::unique_ptr<ExportJob> m_exportJob;
    QProgressDialog *m_exportProgress = nullptr;
    // Bumped whenever a job is replaced, so late signals of an old job are ignored.
    unsigned m_indexGeneration = 0;
    unsigned m_serverGeneration = 0;
    uint32_t m_position = 0; // last requested frame (display order position)

    CutList m_cutList;
    QString m_cutListPath;
    QString m_lastDir;
};

} // namespace adscutter
