#pragma once
#include <QObject>
#include <QStringList>
#include <QMutex>

// Forward declaration to avoid MOC processing NDI headers
struct NDIlib_find_instance;

class NdiManager : public QObject
{
    Q_OBJECT
public:
    explicit NdiManager(QObject *parent = nullptr);
    ~NdiManager();

    QStringList availableSources();
    void refreshSources();

private:
    void ensureInitialized();
    static void *s_finder; // NDIlib_find_instance_t - cast in .cpp to avoid MOC issues
    static QStringList s_sources;
    static QMutex s_mutex;
};
