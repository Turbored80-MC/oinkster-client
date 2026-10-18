cmake_minimum_required(VERSION 3.24)
project(OinksterNative VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.9
  GIT_SHALLOW TRUE)
# Same signature-scanning library and pinned commit Flarial dll-oss builds
# against (see Native/out/reference/flarial-dll-oss/CMakeLists.txt) - kept
# in lockstep so the byte patterns ported from their offset tables are
# interpreted by the exact scanner they were written for.
FetchContent_Declare(libhat
  GIT_REPOSITORY https://github.com/BasedInc/libhat.git
  GIT_TAG d6297514b05fd238f5b46d003325f108c22741e3)
FetchContent_MakeAvailable(minhook imgui libhat)

add_library(OinksterImGui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(OinksterImGui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(OinksterImGui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(OinksterImGui PRIVATE /W3 /EHsc /utf-8)
target_link_libraries(OinksterImGui PUBLIC d3d12 dxgi)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P STREQUAL "8")
  message(FATAL_ERROR "Oinkster Native supports Windows x64 only")
endif()

add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Brepro>)
add_link_options($<$<CONFIG:Release>:/Brepro>)

add_library(OinksterCore STATIC
  src/BedrockAdapter.cpp
  src/BedrockIntegration.cpp
  src/Capabilities.cpp
  src/ControlPipe.cpp
  src/ChatTelemetry.cpp
  src/ClickGui.cpp
  src/CombatTelemetry.cpp
  src/Diagnostics.cpp
  src/EnttRegistry.cpp
  src/Dx12Overlay.cpp
  src/FrameBlend.cpp
  src/GameItems.cpp
  src/HookManager.cpp
  src/Json.cpp
  src/Module.cpp
  src/ModuleManager.cpp
  src/ModuleCatalog.cpp
  src/ModuleSettings.cpp
  src/MouseInputBlocker.cpp
  src/OverlayUi.cpp
  src/PipeWait.cpp
  src/PlayerSkinTelemetry.cpp
  src/PlayerStateTelemetry.cpp
  src/RakNet.cpp
  src/ProcessInjector.cpp
  src/Renderer.cpp
  src/ReplayFile.cpp
  src/ReplayCapture.cpp
  src/ReplayPacketBridge.cpp
  src/ReplayBlockPack.cpp
  src/ReplayCameraPath.cpp
  src/VideoRecorder.cpp
  src/ReplayPlayback.cpp
  src/ReplayInjector.cpp
  src/ReplayWorld.cpp
  src/ReplayTimeline.cpp
  src/ResourcePackTelemetry.cpp
  src/CameraTelemetry.cpp
  src/CrosshairTexture.cpp
  src/Runtime.cpp
  src/SessionTelemetry.cpp
  src/ScreenTelemetry.cpp
  src/ServerPolicy.cpp
  src/SettingsStore.cpp
  src/SignatureScan.cpp
  src/SkinHead.cpp
  src/SoundTelemetry.cpp
  src/PresentationAdapter.cpp
  src/PostProcess.cpp
  src/PacketSender.cpp
  src/StatusPipe.cpp
  src/UnloadGuard.cpp
  src/WorldTelemetry.cpp
  src/Modules/AccessibilityModules.cpp
  src/Modules/FrameStatistics.cpp
  src/Modules/HudFormat.cpp
  src/Modules/InputModules.cpp
  src/Modules/PerformanceModules.cpp
  src/Modules/PvpModules.cpp
  src/Modules/StatusModules.cpp
  src/Modules/TelemetryModules.cpp
  src/Modules/UtilityModules.cpp
  src/Modules/SocialModules.cpp
  src/Modules/PacketLogic.cpp
  src/Modules/HiveLogic.cpp
  src/Modules/VisualModules.cpp)
target_include_directories(OinksterCore PUBLIC include)
target_compile_definitions(OinksterCore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(OinksterCore PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(OinksterCore PUBLIC d2d1 dwrite d3d12 dxgi mfplat mfreadwrite mfuuid advapi32 psapi runtimeobject windowscodecs ole32 shell32 ws2_32 minhook libhat OinksterImGui)

add_library(OinksterClient SHARED src/DllMain.cpp resources/OinksterNative.rc)
set_target_properties(OinksterClient PROPERTIES OUTPUT_NAME "oinkster-client-x64")
target_link_libraries(OinksterClient PRIVATE OinksterCore)
target_compile_options(OinksterClient PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

add_executable(Oinkster.ModuleHarness WIN32 harness/Main.cpp)
target_link_libraries(Oinkster.ModuleHarness PRIVATE OinksterCore user32)
target_compile_options(Oinkster.ModuleHarness PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

enable_testing()
add_executable(Oinkster.Native.Tests tests/NativeTests.cpp)
target_link_libraries(Oinkster.Native.Tests PRIVATE OinksterCore)
target_compile_options(Oinkster.Native.Tests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# The parity test reads the launcher's catalog straight from the source tree.
target_compile_definitions(Oinkster.Native.Tests PRIVATE
  OINKSTER_MODULE_CATALOG_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../Assets/module-catalog.json")
add_test(NAME Oinkster.Native.Tests COMMAND Oinkster.Native.Tests)

add_executable(Oinkster.Dx12Hook.Tests tests/Dx12HookTests.cpp)
target_link_libraries(Oinkster.Dx12Hook.Tests PRIVATE OinksterCore)
target_compile_options(Oinkster.Dx12Hook.Tests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_test(NAME Oinkster.Dx12Hook.Tests COMMAND Oinkster.Dx12Hook.Tests)
set_tests_properties(Oinkster.Dx12Hook.Tests PROPERTIES TIMEOUT 10)

add_executable(Oinkster.Injector tools/InjectorMain.cpp)
target_link_libraries(Oinkster.Injector PRIVATE OinksterCore)
target_compile_options(Oinkster.Injector PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

add_executable(Oinkster.InjectionHost tests/InjectionHost.cpp)
target_compile_options(Oinkster.InjectionHost PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

add_library(Oinkster.TestPayload SHARED tests/TestPayload.cpp)
target_compile_options(Oinkster.TestPayload PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
