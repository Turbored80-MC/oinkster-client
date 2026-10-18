namespace OinksterClient.Models;

public enum UpdateStatus
{
    Ready,
    UpdateAvailable,
    NotInstalled,
    Incompatible,
    Offline,
    TrustNotConfigured,
    InvalidRelease
}

public sealed record InstalledClientBuild(
    string Version,
    string DllPath,
    string Sha256,
    BedrockCompatibility Bedrock);

public sealed record ClientRelease(
    ClientReleaseManifest Manifest,
    Uri DllUri);

// A signed launcher installer newer than the running launcher.
public sealed record LauncherUpdate(
    LauncherInstallerAsset Installer,
    Uri InstallerUri);

public sealed record ClientUpdateResult(
    UpdateStatus Status,
    ClientRelease? Release,
    InstalledClientBuild? InstalledBuild,
    string Message,
    LauncherUpdate? Launcher = null);

public sealed record DownloadProgress(long BytesReceived, long TotalBytes)
{
    public double Percentage => TotalBytes <= 0 ? 0 : (double)BytesReceived / TotalBytes * 100;
}
