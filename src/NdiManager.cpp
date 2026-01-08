#include "NdiManager.h"
#include "Logging.h"
#include <QMutexLocker>
#include <Processing.NDI.Lib.h>

void *NdiManager::s_finder = nullptr;
QStringList NdiManager::s_sources;
QMutex NdiManager::s_mutex;

NdiManager::NdiManager(QObject *parent)
    : QObject(parent)
{
    ensureInitialized();
}

NdiManager::~NdiManager() = default;

void NdiManager::ensureInitialized()
{
    if (!NDIlib_initialize())
    {
        Logger::instance().log("Failed to initialize NDI");
    }
    if (!s_finder)
    {
        s_finder = (void*)NDIlib_find_create_v2();
        if (!s_finder)
        {
            Logger::instance().log("Failed to create NDI finder");
        }
    }
}

QStringList NdiManager::availableSources()
{
    QMutexLocker locker(&s_mutex);
    refreshSources();
    return s_sources;
}

void NdiManager::refreshSources()
{
    if (!s_finder)
    {
        ensureInitialized();
    }
    uint32_t no_sources = 0;
    const NDIlib_source_t *sources = NDIlib_find_get_current_sources((NDIlib_find_instance_t)s_finder, &no_sources);
    QStringList list;
    for (uint32_t i = 0; i < no_sources; ++i)
    {
        list << QString::fromUtf8(sources[i].p_ndi_name);
    }
    s_sources = list;
}
