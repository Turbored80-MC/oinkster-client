using System.Text.Json.Serialization;

namespace OinksterClient.Models;

public sealed record ClientReleaseManifest(
    [property: JsonPropertyName("schemaVersion")] int SchemaVersion,
    [property: JsonPropertyName("channel")] string Channel,
    [property: JsonPropertyName("version")] string Version,
    [property: JsonPropertyName("publishedAt")] DateTimeOffset PublishedAt,
    [property: JsonPropertyName("architecture")] string Architecture,
    [property: JsonPropertyName("nativeInterfaceVersion")] int NativeInterfaceVersion,
    [property: JsonPropertyName("dll")] ClientDllAsset Dll,
    [property: JsonPropertyName("bedrock")] BedrockCompatibility Bedrock,
    // Optional: the launcher installer shipped with this release. Launchers
    // from before self-update ignore it (unknown JSON members are skipped),
    // so it needs no schema bump.
    [property: JsonPropertyName("launcher")] LauncherInstallerAsset? Launcher = null);

public sealed record LauncherInstallerAsset(
    [property: JsonPropertyName("version")] string Version,
    [property: JsonPropertyName("fileName")] string FileName,
    [property: JsonPropertyName("size")] long Size,
    [property: JsonPropertyName("sha256")] string Sha256);

public sealed record ClientDllAsset(
    [property: JsonPropertyName("fileName")] string FileName,
    [property: JsonPropertyName("size")] long Size,
    [property: JsonPropertyName("sha256")] string Sha256);

public sealed record BedrockCompatibility(
    [property: JsonPropertyName("exactVersion")] string ExactVersion);
