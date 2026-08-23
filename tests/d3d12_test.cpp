#include <avernal/rhi/d3d12.hpp>

#include <gtest/gtest.h>

TEST(D3D12, CreatesDevice) {
    const auto device = avernal::create_d3d12_device({
        .backend = avernal::Backend::d3d12,
        .debug = false,
    });
    if (device == nullptr) {
        GTEST_SKIP() << "no D3D12 adapter available";
    }

    EXPECT_EQ(device->backend(), avernal::Backend::d3d12);
    EXPECT_FALSE(device->adapter_name().empty());
}

TEST(D3D12, DeviceMatchesInterface) {
    const auto device = avernal::create_d3d12_device({.debug = false});
    if (device == nullptr) {
        GTEST_SKIP() << "no D3D12 adapter available";
    }

    const avernal::Device& rhi = *device;
    EXPECT_EQ(avernal::backend_name(rhi.backend()), "d3d12");
}
