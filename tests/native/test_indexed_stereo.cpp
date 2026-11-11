#include "indexed_stereo.h"
#include <array>
#include <iostream>
#include <string>

using namespace olvr;

static std::uint32_t checks = 0;

static void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Function>
static void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Unsupported input was accepted");
}

class Recorder final : public IndexedDevice {
public:
    std::array<std::uint32_t, 16> frequencies{};
    std::vector<std::array<std::uint32_t, 16>> draw_states;
    std::vector<std::uint32_t> restoration_slots;
    int get_calls = 0;
    int set_calls = 0;
    int fail_get = -1;
    int fail_set = -1;
    int fail_restore = -1;
    bool fail_draw = false;
    bool restoring = false;
    Draw submitted_draw{};

    Recorder() {
        frequencies.fill(1);
        draw_states.reserve(1);
        restoration_slots.reserve(16);
    }
    std::int32_t get_frequency(std::uint32_t slot, std::uint32_t& value) noexcept override {
        if (++get_calls == fail_get)
            return -100;
        value = frequencies[slot];
        return 0;
    }
    std::int32_t set_frequency(std::uint32_t slot, std::uint32_t value) noexcept override {
        frequencies[slot] = value;
        if (++set_calls == fail_set) {
            restoring = true;
            return -200;
        }
        if (restoring) {
            restoration_slots.push_back(slot);
            if (static_cast<int>(restoration_slots.size()) == fail_restore)
                return -400;
        }
        return 0;
    }
    std::int32_t draw_indexed(const Draw& draw) noexcept override {
        submitted_draw = draw;
        draw_states.push_back(frequencies);
        restoring = true;
        return fail_draw ? -300 : 0;
    }
};

static void run() {
    const Draw draw{4, -2, 0, 3, 9, 1};
    const std::vector<Stream> streams{{0, StreamRate::vertex}, {1, StreamRate::vertex},
                                      {2, StreamRate::object_instance, 1},
                                      {3, StreamRate::object_instance, 3}};
    IndexedStereoPlan cached{};
    plan_indexed_stereo(DrawKind::indexed,true,1,1,16,15,streams,cached);
    const auto cached_capacity=cached.frequencies.capacity();
    for (std::uint32_t objects = 1; objects <= 127; ++objects) {
        const auto plan = plan_indexed_stereo(DrawKind::indexed, true, 1, objects, 16, 15, streams);
        plan_indexed_stereo(DrawKind::indexed,true,1,objects,16,15,streams,cached);
        check(cached.eye_records==plan.eye_records && cached.frequencies.size()==plan.frequencies.size(),
            "Reused stereo plan changed its instance count or retained stale streams");
        check(cached_capacity>=16 && cached.frequencies.capacity()==cached_capacity,
            "Reusable stereo plan did not preserve its warmed-up stream capacity");
        for (std::size_t index=0;index<plan.frequencies.size();++index)
            check(cached.frequencies[index].slot==plan.frequencies[index].slot &&
                cached.frequencies[index].value==plan.frequencies[index].value,
                "Reusable stereo plan differs from a fresh plan");
        Recorder device;
        device.frequencies[2] = instance_data | 1;
        device.frequencies[3] = instance_data | 3;
        device.frequencies[15] = instance_data | 7;
        const auto before = device.frequencies;
        const auto result = submit_indexed_stereo(device, plan, draw);
        check(result.succeeded(), "Valid indexed stereo submission failed");
        check(device.draw_states.size() == 1 && result.native_draw_attempts == 1,
              "Stereo dispatched more or less than one native draw");
        check(device.frequencies == before, "Stream state leaked to the next engine draw");
        check(device.submitted_draw.primitive_type == draw.primitive_type &&
              device.submitted_draw.base_vertex == draw.base_vertex &&
              device.submitted_draw.min_vertex == draw.min_vertex &&
              device.submitted_draw.vertex_count == draw.vertex_count &&
              device.submitted_draw.start_index == draw.start_index &&
              device.submitted_draw.primitive_count == draw.primitive_count,
              "Native geometry arguments were changed");
        const auto& state = device.draw_states.front();
        check(state[0] == (indexed_data | (objects * 2)), "Wrong hardware instance count");
        check(state[1] == 1, "Per-vertex stream became per-instance");
        check(state[2] == (instance_data | 2), "Existing object instancing was not preserved");
        check(state[3] == (instance_data | 6), "Existing non-unit divisor was not preserved");
        check(state[15] == (instance_data | 1), "Eye stream did not advance once per eye");
        for (std::uint32_t instance = 0; instance < objects * 2; ++instance) {
            check(instance / (state[2] & frequency_mask) == (instance / 2),
                  "Left and right eyes selected different object records");
            check(instance / (state[3] & frequency_mask) == (instance / 2) / 3,
                  "Divisor mapping changed object grouping");
        }
    }

    std::vector<Stream> dense;
    for (unsigned slot=0;slot<15;++slot) dense.push_back({slot,StreamRate::vertex});
    plan_indexed_stereo(DrawKind::indexed,true,1,3,16,15,dense,cached);
    Recorder full_device;
    check(cached.frequencies.size()==16 && cached.frequencies.capacity()==cached_capacity &&
        submit_indexed_stereo(full_device,cached,draw).succeeded() &&
        full_device.frequencies==Recorder{}.frequencies,"Full sixteen-slot stack restoration failed");
    rejects([&] {plan_indexed_stereo(DrawKind::indexed,true,1,1,16,15,
        {{0,StreamRate::vertex},{0,StreamRate::vertex}},cached);});
    check(cached.eye_records==0,"Rejected reusable plan retained a submittable instance count");
    Recorder rejected_cached;
    rejects([&] {submit_indexed_stereo(rejected_cached,cached,draw);});
    check(rejected_cached.get_calls==0 && rejected_cached.draw_states.empty(),
        "Rejected reusable plan reached the driver");
    plan_indexed_stereo(DrawKind::indexed,true,1,2,16,15,{{0,StreamRate::vertex}},cached);
    check(cached.frequencies.size()==2 && cached.eye_records==4,
        "Recovered reusable plan retained a previous dense/error schedule");
    const auto plan = plan_indexed_stereo(DrawKind::indexed, true, 1, 7, 16, 15, streams);
    for (int fail = 1; fail <= static_cast<int>(plan.frequencies.size()); ++fail) {
        Recorder get_failure;
        get_failure.fail_get = fail;
        const auto get_result = submit_indexed_stereo(get_failure, plan, draw);
        check(get_result.operation_status == -100 && get_failure.set_calls == 0 &&
              get_failure.draw_states.empty(), "Read failure changed state or drew geometry");

        Recorder set_failure;
        set_failure.fail_set = fail;
        const auto before = set_failure.frequencies;
        const auto set_result = submit_indexed_stereo(set_failure, plan, draw);
        check(set_result.operation_status == -200 && set_failure.draw_states.empty(),
              "Setup failure drew geometry or hid its error");
        check(set_failure.frequencies == before, "Setup failure did not restore state");
    }
    Recorder draw_failure;
    draw_failure.fail_draw = true;
    const auto before = draw_failure.frequencies;
    const auto failed_draw = submit_indexed_stereo(draw_failure, plan, draw);
    check(!failed_draw.succeeded() && failed_draw.operation_status == -300 &&
          failed_draw.native_draw_attempts == 1, "Failed draw was retried or hidden");
    check(draw_failure.frequencies == before, "Draw failure did not restore state");
    for (int fail = 1; fail <= static_cast<int>(plan.frequencies.size()); ++fail) {
        Recorder restore_failure;
        restore_failure.fail_restore = fail;
        const auto result = submit_indexed_stereo(restore_failure, plan, draw);
        check(!result.succeeded() && result.restoration_status == -400 &&
              restore_failure.restoration_slots.size() == plan.frequencies.size(),
              "Restore error was hidden or prevented later restoration attempts");
    }
    Recorder combined_failure;
    combined_failure.fail_draw = true;
    combined_failure.fail_restore = 1;
    const auto combined = submit_indexed_stereo(combined_failure, plan, draw);
    check(combined.operation_status == -300 && combined.restoration_status == -400 &&
          combined.native_draw_attempts == 1, "Draw and restoration errors were not both retained");
    Recorder setup_and_restore_failure;
    setup_and_restore_failure.fail_set = 2;
    setup_and_restore_failure.fail_restore = 1;
    const auto setup_and_restore = submit_indexed_stereo(setup_and_restore_failure, plan, draw);
    check(setup_and_restore.operation_status == -200 && setup_and_restore.restoration_status == -400 &&
          setup_and_restore.native_draw_attempts == 0, "Setup and restoration errors were not both retained");
    const auto maximum = plan_indexed_stereo(
        DrawKind::indexed, true, 1, frequency_mask / 2, 16, 15, streams);
    check(maximum.eye_records == frequency_mask - 1, "Maximum representable instance count was rejected");
    for (auto kind : {DrawKind::non_indexed, DrawKind::primitive_up, DrawKind::indexed_primitive_up})
        rejects([&] { plan_indexed_stereo(kind, true, 1, 1, 16, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, false, 1, 1, 16, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 2, 1, 16, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 0, 16, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, frequency_mask / 2 + 1, 16, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 0, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 2, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 17, 15, streams); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15, {{1, StreamRate::vertex}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, StreamRate::vertex}, {0, StreamRate::vertex}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, StreamRate::object_instance}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, StreamRate::vertex, 2}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, StreamRate::vertex}, {1, StreamRate::object_instance, 0}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, StreamRate::vertex}, {1, StreamRate::object_instance, frequency_mask}}); });
    rejects([&] { plan_indexed_stereo(DrawKind::indexed, true, 1, 1, 16, 15,
                                   {{0, static_cast<StreamRate>(-1)}}); });
    Recorder invalid;
    rejects([&] { submit_indexed_stereo(invalid, plan, Draw{4, 0, 0, 3, 0, 0}); });
    rejects([&] { submit_indexed_stereo(invalid, plan, Draw{4, 0, 0, 0, 0, 1}); });
    rejects([&] { submit_indexed_stereo(invalid, IndexedStereoPlan{2, {}}, draw); });
    rejects([&] { submit_indexed_stereo(invalid, IndexedStereoPlan{2, {{0, indexed_data | 2}}}, draw); });
    rejects([&] { submit_indexed_stereo(invalid,
                 IndexedStereoPlan{2, {{0, indexed_data | 2}, {1, instance_data | 3}}}, draw); });
    rejects([&] { submit_indexed_stereo(invalid, plan, Draw{1, 0, 0, 3, 0, 1}); });
    rejects([&] { submit_indexed_stereo(invalid,
                 IndexedStereoPlan{0x80000000u, {{0, 0xc0000000u}, {1, instance_data | 1}}}, draw); });
    rejects([&] { submit_indexed_stereo(invalid,
                 IndexedStereoPlan{2, {{0, indexed_data | 2}, {1, instance_data | 1},
                                       {2, instance_data | 1}}}, draw); });
    check(invalid.get_calls == 0 && invalid.set_calls == 0 && invalid.draw_states.empty(),
          "Invalid submission touched the device");
}

int main() {
    try {
        run();
        std::cout << "PASS " << checks
                  << " offline scheduling checks; no GPU, game, injection or OpenXR execution\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
