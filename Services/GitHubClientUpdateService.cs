using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using OinksterClient.Models;
using Windows.Storage;

namespace OinksterClient.Services;

public sealed class GitHubClientUpdateService : IClientUpdateService
{
    private const int ManifestSchemaVersion = 2;
    private const int NativeInterfaceVersion = 2;
    private const long MaximumDllSize = 100 * 1024 * 1024;
    private const string ManifestAssetName = "oinkster-manifest.json";
    private const string SignatureAssetName = "oinkster-manifest.sig";
    private const string DllAssetName = "oinkster-client-x64.dll";

    // Metadata calls and the DLL transfer need very different budgets, and
    // HttpClient.Timeout is a single per-client value that also covers reading
    // the response body. Both are therefore driven by per-request linked
    // tokens below, with the client's own timeout disabled in the constructor.
    private static readonly TimeSpan MetadataTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan DownloadTimeout = TimeSpan.FromMinutes(10);

    private readonly HttpClient httpClient;
    private readonly ReleaseTrustOptions trustOptions;
    private readonly string clientDirectory;
    private readonly JsonSerializerOptions jsonOptions = new(JsonSerializerDefaults.Web);

    public GitHubClientUpdateService(HttpClient httpClient, ReleaseTrustOptions trustOptions, string? clientDirectory = null)
    {
        this.httpClient = httpClient;
        this.trustOptions = trustOptions;
        this.clientDirectory = clientDirectory ?? Path.Combine(ApplicationData.Current.LocalFolder.Path, "Client");

        if (!this.httpClient.DefaultRequestHeaders.UserAgent.Any())
        {
            this.httpClient.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("OinksterClient", AppVersion.SemVer));
        }

        // A single client-wide timeout cannot serve both a 15 s metadata call
        // and a multi-minute DLL download, and because it also covers the body
        // read it would abort the download outright. Timeouts are applied per
        // request instead (MetadataTimeout / DownloadTimeout). Setting this is
        // only legal before the first request, which the constructor satisfies.
        try
        {
            this.httpClient.Timeout = Timeout.InfiniteTimeSpan;
        }
        catch (InvalidOperationException)
        {
            // Client already used; its existing timeout stays in effect.
        }
    }

    public async Task<ClientUpdateResult> CheckAsync(string channel, string? bedrockVersion, CancellationToken cancellationToken = default)
    {
        var installed = await LoadInstalledBuildAsync(cancellationToken);
        if (string.IsNullOrWhiteSpace(trustOptions.PublicKeyPem))
        {
            return new ClientUpdateResult(UpdateStatus.TrustNotConfigured, null, installed, "Release signing is not configured yet.");
        }

        using var metadataTimeout = new CancellationTokenSource(MetadataTimeout);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, metadataTimeout.Token);
        var metadataToken = linked.Token;

        try
        {
            var releasesUri = new Uri($"https://api.github.com/repos/{trustOptions.RepositoryOwner}/{trustOptions.RepositoryName}/releases?per_page=30");
            var releases = await httpClient.GetFromJsonAsync<List<GitHubRelease>>(releasesUri, jsonOptions, metadataToken) ?? [];
            var prefix = channel.ToLowerInvariant() switch
            {
                "beta" => "beta-v",
                "nightly" => "nightly-v",
                _ => "release-v"
            };
            var release = releases.FirstOrDefault(item => item.TagName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
            if (release is null)
            {
                return new ClientUpdateResult(UpdateStatus.Offline, null, installed, $"No {channel} release is published.");
            }

            var manifestAsset = FindAsset(release, ManifestAssetName);
            var signatureAsset = FindAsset(release, SignatureAssetName);
            var dllAsset = FindAsset(release, DllAssetName);
            if (manifestAsset is null || signatureAsset is null || dllAsset is null)
            {
                return new ClientUpdateResult(UpdateStatus.InvalidRelease, null, installed, "The release is missing required assets.");
            }

            var manifestBytes = await DownloadSmallAssetAsync(manifestAsset.BrowserDownloadUrl, metadataToken);
            var signatureBytes = await DownloadSmallAssetAsync(signatureAsset.BrowserDownloadUrl, metadataToken);
            if (!VerifySignature(manifestBytes, signatureBytes))
            {
                return new ClientUpdateResult(UpdateStatus.InvalidRelease, null, installed, "The release signature is invalid.");
            }

            var manifest = JsonSerializer.Deserialize<ClientReleaseManifest>(manifestBytes, jsonOptions);
            if (!ValidateManifest(manifest, channel, dllAsset.Size))
            {
                return new ClientUpdateResult(UpdateStatus.InvalidRelease, null, installed, "The signed release manifest is invalid.");
            }

            var clientRelease = new ClientRelease(manifest!, new Uri(dllAsset.BrowserDownloadUrl));
            var compatible = IsCompatible(manifest!.Bedrock, bedrockVersion);
            if (!compatible)
            {
                return new ClientUpdateResult(UpdateStatus.Incompatible, clientRelease, installed, "Waiting for an Oinkster update for this Bedrock version.");
            }

            if (installed is null)
            {
                return new ClientUpdateResult(UpdateStatus.NotInstalled, clientRelease, null, "Oinkster is ready to download.");
            }

            var updateAvailable = Version.TryParse(manifest.Version, out var availableVersion)
                && Version.TryParse(installed.Version, out var installedVersion)
                && availableVersion > installedVersion;
            return new ClientUpdateResult(
                updateAvailable ? UpdateStatus.UpdateAvailable : UpdateStatus.Ready,
                clientRelease,
                installed,
                updateAvailable ? "An Oinkster update is available." : "Oinkster is ready.");
        }
        catch (HttpRequestException)
        {
            return new ClientUpdateResult(UpdateStatus.Offline, null, installed, "Could not reach GitHub. Using the installed client when compatible.");
        }
        catch (TaskCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new ClientUpdateResult(UpdateStatus.Offline, null, installed, "The update check timed out.");
        }
        // ArgumentException covers ECDsa.ImportFromPem on a malformed trust key,
        // which otherwise escaped into the caller's async void handler.
        catch (Exception exception) when (exception is CryptographicException or JsonException
            or InvalidDataException or ArgumentException)
        {
            return new ClientUpdateResult(UpdateStatus.InvalidRelease, null, installed, "The published release could not be verified.");
        }
    }

    public async Task<InstalledClientBuild> InstallAsync(ClientRelease release, IProgress<DownloadProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        if (release.DllUri.Scheme != Uri.UriSchemeHttps
            || !release.DllUri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The client DLL must be downloaded from a GitHub release asset.");
        }

        Directory.CreateDirectory(clientDirectory);
        var partialPath = Path.Combine(clientDirectory, DllAssetName + ".partial");
        var activePath = Path.Combine(clientDirectory, DllAssetName);
        var previousPath = Path.Combine(clientDirectory, DllAssetName + ".previous");
        var metadataPath = Path.Combine(clientDirectory, "installed.json");
        var metadataPartialPath = metadataPath + ".partial";

        // HttpClient.Timeout spans reading the response BODY, not just the
        // headers, so the shared client's short metadata timeout would abort
        // any DLL download that took longer than it. ResponseHeadersRead plus a
        // linked token scoped to this request gives the transfer its own, much
        // longer budget without changing the client used for metadata calls.
        using var downloadTimeout = new CancellationTokenSource(DownloadTimeout);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, downloadTimeout.Token);
        var downloadToken = linked.Token;

        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, release.DllUri);
            using var response = await httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, downloadToken);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is > MaximumDllSize)
            {
                throw new InvalidDataException("The client DLL exceeds the maximum allowed size.");
            }

            await using var source = await response.Content.ReadAsStreamAsync(downloadToken);
            await using var destination = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = new byte[81920];
            long received = 0;
            int read;
            while ((read = await source.ReadAsync(buffer, downloadToken)) > 0)
            {
                received += read;
                if (received > MaximumDllSize || received > release.Manifest.Dll.Size)
                {
                    throw new InvalidDataException("The downloaded DLL has an unexpected size.");
                }

                await destination.WriteAsync(buffer.AsMemory(0, read), downloadToken);
                hash.AppendData(buffer, 0, read);
                progress?.Report(new DownloadProgress(received, release.Manifest.Dll.Size));
            }

            await destination.FlushAsync(downloadToken);
            var actualHash = Convert.ToHexString(hash.GetHashAndReset());
            if (received != release.Manifest.Dll.Size || !actualHash.Equals(release.Manifest.Dll.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("The downloaded DLL failed integrity verification.");
            }

            var installed = new InstalledClientBuild(release.Manifest.Version, activePath, actualHash, release.Manifest.Bedrock);
            await File.WriteAllTextAsync(metadataPartialPath, JsonSerializer.Serialize(installed, jsonOptions), downloadToken);

            if (File.Exists(activePath))
            {
                File.Move(activePath, previousPath, true);
            }

            try
            {
                File.Move(partialPath, activePath, true);
                File.Move(metadataPartialPath, metadataPath, true);
                File.Delete(previousPath);
            }
            catch
            {
                if (File.Exists(previousPath))
                {
                    File.Move(previousPath, activePath, true);
                }

                throw;
            }

            return installed;
        }
        finally
        {
            if (File.Exists(partialPath))
            {
                File.Delete(partialPath);
            }

            if (File.Exists(metadataPartialPath))
            {
                File.Delete(metadataPartialPath);
            }
        }
    }

    private bool VerifySignature(byte[] manifestBytes, byte[] signatureBytes)
    {
        using var ecdsa = ECDsa.Create();
        ecdsa.ImportFromPem(trustOptions.PublicKeyPem);
        return ecdsa.VerifyData(manifestBytes, signatureBytes, HashAlgorithmName.SHA256);
    }

    private static bool ValidateManifest(ClientReleaseManifest? manifest, string channel, long releaseAssetSize)
    {
        return manifest is not null
            && manifest.SchemaVersion == ManifestSchemaVersion
            && manifest.Channel.Equals(channel, StringComparison.OrdinalIgnoreCase)
            && manifest.Architecture.Equals("x64", StringComparison.OrdinalIgnoreCase)
            && manifest.NativeInterfaceVersion == NativeInterfaceVersion
            && manifest.Dll.FileName.Equals(DllAssetName, StringComparison.Ordinal)
            && manifest.Dll.Size > 0
            && manifest.Dll.Size <= MaximumDllSize
            && manifest.Dll.Size == releaseAssetSize
            && manifest.Dll.Sha256.Length == 64
            && manifest.Dll.Sha256.All(Uri.IsHexDigit)
            && Version.TryParse(manifest.Version, out _)
            && Version.TryParse(manifest.Bedrock.ExactVersion, out _);
    }

    private static bool IsCompatible(BedrockCompatibility compatibility, string? bedrockVersion)
    {
        return Version.TryParse(bedrockVersion, out var installed)
            && Version.TryParse(compatibility.ExactVersion, out var supported)
            && installed == supported;
    }

    private async Task<InstalledClientBuild?> LoadInstalledBuildAsync(CancellationToken cancellationToken)
    {
        var metadataPath = Path.Combine(clientDirectory, "installed.json");
        if (!File.Exists(metadataPath))
        {
            return null;
        }

        try
        {
            var installed = JsonSerializer.Deserialize<InstalledClientBuild>(await File.ReadAllTextAsync(metadataPath, cancellationToken), jsonOptions);
            if (installed is null || !File.Exists(installed.DllPath))
            {
                return null;
            }

            // `await using` matters here: HashDataAsync does not dispose the
            // stream it is handed. Every update check (startup, "Check now",
            // channel change, post-install re-check) therefore leaked an open
            // handle on the client DLL, and until the finalizer ran
            // InstallAsync's File.Move of that same DLL could fail with a
            // sharing violation and roll the update back.
            await using var dll = File.OpenRead(installed.DllPath);
            var actualHash = Convert.ToHexString(await SHA256.HashDataAsync(dll, cancellationToken));
            return actualHash.Equals(installed.Sha256, StringComparison.OrdinalIgnoreCase) ? installed : null;
        }
        // Reading installed.json or the DLL can fail for reasons unrelated to
        // JSON (the file locked, ACLs). This is a best-effort probe for an
        // existing install, so any failure means "treat it as not installed"
        // rather than propagating out of CheckAsync - which, because this call
        // sits outside the caller's try, used to crash the launcher at startup.
        catch (Exception exception) when (exception is JsonException or IOException
            or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            return null;
        }
    }

    private async Task<byte[]> DownloadSmallAssetAsync(string url, CancellationToken cancellationToken)
    {
        using var response = await httpClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is > 65536)
        {
            throw new InvalidDataException("Release metadata is too large.");
        }

        var bytes = await response.Content.ReadAsByteArrayAsync(cancellationToken);
        return bytes.Length <= 65536 ? bytes : throw new InvalidDataException("Release metadata is too large.");
    }

    private static GitHubAsset? FindAsset(GitHubRelease release, string name) =>
        release.Assets.FirstOrDefault(asset => asset.Name.Equals(name, StringComparison.Ordinal));

    private sealed record GitHubRelease(
        [property: JsonPropertyName("tag_name")] string TagName,
        [property: JsonPropertyName("assets")] List<GitHubAsset> Assets);

    private sealed record GitHubAsset(
        [property: JsonPropertyName("name")] string Name,
        [property: JsonPropertyName("size")] long Size,
        [property: JsonPropertyName("browser_download_url")] string BrowserDownloadUrl);
}
