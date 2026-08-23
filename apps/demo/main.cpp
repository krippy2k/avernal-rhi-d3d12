#include <avernal/core/assert.hpp>
#include <avernal/rhi/d3d12.hpp>

#include <print>

int main() {
    const auto device = avernal::create_d3d12_device({
        .backend = avernal::Backend::d3d12,
        .debug = true,
    });
    AV_ENSURE(device != nullptr);

    std::println("backend = {}", avernal::backend_name(device->backend()));
    std::println("adapter = {}", device->adapter_name());
    return 0;
}
