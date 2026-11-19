// Runs the analysis from the main window on the broadcast-like recording and
// navigates its break with the keyboard.
//
// usage: analysis_gui_test broadcast_like.mkv [SCREENSHOT.png]
// Run with QT_QPA_PLATFORM=offscreen.

#include "ui/MainWindow.h"
#include "ui/VideoView.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QFile>
#include <QLabel>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

using namespace adscutter;

namespace {

int failures = 0;

int shownFrame(const QLabel *info)
{
    static const QRegularExpression re(QStringLiteral("frame (\\d+) /"));
    const auto m = re.match(info->text());
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

void expectFrame(const QLabel *info, int frame, const char *what)
{
    const bool ok = QTest::qWaitFor([&] { return shownFrame(info) == frame; }, 10000);
    std::printf("%-34s -> frame %d%s\n", what, shownFrame(info), ok ? "" : "  <-- FAIL");
    if (!ok)
        ++failures;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("ADSCUTTER_NO_AUDIO", "1");
    qputenv("ADSCUTTER_HWDEC", "none");
    QTemporaryDir config, cache;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s broadcast_like.mkv [SCREENSHOT.png]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);

    // A copy, so that no cut list or cache lands next to the fixture.
    QTemporaryDir dir;
    const QString media = dir.filePath(QStringLiteral("broadcast.mkv"));
    QFile::copy(QString::fromLocal8Bit(argv[1]), media);

    MainWindow window;
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        return 1;
    window.activateWindow();
    (void)QTest::qWaitForWindowActive(&window);
    window.openFile(media);
    const QLabel *info = window.findChild<QLabel *>(QStringLiteral("positionInfo"));
    expectFrame(info, 1, "after indexing");

    // Navigating before analysing only explains what to do.
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, 1, "Shift+Up before analysis (no move)");

    if (!window.startAnalysis()) {
        std::fprintf(stderr, "FAIL: analysis did not start\n");
        return 1;
    }
    const bool done = QTest::qWaitFor([&] { return !window.isAnalyzing(); }, 60000);
    std::printf("%-34s -> %s, %zu break(s)\n", "analysis", done ? "finished" : "TIMEOUT", window.breaks().size());
    if (!done || window.breaks().size() != 1) {
        std::fprintf(stderr, "FAIL: expected one break\n");
        return 1;
    }
    window.activateWindow();
    (void)QTest::qWaitForWindowActive(&window);

    // The break runs from the programme fade to the last separator (frame
    // 1223, 0-based); the label counts from 1.
    const int beforeBreak = int(window.breaks().front().first); // 0-based first - 1, shown 1-based
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, beforeBreak, "Shift+Up: last frame before break");
    if (beforeBreak < 486 || beforeBreak > 499)
        ++failures, std::fprintf(stderr, "FAIL: the break should start at the programme fade\n");
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, 1225, "Shift+Up: first frame after break");
    QTest::keyClick(&window, Qt::Key_Down, Qt::ShiftModifier);
    expectFrame(info, beforeBreak, "Shift+Down: back before the break");
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier | Qt::ControlModifier);
    expectFrame(info, 901, "Ctrl+Shift+Up: first separator");
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier | Qt::ControlModifier);
    expectFrame(info, 1213, "Ctrl+Shift+Up: next separator");

    // --- Method 2: a region drawn on the picture -----------------------------
    // Into the break (cut point 672), where "MEDIASET" is shown.
    QTest::keyClick(&window, Qt::Key_Home);
    for (int i = 0; i < 7; ++i)
        QTest::keyClick(&window, Qt::Key_Up);
    expectFrame(info, 673, "into the break");

    auto *view = window.findChild<VideoView *>();
    QPushButton *draw = nullptr;
    for (QPushButton *b : window.findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Draw…"))
            draw = b;
    if (!view || !draw) {
        std::fprintf(stderr, "FAIL: video view or Draw button not found\n");
        return 1;
    }
    draw->click();
    if (!view->isDrawing())
        ++failures, std::fprintf(stderr, "FAIL: not in drawing mode\n");

    // The dialog that follows the drawing: name it, "shown during ads".
    QTimer::singleShot(500, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        dialog->findChild<QLineEdit *>(QStringLiteral("regionName"))->setText(QStringLiteral("MEDIASET"));
        dialog->findChild<QComboBox *>(QStringLiteral("regionKind"))->setCurrentIndex(1);
        dialog->accept();
    });
    // Picture rectangle 1045,612 200x44 (1280x720, 16:9) to widget coordinates.
    const double aspect = 16.0 / 9.0, widgetAspect = double(view->width()) / view->height();
    QRectF target;
    if (widgetAspect > aspect)
        target = QRectF((view->width() - view->height() * aspect) / 2, 0, view->height() * aspect, view->height());
    else
        target = QRectF(0, (view->height() - view->width() / aspect) / 2, view->width(), view->width() / aspect);
    auto toWidget = [&](double x, double y) {
        return QPoint(int(target.left() + x * target.width() / 1280), int(target.top() + y * target.height() / 720));
    };
    QTest::mousePress(view, Qt::LeftButton, {}, toWidget(1045, 612));
    QTest::mouseMove(view, toWidget(1150, 640));
    QTest::mouseMove(view, toWidget(1245, 656));
    QTest::mouseRelease(view, Qt::LeftButton, {}, toWidget(1245, 656));
    const bool added = QTest::qWaitFor([&] { return window.regions().size() == 1; }, 5000);
    std::printf("%-34s -> %s\n", "region drawn", added ? qPrintable(window.regions()[0].name) : "NONE");
    if (!added)
        return 1;
    if (window.regions()[0].kind != WatchRegion::Kind::VisibleInAds)
        ++failures, std::fprintf(stderr, "FAIL: region kind\n");

    window.findChild<QSpinBox *>()->setValue(5); // the test break lasts 14 s
    if (!window.startRegionScan())
        ++failures, std::fprintf(stderr, "FAIL: region scan did not start\n");
    (void)QTest::qWaitFor([&] { return !window.isScanningRegions(); }, 60000);
    const auto &results = window.regionResults();
    const bool exact = results.size() == 1 && results[0].changes.size() == 2
                       && std::abs(int(results[0].changes[0]) - 500) <= 1
                       && std::abs(int(results[0].changes[1]) - 1224) <= 1;
    std::printf("%-34s -> %s\n", "changes found", exact ? "500 and 1224" : "WRONG");
    if (!exact)
        ++failures;
    window.activateWindow();
    (void)QTest::qWaitForWindowActive(&window);

    // Cues from both methods, in order: the fade (last frame before it),
    // the region turning to "MEDIASET" (programme ends at 499), then both
    // resume at 1224.
    QTest::keyClick(&window, Qt::Key_Home);
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, beforeBreak, "Shift+Up: fade cue (O)");
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, 500, "Shift+Up: region cue (O)");
    QTest::keyClick(&window, Qt::Key_Up, Qt::ShiftModifier);
    expectFrame(info, 1225, "Shift+Up: programme resumes (I)");

    const bool saved = !QSettings().value(QStringLiteral("regions")).toByteArray().isEmpty();
    std::printf("%-34s -> %s\n", "regions kept in the settings", saved ? "yes" : "NO");
    if (!saved)
        ++failures;

    if (argc >= 3) {
        QTest::qWait(300);
        window.grab().save(QString::fromLocal8Bit(argv[2]));
    }

    // Reopening shows the cached analysis without analysing again.
    MainWindow again;
    again.show();
    (void)QTest::qWaitForWindowExposed(&again);
    again.openFile(media);
    const QLabel *info2 = again.findChild<QLabel *>(QStringLiteral("positionInfo"));
    expectFrame(info2, 1, "reopened");
    std::printf("%-34s -> %zu break(s)\n", "cached analysis on reopen", again.breaks().size());
    if (again.breaks().size() != 1)
        ++failures;

    return failures == 0 ? 0 : 1;
}
