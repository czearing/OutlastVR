cmake_minimum_required(VERSION 3.24)
project(OutlastVR VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Outlast VR requires 64-bit Windows.")
endif()
if(NOT MINGW)
    message(FATAL_ERROR "Use the documented MinGW-w64 GCC toolchain (Ninja generator).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
option(OLVR_BUILD_DIAGNOSTICS "Build the developer-only headset-free probe" OFF)
option(OLVR_GPU_TESTS "Run tests requiring a real D3D12 hardware GPU" OFF)
set(OLVR_RETAIL_EXECUTABLE "" CACHE FILEPATH "Optional locally owned supported OLGame.exe for offline tests")
set(OLVR_ALTERNATE_EXECUTABLE "" CACHE FILEPATH "Optional locally owned alternate Outlast image for offline discovery tests")
set(OLVR_HERO_FIXTURE "" CACHE FILEPATH "Optional locally retained native rig fixture; never distributed")

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG 75c53b6e853dc12c7b3c771edc9c9c841b15faaa)
FetchContent_MakeAvailable(minhook openxr)
target_link_options(openxr_loader PRIVATE -static)
set_target_properties(openxr_loader PROPERTIES PREFIX "")

file(GLOB sdk_headers "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/*/um")
list(SORT sdk_headers COMPARE NATURAL ORDER DESCENDING)
find_path(OLVR_WINDOWS_SDK_UM d3d9on12.h PATHS ${sdk_headers})
if(NOT OLVR_WINDOWS_SDK_UM)
    message(FATAL_ERROR "Install the Windows 10/11 SDK, or set OLVR_WINDOWS_SDK_UM to its Include/<version>/um directory.")
endif()

add_library(olvr_options INTERFACE)
target_include_directories(olvr_options INTERFACE src "${openxr_SOURCE_DIR}/include")
target_compile_options(olvr_options INTERFACE -Wall -Wextra -Werror -O2
    -idirafter "${OLVR_WINDOWS_SDK_UM}")
target_link_options(olvr_options INTERFACE -static -static-libgcc -static-libstdc++)
target_link_libraries(olvr_options INTERFACE d3d9 d3d12 dxgi d3dcompiler_47 user32 gdi32 bcrypt)
target_compile_definitions(olvr_options INTERFACE OLVR_VERSION="${PROJECT_VERSION}")

add_library(olvr_stereo STATIC src/d3d9_indexed_device.cpp src/d3d9_stereo_pipeline.cpp)
target_link_libraries(olvr_stereo PUBLIC olvr_options)
add_library(OutlastVRSinglePass SHARED src/outlast_runtime.cpp)
set_target_properties(OutlastVRSinglePass PROPERTIES PREFIX "")
target_link_libraries(OutlastVRSinglePass PRIVATE olvr_stereo minhook)
add_executable(OutlastVRLauncher src/outlast_launcher.cpp)
target_link_libraries(OutlastVRLauncher PRIVATE olvr_options psapi)
target_link_options(OutlastVRLauncher PRIVATE -municode)
add_library(OutlastVRAutoload SHARED src/outlast_autoload.cpp)
set_target_properties(OutlastVRAutoload PROPERTIES PREFIX "" OUTPUT_NAME "d3d9")
target_link_libraries(OutlastVRAutoload PRIVATE olvr_options)

if(OLVR_BUILD_DIAGNOSTICS)
    add_library(OutlastVRHeadlessProbe SHARED src/native_probe_runtime.cpp)
    set_target_properties(OutlastVRHeadlessProbe PROPERTIES PREFIX "")
    target_compile_options(OutlastVRHeadlessProbe PRIVATE -Wno-unused-function)
    target_include_directories(OutlastVRHeadlessProbe PRIVATE tests/native)
    target_link_libraries(OutlastVRHeadlessProbe PRIVATE olvr_stereo minhook)
endif()

include(CTest)
if(BUILD_TESTING)
    include(cmake/Tests.cmake)
endif()

install(TARGETS OutlastVRSinglePass OutlastVRLauncher OutlastVRAutoload openxr_loader
    RUNTIME DESTINATION . COMPONENT Runtime
    LIBRARY DESTINATION . COMPONENT Runtime)
install(FILES config/OutlastVR.ini README.md LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION . COMPONENT Runtime)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME MinHook.txt COMPONENT Runtime)
install(FILES "${openxr_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME OpenXR.txt COMPONENT Runtime)
install(FILES "${openxr_SOURCE_DIR}/LICENSES/MIT.txt" DESTINATION licenses RENAME OpenXR-MIT.txt COMPONENT Runtime)
install(FILES "${openxr_SOURCE_DIR}/src/external/jsoncpp/LICENSE"
    DESTINATION licenses RENAME JsonCpp.txt COMPONENT Runtime)
install(FILES licenses/Winpthreads.txt DESTINATION licenses COMPONENT Runtime)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME OutlastVR)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "OutlastVR-${PROJECT_VERSION}-win64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL Runtime)
include(CPack)
