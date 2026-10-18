using System.Text.Json.Serialization;

namespace OinksterClient.Models;

public sealed record NativeClientStatus(
    [property: JsonPropertyName("interfaceVersion")] int InterfaceVersion,
    [property: JsonPropertyName("state")] string State,
    [property: JsonPropertyName("moduleCount")] int? ModuleCount,
    [property: JsonPropertyName("hooksInstalled")] bool? HooksInstalled,
    [property: JsonPropertyName("supportedBedrock")] string? SupportedBedrock,
    [property: JsonPropertyName("inputRouting")] string? InputRouting,
    [property: JsonPropertyName("serverNetwork")] string? ServerNetwork = null,
    [property: JsonPropertyName("competitivePolicies")] IReadOnlyDictionary<string, string>? CompetitivePolicies = null,
    // Live capability states ("available" / "pending" / "unavailable"),
    // keyed by the same ids as module-catalog.json. Optional: older native
    // builds do not send it.
    [property: JsonPropertyName("capabilities")] IReadOnlyDictionary<string, string>? Capabilities = null,
    // Optional, additive diagnostics for unavailable/pending capabilities.
    // Older native builds omit this member.
    [property: JsonPropertyName("capabilityReasons")] IReadOnlyDictionary<string, string>? CapabilityReasons = null);
