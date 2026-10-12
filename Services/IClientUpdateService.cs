using OinksterClient.Models;

namespace OinksterClient.Services;

public interface IClientUpdateService
{
    Task<ClientUpdateResult> CheckAsync(string channel, string? bedrockVersion, CancellationToken cancellationToken = default);

    Task<InstalledClientBuild> InstallAsync(
        ClientRelease release,
        IProgress<DownloadProgress>? progress = null,
        CancellationToken cancellationToken = default);
}
