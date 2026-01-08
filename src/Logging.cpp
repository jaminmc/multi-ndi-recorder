#include "Logging.h"
#include <QDir>
#include <QMutexLocker>
#include <QDebug>
#include <QTextStream>
#include <iostream>
#include <cstdio>

Logger &Logger::instance()
{
    static Logger inst;
    return inst;
}

Logger::Logger()
    : m_file(), m_stream(&m_file), m_verbose(false)
{
    QDir().mkpath("logs");
    m_file.setFileName("logs/app.log");
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // Log file couldn't be opened, but continue anyway
        QTextStream(stderr) << "Warning: Could not open log file: " << m_file.fileName() << "\n";
    }
}

void Logger::setVerbose(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_verbose = enabled;
}

void Logger::log(const QString &message)
{
    QMutexLocker locker(&m_mutex);
    QString line = QString("[%1] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message);
    
    // Always write to file
    if (m_file.isOpen())
    {
        m_stream << line << "\n";
        m_stream.flush();
    }
    
    // Also output to console if verbose mode is enabled
    if (m_verbose)
    {
        QTextStream(stderr) << line << "\n";
        fflush(stderr);
    }
}

void Logger::verbose(const QString &message)
{
    QMutexLocker locker(&m_mutex);
    QString line = QString("[%1] [VERBOSE] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message);
    
    // Only write to file and console if verbose mode is enabled
    if (m_verbose)
    {
        // Write to file
        if (m_file.isOpen())
        {
            m_stream << line << "\n";
            m_stream.flush();
        }
        
        // Output to console
        QTextStream(stderr) << line << "\n";
        fflush(stderr);
    }
}

QString Logger::logFilePath() const
{
    return m_file.fileName();
}
