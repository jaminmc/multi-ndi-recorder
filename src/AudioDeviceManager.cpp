#include "AudioDeviceManager.h"
#include "Logging.h"

#ifdef Q_OS_WIN
#include <atlbase.h>
#include <Functiondiscoverykeys_devpkey.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <QVector>
#include <QDebug>
#endif

AudioDeviceManager::AudioDeviceManager(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = m_enum.CoCreateInstance(__uuidof(MMDeviceEnumerator));
    if (FAILED(hr))
    {
        Logger::instance().log("Failed to create MMDeviceEnumerator: " + QString::number(hr, 16));
    }
#endif
    enumerate();
}

AudioDeviceManager::~AudioDeviceManager()
{
#ifdef Q_OS_WIN
    CoUninitialize();
#endif
}

void AudioDeviceManager::enumerate()
{
    QMutexLocker locker(&m_mutex);
    m_devices.clear();

#ifdef Q_OS_WIN
    if (!m_enum)
        return;
    CComPtr<IMMDeviceCollection> collection;
    HRESULT hr = m_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr))
    {
        Logger::instance().log("Failed to enumerate audio endpoints: " + QString::number(hr, 16));
        return;
    }
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr))
    {
        Logger::instance().log("Failed to get audio endpoint count: " + QString::number(hr, 16));
        return;
    }
    for (UINT i = 0; i < count; ++i)
    {
        CComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device)))
        {
            CComPtr<IPropertyStore> props;
            hr = device->OpenPropertyStore(STGM_READ, &props);
            if (FAILED(hr) || !props)
            {
                Logger::instance().log("Failed to open property store for device " + QString::number(i) + ": " + QString::number(hr, 16));
                continue;
            }
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
            {
                m_devices << QString::fromWCharArray(varName.pwszVal);
                PropVariantClear(&varName);
            }
        }
    }
#elif defined(Q_OS_MACOS)
    // Enumerate Core Audio input devices
    UInt32 propertySize = 0;
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, nullptr, &propertySize);
    if (status != noErr || propertySize == 0)
    {
        Logger::instance().log("Failed to get audio device property size");
        return;
    }
    
    int deviceCount = propertySize / sizeof(AudioDeviceID);
    if (deviceCount == 0)
        return;
    
    QVector<AudioDeviceID> devices(deviceCount);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, nullptr, &propertySize, devices.data());
    if (status != noErr)
    {
        Logger::instance().log("Failed to get audio device list");
        return;
    }
    
    // Check each device for input capability
    for (int i = 0; i < deviceCount; ++i)
    {
        AudioDeviceID deviceID = devices[i];
        
        // Check if device has input channels
        propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
        propertyAddress.mScope = kAudioDevicePropertyScopeInput;
        propertyAddress.mElement = kAudioObjectPropertyElementMain;
        
        status = AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, nullptr, &propertySize);
        if (status != noErr || propertySize == 0)
            continue;
        
        AudioBufferList *bufferList = (AudioBufferList *)malloc(propertySize);
        status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nullptr, &propertySize, bufferList);
        bool hasInput = false;
        if (status == noErr && bufferList && bufferList->mNumberBuffers > 0)
        {
            hasInput = true;
        }
        free(bufferList);
        
        if (!hasInput)
            continue;
        
        // Get device name
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        CFStringRef deviceNameCF = nullptr;
        propertySize = sizeof(deviceNameCF);
        status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nullptr, &propertySize, &deviceNameCF);
        if (status == noErr && deviceNameCF)
        {
            char deviceName[256];
            Boolean result = CFStringGetCString(deviceNameCF, deviceName, sizeof(deviceName), kCFStringEncodingUTF8);
            if (result)
            {
                m_devices << QString::fromUtf8(deviceName);
            }
            CFRelease(deviceNameCF);
        }
    }
#else
    // Linux: Could use ALSA or PulseAudio here if needed
    Logger::instance().log("Audio device enumeration not implemented for this platform");
#endif
}

QStringList AudioDeviceManager::inputDevices()
{
    enumerate();
    return m_devices;
}

void AudioDeviceManager::refresh()
{
    enumerate();
}

#ifdef Q_OS_WIN
IMMDevice *AudioDeviceManager::deviceByName(const QString &name)
{
    if (!m_enum)
        return nullptr;
    CComPtr<IMMDeviceCollection> collection;
    HRESULT hr = m_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr))
    {
        return nullptr;
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count)))
    {
        return nullptr;
    }
    for (UINT i = 0; i < count; ++i)
    {
        CComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device)))
        {
            CComPtr<IPropertyStore> props;
            if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props)
            {
                continue;
            }
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
            {
                QString devName = QString::fromWCharArray(varName.pwszVal);
                PropVariantClear(&varName);
                if (devName == name)
                {
                    device.p->AddRef();
                    return device;
                }
            }
        }
    }
    return nullptr;
}
#else
void *AudioDeviceManager::deviceByName(const QString &name)
{
    // Not implemented for non-Windows platforms
    Q_UNUSED(name);
    return nullptr;
}
#endif
