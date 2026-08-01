//go:build windows

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

extern "C" int dxgi_adapter_count() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return 0;
    }

    int count = 0;
    IDXGIAdapter1* adapter = nullptr;
    while (factory->EnumAdapters1(static_cast<UINT>(count), &adapter) != DXGI_ERROR_NOT_FOUND) {
        if (!adapter) {
            break;
        }
        adapter->Release();
        adapter = nullptr;
        ++count;
    }
    factory->Release();
    return count;
}

extern "C" int dxgi_adapter_name(int index, char* buffer, int buffer_size) {
    if (!buffer || buffer_size <= 0 || index < 0) {
        return 0;
    }
    buffer[0] = '\0';

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return 0;
    }

    IDXGIAdapter1* adapter = nullptr;
    const HRESULT result = factory->EnumAdapters1(static_cast<UINT>(index), &adapter);
    if (FAILED(result) || !adapter) {
        factory->Release();
        return 0;
    }

    DXGI_ADAPTER_DESC1 description = {};
    const HRESULT descResult = adapter->GetDesc1(&description);
    adapter->Release();
    factory->Release();
    if (FAILED(descResult)) {
        return 0;
    }

    return WideCharToMultiByte(
        CP_UTF8,
        0,
        description.Description,
        -1,
        buffer,
        buffer_size,
        nullptr,
        nullptr
    ) > 0;
}
