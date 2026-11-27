/* WASAPI shared-mode playback of the voice service's headset output, on Windows builds that do not
 * link SDL2 (PS3RECOMP_PAD_USE_SDL2=OFF). The default render endpoint takes 16 kHz mono 16-bit
 * samples; the audio engine converts them to its mix format. A worker thread owns the COM objects
 * and fills the endpoint buffer each time the engine asks. */
#include "mag_voice_output.h"
#include "mag_voice_core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

namespace mag::voice {
namespace {
template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string failed(const char* what, HRESULT hr) {
    char text[96];
    std::snprintf(text, sizeof text, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return text;
}

class WasapiOutput final : public VoiceOutput {
public:
    explicit WasapiOutput(RenderFn render) : render_(std::move(render)) {}
    ~WasapiOutput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool open(std::string& detail) {
        std::promise<std::string> opened;
        auto result = opened.get_future();
        thread_ = std::thread([this, &opened] { run(opened); });
        detail = result.get();
        if (detail.rfind("WASAPI", 0) == 0) return true;
        thread_.join();
        return false;
    }

private:
    void run(std::promise<std::string>& opened) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* renderer = nullptr;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::string error;
        UINT32 frames = 0;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) error = failed("device enumerator", hr);
        if (error.empty() && FAILED(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            error = failed("default playback device", hr);
        if (error.empty() && FAILED(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                          reinterpret_cast<void**>(&client))))
            error = failed("audio client", hr);
        if (error.empty()) {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = SampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 2;
            format.nAvgBytesPerSec = SampleRate * 2;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            // 100 ms of buffer, in 100-nanosecond units.
            if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format, nullptr)))
                error = failed("stream initialise", hr);
        }
        if (error.empty() && FAILED(hr = client->GetBufferSize(&frames))) error = failed("buffer size", hr);
        if (error.empty() && FAILED(hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderer))))
            error = failed("render client", hr);
        if (error.empty() && (!ready || FAILED(hr = client->SetEventHandle(ready)))) error = failed("event handle", hr);
        if (error.empty() && FAILED(hr = client->Start())) error = failed("stream start", hr);
        opened.set_value(error.empty() ? std::string("WASAPI shared playback") : error);
        while (error.empty() && !stop_) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= frames) continue;
            const UINT32 room = frames - padding;
            BYTE* buffer = nullptr;
            if (FAILED(renderer->GetBuffer(room, &buffer)) || !buffer) continue;
            render_(reinterpret_cast<int16_t*>(buffer), room);
            renderer->ReleaseBuffer(room, 0);
        }
        if (client) client->Stop();
        release(renderer);
        release(client);
        release(device);
        release(enumerator);
        if (ready) CloseHandle(ready);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    RenderFn render_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
}  // namespace

std::unique_ptr<VoiceOutput> open_device_output(RenderFn render, std::string& detail) {
    auto output = std::make_unique<WasapiOutput>(std::move(render));
    if (!output->open(detail)) return nullptr;
    return output;
}

}  // namespace mag::voice
