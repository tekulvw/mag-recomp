/* WASAPI shared-mode microphone capture for the MAG voice service, on Windows builds that do not
 * link SDL2 (PS3RECOMP_PAD_USE_SDL2=OFF). The endpoint's mix format (usually 32-bit float at 48 kHz)
 * goes to the portable CaptureBuffer, which makes the service's 16 kHz mono of it. A refused
 * Windows microphone privacy setting fails the open, and voice stays receive-only. */
#include "mag_voice_capture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace mag::voice {
namespace {
template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(size_t(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string hresult(const char* what, HRESULT hr) {
    if (hr == E_ACCESSDENIED) return "microphone access is off in the Windows privacy settings";
    char text[96];
    std::snprintf(text, sizeof text, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return text;
}

enum class Sample { Float32, Int16, Int32, Unsupported };

Sample sample_kind(const WAVEFORMATEX* format) {
    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) return Sample::Float32;
    if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) return Sample::Int16;
    if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32) return Sample::Int32;
    return Sample::Unsupported;
}

class WasapiCapture final : public CaptureDevice {
public:
    ~WasapiCapture() override { close(); }

    Open open(CaptureBuffer& sink, const std::string& name, std::string& error) override {
        sink_ = &sink;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) {
            error = "cannot create an event";
            return Open::Failed;
        }
        std::promise<std::string> ready;
        auto result = ready.get_future();
        thread_ = std::thread([this, name, &ready] { run(name, ready); });
        error = result.get();
        if (!error.empty()) {
            thread_.join();
            CloseHandle(stop_);
            stop_ = nullptr;
            return Open::Failed;
        }
        return Open::Opened;
    }

    void close() override {
        if (!thread_.joinable()) return;
        SetEvent(stop_);
        thread_.join();
        CloseHandle(stop_);
        stop_ = nullptr;
    }

private:
    // The capture thread: COM, the endpoint and the client live here; `ready` gets "" once audio
    // flows, or the reason it cannot.
    void run(const std::string& name, std::promise<std::string>& ready) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* devices = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* format = nullptr;
        HANDLE packet = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::string error;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&devices));
        if (FAILED(hr)) error = hresult("the device list", hr);
        if (error.empty()) {
            if (name.empty()) {
                hr = devices->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
                if (FAILED(hr)) error = hr == E_NOTFOUND ? "no capture device" : hresult("the default capture device", hr);
            } else {
                device = find(devices, name);
                if (!device) error = "no capture device named \"" + name + "\"";
            }
        }
        if (error.empty()) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
            if (FAILED(hr)) error = hresult("opening the capture device", hr);
        }
        if (error.empty()) {
            hr = client->GetMixFormat(&format);
            if (FAILED(hr)) error = hresult("reading the capture format", hr);
            else if (sample_kind(format) == Sample::Unsupported) error = "unsupported capture format";
        }
        if (error.empty()) {
            constexpr REFERENCE_TIME buffer = 2000000;  // 200 ms, in 100 ns units
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer, 0, format, nullptr);
            if (FAILED(hr)) error = hresult("starting the capture", hr);
        }
        if (error.empty() && FAILED(hr = client->SetEventHandle(packet))) error = hresult("the capture event", hr);
        if (error.empty() && FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))))
            error = hresult("the capture service", hr);
        if (error.empty() && FAILED(hr = client->Start())) error = hresult("starting the capture", hr);
        if (error.empty())
            std::fprintf(stderr, "[voice] capture through WASAPI: %lu Hz, %u channel(s)\n",
                         static_cast<unsigned long>(format->nSamplesPerSec), unsigned(format->nChannels));
        const bool running = error.empty();
        ready.set_value(error);  // `ready` is gone after this
        if (running) pump(client, capture, format, packet);
        if (client) client->Stop();
        release(capture);
        release(client);
        if (format) CoTaskMemFree(format);
        release(device);
        release(devices);
        if (packet) CloseHandle(packet);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void pump(IAudioClient*, IAudioCaptureClient* capture, const WAVEFORMATEX* format, HANDLE packet) {
        const Sample kind = sample_kind(format);
        const unsigned channels = format->nChannels ? format->nChannels : 1, rate = format->nSamplesPerSec;
        std::vector<float> scratch;
        HANDLE waits[2] = {stop_, packet};
        for (;;) {
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (woke == WAIT_OBJECT_0) return;
            UINT32 frames = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
                scratch.assign(size_t(frames) * channels, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    for (size_t i = 0; i < scratch.size(); ++i) {
                        if (kind == Sample::Float32) std::memcpy(&scratch[i], data + 4 * i, 4);
                        else if (kind == Sample::Int16) {
                            int16_t v;
                            std::memcpy(&v, data + 2 * i, 2);
                            scratch[i] = v / 32768.0f;
                        } else {
                            int32_t v;
                            std::memcpy(&v, data + 4 * i, 4);
                            scratch[i] = float(v / 2147483648.0);
                        }
                    }
                }
                capture->ReleaseBuffer(frames);
                sink_->push(scratch.data(), frames, rate, channels);
            }
        }
    }

    static IMMDevice* find(IMMDeviceEnumerator* devices, const std::string& name) {
        IMMDeviceCollection* list = nullptr;
        if (FAILED(devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list))) return nullptr;
        UINT count = 0;
        list->GetCount(&count);
        IMMDevice* found = nullptr;
        for (UINT i = 0; i < count && !found; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(list->Item(i, &device))) continue;
            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR &&
                    utf8(value.pwszVal) == name)
                    found = device;
                PropVariantClear(&value);
                release(properties);
            }
            if (found != device) release(device);
        }
        release(list);
        return found;
    }

    CaptureBuffer* sink_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread thread_;
};
}  // namespace

CaptureDeviceFactory host_capture_devices() {
    return [] { return std::unique_ptr<CaptureDevice>(new WasapiCapture()); };
}

}  // namespace mag::voice
