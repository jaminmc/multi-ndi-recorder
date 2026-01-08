#pragma once
#include <QObject>
#include <QStringList>
#include <QMutex>

#ifdef Q_OS_WIN
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <atlbase.h>
#endif

#ifdef Q_OS_MACOS
#include <CoreAudio/CoreAudio.h>
#endif

class AudioDeviceManager : public QObject
{
    Q_OBJECT
public:
    explicit AudioDeviceManager(QObject *parent = nullptr);
    ~AudioDeviceManager();

    QStringList inputDevices();
    void refresh();
    
#ifdef Q_OS_WIN
    IMMDevice *deviceByName(const QString &name);
#else
    void *deviceByName(const QString &name); // Platform-agnostic, returns nullptr on non-Windows
#endif

private:
    void enumerate();
    QStringList m_devices;
    QMutex m_mutex;
    
#ifdef Q_OS_WIN
    CComPtr<IMMDeviceEnumerator> m_enum;
#endif
};
