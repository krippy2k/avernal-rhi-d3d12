#include <avernal/core/assert.hpp>
#include <avernal/rhi/d3d12.hpp>
#include <avernal/window/window.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace avernal {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT frame_count = 2;

constexpr const char* color_shader = R"(
cbuffer Color : register(b0) {
    float4 color;
};

struct VSOut {
    float4 position : SV_Position;
};

VSOut vs_main(float2 position : POSITION) {
    VSOut output;
    output.position = float4(position, 0.0, 1.0);
    return output;
}

float4 ps_main(VSOut input) : SV_Target {
    return color;
}
)";

void throw_if_failed(HRESULT hr) {
    AV_ENSURE(SUCCEEDED(hr));
}

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

[[nodiscard]] bool try_create_device(IDXGIAdapter1* adapter, ComPtr<ID3D12Device>& device) {
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

[[nodiscard]] DXGI_FORMAT to_dxgi(Format format) {
    switch (format) {
    case Format::rgba8_unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::bgra8_unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        AV_UNREACHABLE("unsupported render-target format");
    }
}

[[nodiscard]] D3D12_RESOURCE_BARRIER barrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER result{};
    result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    result.Transition.pResource = resource;
    result.Transition.StateBefore = before;
    result.Transition.StateAfter = after;
    result.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return result;
}

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(
        color_shader, std::strlen(color_shader), "color.hlsl", nullptr, nullptr, entry, target, 0, 0,
        &blob, &errors);
    if (FAILED(hr)) {
        if (errors != nullptr) {
            std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        AV_ENSURE(false);
    }
    return blob;
}

class D3D12Buffer final : public Buffer {
public:
    ComPtr<ID3D12Resource> resource;
};

class D3D12Pipeline final : public Pipeline {
public:
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    float color[4]{};
};

class D3D12Device;

class D3D12Swapchain final : public Swapchain {
public:
    D3D12Swapchain(D3D12Device& device, const Window& window);

    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] Format format() const noexcept override { return Format::rgba8_unorm; }

    void resize(std::uint32_t width, std::uint32_t height) override;
    void recreate_buffers();

    [[nodiscard]] ID3D12Resource* current_target() const {
        return targets_[swapchain_->GetCurrentBackBufferIndex()].Get();
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE current_rtv() const {
        const UINT index = swapchain_->GetCurrentBackBufferIndex();
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
            static_cast<SIZE_T>(index) * rtv_stride_};
    }

    D3D12Device* device_{};
    HWND hwnd_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12Resource> targets_[frame_count];
    UINT rtv_stride_{};
};

class D3D12CommandList final : public CommandList {
public:
    explicit D3D12CommandList(ID3D12Device* device) {
        throw_if_failed(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)));
        throw_if_failed(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&commands_)));
        throw_if_failed(commands_->Close());
    }

    void reset() override {
        AV_ASSERT(!open_);
        throw_if_failed(allocator_->Reset());
        throw_if_failed(commands_->Reset(allocator_.Get(), nullptr));
        open_ = true;
        rendering_ = false;
        swapchain_ = nullptr;
    }

    void begin_render(Swapchain& swapchain) override {
        AV_ASSERT(open_);
        AV_ASSERT(!rendering_);
        swapchain_ = static_cast<D3D12Swapchain*>(&swapchain);
        if (swapchain_->width() == 0 || swapchain_->height() == 0) {
            swapchain_ = nullptr;
            return;
        }

        const D3D12_RESOURCE_BARRIER to_rtv = barrier(swapchain_->current_target(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands_->ResourceBarrier(1, &to_rtv);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->current_rtv();
        commands_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(swapchain_->width()),
            static_cast<float>(swapchain_->height()), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(swapchain_->width()),
            static_cast<LONG>(swapchain_->height())};
        commands_->RSSetViewports(1, &viewport);
        commands_->RSSetScissorRects(1, &scissor);
        rendering_ = true;
    }

    void clear_color(float r, float g, float b, float a) override {
        AV_ASSERT(rendering_);
        const float color[] = {r, g, b, a};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->current_rtv();
        commands_->ClearRenderTargetView(rtv, color, 0, nullptr);
    }

    void set_pipeline(Pipeline& pipeline) override {
        AV_ASSERT(rendering_);
        auto& d3d = static_cast<D3D12Pipeline&>(pipeline);
        commands_->SetGraphicsRootSignature(d3d.root_signature.Get());
        commands_->SetPipelineState(d3d.pipeline.Get());
        commands_->SetGraphicsRoot32BitConstants(0, 4, d3d.color, 0);
        commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void set_vertex_buffer(Buffer& buffer, std::uint32_t stride) override {
        AV_ASSERT(rendering_);
        AV_ASSERT(stride > 0);
        auto& d3d = static_cast<D3D12Buffer&>(buffer);
        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = d3d.resource->GetGPUVirtualAddress();
        view.SizeInBytes = static_cast<UINT>(d3d.resource->GetDesc().Width);
        view.StrideInBytes = stride;
        commands_->IASetVertexBuffers(0, 1, &view);
    }

    void draw(std::uint32_t vertex_count) override {
        AV_ASSERT(rendering_);
        AV_ASSERT(vertex_count > 0);
        commands_->DrawInstanced(vertex_count, 1, 0, 0);
    }

    void end_render() override {
        if (!rendering_) {
            return;
        }
        const D3D12_RESOURCE_BARRIER to_present = barrier(swapchain_->current_target(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        commands_->ResourceBarrier(1, &to_present);
        rendering_ = false;
        swapchain_ = nullptr;
    }

    void close() override {
        AV_ASSERT(open_);
        AV_ASSERT(!rendering_);
        throw_if_failed(commands_->Close());
        open_ = false;
    }

    [[nodiscard]] ID3D12GraphicsCommandList* native() const noexcept { return commands_.Get(); }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

private:
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    D3D12Swapchain* swapchain_{};
    bool open_{};
    bool rendering_{};
};

class D3D12Queue final : public Queue {
public:
    D3D12Queue(ID3D12Device* device, ComPtr<ID3D12CommandQueue> queue) : queue_(std::move(queue)) {
        throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        AV_ENSURE(fence_event_ != nullptr);
    }

    ~D3D12Queue() override {
        wait_idle();
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
        }
    }

    [[nodiscard]] ID3D12CommandQueue* native() const noexcept { return queue_.Get(); }

    void submit(CommandList& list) override {
        auto& d3d = static_cast<D3D12CommandList&>(list);
        AV_ASSERT(!d3d.is_open());
        ID3D12CommandList* lists[] = {d3d.native()};
        queue_->ExecuteCommandLists(1, lists);
    }

    void present(Swapchain& swapchain) override {
        auto& d3d = static_cast<D3D12Swapchain&>(swapchain);
        throw_if_failed(d3d.swapchain_->Present(1, 0));
        wait_idle();
    }

    void wait_idle() override {
        const UINT64 value = ++fence_value_;
        throw_if_failed(queue_->Signal(fence_.Get(), value));
        if (fence_->GetCompletedValue() < value) {
            throw_if_failed(fence_->SetEventOnCompletion(value, fence_event_));
            AV_ENSURE(WaitForSingleObject(fence_event_, INFINITE) == WAIT_OBJECT_0);
        }
    }

private:
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_{};
    UINT64 fence_value_{};
};

class D3D12Device final : public Device {
public:
    D3D12Device(ComPtr<IDXGIFactory4> factory, ComPtr<ID3D12Device> device,
        ComPtr<ID3D12CommandQueue> queue, std::string adapter_name)
        : factory_(std::move(factory)),
          device_(std::move(device)),
          graphics_queue_(device_.Get(), std::move(queue)),
          adapter_name_(std::move(adapter_name)) {}

    [[nodiscard]] Backend backend() const noexcept override { return Backend::d3d12; }
    [[nodiscard]] std::string_view adapter_name() const noexcept override { return adapter_name_; }
    [[nodiscard]] Queue& graphics_queue() noexcept override { return graphics_queue_; }

    [[nodiscard]] IDXGIFactory4* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] ID3D12Device* native_device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* native_queue() const noexcept {
        return graphics_queue_.native();
    }

    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const Window& window) override {
        return std::make_unique<D3D12Swapchain>(*this, window);
    }

    [[nodiscard]] std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) override;
    [[nodiscard]] std::unique_ptr<Pipeline> create_graphics_pipeline(
        const GraphicsPipelineDesc& desc) override;

    [[nodiscard]] std::unique_ptr<CommandList> create_command_list() override {
        return std::make_unique<D3D12CommandList>(device_.Get());
    }

private:
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    D3D12Queue graphics_queue_;
    std::string adapter_name_;
};

D3D12Swapchain::D3D12Swapchain(D3D12Device& device, const Window& window)
    : device_(&device),
      hwnd_(static_cast<HWND>(window.native_handle())),
      width_(window.width()),
      height_(window.height()) {
    AV_ENSURE(hwnd_ != nullptr);
    AV_ENSURE(width_ > 0 && height_ > 0);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors = frame_count;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    throw_if_failed(device_->native_device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)));
    rtv_stride_ = device_->native_device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = frame_count;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapchain;
    throw_if_failed(device_->factory()->CreateSwapChainForHwnd(
        device_->native_queue(), hwnd_, &desc, nullptr, nullptr, &swapchain));
    throw_if_failed(swapchain.As(&swapchain_));
    recreate_buffers();
}

void D3D12Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }

    device_->graphics_queue().wait_idle();
    width_ = width;
    height_ = height;
    for (auto& target : targets_) {
        target.Reset();
    }
    throw_if_failed(
        swapchain_->ResizeBuffers(frame_count, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    recreate_buffers();
}

void D3D12Swapchain::recreate_buffers() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < frame_count; ++i) {
        throw_if_failed(swapchain_->GetBuffer(i, IID_PPV_ARGS(&targets_[i])));
        device_->native_device()->CreateRenderTargetView(targets_[i].Get(), nullptr, rtv);
        rtv.ptr += rtv_stride_;
    }
}

std::unique_ptr<Buffer> D3D12Device::create_buffer(const BufferDesc& desc) {
    AV_ENSURE(desc.is_valid());

    auto buffer = std::make_unique<D3D12Buffer>();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = desc.size;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    throw_if_failed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer->resource)));

    if (desc.data != nullptr) {
        void* mapped = nullptr;
        throw_if_failed(buffer->resource->Map(0, nullptr, &mapped));
        std::memcpy(mapped, desc.data, static_cast<std::size_t>(desc.size));
        buffer->resource->Unmap(0, nullptr);
    }

    return buffer;
}

std::unique_ptr<Pipeline> D3D12Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const ComPtr<ID3DBlob> vs = compile_shader("vs_main", "vs_5_0");
    const ComPtr<ID3DBlob> ps = compile_shader("ps_main", "ps_5_0");

    D3D12_ROOT_PARAMETER color{};
    color.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    color.Constants.Num32BitValues = 4;
    color.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &color;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    throw_if_failed(
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr));

    auto pipeline = std::make_unique<D3D12Pipeline>();
    std::memcpy(pipeline->color, desc.color, sizeof(pipeline->color));
    throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
        root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));

    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = pipeline->root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.InputLayout = {input, 1};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = to_dxgi(desc.color_format);
    pso.SampleDesc.Count = 1;
    throw_if_failed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline->pipeline)));
    return pipeline;
}

[[nodiscard]] const D3D12Device& as_d3d12(const Device& device) {
    AV_ENSURE(device.backend() == Backend::d3d12);
    return static_cast<const D3D12Device&>(device);
}

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

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    AV_ENSURE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));

    return std::make_unique<D3D12Device>(
        std::move(factory), std::move(device), std::move(queue), std::move(adapter_name));
}

std::unique_ptr<Device> create_device(const DeviceDesc& desc) {
    AV_ENSURE(desc.is_valid());
    if (desc.backend == Backend::null) {
        return create_null_device();
    }
    if (desc.backend != Backend::d3d12) {
        return nullptr;
    }
    return create_d3d12_device(desc);
}

void* d3d12_native_factory(const Device& device) {
    return as_d3d12(device).factory();
}

void* d3d12_native_device(const Device& device) {
    return as_d3d12(device).native_device();
}

void* d3d12_native_queue(const Device& device) {
    return as_d3d12(device).native_queue();
}

}  // namespace avernal
