#pragma once

#include "analysis/AnalysisJob.h"
#include "analysis/Proposal.h"
#include "analysis/RegionJob.h"
#include "media/ExportJob.h"
#include "media/FrameDecoder.h"
#include "media/FrameServer.h"
#include "media/HwDecode.h"
#include "media/IndexJob.h"
#include "media/MediaFile.h"
#include "media/PacketIndex.h"
#include "model/CutList.h"
#include "ui/Playback.h"

#include <QColor>
#include <QMainWindow>
#include <memory>

class QDockWidget;
class QLabel;
class QProgressBar;
class QProgressDialog;
class QToolButton;

namespace adscutter {

class NavigationBar;
class RegionPanel;
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
    // Starts the analysis of the open recording (in the background).
    bool startAnalysis();
    bool isAnalyzing() const { return m_analysisJob != nullptr; }
    const std::vector<BreakRegion> &breaks() const { return m_breaks; }
    // Method 2: watched regions.
    const std::vector<WatchRegion> &regions() const { return m_regions; }
    bool startRegionScan();
    bool isScanningRegions() const { return m_regionJob != nullptr; }
    const std::vector<RegionResult> &regionResults() const { return m_regionResults; }

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
    void stopAnalysis();
    void analysisFinished();
    void setAnalysis(std::unique_ptr<AnalysisData> analysis);
    void jumpToBreak(bool forward, bool separators);
    struct Cue {
        uint32_t frame;      // where to put the mark
        bool programmeEnds;  // O here; otherwise the programme resumes: I
        QString source;
    };
    std::vector<Cue> cues() const;
    void drawRegion();
    void regionDrawn(const QRect &rect);
    void regionsChanged(bool keepResults);
    void updateRegionViews();
    void regionScanFinished();
    void stopRegionScan();
    void saveRegionPreset();
    void loadRegionPreset(const QString &name);
    void deleteRegionPreset(const QString &name);
    QStringList regionPresetNames() const;
    void navigate(Step step);
    void goTo(int64_t pos);
    void showPosition(uint32_t pos); // move without stopping playback
    void togglePlayback();
    // Smart play: a few seconds around each cue / each cut, nothing else.
    void previewCues();
    void previewCuts();
    void stopAtPreview(); // stops on the frame the current preview is about
    // A cue or cut a preview is about: a marker flashes on the video when
    // its frame is shown.
    struct PreviewMark {
        uint32_t frame;
        QString text;
        QColor color;
    };
    void startPreview(std::vector<PlaybackController::Range> ranges, std::vector<PreviewMark> marks);
    void showPreviewMarks(uint32_t shownPos);
    void proposeCutList();
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
    QList<QAction *> m_previewActions;
    ProposalSettings::StartAt m_startAt = ProposalSettings::StartAt::CutPointBefore;
    PlaybackController *m_playback = nullptr;
    std::vector<PreviewMark> m_previewMarks; // sorted by frame; only while previewing
    int64_t m_marksShownUpTo = -1;           // marks up to this frame have been shown
    int m_lastMark = -1;                     // the mark flashed last
    QTimer *m_markerTimer = nullptr;         // hides the marker
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
    std::unique_ptr<AnalysisJob> m_analysisJob;
    std::unique_ptr<AnalysisData> m_analysis;
    std::vector<BreakCandidate> m_candidates;
    std::vector<BreakRegion> m_breaks;
    QProgressBar *m_analysisProgress = nullptr;
    RegionPanel *m_regionPanel = nullptr;
    QDockWidget *m_regionsDock = nullptr;
    std::vector<WatchRegion> m_regions;
    std::vector<RegionResult> m_regionResults;
    std::unique_ptr<RegionJob> m_regionJob;
    QToolButton *m_analysisCancel = nullptr;
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
