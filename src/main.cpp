#include <QApplication>
#include <QCommandLineParser>
#include "MainWindow.h"
#include "Logging.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setOrganizationName("MultiNdiRecorder");
    QApplication::setApplicationName("MultiNdiRecorder");
    QApplication::setApplicationVersion("1.0");
    
    // Parse command-line arguments
    QCommandLineParser parser;
    parser.setApplicationDescription("Multi NDI Recorder - Record multiple NDI sources simultaneously");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption verboseOption(QStringList() << "verbose",
                                     "Enable verbose output for debugging");
    parser.addOption(verboseOption);
    
    parser.process(a);
    
    // Enable verbose mode if requested
    if (parser.isSet(verboseOption))
    {
        Logger::instance().setVerbose(true);
        QTextStream(stderr) << "Verbose mode enabled\n";
    }
    
    Logger::instance().log("Application started");
    MainWindow w;
    w.show();
    int ret = a.exec();
    Logger::instance().log("Application exit");
    return ret;
}
