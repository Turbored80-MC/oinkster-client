#pragma once

#include "Oinkster/Modules/ModuleBase.hpp"

// One factory per catalog module. Grouped by the file that implements them.
namespace Oinkster::Modules {

// PerformanceModules.cpp
std::unique_ptr<IModule> MakeFps(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFrameTime(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFpsStatistics(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFrameTimeGraph(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeProcessMemory(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakePerformanceSummary(ModuleMetadata, HudLayout, SharedServices&);

// InputModules.cpp
std::unique_ptr<IModule> MakeCps(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeKeystrokes(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeInputLatency(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeDcPrevent(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeCpsCap(ModuleMetadata, HudLayout, SharedServices&);

// TelemetryModules.cpp
std::unique_ptr<IModule> MakePing(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeCoordinates(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeDirection(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeMovementSpeed(ModuleMetadata, HudLayout, SharedServices&);

// StatusModules.cpp
std::unique_ptr<IModule> MakeClock(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeStopwatch(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeSessionStatistics(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeArmorStatus(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHeldItemDurability(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeItemCounter(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakePotionEffects(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeServerDisplay(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeResourcePackDisplay(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeReplayMod(ModuleMetadata, HudLayout, SharedServices&);

// PvpModules.cpp
std::unique_ptr<IModule> MakeComboCounter(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHitDistance(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeEquipmentWarnings(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakePotionCounter(ModuleMetadata, HudLayout, SharedServices&);

// VisualModules.cpp
std::unique_ptr<IModule> MakeCustomCrosshair(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeBlockOutline(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHurtColor(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeMotionBlur(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFovProfiles(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeViewBobbing(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHandView(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeZoom(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFreelook(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeCinematicCamera(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeThirdPersonDistance(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeFullbright(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeTimeChanger(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeWeatherSuppression(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeScreenOverlaySuppression(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHurtCamera(ModuleMetadata, HudLayout, SharedServices&);

// AccessibilityModules.cpp
std::unique_ptr<IModule> MakeSubtitles(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeChatTools(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeToggleSprintDynamicFps(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeToggleSneak(ModuleMetadata, HudLayout, SharedServices&);

// UtilityModules.cpp
std::unique_ptr<IModule> MakeScrollWheelDisabler(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeSensitivityMultiplier(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeNullMovement(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeModernKeyboardHandling(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeSnapLook(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeChunkBorders(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakePostProcessing(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeDebugMenu(ModuleMetadata, HudLayout, SharedServices&);

// SocialModules.cpp
std::unique_ptr<IModule> MakePlayerList(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeHiveUtils(ModuleMetadata, HudLayout, SharedServices&);
std::unique_ptr<IModule> MakeSkinGrabber(ModuleMetadata, HudLayout, SharedServices&);

}
