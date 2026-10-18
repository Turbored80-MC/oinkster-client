using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using OinksterClient.Models;

namespace OinksterClient.Services;

public sealed class GitHubClientUpdateService : IClientUpdateService
{
    private const int ManifestSchemaVersion = 2;
    private const int NativeInterfaceVersion = 2;
    private const long MaximumDllSize = 100 * 1024 * 1024;
    private const long MaximumInstallerSize = 400 * 1024 * 1024;
    private const string InstallerNamePrefix = "OinksterClient-Setup-";
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
    private readonly string launcherDownloadDirectory;
    private readonly Version launcherVersion;
    private readonly JsonSerializerOptions jsonOptions = new(JsonSerializerDefaults.Web);

    public GitHubClientUpdateService(
        HttpClient httpClient,
        ReleaseTrustOptions trustOptions,
        string? clientDirectory = null,
        Version? launcherVersion = null,
        string? launcherDownloadDirectory = null)
    {
        this.httpClient = httpClient;
        this.trustOptions = trustOptions;
        var oinksterData = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Oinkster");
        this.clientDirectory = clientDirectory ?? Path.Combine(oinksterData, "Releases");
        this.launcherDownloadDirectory = launcherDownloadDirectory ?? Path.Combine(oinksterData, "LauncherUpdates");
        this.launcherVersion = Normalize(launcherVersion ?? AppVersion.Number);

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
            // Found before the compatibility check: a new launcher matters most
            // exactly when this one cannot run the client.
            var launcher = FindLauncherUpdate(release, manifest!);
            var compatible = IsCompatible(manifest!.Bedrock, bedrockVersion);
            if (!compatible)
            {
                return new ClientUpdateResult(UpdateStatus.Incompatible, clientRelease, installed, "Waiting for an Oinkster update for this Bedrock version.", launcher);
            }

            if (installed is null)
            {
                return new ClientUpdateResult(UpdateStatus.NotInstalled, clientRelease, null, "Oinkster is ready to download.", launcher);
            }

            var updateAvailable = Version.TryParse(manifest.Version, out var availableVersion)
                && Version.TryParse(installed.Version, out var installedVersion)
                && availableVersion > installedVersion;
            return new ClientUpdateResult(
                updateAvailable ? UpdateStatus.UpdateAvailable : UpdateStatus.Ready,
                clientRelease,
                installed,
                updateAvailable ? "An Oinkster update is available." : "Oinkster is ready.",
                launcher);
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
        RequireGitHubAsset(release.DllUri, "client DLL");
        Directory.CreateDirectory(clientDirectory);
        var partialPath = Path.Combine(clientDirectory, DllAssetName + ".partial");
        var activePath = Path.Combine(clientDirectory, DllAssetName);
        // Minecraft keeps the old DLL mapped until it restarts (ejecting does
        // not unmap it). Windows lets a mapped image be renamed but not deleted
        // or replaced, so each update parks the old copy under a fresh name and
        // removes parked copies only when nothing holds them any more. A fixed
        // ".previous" name failed the update outright - and rolled it back -
        // whenever the game still had the old client loaded.
        DeleteParkedCopies();
        var previousPath = Path.Combine(clientDirectory, $"{DllAssetName}.previous-{Guid.NewGuid():N}");
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
            var actualHash = await DownloadVerifiedAsync(release.DllUri, partialPath, release.Manifest.Dll.Size,
                release.Manifest.Dll.Sha256, MaximumDllSize, "client DLL", progress, downloadToken);

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
            }
            catch
            {
                if (File.Exists(previousPath))
                {
                    File.Move(previousPath, activePath, true);
                }

                throw;
            }

            DeleteParkedCopies();
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

    public async Task<string> DownloadLauncherAsync(LauncherUpdate update, IProgress<DownloadProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        RequireGitHubAsset(update.InstallerUri, "launcher installer");
        Directory.CreateDirectory(launcherDownloadDirectory);
        // Installers from earlier updates have already run; clear them out.
        foreach (var stale in Directory.EnumerateFiles(launcherDownloadDirectory))
        {
            try
            {
                File.Delete(stale);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
            }
        }

        var installerPath = Path.Combine(launcherDownloadDirectory, update.Installer.FileName);
        var partialPath = installerPath + ".partial";
        using var downloadTimeout = new CancellationTokenSource(DownloadTimeout);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, downloadTimeout.Token);
        try
        {
            await DownloadVerifiedAsync(update.InstallerUri, partialPath, update.Installer.Size, update.Installer.Sha256,
                MaximumInstallerSize, "launcher installer", progress, linked.Token);
            File.Move(partialPath, installerPath, true);
            return installerPath;
        }
        finally
        {
            if (File.Exists(partialPath))
            {
                File.Delete(partialPath);
            }
        }
    }

    // Streams a release asset to `path`, enforcing the signed size and SHA-256.
    // Returns the hash. The file is closed on return, so the caller can move it.
    private async Task<string> DownloadVerifiedAsync(Uri uri, string path, long expectedSize, string expectedSha256,
        long maximumSize, string label, IProgress<DownloadProgress>? progress, CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        using var response = await httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is { } length && length > maximumSize)
        {
            throw new InvalidDataException($"The {label} exceeds the maximum allowed size.");
        }

        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long received = 0;
        // Scoped so the file is closed before the caller moves it into place;
        // moving it while still open fails on Windows.
        await using (var source = await response.Content.ReadAsStreamAsync(cancellationToken))
        await using (var destination = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true))
        {
            var buffer = new byte[81920];
            int read;
            while ((read = await source.ReadAsync(buffer, cancellationToken)) > 0)
            {
                received += read;
                if (received > maximumSize || received > expectedSize)
                {
                    throw new InvalidDataException($"The downloaded {label} has an unexpected size.");
                }

                await destination.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                hash.AppendData(buffer, 0, read);
                progress?.Report(new DownloadProgress(received, expectedSize));
            }

            await destination.FlushAsync(cancellationToken);
        }

        var actualHash = Convert.ToHexString(hash.GetHashAndReset());
        if (received != expectedSize || !actualHash.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException($"The downloaded {label} failed integrity verification.");
        }

        return actualHash;
    }

    private static void RequireGitHubAsset(Uri uri, string label)
    {
        if (uri.Scheme != Uri.UriSchemeHttps || !uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException($"The {label} must be downloaded from a GitHub release asset.");
        }
    }

    // The installer is only offered when the signed manifest names it, the
    // release actually carries it at the signed size, and it is newer than
    // the running launcher.
    private LauncherUpdate? FindLauncherUpdate(GitHubRelease release, ClientReleaseManifest manifest)
    {
        var installer = manifest.Launcher;
        if (installer is null
            || !Version.TryParse(installer.Version, out var available)
            || Normalize(available) <= launcherVersion
            || !installer.FileName.StartsWith(InstallerNamePrefix, StringComparison.Ordinal)
            || !installer.FileName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            || installer.FileName != Path.GetFileName(installer.FileName)
            || installer.FileName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0
            || installer.Size <= 0
            || installer.Size > MaximumInstallerSize
            || installer.Sha256.Length != 64
            || !installer.Sha256.All(Uri.IsHexDigit))
        {
            return null;
        }

        var asset = FindAsset(release, installer.FileName);
        return asset is null || asset.Size != installer.Size
            ? null
            : new LauncherUpdate(installer, new Uri(asset.BrowserDownloadUrl));
    }

    // 1.0.1 and 1.0.1.0 must compare equal; Version treats missing parts as -1.
    private static Version Normalize(Version version) =>
        new(version.Major, version.Minor, Math.Max(version.Build, 0), Math.Max(version.Revision, 0));

    // Best effort: a copy the game still has loaded stays until a later run.
    private void DeleteParkedCopies()
    {
        foreach (var path in Directory.EnumerateFiles(clientDirectory, DllAssetName + ".previous*"))
        {
            try
            {
                File.Delete(path);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
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
