using System.Text.Json.Serialization;

namespace OinksterClient.Models;

public sealed record ModuleCatalogDocument(
    [property: JsonPropertyName("schemaVersion")] int SchemaVersion,
    [property: JsonPropertyName("modules")] IReadOnlyList<ModuleDefinition> Modules);

// The string members are declared non-nullable but System.Text.Json will
// happily leave them null for a catalog entry that omits the property, which
// then threw a NullReferenceException inside RenderModuleList's search filter.
// JsonRequired makes a malformed catalog a clean JsonException at parse time
// (surfaced by ModuleSettingsService.LoadCatalogAsync) instead.
public sealed record ModuleDefinition(
    [property: JsonPropertyName("id"), JsonRequired] string Id,
    [property: JsonPropertyName("name"), JsonRequired] string Name,
    [property: JsonPropertyName("description"), JsonRequired] string Description,
    [property: JsonPropertyName("category"), JsonRequired] string Category,
    [property: JsonPropertyName("available")] bool Available,
    [property: JsonPropertyName("hud")] bool Hud,
    [property: JsonPropertyName("unavailableReason")] string? UnavailableReason,
    // Capability ids the module needs (e.g. "render-metrics"). "available"
    // is true exactly when every one has a reviewed implementation for the
    // pinned build; live activation is reported separately by native status.
    [property: JsonPropertyName("capabilities")] IReadOnlyList<string>? Capabilities = null);

public sealed class ModuleClientSettings
{
    public bool Enabled { get; set; }
    public uint? Keybind { get; set; }
    public HudClientSettings Hud { get; set; } = new();
    public Dictionary<string, string> Options { get; set; } = [];
}

public sealed class HudClientSettings
{
    public float X { get; set; } = 0.02F;
    public float Y { get; set; } = 0.05F;
    public float Scale { get; set; } = 1.0F;
    public bool Background { get; set; } = true;
    public float Opacity { get; set; } = 0.88F;
    public string BackgroundColor { get; set; } = "#FF111417";
    public string TextColor { get; set; } = "#FFE8ECEF";
    public string AccentColor { get; set; } = "#FFF28FB5";
}

public sealed class ModuleSettingsDocument
{
    public int SchemaVersion { get; set; } = 2;
    public ulong Revision { get; set; }
    public uint MenuKey { get; set; } = 0x4B;
    public uint UnloadKey { get; set; } = 0xDC;
    public Dictionary<string, ModuleClientSettings> Modules { get; set; } = [];
    [JsonPropertyName("legacyModuleSettings")]
    public Dictionary<string, string> ModuleSettings { get; set; } = [];
}
