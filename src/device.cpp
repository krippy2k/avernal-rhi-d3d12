#include <avernal/core/assert.hpp>
#include <avernal/rhi/d3d12.hpp>
#include <avernal/window/window.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <d3dx12.h>

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

VSOut vs_main(float3 position : POSITION) {
    VSOut output;
    output.position = float4(position.xy, 0.0, 1.0);  // Use only XY for 2D
    return output;
}

float4 ps_main(VSOut input) : SV_Target {
    return color;
}
)";

constexpr const char* texture_shader = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct VSIn {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD;
    float4 color : COLOR;
};

struct VSOut {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float4 color : COLOR;
};

VSOut vs_main(VSIn input) {
    VSOut output;
    output.position = float4(input.position.xy, 0.0, 1.0);  // Use only XY for 2D
    output.texcoord = input.texcoord;
    output.color = input.color;
    return output;
}

float4 ps_main(VSOut input) : SV_Target {
    return tex.Sample(samp, input.texcoord) * input.color;
}
)";

constexpr const char* texture_3d_shader = R"(
cbuffer Transform : register(b0) {
    float4x4 mvp;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct VSIn {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD;
};

struct VSOut {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
};

VSOut vs_main(VSIn input) {
    VSOut output;
    output.position = mul(mvp, float4(input.position, 1.0));
    output.texcoord = input.texcoord;
    return output;
}

float4 ps_main(VSOut input) : SV_Target {
    return tex.Sample(samp, input.texcoord);
}
)";

constexpr const char* depth_only_shader = R"(
cbuffer Transform : register(b0) {
    float4x4 mvp;
};

float4 vs_main(float3 position : POSITION) : SV_Position {
    return mul(mvp, float4(position, 1.0));
}
)";

constexpr const char* pbr_shader = R"(
cbuffer Frame : register(b0) {
    float4x4 mvp;
    float4x4 model;
    float4x4 light_view_proj;
    float4 light_dir_intensity;
    float4 light_color_ambient;
    float4 camera_pos;
    float4 material;
};

Texture2D tex : register(t0);
Texture2D shadow_map : register(t1);
SamplerState samp : register(s0);
SamplerState shadow_samp : register(s1);

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD;
};

struct VSOut {
    float4 position : SV_Position;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float2 texcoord : TEXCOORD0;
    float4 light_clip : TEXCOORD3;
};

static const float PI = 3.14159265;

VSOut vs_main(VSIn input) {
    VSOut output;
    float4 world = mul(model, float4(input.position, 1.0));
    output.world_pos = world.xyz;
    // Inverse-transpose via cofactors so non-uniform scale keeps face normals.
    float3x3 m = (float3x3)model;
    float3 c0 = mul(m, float3(1.0, 0.0, 0.0));
    float3 c1 = mul(m, float3(0.0, 1.0, 0.0));
    float3 c2 = mul(m, float3(0.0, 0.0, 1.0));
    output.world_normal = cross(c1, c2) * input.normal.x +
                          cross(c2, c0) * input.normal.y +
                          cross(c0, c1) * input.normal.z;
    output.texcoord = input.texcoord;
    output.light_clip = mul(light_view_proj, world);
    output.position = mul(mvp, float4(input.position, 1.0));
    return output;
}

float distribution_ggx(float3 n, float3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float geometry_schlick_ggx(float ndotx, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotx / (ndotx * (1.0 - k) + k);
}

float geometry_smith(float3 n, float3 v, float3 l, float roughness) {
    return geometry_schlick_ggx(max(dot(n, v), 0.0), roughness) *
           geometry_schlick_ggx(max(dot(n, l), 0.0), roughness);
}

float3 fresnel_schlick(float cos_theta, float3 f0) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cos_theta), 5.0);
}

float shadow_visibility(float4 light_clip, float ndotl) {
    float3 ndc = light_clip.xyz / max(light_clip.w, 0.0001);
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    float bias = max(0.004 * (1.0 - ndotl), 0.0015);
    float w;
    float h;
    shadow_map.GetDimensions(w, h);
    float2 texel = 1.0 / float2(w, h);
    float shadow = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float closest = shadow_map.Sample(shadow_samp, uv + float2(x, y) * texel).r;
            shadow += ndc.z <= closest + bias ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

float4 ps_main(VSOut input) : SV_Target {
    float3 albedo = tex.Sample(samp, input.texcoord).rgb;
    float metallic = material.x;
    float roughness = clamp(material.y, 0.04, 1.0);

    float3 v = normalize(camera_pos.xyz - input.world_pos);
    float nlen = length(input.world_normal);
    float3 n = nlen > 1e-8 ? input.world_normal / nlen : float3(0.0, 1.0, 0.0);
    float3 n_geo = cross(ddx(input.world_pos), ddy(input.world_pos));
    if (dot(n_geo, n_geo) > 1e-12) {
        n_geo = normalize(n_geo);
        // Keep the authored facing. Facing the camera two-sides walls and
        // makes the inner/outer faces look inverted.
        if (dot(n, n_geo) < 0.0) {
            n_geo = -n_geo;
        }
        if (nlen <= 1e-8 || dot(n, n_geo) < 0.2) {
            n = n_geo;
        }
    }

    float3 l = normalize(-light_dir_intensity.xyz);
    float3 h = normalize(v + l);
    float ndotl = max(dot(n, l), 0.0);
    float wrap = saturate(dot(n, l) * 0.5 + 0.5);

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float ndf = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, l, roughness);
    float3 f = fresnel_schlick(max(dot(h, v), 0.0), f0);

    float3 specular = (ndf * g * f) / max(4.0 * max(dot(n, v), 0.0) * ndotl, 0.001);
    float3 radiance = light_color_ambient.rgb * light_dir_intensity.w;
    // Soften Lambert on dielectrics so vertical walls are not unlit cardboard.
    float diffuse_light = lerp(ndotl, wrap, 0.35);
    float3 diffuse = albedo * (1.0 - metallic) * diffuse_light;
    float3 metal_scatter = f0 * wrap * metallic * 0.55;
    float3 direct = (diffuse + metal_scatter + specular * ndotl) * radiance;

    float shadow = shadow_visibility(input.light_clip, ndotl);
    float3 r = reflect(-v, n);
    float env_w = saturate(r.y * 0.5 + 0.5);
    float3 fake_env = lerp(float3(0.34, 0.30, 0.24), float3(1.05, 1.00, 0.90), env_w);
    float3 metal_ibl = f0 * fake_env * (1.15 - 0.35 * roughness);
    float hemi = saturate(n.y * 0.5 + 0.5);
    float3 dielectric_ibl = albedo * light_color_ambient.a * (0.48 + 0.22 * hemi);
    float3 ambient = lerp(dielectric_ibl, metal_ibl, metallic);
    // Metals keep most of the fake IBL in shadow so thin casters do not go black.
    float ambient_shadow = lerp(0.50, 0.72, metallic) + lerp(0.50, 0.28, metallic) * shadow;
    float3 color = ambient * ambient_shadow + direct * shadow;
    return float4(saturate(color), 1.0);
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
    case Format::d32_float:
        return DXGI_FORMAT_D32_FLOAT;
    case Format::r32_float:
        return DXGI_FORMAT_R32_FLOAT;
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

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(
    const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "shader.hlsl", nullptr, nullptr, entry,
        target, 0, 0, &blob, &errors);
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

class D3D12Texture final : public Texture {
public:
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> upload;
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    std::uint32_t width{};
    std::uint32_t height{};
    bool is_depth{};
};

class D3D12Pipeline final : public Pipeline {
public:
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    float color[4]{};
    bool use_texture{};
    bool use_3d{};
    bool use_pbr{};
    bool depth_only{};
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

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv() const {
        return dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12Device* device_{};
    HWND hwnd_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12Resource> depth_buffer_;
    ComPtr<ID3D12Resource> targets_[frame_count];
    UINT rtv_stride_{};
};

class D3D12CommandList final : public CommandList {
public:
    explicit D3D12CommandList(D3D12Device* device);

    void reset() override {
        AV_ASSERT(!open_);
        throw_if_failed(allocator_->Reset());
        throw_if_failed(commands_->Reset(allocator_.Get(), nullptr));
        open_ = true;
        rendering_ = false;
        swapchain_ = nullptr;
        depth_target_ = nullptr;
        current_pipeline_ = nullptr;
    }

    void begin_render(Swapchain& swapchain) override;

    void begin_depth(Texture& depth) override;

    void clear_color(const Color& color) override {
        AV_ASSERT(rendering_);
        const float color_array[] = {color.r, color.g, color.b, color.a};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->current_rtv();
        commands_->ClearRenderTargetView(rtv, color_array, 0, nullptr);
    }

    void clear_depth(float depth) override {
        AV_ASSERT(rendering_);
        if (depth_target_ != nullptr) {
            commands_->ClearDepthStencilView(
                depth_target_->dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
        } else if (swapchain_ != nullptr) {
            commands_->ClearDepthStencilView(
                swapchain_->dsv(), D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
        }
    }

    void set_pipeline(Pipeline& pipeline) override {
        AV_ASSERT(rendering_);
        auto& d3d = static_cast<D3D12Pipeline&>(pipeline);
        current_pipeline_ = &d3d;
        commands_->SetGraphicsRootSignature(d3d.root_signature.Get());
        commands_->SetPipelineState(d3d.pipeline.Get());
        if (!d3d.use_texture && !d3d.use_3d) {
            commands_->SetGraphicsRoot32BitConstants(0, 4, d3d.color, 0);
        }
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

    void set_index_buffer(Buffer& buffer) override {
        AV_ASSERT(rendering_);
        auto& d3d = static_cast<D3D12Buffer&>(buffer);
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = d3d.resource->GetGPUVirtualAddress();
        view.SizeInBytes = static_cast<UINT>(d3d.resource->GetDesc().Width);
        view.Format = DXGI_FORMAT_R16_UINT;
        commands_->IASetIndexBuffer(&view);
    }

    void set_constant_buffer(Buffer& buffer, std::uint32_t slot) override {
        AV_ASSERT(rendering_);
        auto& d3d = static_cast<D3D12Buffer&>(buffer);
        commands_->SetGraphicsRootConstantBufferView(slot, d3d.resource->GetGPUVirtualAddress());
    }

    void set_texture(Texture& texture, std::uint32_t index) override {
        AV_ASSERT(rendering_);
        AV_ASSERT(current_pipeline_ != nullptr);
        if (current_pipeline_->depth_only) {
            return;
        }
        auto& d3d = static_cast<D3D12Texture&>(texture);
        const UINT slot = current_pipeline_->use_3d ? 1u + index : index;
        commands_->SetGraphicsRootDescriptorTable(slot, d3d.srv_gpu);
    }

    void set_color(const Color& color) override {
        AV_ASSERT(rendering_);
        if (current_pipeline_ == nullptr || current_pipeline_->use_texture ||
            current_pipeline_->use_3d) {
            return;
        }
        const float values[] = {color.r, color.g, color.b, color.a};
        commands_->SetGraphicsRoot32BitConstants(0, 4, values, 0);
    }

    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex) override {
        AV_ASSERT(rendering_);
        AV_ASSERT(vertex_count > 0);
        commands_->DrawInstanced(vertex_count, 1, first_vertex, 0);
    }

    void draw_indexed(std::uint32_t index_count) override {
        AV_ASSERT(rendering_);
        AV_ASSERT(index_count > 0);
        commands_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    }

    void end_render() override {
        if (!rendering_) {
            return;
        }
        if (swapchain_ != nullptr) {
            const D3D12_RESOURCE_BARRIER to_present = barrier(swapchain_->current_target(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            commands_->ResourceBarrier(1, &to_present);
            swapchain_ = nullptr;
        }
        if (depth_target_ != nullptr) {
            if (depth_target_->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                const D3D12_RESOURCE_BARRIER to_shader = barrier(depth_target_->resource.Get(),
                    depth_target_->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commands_->ResourceBarrier(1, &to_shader);
                depth_target_->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            depth_target_ = nullptr;
        }
        rendering_ = false;
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
    D3D12Device* device_{};
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    D3D12Swapchain* swapchain_{};
    D3D12Texture* depth_target_{};
    D3D12Pipeline* current_pipeline_{};
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
          adapter_name_(std::move(adapter_name)) {
        // Create descriptor heap for SRVs
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = 256;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throw_if_failed(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)));
        srv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap{};
        dsv_heap.NumDescriptors = 16;
        dsv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        throw_if_failed(device_->CreateDescriptorHeap(&dsv_heap, IID_PPV_ARGS(&dsv_heap_)));
        dsv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }

    [[nodiscard]] Backend backend() const noexcept override { return Backend::d3d12; }
    [[nodiscard]] std::string_view adapter_name() const noexcept override { return adapter_name_; }
    [[nodiscard]] Queue& graphics_queue() noexcept override { return graphics_queue_; }

    [[nodiscard]] IDXGIFactory4* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] ID3D12Device* native_device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* native_queue() const noexcept {
        return graphics_queue_.native();
    }
    [[nodiscard]] ID3D12DescriptorHeap* srv_heap() const noexcept { return srv_heap_.Get(); }

    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const Window& window) override {
        return std::make_unique<D3D12Swapchain>(*this, window);
    }

    [[nodiscard]] std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) override;
    [[nodiscard]] std::unique_ptr<Texture> create_texture(const TextureDesc& desc) override;
    [[nodiscard]] std::unique_ptr<Pipeline> create_graphics_pipeline(
        const GraphicsPipelineDesc& desc) override;

    [[nodiscard]] std::unique_ptr<CommandList> create_command_list() override {
        return std::make_unique<D3D12CommandList>(this);
    }

private:
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    D3D12Queue graphics_queue_;
    std::string adapter_name_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    UINT srv_stride_{};
    UINT dsv_stride_{};
    UINT srv_index_{};
    UINT dsv_index_{};
};

D3D12CommandList::D3D12CommandList(D3D12Device* device) : device_(device) {
    throw_if_failed(device_->native_device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)));
    throw_if_failed(device_->native_device()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&commands_)));
    throw_if_failed(commands_->Close());
}

void D3D12CommandList::begin_render(Swapchain& swapchain) {
    AV_ASSERT(open_);
    AV_ASSERT(!rendering_);
    swapchain_ = static_cast<D3D12Swapchain*>(&swapchain);
    if (swapchain_->width() == 0 || swapchain_->height() == 0) {
        swapchain_ = nullptr;
        return;
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = {device_->srv_heap()};
    commands_->SetDescriptorHeaps(1, heaps);

    const D3D12_RESOURCE_BARRIER to_rtv = barrier(swapchain_->current_target(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands_->ResourceBarrier(1, &to_rtv);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_->current_rtv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapchain_->dsv();
    commands_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    
    // Clear depth buffer
    commands_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(swapchain_->width()),
        static_cast<float>(swapchain_->height()), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(swapchain_->width()),
        static_cast<LONG>(swapchain_->height())};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    rendering_ = true;
}

void D3D12CommandList::begin_depth(Texture& depth) {
    AV_ASSERT(open_);
    AV_ASSERT(!rendering_);
    auto& texture = static_cast<D3D12Texture&>(depth);
    AV_ENSURE(texture.is_depth);
    AV_ENSURE(texture.resource != nullptr);

    ID3D12DescriptorHeap* heaps[] = {device_->srv_heap()};
    commands_->SetDescriptorHeaps(1, heaps);

    if (texture.state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        const D3D12_RESOURCE_BARRIER to_depth =
            barrier(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commands_->ResourceBarrier(1, &to_depth);
        texture.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    commands_->OMSetRenderTargets(0, nullptr, FALSE, &texture.dsv);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(texture.width),
        static_cast<float>(texture.height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(texture.width), static_cast<LONG>(texture.height)};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    depth_target_ = &texture;
    rendering_ = true;
}

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

    // Create depth-stencil heap
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throw_if_failed(device_->native_device()->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap_)));

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
    
    // Create depth buffer
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width_;
    depth_desc.Height = height_;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES depth_heap{};
    depth_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    throw_if_failed(device_->native_device()->CreateCommittedResource(
        &depth_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, IID_PPV_ARGS(&depth_buffer_)));

    device_->native_device()->CreateDepthStencilView(
        depth_buffer_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
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
    depth_buffer_.Reset();
    
    throw_if_failed(
        swapchain_->ResizeBuffers(frame_count, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    recreate_buffers();
    
    // Recreate depth buffer
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width_;
    depth_desc.Height = height_;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES depth_heap{};
    depth_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    throw_if_failed(device_->native_device()->CreateCommittedResource(
        &depth_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, IID_PPV_ARGS(&depth_buffer_)));

    device_->native_device()->CreateDepthStencilView(
        depth_buffer_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
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

std::unique_ptr<Texture> D3D12Device::create_texture(const TextureDesc& desc) {
    AV_ENSURE(desc.is_valid());

    auto texture = std::make_unique<D3D12Texture>();
    texture->width = desc.width;
    texture->height = desc.height;

    if (desc.format == Format::d32_float) {
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource.Width = desc.width;
        resource.Height = desc.height;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = 1;
        resource.Format = DXGI_FORMAT_R32_TYPELESS;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        throw_if_failed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&texture->resource)));
        texture->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        texture->is_depth = true;

        const UINT dsv_idx = dsv_index_++;
        AV_ENSURE(dsv_idx < 16);
        texture->dsv.ptr = dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                           static_cast<SIZE_T>(dsv_idx) * dsv_stride_;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(texture->resource.Get(), &dsv_desc, texture->dsv);

        const UINT srv_idx = srv_index_++;
        texture->srv.ptr = srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                           static_cast<SIZE_T>(srv_idx) * srv_stride_;
        texture->srv_gpu.ptr = srv_heap_->GetGPUDescriptorHandleForHeapStart().ptr +
                               static_cast<SIZE_T>(srv_idx) * srv_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(texture->resource.Get(), &srv_desc, texture->srv);
        return texture;
    }

    // Create texture resource
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = to_dxgi(desc.format);
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    throw_if_failed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture->resource)));

    // Create upload buffer if data provided
    if (desc.data != nullptr) {
        const UINT64 upload_size = GetRequiredIntermediateSize(texture->resource.Get(), 0, 1);

        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_resource{};
        upload_resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_resource.Width = upload_size;
        upload_resource.Height = 1;
        upload_resource.DepthOrArraySize = 1;
        upload_resource.MipLevels = 1;
        upload_resource.SampleDesc.Count = 1;
        upload_resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        throw_if_failed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
            &upload_resource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&texture->upload)));

        // Upload texture data
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        throw_if_failed(
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        throw_if_failed(device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));

        D3D12_SUBRESOURCE_DATA subresource{};
        subresource.pData = desc.data;
        subresource.RowPitch = desc.width * 4;  // RGBA8
        subresource.SlicePitch = subresource.RowPitch * desc.height;

        UpdateSubresources(
            commands.Get(), texture->resource.Get(), texture->upload.Get(), 0, 0, 1, &subresource);

        const D3D12_RESOURCE_BARRIER barrier_to_read = barrier(
            texture->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &barrier_to_read);
        throw_if_failed(commands->Close());

        ID3D12CommandList* lists[] = {commands.Get()};
        graphics_queue_.native()->ExecuteCommandLists(1, lists);
        graphics_queue_.wait_idle();
        texture->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // Create SRV
    const UINT srv_idx = srv_index_++;
    texture->srv.ptr = srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
                       static_cast<SIZE_T>(srv_idx) * srv_stride_;
    texture->srv_gpu.ptr = srv_heap_->GetGPUDescriptorHandleForHeapStart().ptr +
                           static_cast<SIZE_T>(srv_idx) * srv_stride_;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = to_dxgi(desc.format);
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;

    device_->CreateShaderResourceView(texture->resource.Get(), &srv_desc, texture->srv);

    return texture;
}

std::unique_ptr<Pipeline> D3D12Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const char* shader_source = color_shader;
    if (desc.depth_only) {
        shader_source = depth_only_shader;
    } else if (desc.use_pbr) {
        shader_source = pbr_shader;
    } else if (desc.use_texture) {
        shader_source = desc.use_3d ? texture_3d_shader : texture_shader;
    }

    const ComPtr<ID3DBlob> vs = compile_shader(shader_source, "vs_main", "vs_5_0");
    ComPtr<ID3DBlob> ps;
    if (!desc.depth_only) {
        ps = compile_shader(shader_source, "ps_main", "ps_5_0");
    }

    auto pipeline = std::make_unique<D3D12Pipeline>();
    pipeline->color[0] = desc.color.r;
    pipeline->color[1] = desc.color.g;
    pipeline->color[2] = desc.color.b;
    pipeline->color[3] = desc.color.a;
    pipeline->use_texture = desc.use_texture;
    pipeline->use_3d = desc.use_3d || desc.depth_only || desc.use_pbr;
    pipeline->use_pbr = desc.use_pbr;
    pipeline->depth_only = desc.depth_only;

    if (desc.depth_only) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> root_blob;
        throw_if_failed(
            D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr));
        throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));
    } else if (desc.use_pbr) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 3;
        root_desc.pParameters = params;
        root_desc.NumStaticSamplers = 2;
        root_desc.pStaticSamplers = samplers;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> root_blob;
        throw_if_failed(
            D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr));
        throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));
    } else if (desc.use_texture && desc.use_3d) {
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 2;
        root_desc.pParameters = params;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> root_blob;
        throw_if_failed(
            D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr));
        throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));
    } else if (desc.use_texture) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> root_blob;
        throw_if_failed(
            D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr));
        throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));
    } else {
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
        throw_if_failed(device_->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root_signature)));
    }

    D3D12_INPUT_ELEMENT_DESC input[4];
    UINT input_count = 0;

    if (desc.use_3d || desc.depth_only || desc.use_pbr) {
        input[input_count++] = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        if (desc.use_pbr) {
            input[input_count++] = {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            input[input_count++] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        } else if (desc.use_texture) {
            input[input_count++] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        }
    } else {
        input[input_count++] = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        if (desc.use_texture) {
            input[input_count++] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            input[input_count++] = {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = pipeline->root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    if (ps != nullptr) {
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    }
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        desc.depth_only ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    if (!desc.use_3d && !desc.depth_only && !desc.use_pbr) {
        pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode =
        desc.two_sided                          ? D3D12_CULL_MODE_NONE
        : (desc.use_3d || desc.depth_only || desc.use_pbr) ? D3D12_CULL_MODE_BACK
                                                : D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = TRUE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    if (desc.depth_only) {
        pso.RasterizerState.DepthBias = 2000;
        pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    }
    pso.InputLayout = {input, input_count};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (desc.depth_only) {
        pso.NumRenderTargets = 0;
        pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    } else {
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = to_dxgi(desc.color_format);
    }
    pso.SampleDesc.Count = 1;

    if (desc.use_depth || desc.depth_only) {
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    }

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
