namespace OinksterClient.Models;

public sealed record BedrockInstallation(bool IsInstalled, string? Version)
{
    public static BedrockInstallation NotInstalled { get; } = new(false, null);
}
