#pragma once
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QDateTime>

class Logger
{
public:
    static Logger &instance();
    void log(const QString &message);
    void verbose(const QString &message);  // Verbose logging (only if verbose mode enabled)
    QString logFilePath() const;
    void setVerbose(bool enabled);

private:
    Logger();
    QFile m_file;
    QTextStream m_stream;
    mutable QMutex m_mutex;
    bool m_verbose;
};
