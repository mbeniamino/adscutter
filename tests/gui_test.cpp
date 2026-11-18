// Drives the main window with keyboard shortcuts on the MPEG-2 test file
// (25 fps, a cut point every 12 frames) and checks where navigation lands.
//
// usage: gui_test FILE [SCREENSHOT.png]
// Run with QT_QPA_PLATFORM=offscreen.

#include "ui/MainWindow.h"

#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
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
    std::printf("%-24s -> frame %d%s\n", what, shownFrame(info), ok ? "" : "  <-- FAIL");
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s: expected frame %d, label says \"%s\"\n", what, frame,
                     qPrintable(info->text()));
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE [SCREENSHOT.png]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);

    MainWindow window;
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
        std::fprintf(stderr, "FAIL: window not exposed\n");
        return 1;
    }
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window))
        std::printf("note: window not active (shortcuts may not fire)\n");

    if (!window.openFile(QString::fromLocal8Bit(argv[1]))) {
        std::fprintf(stderr, "FAIL: cannot open file\n");
        return 1;
    }
    const QLabel *info = window.findChild<QLabel *>(QStringLiteral("positionInfo"));
    if (!info) {
        std::fprintf(stderr, "FAIL: no position label\n");
        return 1;
    }

    expectFrame(info, 1, "after indexing");
    struct Step {
        QKeyCombination key;
        int frame;
        const char *what;
    };
    const Step steps[] = {
        {Qt::Key_Right, 2, "Right (next frame)"},
        {Qt::Key_Right, 3, "Right"},
        {Qt::Key_Left, 2, "Left (previous frame)"},
        {Qt::Key_Up, 13, "Up (next cut point)"},
        {Qt::Key_Up, 25, "Up"},
        {Qt::Key_Left, 24, "Left (into previous GOP)"},
        {Qt::Key_Down, 13, "Down (previous cut point)"},
        {Qt::CTRL | Qt::Key_Right, 38, "Ctrl+Right (+1 s)"},
        {Qt::Key_End, 150, "End"},
        {Qt::Key_Left, 149, "Left"},
        {Qt::SHIFT | Qt::Key_Left, 1, "Shift+Left (-10 s)"},
        {Qt::Key_PageDown, 150, "PageDown (+1 min)"},
        {Qt::Key_Home, 1, "Home"},
    };
    for (const Step &s : steps) {
        QTest::keyClick(&window, s.key.key(), s.key.keyboardModifiers());
        expectFrame(info, s.frame, s.what);
    }

    if (argc >= 3) {
        QTest::keyClick(&window, Qt::Key_Up);
        expectFrame(info, 13, "Up (for screenshot)");
        QTest::qWait(200);
        window.grab().save(QString::fromLocal8Bit(argv[2]));
        std::printf("screenshot: %s\n", argv[2]);
    }
    return failures == 0 ? 0 : 1;
}
