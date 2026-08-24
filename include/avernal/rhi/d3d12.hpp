#pragma once

#include <avernal/rhi/rhi.hpp>

#include <memory>

namespace avernal {

[[nodiscard]] std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc = {});

[[nodiscard]] void* d3d12_native_factory(const Device& device);
[[nodiscard]] void* d3d12_native_device(const Device& device);
[[nodiscard]] void* d3d12_native_queue(const Device& device);

}  // namespace avernal
