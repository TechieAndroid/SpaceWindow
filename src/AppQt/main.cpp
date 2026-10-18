// SpaceWindow (Qt) - the program.
//
//   spacewindow [folder or drive]     opens the window, and scans the folder if one is given
//   spacewindow --capture <file.png> <folder>
//                                     scans the folder, saves the window as a picture, and quits
//   spacewindow --help, --version
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>

#include "FolderTree.h"
#include "MainWindow.h"
#include "Posix.h"
#include "QtText.h"
#include "Version.h"

namespace
{
    // An argument as the system passed it: bytes, kept whole whatever their
    // encoding (Qt's copy is decoded, and a name that is not UTF-8 would not
    // survive it).  QApplication has taken its own options out of argv.
    std::string RawArgument(int argc, char** argv, QString const& decoded)
    {
        for (int i = argc - 1; i > 0; --i)
        {
            if (QString::fromLocal8Bit(argv[i]) == decoded)
            {
                return { argv[i], std::strlen(argv[i]) };
            }
        }
        return QFile::encodeName(decoded).toStdString();
    }
}

int main(int argc, char** argv)
{
    // From an AppImage, the desktop's own Qt theme plugins (Plasma's, GTK's) cannot
    // load into this Qt; the desktop portal's theme, which comes with it, gives the
    // desktop's file dialogs and its light or dark.  ("gtk2" is the AppImage
    // tooling's pick for GTK desktops: Qt 6 has no such theme.)
    if (qEnvironmentVariableIsSet("APPIMAGE"))
    {
        if (QByteArray const theme = qgetenv("QT_QPA_PLATFORMTHEME"); theme.isEmpty() || theme == "gtk2")
        {
            qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
        }
    }
    QApplication::setApplicationName(QStringLiteral("SpaceWindow"));
    QApplication::setApplicationVersion(QStringLiteral(SPACEWINDOW_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(SPACEWINDOW_APP_ID));
    QApplication const application(argc, argv);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/spacewindow.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("See where your disk space goes."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption const capture(QStringLiteral("capture"),
                                     QStringLiteral("Scan the folder, save the window as a picture, and quit."),
                                     QStringLiteral("file.png"));
    parser.addOption(capture);
    parser.addPositionalArgument(QStringLiteral("folder"), QStringLiteral("The drive or folder to scan."),
                                 QStringLiteral("[folder]"));
    parser.process(application);

    int result = 0;
    {
        sm::qt::MainWindow window;
        if (parser.isSet(capture))
        {
            window.CaptureWhenShown(parser.value(capture));
        }
        window.show();
        if (QStringList const folders = parser.positionalArguments(); !folders.isEmpty())
        {
            window.StartScan(sm::posix::FromNative(RawArgument(argc, argv, folders.front())));
        }
        result = QApplication::exec();
    }
    if (sm::qt::MainWindow::LeftWorkersRunning())
    {
        // A scan stuck in the system must not keep the program, nor run on while
        // everything it could touch is torn down: end here.
        (void)std::fflush(nullptr);
        std::_Exit(result);
    }
    return result;
}
