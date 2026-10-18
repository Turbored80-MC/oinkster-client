using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

return args.FirstOrDefault()?.ToLowerInvariant() switch
{
    "keygen" => GenerateKey(args),
    "sign" => SignRelease(args),
    _ => ShowUsage()
};

static int GenerateKey(string[] arguments)
{
    if (arguments.Length != 3) return ShowUsage();
    var privatePath = Path.GetFullPath(arguments[1]);
    var publicPath = Path.GetFullPath(arguments[2]);
    if (File.Exists(privatePath))
    {
        Console.Error.WriteLine($"A private key already exists at {privatePath}");
        return 2;
    }

    Directory.CreateDirectory(Path.GetDirectoryName(privatePath)!);
    Directory.CreateDirectory(Path.GetDirectoryName(publicPath)!);
    using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
    File.WriteAllText(privatePath, key.ExportECPrivateKeyPem(), new UTF8Encoding(false));
    File.WriteAllText(publicPath, key.ExportSubjectPublicKeyInfoPem(), new UTF8Encoding(false));
    Console.WriteLine($"Private key: {privatePath}");
    Console.WriteLine($"Public key:  {publicPath}");
    Console.WriteLine("Store the private PEM as the OINKSTER_RELEASE_PRIVATE_KEY_PEM GitHub Actions secret.");
    return 0;
}

static int SignRelease(string[] arguments)
{
    if (arguments.Length != 6) return ShowUsage();
    var privateKey = Environment.GetEnvironmentVariable("OINKSTER_RELEASE_PRIVATE_KEY_PEM");
    if (string.IsNullOrWhiteSpace(privateKey))
    {
        Console.Error.WriteLine("OINKSTER_RELEASE_PRIVATE_KEY_PEM is not set.");
        return 2;
    }

    var sourceDll = Path.GetFullPath(arguments[1]);
    var version = Version.Parse(arguments[2]);
    var channel = arguments[3];
    var exactBedrock = Version.Parse(arguments[4]);
    var output = Path.GetFullPath(arguments[5]);
    Directory.CreateDirectory(output);
    const string dllName = "oinkster-client-x64.dll";
    var dllPath = Path.Combine(output, dllName);
    File.Copy(sourceDll, dllPath, true);
    var dllBytes = File.ReadAllBytes(dllPath);
    var manifest = new
    {
        schemaVersion = 2,
        channel,
        version = version.ToString(),
        publishedAt = DateTimeOffset.UtcNow,
        architecture = "x64",
        nativeInterfaceVersion = 2,
        dll = new { fileName = dllName, size = dllBytes.LongLength, sha256 = Convert.ToHexString(SHA256.HashData(dllBytes)) },
        bedrock = new { exactVersion = exactBedrock.ToString() }
    };
    var manifestBytes = JsonSerializer.SerializeToUtf8Bytes(manifest);
    File.WriteAllBytes(Path.Combine(output, "oinkster-manifest.json"), manifestBytes);
    using var key = ECDsa.Create();
    key.ImportFromPem(privateKey);
    File.WriteAllBytes(Path.Combine(output, "oinkster-manifest.sig"), key.SignData(manifestBytes, HashAlgorithmName.SHA256));
    Console.WriteLine($"Created release assets in {output}");
    return 0;
}

static int ShowUsage()
{
    Console.Error.WriteLine("Usage:");
    Console.Error.WriteLine("  keygen <private-key-path> <public-key-path>");
    Console.Error.WriteLine("  sign <dll-path> <version> <channel> <exact-bedrock-version> <output-directory>");
    return 1;
}
