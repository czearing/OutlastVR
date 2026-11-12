function(olvr_test name)
    add_executable(${name} "tests/native/${name}.cpp")
    target_include_directories(${name} PRIVATE tests/native)
    target_link_libraries(${name} PRIVATE olvr_stereo minhook)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "cpu" TIMEOUT 120)
endfunction()

foreach(name IN ITEMS
    test_indexed_stereo test_renderer_ownership test_stereo_allocations
    test_stereo_frustum test_stereo_reconstruction test_xr_view_constants
    test_outlast_config test_outlast_night_vision test_outlast_camcorder
    test_native_camcorder_lifecycle test_native_camera_resources
    test_outlast_desktop_cursor test_outlast_focus_pause test_outlast_render_size
    test_outlast_motion_controls test_outlast_native_use test_outlast_hand_collision
    test_outlast_body_lifecycle
    test_outlast_comfort
    test_native_probe_progress test_native_probe_input test_outlast_locomotion_camera
    test_native_glass_eye_ownership test_native_ppm test_native_detours test_openxr_motion_input
    test_headlocked_ui test_world_scissor test_outlast_native_view)
    olvr_test(${name})
endforeach()
add_test(NAME test_native_hidden_bones COMMAND test_outlast_body_lifecycle --hidden-bones)
set_tests_properties(test_native_hidden_bones PROPERTIES LABELS "cpu" TIMEOUT 120)
add_test(NAME test_passive_hud_safe_area COMMAND test_headlocked_ui --hud-safe-area)
set_tests_properties(test_passive_hud_safe_area PROPERTIES LABELS "cpu" TIMEOUT 120)
foreach(mode IN ITEMS pause-chord fallback-gestures button-recovery snap-recovery analog-recovery)
    string(REPLACE "-" "_" test_mode "${mode}")
    add_test(NAME test_input_${test_mode} COMMAND test_openxr_motion_input --${mode})
    set_tests_properties(test_input_${test_mode} PROPERTIES LABELS "cpu" TIMEOUT 120)
endforeach()
add_executable(test_native_pixel_delta tests/native/test_native_pixel_delta.cpp)
target_link_libraries(test_native_pixel_delta PRIVATE olvr_options)
target_link_options(test_native_pixel_delta PRIVATE -municode)
add_test(NAME launcher_help COMMAND OutlastVRLauncher --help)
add_test(NAME launcher_version COMMAND OutlastVRLauncher --version)
add_test(NAME launcher_unknown_option COMMAND OutlastVRLauncher --unknown)
set_tests_properties(launcher_unknown_option PROPERTIES WILL_FAIL TRUE)

if(OLVR_HERO_FIXTURE)
    foreach(name IN ITEMS test_controller_interaction test_outlast_native_camcorder
            test_outlast_body_ik test_outlast_hand_alignment)
        olvr_test(${name} "${OLVR_HERO_FIXTURE}")
    endforeach()
endif()
if(OLVR_RETAIL_EXECUTABLE)
    foreach(name IN ITEMS test_native_image test_native_visibility test_native_sphere_inside)
        olvr_test(${name} "${OLVR_RETAIL_EXECUTABLE}")
        target_link_options(${name} PRIVATE -municode)
    endforeach()
endif()

if(OLVR_GPU_TESTS)
    foreach(name IN ITEMS test_fractional_eye_atlas test_tracked_uniforms test_mono_atlas_resolve
            test_atlas_pixel_routing test_camcorder_screen test_point_eye_atlas
            test_world_panel test_world_scissor_gpu test_stereo_transfer test_night_vision_filter)
        olvr_test(${name})
        target_compile_definitions(${name} PRIVATE OLVR_ISOLATED_9ON12)
        set_tests_properties(${name} PROPERTIES LABELS "gpu" RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME test_world_panel_canted COMMAND test_world_panel --canted)
    set_tests_properties(test_world_panel_canted PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 120)
    add_test(NAME test_runtime_gpu_adapter COMMAND test_stereo_transfer --adapter-selection)
    set_tests_properties(test_runtime_gpu_adapter PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 120)
    add_test(NAME test_material_camera_precision COMMAND test_tracked_uniforms --material-large-origin)
    add_test(NAME test_material_world_position COMMAND test_tracked_uniforms --material-world-position)
    add_test(NAME test_material_world_position_native COMMAND test_tracked_uniforms --material-world-position-native)
    add_test(NAME test_material_world_position_precision COMMAND test_tracked_uniforms --material-world-position-large-origin)
    add_test(NAME test_material_alternate_varying COMMAND test_tracked_uniforms --material-alternate-varying)
    add_test(NAME test_material_full_samplers COMMAND test_tracked_uniforms --material-full-samplers)
    add_test(NAME test_material_camera_projection COMMAND test_tracked_uniforms --material-camera-only-projection)
    add_test(NAME test_converted_index_cache COMMAND test_tracked_uniforms --index-cache)
    set_tests_properties(test_material_camera_precision test_material_world_position
        test_material_world_position_native test_material_world_position_precision
        test_material_alternate_varying test_material_full_samplers
        test_material_camera_projection test_converted_index_cache
        PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 120)
    olvr_test(test_openxr_session)
    target_compile_definitions(test_openxr_session PRIVATE OLVR_ISOLATED_9ON12)
    set_tests_properties(test_openxr_session PROPERTIES LABELS "gpu" RUN_SERIAL TRUE)
    olvr_test(test_openxr_lifecycle)
    target_compile_definitions(test_openxr_lifecycle PRIVATE OLVR_ISOLATED_9ON12)
    set_tests_properties(test_openxr_lifecycle PROPERTIES LABELS "gpu" RUN_SERIAL TRUE)
    foreach(mode IN ITEMS tracking-resume asymmetric-extents invalid-pose session-restart)
        string(REPLACE "-" "_" test_mode "${mode}")
        add_test(NAME test_openxr_${test_mode} COMMAND test_openxr_session --${mode})
        set_tests_properties(test_openxr_${test_mode} PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
    foreach(failure IN ITEMS extension adapter view-count limits format swapchain images)
        string(REPLACE "-" "_" test_failure "${failure}")
        add_test(NAME test_openxr_reject_${test_failure} COMMAND test_openxr_session --reject-${failure})
        set_tests_properties(test_openxr_reject_${test_failure} PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
    if(OLVR_RETAIL_EXECUTABLE)
        olvr_test(test_integrated_mod "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}")
        target_compile_definitions(test_integrated_mod PRIVATE OLVR_ISOLATED_9ON12)
        set_tests_properties(test_integrated_mod PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE)
        add_test(NAME test_integrated_comfort COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --comfort)
        set_tests_properties(test_integrated_comfort PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_snap COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --snap)
        set_tests_properties(test_integrated_snap PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_snap_recovery COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --snap-recovery)
        set_tests_properties(test_integrated_snap_recovery PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_ipd_cycle COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --ipd-cycle)
        set_tests_properties(test_integrated_ipd_cycle PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_input_chords COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --input-chords)
        set_tests_properties(test_integrated_input_chords PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_input_recovery COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --input-recovery)
        set_tests_properties(test_integrated_input_recovery PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME test_integrated_reset_retry COMMAND test_integrated_mod
            "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --reset-retry)
        set_tests_properties(test_integrated_reset_retry PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        foreach(mode IN ITEMS transitions upload wrap)
            add_test(NAME test_integrated_flat_${mode} COMMAND test_integrated_mod
                "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --flat-${mode})
            set_tests_properties(test_integrated_flat_${mode} PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        endforeach()
        foreach(mode IN ITEMS thread-handoff thread-overlap ui ui-in-renderer
                ui-scaleform ui-scaleform-handoff camera-tilt camera-bob floor-startup)
            string(REPLACE "-" "_" test_mode "${mode}")
            add_test(NAME test_integrated_${test_mode} COMMAND test_integrated_mod
                "$<TARGET_FILE:OutlastVRSinglePass>" "${OLVR_RETAIL_EXECUTABLE}" --${mode})
            set_tests_properties(test_integrated_${test_mode}
                PROPERTIES LABELS "gpu;retail" RUN_SERIAL TRUE TIMEOUT 120)
        endforeach()
    endif()
endif()

if(OLVR_BUILD_DIAGNOSTICS)
    foreach(name IN ITEMS test_native_draw_probe test_native_replay test_native_shader_rejection
                          test_native_soft_particles)
        add_executable(${name} tests/native/${name}.cpp)
        target_link_libraries(${name} PRIVATE olvr_stereo olvr_options minhook)
        target_compile_definitions(${name} PRIVATE OLVR_ISOLATED_9ON12)
    endforeach()
    target_link_options(test_native_replay PRIVATE -municode)
    target_link_options(test_native_shader_rejection PRIVATE -municode)
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME python_regressions COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${PROJECT_SOURCE_DIR}/tests/python" -p "test_*.py")
set_tests_properties(python_regressions PROPERTIES LABELS "python" TIMEOUT 180
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools/diagnostics;OLVR_PIXEL_DELTA_WRITER=$<TARGET_FILE:test_native_pixel_delta>;OLVR_RETAIL_EXECUTABLE=${OLVR_RETAIL_EXECUTABLE};OLVR_LAUNCHER=$<TARGET_FILE:OutlastVRLauncher>;OLVR_LOADER=$<TARGET_FILE:openxr_loader>")
