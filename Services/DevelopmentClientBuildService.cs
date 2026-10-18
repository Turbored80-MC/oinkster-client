using System.Security.Cryptography;
using OinksterClient.Models;

namespace OinksterClient.Services;

public static class DevelopmentClientBuildService
{
    /// <summary>
    /// Bedrock package version the locally built native DLL targets.
    /// <para>
    /// This must match <c>SupportedPackageVersion</c> in
    /// <c>Native/src/BedrockAdapter.cpp</c>, which is the authority: the
    /// injector compares this string against the running package name, and the
    /// native adapter refuses to attach to anything else. The two live in
    /// different toolchains and cannot share a definition, so a running client
    /// also reports its own value over the status pipe
    /// (<see cref="Models.NativeClientStatus.SupportedBedrock"/>) and the
    /// launcher cross-checks it after injection.
    /// </para>
    /// </summary>
    public const string SupportedBedrockVersion = "1.26.5203.0";

    public static InstalledClientBuild? TryLoad()
    {
#if DEBUG
        var baseDirectory = new DirectoryInfo(AppContext.BaseDirectory);
        var externalDevelopmentPath = baseDirectory.Name.Equals("AppX", StringComparison.OrdinalIgnoreCase)
            ? Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "Native-v3", "oinkster-client-x64.dll"))
            : null;
        var dllPath = externalDevelopmentPath is not null && File.Exists(externalDevelopmentPath)
            ? externalDevelopmentPath
            : Path.Combine(AppContext.BaseDirectory, "Native-v3", "oinkster-client-x64.dll");
        if (!File.Exists(dllPath))
        {
            return null;
        }

        var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(dllPath)));
        return new InstalledClientBuild(
            "0.1.0-dev",
            dllPath,
            hash,
            new BedrockCompatibility(SupportedBedrockVersion));
#else
        return null;
#endif
    }
}
