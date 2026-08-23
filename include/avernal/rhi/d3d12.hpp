#pragma once

#include <avernal/rhi/rhi.hpp>

#include <memory>

namespace avernal {

[[nodiscard]] std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc = {});

}  // namespace avernal
