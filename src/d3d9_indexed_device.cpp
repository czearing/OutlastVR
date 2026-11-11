#include "d3d9_indexed_device.h"

namespace olvr {

std::int32_t D3D9IndexedDevice::get_frequency(std::uint32_t slot, std::uint32_t& value) noexcept {
    UINT frequency = 0;
    const HRESULT status = device_.GetStreamSourceFreq(slot, &frequency);
    if (SUCCEEDED(status))
        value = frequency;
    return static_cast<std::int32_t>(status);
}

std::int32_t D3D9IndexedDevice::set_frequency(std::uint32_t slot, std::uint32_t value) noexcept {
    return static_cast<std::int32_t>(device_.SetStreamSourceFreq(slot, value));
}

std::int32_t D3D9IndexedDevice::draw_indexed(const Draw& draw) noexcept {
    return static_cast<std::int32_t>(device_.DrawIndexedPrimitive(
        static_cast<D3DPRIMITIVETYPE>(draw.primitive_type), draw.base_vertex,
        draw.min_vertex, draw.vertex_count, draw.start_index, draw.primitive_count));
}

} // namespace olvr
