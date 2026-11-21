#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AdsCutter"));
    QApplication::setApplicationVersion(QStringLiteral(ADSCUTTER_VERSION));
    QApplication::setOrganizationName(QStringLiteral("adscutter"));
    // Used as the Wayland app_id; must match the .desktop file name.
    QGuiApplication::setDesktopFileName(QStringLiteral("adscutter"));
    // Wayland compositors take the icon from the .desktop file; X11 and
    // the About box need it set on the application.
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("adscutter")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cut ads out of TV recordings without re-encoding."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption verbose({QStringLiteral("v"), QStringLiteral("verbose")},
                               QStringLiteral("Print FFmpeg diagnostics."));
    parser.addOption(verbose);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Recording to open."), QStringLiteral("[file]"));
    parser.process(app);

    // Broadcast recordings start mid-stream, which makes decoders complain
    // loudly about missing references. Keep FFmpeg quiet unless asked.
    av_log_set_level(parser.isSet(verbose) ? AV_LOG_INFO : AV_LOG_FATAL);

    adscutter::MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openFile(args.first());

    return app.exec();
}
