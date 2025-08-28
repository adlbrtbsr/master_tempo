#pragma once

#if JUCE_WINDOWS
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <future>
#include <JuceHeader.h>

class WASAPILoopbackCapture
{
public:
    using SampleReadyFn = std::function<void (const float* interleaved, int numFrames, int numChannels, double sampleRate)>;

    WASAPILoopbackCapture() = default;
    ~WASAPILoopbackCapture() { stop(); }

    static juce::StringArray listRenderEndpoints()
    {
        juce::StringArray names;
        const auto comHr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
        {
            Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
            if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
            {
                UINT count = 0; devices->GetCount (&count);
                for (UINT i = 0; i < count; ++i)
                {
                    Microsoft::WRL::ComPtr<IMMDevice> dev;
                    if (FAILED (devices->Item (i, &dev))) continue;
                    Microsoft::WRL::ComPtr<IPropertyStore> props;
                    if (FAILED (dev->OpenPropertyStore (STGM_READ, &props))) continue;
                    PROPVARIANT varName; PropVariantInit (&varName);
                    if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &varName)))
                        names.add (juce::String (varName.pwszVal));
                    PropVariantClear (&varName);
                }
            }
        }
        if (comHr == S_OK) CoUninitialize();
        return names;
    }

    juce::String getLastError() const { return lastError; }

    bool start (const juce::String& outputFriendlyNameContains, SampleReadyFn onSamples)
    {
        stop();
        sampleCallback = std::move (onSamples);

        std::promise<bool> initPromise;
        auto initFuture = initPromise.get_future();
        auto promiseSet = std::make_shared<std::atomic<bool>>(false);

        running = true;
        captureThread = std::thread ([this, outputFriendlyNameContains, p = std::move (initPromise), promiseSet]() mutable
        {
            const auto comHr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            const bool comInited = SUCCEEDED (comHr) || comHr == RPC_E_CHANGED_MODE; // already initialized STA/MTA

            auto finish = [&](bool ok)
            {
                if (!promiseSet->exchange (true))
                {
                    try { p.set_value (ok); } catch (...) {}
                }
            };

            if (! comInited)
            {
                lastError = "CoInitializeEx failed";
                finish (false);
                return;
            }

            Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
            if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
            {
                lastError = "MMDeviceEnumerator creation failed";
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
            if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
            {
                lastError = "EnumAudioEndpoints failed";
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            UINT count = 0;
            devices->GetCount (&count);

            Microsoft::WRL::ComPtr<IMMDevice> chosen;
            for (UINT i = 0; i < count; ++i)
            {
                Microsoft::WRL::ComPtr<IMMDevice> dev;
                if (FAILED (devices->Item (i, &dev))) continue;
                Microsoft::WRL::ComPtr<IPropertyStore> props;
                if (FAILED (dev->OpenPropertyStore (STGM_READ, &props))) continue;
                PROPVARIANT varName; PropVariantInit (&varName);
                if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &varName)))
                {
                    juce::String name = juce::String (varName.pwszVal);
                    if (name.containsIgnoreCase (outputFriendlyNameContains) || name.containsIgnoreCase ("Speakers"))
                        chosen = dev;
                    DBG ("Render endpoint: " + name);
                }
                PropVariantClear (&varName);
            }
            if (chosen == nullptr)
            {
                // Fallback to default render endpoint
                if (FAILED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &chosen)))
                {
                    lastError = "No matching device and GetDefaultAudioEndpoint failed";
                    finish (false);
                    if (comHr == S_OK) CoUninitialize();
                    return;
                }
            }

            Microsoft::WRL::ComPtr<IAudioClient> client;
            if (FAILED (chosen->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &client)))
            {
                lastError = "IAudioClient activation failed";
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            WAVEFORMATEX* mix = nullptr;
            if (FAILED (client->GetMixFormat (&mix)) || mix == nullptr)
            {
                lastError = "GetMixFormat failed";
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            auto isFloatFormat = [](const WAVEFORMATEX* wf) -> bool
            {
                if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) return true;
                if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    const auto* wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
                    return IsEqualGUID (wfx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wfx->Samples.wValidBitsPerSample == 32;
                }
                return false;
            };

            // For loopback, the stream format must match the device mix format in shared mode
            WAVEFORMATEX* formatToUse = mix;

            // Use default period in shared mode: set both durations to 0
            HRESULT initHr = client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                 0, 0, formatToUse, nullptr);
            if (FAILED (initHr))
            {
                // Fallback: try default render endpoint explicitly
                Microsoft::WRL::ComPtr<IMMDevice> def;
                if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &def)))
                {
                    client.Reset();
                    if (SUCCEEDED (def->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &client)))
                    {
                        WAVEFORMATEX* mix2 = nullptr;
                        if (SUCCEEDED (client->GetMixFormat (&mix2)) && mix2 != nullptr)
                        {
                            CoTaskMemFree (mix);
                            mix = mix2;
                            formatToUse = mix;
                            initHr = client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                                         AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                         0, 0, formatToUse, nullptr);
                        }
                    }
                }

                if (FAILED (initHr))
                {
                    juce::String hrStr = "0x" + juce::String::toHexString ((int) initHr);
                    lastError = "IAudioClient::Initialize failed, hr=" + hrStr;
                    CoTaskMemFree (mix);
                    finish (false);
                    if (comHr == S_OK) CoUninitialize();
                    return;
                }
            }

            Microsoft::WRL::ComPtr<IAudioCaptureClient> cap;
            if (FAILED (client->GetService (__uuidof (IAudioCaptureClient), &cap)))
            {
                lastError = "GetService(IAudioCaptureClient) failed";
                CoTaskMemFree (mix);
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            const bool inputIsFloat = isFloatFormat (formatToUse);
            const int channels = formatToUse->nChannels;
            const double sr = (double) formatToUse->nSamplesPerSec;
            auto getValidBits = [](const WAVEFORMATEX* wf) -> int
            {
                if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    const auto* wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
                    return (int) wfx->Samples.wValidBitsPerSample;
                }
                return (int) wf->wBitsPerSample;
            };
            juce::HeapBlock<float> convertBuffer;

            if (FAILED (client->Start()))
            {
                lastError = "IAudioClient::Start failed";
                CoTaskMemFree (mix);
                finish (false);
                if (comHr == S_OK) CoUninitialize();
                return;
            }

            finish (true);

            while (running)
            {
                UINT32 packetFrames = 0;
                if (cap->GetNextPacketSize (&packetFrames) == S_OK && packetFrames > 0)
                {
                    BYTE* data = nullptr; UINT32 numFrames = 0; DWORD flags = 0; UINT64 pos = 0; UINT64 qpc = 0;
                    if (SUCCEEDED (cap->GetBuffer (&data, &numFrames, &flags, &pos, &qpc)))
                    {
                        const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                        const size_t numSamples = (size_t) numFrames * (size_t) channels;
                        const float* floatData = nullptr;

                        if (isSilent)
                        {
                            convertBuffer.allocate (numSamples, true);
                            juce::FloatVectorOperations::clear (convertBuffer.get(), (int) numSamples);
                            floatData = convertBuffer.get();
                        }
                        else if (inputIsFloat)
                        {
                            floatData = reinterpret_cast<const float*> (data);
                        }
                        else
                        {
                            // Minimal PCM16 -> float32 conversion fallback
                            convertBuffer.allocate (numSamples, true);
                            const int validBits = getValidBits (formatToUse);
                            if (validBits == 16)
                            {
                                auto* in16 = reinterpret_cast<const int16_t*> (data);
                                for (size_t i = 0; i < numSamples; ++i)
                                    convertBuffer[i] = (float) (in16[i] / 32768.0f);
                            }
                            else
                            {
                                // Unknown format, zero out to avoid garbage
                                juce::FloatVectorOperations::clear (convertBuffer.get(), (int) numSamples);
                            }
                            floatData = convertBuffer.get();
                        }

                        if (sampleCallback && numFrames > 0 && channels > 0)
                            sampleCallback (floatData, (int) numFrames, channels, sr);

                        cap->ReleaseBuffer (numFrames);
                    }
                }
                ::Sleep (2);
            }

            client->Stop();
            CoTaskMemFree (mix);
            if (comHr == S_OK) CoUninitialize();
        });

        return initFuture.get();
    }

    void stop()
    {
        running = false;
        if (captureThread.joinable()) captureThread.join();
        sampleCallback = nullptr;
    }

private:
    std::thread captureThread;
    std::atomic<bool> running { false };
    SampleReadyFn sampleCallback;
    juce::String lastError;
};

#endif // JUCE_WINDOWS


