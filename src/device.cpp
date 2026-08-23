#include <avernal/core/assert.hpp>
#include <avernal/rhi/d3d12.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

namespace avernal {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int count = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    AV_ENSURE(count > 0);

    std::string utf8(static_cast<std::size_t>(count), '\0');
    AV_ENSURE(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                  utf8.data(), count, nullptr, nullptr) == count);
    return utf8;
}

void try_enable_debug_layer() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
}

[[nodiscard]] bool try_create_device(
    IDXGIAdapter1* adapter, ComPtr<ID3D12Device>& device) {
    static constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_0,
    };

    for (const D3D_FEATURE_LEVEL level : levels) {
        if (SUCCEEDED(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&device)))) {
            return true;
        }
        device.Reset();
    }
    return false;
}

class D3D12Device final : public Device {
public:
    D3D12Device(ComPtr<ID3D12Device> device, std::string adapter_name)
        : device_(std::move(device)), adapter_name_(std::move(adapter_name)) {}

    [[nodiscard]] Backend backend() const noexcept override { return Backend::d3d12; }
    [[nodiscard]] std::string_view adapter_name() const noexcept override { return adapter_name_; }

private:
    ComPtr<ID3D12Device> device_;
    std::string adapter_name_;
};

}  // namespace

std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc) {
    AV_ENSURE(desc.is_valid());

    if (desc.debug) {
        try_enable_debug_layer();
    }

    UINT factory_flags = 0;
    if (desc.debug) {
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory));
    if (FAILED(hr) && factory_flags != 0) {
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    }
    AV_ENSURE(SUCCEEDED(hr));

    ComPtr<ID3D12Device> device;
    std::string adapter_name;

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc))) {
            continue;
        }
        if ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (!try_create_device(adapter.Get(), device)) {
            continue;
        }

        adapter_name = to_utf8(adapter_desc.Description);
        break;
    }

    if (device == nullptr) {
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            ComPtr<IDXGIAdapter1> warp1;
            if (SUCCEEDED(warp.As(&warp1)) && try_create_device(warp1.Get(), device)) {
                DXGI_ADAPTER_DESC1 adapter_desc{};
                if (SUCCEEDED(warp1->GetDesc1(&adapter_desc))) {
                    adapter_name = to_utf8(adapter_desc.Description);
                } else {
                    adapter_name = "WARP";
                }
            }
        }
    }

    if (device == nullptr) {
        return nullptr;
    }

    return std::make_unique<D3D12Device>(std::move(device), std::move(adapter_name));
}

}  // namespace avernal
