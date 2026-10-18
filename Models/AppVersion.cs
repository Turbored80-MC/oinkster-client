namespace OinksterClient.Models;

/// <summary>
/// The launcher's own version. Keep in step with the Version properties in
/// OinksterClient.csproj and the Identity version in Package.appxmanifest.
/// </summary>
public static class AppVersion
{
    public const string Display = "1.0.2 Beta";
    public const string SemVer = "1.0.2-beta";

    // The numeric part of SemVer, compared against a release's launcher version.
    public static Version Number { get; } = Version.Parse(SemVer.Split('-')[0]);
}
