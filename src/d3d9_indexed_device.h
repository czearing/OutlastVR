#pragma once

#include "indexed_stereo.h"
#include <d3d9.h>

namespace olvr {

// Borrowed device reference. This does not load, inject into, or modify Outlast.
class D3D9IndexedDevice final : public IndexedDevice {
public:
    explicit D3D9IndexedDevice(IDirect3DDevice9& device) : device_(device) {}
    std::int32_t get_frequency(std::uint32_t slot, std::uint32_t& value) noexcept override;
    std::int32_t set_frequency(std::uint32_t slot, std::uint32_t value) noexcept override;
    std::int32_t draw_indexed(const Draw& draw) noexcept override;

private:
    IDirect3DDevice9& device_;
};

} // namespace olvr
