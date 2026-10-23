#include "common.h"

#include <wincodec.h>

namespace k7 {

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(width, height))) break;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&format))) break;
        if (FAILED(frame->WritePixels(height, pitch, pitch * height, const_cast<BYTE*>(bgra)))) break;
        if (FAILED(frame->Commit())) break;
        ok = SUCCEEDED(encoder->Commit());
    } while (false);
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) logf("save_png failed: %ls", path.c_str());
    return ok;
}

}  // namespace k7
