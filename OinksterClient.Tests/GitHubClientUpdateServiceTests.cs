using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using OinksterClient.Models;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

public sealed class GitHubClientUpdateServiceTests
{
    [Fact]
    public async Task CheckAsync_ValidSignedRelease_ReturnsNotInstalled()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var manifest = CreateManifest();
        var manifestBytes = Encoding.UTF8.GetBytes(manifest);
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var handler = new ReleaseHandler(manifestBytes, signature);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(handler),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);

        var result = await service.CheckAsync("Release", "1.21.50.0");

            Assert.Equal(UpdateStatus.NotInstalled, result.Status);
            Assert.Equal("1.0.0", result.Release?.Manifest.Version);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task CheckAsync_TamperedManifest_ReturnsInvalidRelease()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var original = Encoding.UTF8.GetBytes(CreateManifest());
        var signature = signingKey.SignData(original, HashAlgorithmName.SHA256);
        var tampered = Encoding.UTF8.GetBytes(CreateManifest().Replace("1.0.0", "2.0.0", StringComparison.Ordinal));
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(tampered, signature)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);

            var result = await service.CheckAsync("Release", "1.21.50.0");

            Assert.Equal(UpdateStatus.InvalidRelease, result.Status);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task CheckAsync_UnsupportedBedrock_ReturnsIncompatible()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest());
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);

            var result = await service.CheckAsync("Release", "1.22.0.0");

            Assert.Equal(UpdateStatus.Incompatible, result.Status);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task CheckAsync_WrongNativeInterface_ReturnsInvalidRelease()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest().Replace(
            "\"nativeInterfaceVersion\":2",
            "\"nativeInterfaceVersion\":3",
            StringComparison.Ordinal));
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);

            var result = await service.CheckAsync("Release", "1.21.50.0");

            Assert.Equal(UpdateStatus.InvalidRelease, result.Status);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task InstallAsync_VerifiedDownload_InstallsClient()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var dll = "abc"u8.ToArray();
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest(Convert.ToHexString(SHA256.HashData(dll))));
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature, dll)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);
            var result = await service.CheckAsync("Release", "1.21.50.0");

            var installed = await service.InstallAsync(result.Release!);

            Assert.Equal(dll, File.ReadAllBytes(installed.DllPath));
            Assert.False(File.Exists(installed.DllPath + ".partial"));
            Assert.Equal(UpdateStatus.Ready, (await service.CheckAsync("Release", "1.21.50.0")).Status);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task InstallAsync_PreviousCopyStillLocked_InstallsUpdate()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var dll = "abc"u8.ToArray();
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest(Convert.ToHexString(SHA256.HashData(dll))));
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature, dll)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory);
            var result = await service.CheckAsync("Release", "1.21.50.0");
            var first = await service.InstallAsync(result.Release!);
            var parked = first.DllPath + ".previous";
            File.WriteAllBytes(parked, "old"u8.ToArray());

            InstalledClientBuild second;
            using (new FileStream(parked, FileMode.Open, FileAccess.Read, FileShare.None))
            {
                second = await service.InstallAsync(result.Release!);
            }

            Assert.Equal(dll, File.ReadAllBytes(second.DllPath));
            Assert.Equal(UpdateStatus.Ready, (await service.CheckAsync("Release", "1.21.50.0")).Status);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    [Fact]
    public async Task CheckAsync_NewerLauncherInManifest_ReturnsLauncherUpdate()
    {
        var installer = "setup"u8.ToArray();
        var result = await CheckWithLauncherAsync(installer, "1.0.2", new Version(1, 0, 1));

        Assert.NotNull(result.Launcher);
        Assert.Equal("1.0.2", result.Launcher!.Installer.Version);
        Assert.Equal("https://github.com/installer", result.Launcher.InstallerUri.AbsoluteUri);
    }

    [Fact]
    public async Task CheckAsync_LauncherNotNewer_ReturnsNoLauncherUpdate()
    {
        var installer = "setup"u8.ToArray();
        var result = await CheckWithLauncherAsync(installer, "1.0.1", new Version(1, 0, 1));

        Assert.Null(result.Launcher);
    }

    [Fact]
    public async Task CheckAsync_IncompatibleBedrock_StillReturnsLauncherUpdate()
    {
        var installer = "setup"u8.ToArray();
        var result = await CheckWithLauncherAsync(installer, "1.0.2", new Version(1, 0, 1), "1.22.0.0");

        Assert.Equal(UpdateStatus.Incompatible, result.Status);
        Assert.NotNull(result.Launcher);
    }

    [Fact]
    public async Task DownloadLauncherAsync_VerifiedInstaller_ReturnsLocalCopy()
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var installer = "setup"u8.ToArray();
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest(launcher: Launcher(installer, "1.0.2")));
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature, installer: installer)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory, new Version(1, 0, 1), Path.Combine(directory, "launcher"));
            var result = await service.CheckAsync("Release", "1.21.50.0");

            var path = await service.DownloadLauncherAsync(result.Launcher!);

            Assert.Equal("OinksterClient-Setup-1.0.2.exe", Path.GetFileName(path));
            Assert.Equal(installer, File.ReadAllBytes(path));
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    private static async Task<ClientUpdateResult> CheckWithLauncherAsync(
        byte[] installer, string launcherVersion, Version runningLauncher, string bedrock = "1.21.50.0")
    {
        using var signingKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var manifestBytes = Encoding.UTF8.GetBytes(CreateManifest(launcher: Launcher(installer, launcherVersion)));
        var signature = signingKey.SignData(manifestBytes, HashAlgorithmName.SHA256);
        var directory = CreateTemporaryDirectory();

        try
        {
            var service = new GitHubClientUpdateService(
                new HttpClient(new ReleaseHandler(manifestBytes, signature, installer: installer)),
                new ReleaseTrustOptions("Turbored80-MC", "oinkster-client", signingKey.ExportSubjectPublicKeyInfoPem()),
                directory, runningLauncher, Path.Combine(directory, "launcher"));
            return await service.CheckAsync("Release", bedrock);
        }
        finally
        {
            Directory.Delete(directory, true);
        }
    }

    private static object Launcher(byte[] installer, string version) => new
    {
        version,
        fileName = $"OinksterClient-Setup-{version}.exe",
        size = installer.Length,
        sha256 = Convert.ToHexString(SHA256.HashData(installer))
    };

    private static string CreateManifest(string? dllSha256 = null, object? launcher = null) => JsonSerializer.Serialize(new
    {
        schemaVersion = 2,
        channel = "Release",
        version = "1.0.0",
        publishedAt = "2026-10-01T12:00:00Z",
        architecture = "x64",
        nativeInterfaceVersion = 2,
        dll = new { fileName = "oinkster-client-x64.dll", size = 3, sha256 = dllSha256 ?? new string('A', 64) },
        bedrock = new { exactVersion = "1.21.50.0" },
        launcher
    });

    private static string CreateTemporaryDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "OinksterClientTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private sealed class ReleaseHandler(byte[] manifest, byte[] signature, byte[]? dll = null, byte[]? installer = null) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var url = request.RequestUri!.AbsoluteUri;
            if (url.Contains("api.github.com", StringComparison.Ordinal))
            {
                // Every installer version the tests sign is listed; only the one
                // the manifest names is ever looked up.
                var installerAssets = installer is null ? "" : string.Concat(new[] { "1.0.1", "1.0.2" }.Select(version =>
                    $$""",{"name":"OinksterClient-Setup-{{version}}.exe","size":{{installer.Length}},"browser_download_url":"https://github.com/installer"}"""));
                var json = $$"""
                    [{"tag_name":"release-v1.0.0","assets":[
                      {"name":"oinkster-manifest.json","size":1,"browser_download_url":"https://github.com/manifest"},
                      {"name":"oinkster-manifest.sig","size":1,"browser_download_url":"https://github.com/signature"},
                      {"name":"oinkster-client-x64.dll","size":3,"browser_download_url":"https://github.com/client"}{{installerAssets}}
                    ]}]
                    """;
                return Task.FromResult(Response(Encoding.UTF8.GetBytes(json), "application/json"));
            }

            if (url.EndsWith("installer", StringComparison.Ordinal) && installer is not null)
            {
                return Task.FromResult(Response(installer, "application/octet-stream"));
            }

            if (url.EndsWith("client", StringComparison.Ordinal) && dll is not null)
            {
                return Task.FromResult(Response(dll, "application/octet-stream"));
            }

            return Task.FromResult(url.EndsWith("manifest", StringComparison.Ordinal)
                ? Response(manifest, "application/json")
                : Response(signature, "application/octet-stream"));
        }

        private static HttpResponseMessage Response(byte[] content, string contentType) => new(HttpStatusCode.OK)
        {
            Content = new ByteArrayContent(content) { Headers = { ContentType = new(contentType) } }
        };
    }
}
