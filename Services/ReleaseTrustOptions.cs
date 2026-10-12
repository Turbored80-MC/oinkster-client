namespace OinksterClient.Services;

public sealed record ReleaseTrustOptions(
    string RepositoryOwner,
    string RepositoryName,
    string PublicKeyPem)
{
    public static ReleaseTrustOptions Production { get; } = new(
        "Turbored80-MC",
        "oinkster-client",
        LoadPackagedPublicKey());

    private static string LoadPackagedPublicKey()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "OinksterReleasePublicKey.pem");
        return File.Exists(path) ? File.ReadAllText(path) : string.Empty;
    }
}
