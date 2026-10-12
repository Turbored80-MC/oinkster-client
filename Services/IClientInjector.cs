using OinksterClient.Models;

namespace OinksterClient.Services;

public interface IClientInjector
{
    Task<InjectionResult> InjectAsync(
        int processId,
        InstalledClientBuild clientBuild,
        CancellationToken cancellationToken = default);
}
