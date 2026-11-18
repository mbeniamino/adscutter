// Drives the main window on a copy of the MPEG-2 test file (25 fps, 150
// frames, a cut point every 12 frames) and checks:
//  - keyboard navigation lands on the expected frames;
//  - marking in/out creates segments (in snapped to cut points);
//  - undo/redo, delete, segment boundary navigation;
//  - dragging segment edges on the timeline (in snaps, out is exact);
//  - the cut list is saved and loaded back when the file is reopened.
//
// usage: gui_test FILE [SCREENSHOT.png]
// Run with QT_QPA_PLATFORM=offscreen.

#include "TestUtil.h"
#include "ui/MainWindow.h"
#include "ui/Timeline.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
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
using Segs = std::vector<Segment>;

int shownFrame(const QLabel *info)
{
    static const QRegularExpression re(QStringLiteral("frame (\\d+) /"));
    const auto m = re.match(info->text());
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

void expectFrame(const QLabel *info, int frame, const char *what)
{
    const bool ok = QTest::qWaitFor([&] { return shownFrame(info) == frame; }, 10000);
    std::printf("%-30s -> frame %d%s\n", what, shownFrame(info), ok ? "" : "  <-- FAIL");
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s: expected frame %d, label says \"%s\"\n", what, frame,
                     qPrintable(info->text()));
        ++failures;
    }
}

QString describe(const Segs &segs)
{
    QStringList parts;
    for (const Segment &s : segs)
        parts << QStringLiteral("[%1,%2]").arg(s.in).arg(s.out);
    return parts.join(QLatin1Char(' '));
}

void expectSegments(const MainWindow &w, const Segs &expected, const char *what)
{
    const bool ok = w.cutList().segments() == expected;
    std::printf("%-30s -> %s%s\n", what, qPrintable(describe(w.cutList().segments())), ok ? "" : "  <-- FAIL");
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s: expected %s\n", what, qPrintable(describe(expected)));
        ++failures;
    }
}

void key(QWidget *w, QKeyCombination k, int times = 1)
{
    for (int i = 0; i < times; ++i)
        QTest::keyClick(w, k.key(), k.keyboardModifiers());
}

// Drags on the timeline track from frame edge `from` to frame edge `to`
// (positions in frames, x = pos / framesPerPixel).
void dragTimeline(Timeline *t, double from, double to)
{
    const int y = 22 + 17; // middle of the track
    const auto x = [&](double pos) { return int(std::lround(pos / t->framesPerPixel())); };
    QTest::mousePress(t, Qt::LeftButton, {}, QPoint(x(from), y));
    QTest::mouseMove(t, QPoint((x(from) + x(to)) / 2, y));
    QTest::mouseMove(t, QPoint(x(to), y));
    QTest::mouseRelease(t, Qt::LeftButton, {}, QPoint(x(to), y));
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

    // Work on a copy so the saved cut list does not end up next to the fixture.
    QTemporaryDir dir;
    const QString media = dir.filePath(QStringLiteral("recording.ts"));
    if (!QFile::copy(QString::fromLocal8Bit(argv[1]), media)) {
        std::fprintf(stderr, "FAIL: cannot copy test file\n");
        return 1;
    }

    auto window = std::make_unique<MainWindow>();
    window->show();
    if (!QTest::qWaitForWindowExposed(window.get())) {
        std::fprintf(stderr, "FAIL: window not exposed\n");
        return 1;
    }
    window->activateWindow();
    if (!QTest::qWaitForWindowActive(window.get()))
        std::printf("note: window not active (shortcuts may not fire)\n");

    if (!window->openFile(media)) {
        std::fprintf(stderr, "FAIL: cannot open file\n");
        return 1;
    }
    const QLabel *info = window->findChild<QLabel *>(QStringLiteral("positionInfo"));
    Timeline *timeline = window->findChild<Timeline *>();
    if (!info || !timeline) {
        std::fprintf(stderr, "FAIL: widgets not found\n");
        return 1;
    }
    QWidget *w = window.get();

    // --- Navigation ---------------------------------------------------------
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
        key(w, s.key);
        expectFrame(info, s.frame, s.what);
    }

    // --- Marking segments ---------------------------------------------------
    key(w, Qt::Key_Up);
    key(w, Qt::Key_Right, 5);
    expectFrame(info, 18, "to frame 18 (pos 17)");
    key(w, Qt::Key_I);
    if (window->cutList().pendingIn() != 12u) {
        std::fprintf(stderr, "FAIL: in mark should snap to 12\n");
        ++failures;
    }
    key(w, Qt::Key_Up, 2);
    key(w, Qt::Key_Right, 3);
    expectFrame(info, 40, "to frame 40 (pos 39)");
    key(w, Qt::Key_O);
    expectSegments(*window, {{12, 39}}, "I at 17, O at 39");

    key(w, Qt::Key_Up, 2);
    key(w, Qt::Key_Right, 2);
    expectFrame(info, 63, "to frame 63 (pos 62)");
    key(w, Qt::Key_I);
    key(w, Qt::Key_End);
    key(w, Qt::Key_O);
    expectSegments(*window, {{12, 39}, {60, 149}}, "I at 62, O at end");
    if (!window->windowTitle().contains(QLatin1Char('*'))) {
        std::fprintf(stderr, "FAIL: title should show unsaved changes\n");
        ++failures;
    }

    key(w, Qt::CTRL | Qt::Key_Z);
    expectSegments(*window, {{12, 39}}, "Undo");
    key(w, Qt::CTRL | Qt::SHIFT | Qt::Key_Z);
    expectSegments(*window, {{12, 39}, {60, 149}}, "Redo");

    expectFrame(info, 150, "playhead at end");
    key(w, Qt::Key_Delete);
    expectSegments(*window, {{12, 39}}, "Delete at playhead");
    key(w, Qt::CTRL | Qt::Key_Z);
    expectSegments(*window, {{12, 39}, {60, 149}}, "Undo delete");

    key(w, Qt::CTRL | Qt::Key_Down);
    expectFrame(info, 61, "Ctrl+Down (prev boundary)");
    key(w, Qt::CTRL | Qt::Key_Down);
    expectFrame(info, 40, "Ctrl+Down");
    key(w, Qt::CTRL | Qt::Key_Up);
    expectFrame(info, 61, "Ctrl+Up (next boundary)");

    // --- Timeline edge drags --------------------------------------------------
    timeline->zoomFit();
    dragTimeline(timeline, 12, 30.2);
    expectSegments(*window, {{24, 39}, {60, 149}}, "drag in edge 12 -> 30");
    expectFrame(info, 25, "playhead follows the edge");
    dragTimeline(timeline, 40, 45);
    expectSegments(*window, {{24, 44}, {60, 149}}, "drag out edge 40 -> 45");
    dragTimeline(timeline, 45, 70);
    expectSegments(*window, {{24, 59}, {60, 149}}, "drag out edge into next");
    key(w, Qt::CTRL | Qt::Key_Z);
    expectSegments(*window, {{24, 44}, {60, 149}}, "Undo drag");

    // --- Save and reload --------------------------------------------------------
    key(w, Qt::CTRL | Qt::Key_S);
    const QString cutList = window->cutListPath();
    if (!QFile::exists(cutList) || window->windowTitle().contains(QLatin1Char('*'))) {
        std::fprintf(stderr, "FAIL: cut list not saved to %s\n", qPrintable(cutList));
        ++failures;
    }
    std::printf("%-30s -> %s\n", "saved", qPrintable(QFileInfo(cutList).fileName()));

    // --- Export -----------------------------------------------------------------
    const QString exported = dir.filePath(QStringLiteral("recording.cut.mkv"));
    if (!window->startExport(exported)) {
        std::fprintf(stderr, "FAIL: export did not start\n");
        ++failures;
    }
    const bool exportDone = QTest::qWaitFor([&] { return !window->isExporting(); }, 30000);
    auto *result = window->findChild<QMessageBox *>(QStringLiteral("exportResult"));
    std::printf("%-30s -> %s\n", "export", result ? qPrintable(result->windowTitle()) : "no result dialog");
    if (!exportDone || !result || result->windowTitle() != QStringLiteral("Export complete")) {
        std::fprintf(stderr, "FAIL: export did not complete: %s\n", result ? qPrintable(result->text()) : "");
        ++failures;
    }
    if (result)
        result->close();
    // Segments [24,44] and [60,149]: 21 + 90 frames, plus P frame 45 which
    // B frame 44 depends on (reported as one extra frame).
    const auto decoded = test::decodeAll(exported, 0);
    std::printf("%-30s -> %zu frames\n", "exported file", decoded.hashes.size());
    if (decoded.hashes.size() != 112) {
        std::fprintf(stderr, "FAIL: exported file should have 112 frames\n");
        ++failures;
    }

    if (argc >= 3) {
        key(w, Qt::Key_Home);
        key(w, Qt::Key_Up, 3);
        key(w, Qt::Key_Right, 4);
        expectFrame(info, 41, "to frame 41 (screenshot)");
        timeline->zoomIn();
        timeline->zoomIn();
        QTest::qWait(200);
        window->grab().save(QString::fromLocal8Bit(argv[2]));
        std::printf("screenshot: %s\n", argv[2]);
    }

    window = std::make_unique<MainWindow>();
    window->show();
    if (!QTest::qWaitForWindowExposed(window.get()) || !window->openFile(media)) {
        std::fprintf(stderr, "FAIL: cannot reopen\n");
        return 1;
    }
    info = window->findChild<QLabel *>(QStringLiteral("positionInfo"));
    expectFrame(info, 1, "reopened");
    expectSegments(*window, {{24, 44}, {60, 149}}, "cut list loaded on reopen");
    if (window->windowTitle().contains(QLatin1Char('*'))) {
        std::fprintf(stderr, "FAIL: freshly loaded cut list should not be modified\n");
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
