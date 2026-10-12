using OinksterClient.Models;

namespace OinksterClient.Services;

public sealed class ReviewedInjectorRequired : IClientInjector
{
    public Task<InjectionResult> InjectAsync(
        int processId,
        InstalledClientBuild clientBuild,
        CancellationToken cancellationToken = default)
    {
        return Task.FromResult(new InjectionResult(
            InjectionStatus.NotImplemented,
            "Client activation is disabled until the native injector passes security review."));
    }
}
