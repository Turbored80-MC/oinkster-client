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

    private static string CreateManifest(string? dllSha256 = null) => JsonSerializer.Serialize(new
    {
        schemaVersion = 2,
        channel = "Release",
        version = "1.0.0",
        publishedAt = "2026-10-01T12:00:00Z",
        architecture = "x64",
        nativeInterfaceVersion = 2,
        dll = new { fileName = "oinkster-client-x64.dll", size = 3, sha256 = dllSha256 ?? new string('A', 64) },
        bedrock = new { exactVersion = "1.21.50.0" }
    });

    private static string CreateTemporaryDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "OinksterClientTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private sealed class ReleaseHandler(byte[] manifest, byte[] signature, byte[]? dll = null) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var url = request.RequestUri!.AbsoluteUri;
            if (url.Contains("api.github.com", StringComparison.Ordinal))
            {
                var json = """
                    [{"tag_name":"release-v1.0.0","assets":[
                      {"name":"oinkster-manifest.json","size":1,"browser_download_url":"https://github.com/manifest"},
                      {"name":"oinkster-manifest.sig","size":1,"browser_download_url":"https://github.com/signature"},
                      {"name":"oinkster-client-x64.dll","size":3,"browser_download_url":"https://github.com/client"}
                    ]}]
                    """;
                return Task.FromResult(Response(Encoding.UTF8.GetBytes(json), "application/json"));
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
