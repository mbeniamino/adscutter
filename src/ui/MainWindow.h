#pragma once

#include "media/FrameDecoder.h"
#include "media/MediaFile.h"

#include <QMainWindow>
#include <memory>

class QLabel;

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

private:
    void createMenus();
    void closeFile();

    VideoView *m_videoView;
    StreamPanel *m_streamPanel;
    QLabel *m_statusInfo;

    std::unique_ptr<MediaFile> m_file;
    std::unique_ptr<FrameDecoder> m_decoder;
    QString m_lastDir;
};

} // namespace adscutter
