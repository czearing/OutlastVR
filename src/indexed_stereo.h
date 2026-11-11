#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <stdexcept>
#include <vector>

namespace olvr {

constexpr std::uint32_t indexed_data = 0x40000000u;
constexpr std::uint32_t instance_data = 0x80000000u;
constexpr std::uint32_t frequency_mask = 0x3fffffffu;

enum class DrawKind { indexed, non_indexed, primitive_up, indexed_primitive_up };
enum class StreamRate { vertex, object_instance };

struct Stream {
    std::uint32_t slot;
    StreamRate rate;
    std::uint32_t divisor = 1;
};

struct Draw {
    std::uint32_t primitive_type;
    std::int32_t base_vertex;
    std::uint32_t min_vertex;
    std::uint32_t vertex_count;
    std::uint32_t start_index;
    std::uint32_t primitive_count;
};

struct Frequency {
    std::uint32_t slot;
    std::uint32_t value;
};

// Submission scheduling only: shaders, eye data, atlas clipping, scene-view
// selection and culling must be established by the integrating renderer.
struct IndexedStereoPlan {
    std::uint32_t eye_records;
    std::vector<Frequency> frequencies;
};

inline void plan_indexed_stereo(
    DrawKind kind, bool hardware_instancing, std::uint32_t software_repeat_count,
    std::uint32_t object_instances, std::uint32_t max_streams,
    std::uint32_t eye_stream, const std::vector<Stream>& streams,IndexedStereoPlan& plan) {
    plan.eye_records=0;
    plan.frequencies.clear();
    if (kind != DrawKind::indexed)
        throw std::invalid_argument("Non-indexed and UP draws require indexed-buffer conversion");
    if (!hardware_instancing)
        throw std::invalid_argument("Hardware instancing is required; draw replay is forbidden");
    if (software_repeat_count != 1)
        throw std::invalid_argument("Engine software draw replay must not be active");
    if (object_instances == 0 || object_instances > frequency_mask / 2)
        throw std::invalid_argument("Stereo instance count is outside the D3D9 frequency range");
    if (max_streams == 0 || max_streams > 16 || eye_stream == 0 || eye_stream >= max_streams)
        throw std::invalid_argument("Eye stream must be a nonzero supported spare stream");

    const auto eye_records=object_instances*2;
    plan.frequencies.reserve(max_streams);
    std::array<bool,16> occupied{};
    for (const auto& stream : streams) {
        if (stream.slot >= max_streams || stream.slot == eye_stream || occupied[stream.slot])
            throw std::invalid_argument("Vertex declaration has conflicting or invalid streams");
        occupied[stream.slot] = true;
        if (stream.divisor == 0 || stream.divisor > frequency_mask / 2)
            throw std::invalid_argument("Stream divisor cannot be represented for stereo");
        if (stream.rate != StreamRate::vertex && stream.rate != StreamRate::object_instance)
            throw std::invalid_argument("Unknown stream rate");
        if (stream.rate == StreamRate::object_instance) {
            if (stream.slot == 0)
                throw std::invalid_argument("Stream zero must carry indexed vertex geometry");
            // L,R are consecutive hardware instances of the same object.
            plan.frequencies.push_back({stream.slot, instance_data | (stream.divisor * 2)});
        } else {
            if (stream.divisor != 1)
                throw std::invalid_argument("Per-vertex streams must have divisor one");
            plan.frequencies.push_back(
                {stream.slot, stream.slot == 0 ? indexed_data | eye_records : 1});
        }
    }
    if (!occupied[0])
        throw std::invalid_argument("Indexed geometry requires vertex stream zero");
    plan.frequencies.push_back({eye_stream, instance_data | 1});
    plan.eye_records=eye_records;
}

inline IndexedStereoPlan plan_indexed_stereo(
    DrawKind kind, bool hardware_instancing, std::uint32_t software_repeat_count,
    std::uint32_t object_instances, std::uint32_t max_streams,
    std::uint32_t eye_stream, const std::vector<Stream>& streams) {
    IndexedStereoPlan plan{};
    plan_indexed_stereo(kind,hardware_instancing,software_repeat_count,object_instances,
        max_streams,eye_stream,streams,plan);
    return plan;
}

// Native API calls return HRESULT-shaped signed status values. Tests use a
// recording backend; the adapter forwards to the real D3D9 device without hooks.
class IndexedDevice {
public:
    virtual ~IndexedDevice() = default;
    virtual std::int32_t get_frequency(std::uint32_t slot, std::uint32_t& value) noexcept = 0;
    virtual std::int32_t set_frequency(std::uint32_t slot, std::uint32_t value) noexcept = 0;
    virtual std::int32_t draw_indexed(const Draw& draw) noexcept = 0;
};

struct SubmissionResult {
    std::int32_t operation_status = 0;
    std::int32_t restoration_status = 0;
    std::uint32_t native_draw_attempts = 0;
    bool succeeded() const {
        return operation_status >= 0 && restoration_status >= 0 && native_draw_attempts == 1;
    }
};

inline SubmissionResult submit_indexed_stereo(
    IndexedDevice& device, const IndexedStereoPlan& plan, const Draw& draw) {
    if (draw.primitive_count == 0 || draw.vertex_count == 0)
        throw std::invalid_argument("Stereo submission requires nonempty indexed geometry");
    if (draw.primitive_type != 2 && draw.primitive_type != 4 && draw.primitive_type != 5)
        throw std::invalid_argument("Unsupported Outlast primitive topology");
    if (plan.eye_records < 2 || plan.eye_records > frequency_mask ||
        (plan.eye_records & 1) != 0 || plan.frequencies.empty() || plan.frequencies.size()>16)
        throw std::invalid_argument("Invalid stereo submission plan");
    bool has_indexed_geometry = false;
    std::uint32_t eye_streams = 0;
    std::array<bool,16> occupied{};
    for (const auto& frequency : plan.frequencies) {
        if (frequency.slot >= 16 || occupied[frequency.slot])
            throw std::invalid_argument("Invalid or duplicate frequency slot");
        occupied[frequency.slot] = true;
        if (frequency.slot == 0) {
            if (frequency.value != (indexed_data | plan.eye_records))
                throw std::invalid_argument("Stream zero must select the stereo instance count");
            has_indexed_geometry = true;
        } else if (frequency.value == (instance_data | 1)) {
            ++eye_streams;
        } else if (frequency.value != 1 &&
                   ((frequency.value & ~frequency_mask) != instance_data ||
                    (frequency.value & frequency_mask) < 2 ||
                    (frequency.value & 1) != 0)) {
            throw std::invalid_argument("Object instance divisors must advance after both eyes");
        }
    }
    if (!has_indexed_geometry || eye_streams != 1)
        throw std::invalid_argument("Stereo geometry and an eye stream are required");

    SubmissionResult result;
    std::array<Frequency,16> saved{};
    std::size_t saved_count=0;
    for (const auto& frequency : plan.frequencies) {
        std::uint32_t previous = 0;
        result.operation_status = device.get_frequency(frequency.slot, previous);
        if (result.operation_status < 0)
            return result;
        saved[saved_count++]={frequency.slot, previous};
    }

    std::size_t changed = 0;
    for (const auto& frequency : plan.frequencies) {
        // Also restore the attempted slot: a failed driver call is not used
        // as evidence that no state changed.
        ++changed;
        result.operation_status = device.set_frequency(frequency.slot, frequency.value);
        if (result.operation_status < 0)
            break;
    }
    if (result.operation_status >= 0) {
        result.native_draw_attempts = 1;
        result.operation_status = device.draw_indexed(draw);
    }
    while (changed != 0) {
        const auto& previous = saved[--changed];
        const auto status = device.set_frequency(previous.slot, previous.value);
        if (status < 0 && result.restoration_status >= 0)
            result.restoration_status = status;
    }
    return result;
}

} // namespace olvr
