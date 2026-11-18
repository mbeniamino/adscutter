// Runs the analysis from the main window on the broadcast-like recording and
// navigates its break with the keyboard.
//
// usage: analysis_gui_test broadcast_like.mkv [SCREENSHOT.png]
// Run with QT_QPA_PLATFORM=offscreen.

#include "ui/MainWindow.h"

#include <QApplication>
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
