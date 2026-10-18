using System.Text.Json;
using OinksterClient.Models;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

// The launcher half of the catalog contract. Oinkster.Native.Tests checks
// this same file field-by-field against the native module table; these
// tests check the launcher can load it and that it is internally consistent.
public sealed class ModuleCatalogTests
{
    // Capabilities the shipped native client has a reviewed implementation for
    // (its own DX12 hook and window routing, plus the game adapters).
    // Mirrors Native/src/Capabilities.cpp.
    private static readonly HashSet<string> SupportedCapabilities =
    [
        "render-metrics", "frame-composition", "local-input", "input-synthesis", "input-filtering",
        "dc-prevent-policy", "cps-cap-policy", "local-player-position",
        "local-player-rotation", "effects", "equipment", "inventory", "network-metrics", "server-state",
        "resource-packs", "combat-events", "sound-events", "chat-events", "packet-capture",
        "selected-block-render", "damage-render", "camera-presentation", "camera-rotation", "camera-motion",
        "third-person-presentation", "lighting-presentation", "time-presentation", "weather-presentation",
        "screen-effects-presentation", "hand-presentation",
        "perspective-presentation",
        "packet-send"
    ];

    private static readonly HashSet<string> KnownCapabilities =
    [
        .. SupportedCapabilities
    ];

    [Fact]
    public async Task LoadCatalogAsync_ShippedCatalog_ContainsAllUniqueModules()
    {
        var catalog = await ModuleSettingsService.LoadCatalogAsync();

        Assert.Equal(60, catalog.Count);
        Assert.Equal(catalog.Count, catalog.Select(module => module.Id).Distinct(StringComparer.Ordinal).Count());
        Assert.All(catalog, module => Assert.Matches("^[a-z0-9-]+$", module.Id));
    }

    [Fact]
    public async Task LoadCatalogAsync_ShippedCatalog_AvailabilityFollowsCapabilities()
    {
        var catalog = await ModuleSettingsService.LoadCatalogAsync();

        Assert.All(catalog, module =>
        {
            Assert.NotNull(module.Capabilities);
            Assert.All(module.Capabilities!, capability => Assert.Contains(capability, KnownCapabilities));
            var supported = module.Capabilities!.All(SupportedCapabilities.Contains);
            Assert.Equal(supported, module.Available);
            if (module.Available) Assert.Null(module.UnavailableReason);
            else Assert.False(string.IsNullOrWhiteSpace(module.UnavailableReason));
        });
    }

    [Fact]
    public async Task LoadCatalogAsync_ToggleSprintDynamicFps_IsDeliberatelyAvailable()
    {
        // Native and launcher used to disagree here (native: available,
        // catalog: unavailable). It needs only Oinkster's own hooks.
        var catalog = await ModuleSettingsService.LoadCatalogAsync();
        var module = Assert.Single(catalog, item => item.Id == "toggle-sprint-dynamic-fps");

        Assert.True(module.Available);
        Assert.Equal(["render-metrics", "local-input", "input-synthesis"], module.Capabilities);
    }

    [Fact]
    public void ModuleDefinition_WithoutCapabilities_StillDeserializes()
    {
        var definition = JsonSerializer.Deserialize<ModuleDefinition>(
            """{"id":"fps","name":"FPS","description":"d","category":"Performance","available":true,"hud":true}""",
            new JsonSerializerOptions(JsonSerializerDefaults.Web));

        Assert.NotNull(definition);
        Assert.Null(definition!.Capabilities);
    }

    [Fact]
    public void NativeClientStatus_WithCapabilities_DeserializesStates()
    {
        var status = JsonSerializer.Deserialize<NativeClientStatus>(
            """{"interfaceVersion":2,"state":"ready","moduleCount":36,"hooksInstalled":true,"capabilities":{"render-metrics":"available","local-player-position":"pending"},"capabilityReasons":{"local-player-position":"validating signature"}}""",
            new JsonSerializerOptions(JsonSerializerDefaults.Web));

        Assert.NotNull(status);
        Assert.Equal("available", status!.Capabilities!["render-metrics"]);
        Assert.Equal("pending", status.Capabilities["local-player-position"]);
        Assert.Equal("validating signature", status.CapabilityReasons!["local-player-position"]);
    }

    [Fact]
    public void NativeClientStatus_FromOlderNativeBuild_HasNoCapabilities()
    {
        var status = JsonSerializer.Deserialize<NativeClientStatus>(
            """{"interfaceVersion":2,"state":"ready","hooksInstalled":true}""",
            new JsonSerializerOptions(JsonSerializerDefaults.Web));

        Assert.NotNull(status);
        Assert.Null(status!.Capabilities);
        Assert.Null(status.CapabilityReasons);
    }
}
