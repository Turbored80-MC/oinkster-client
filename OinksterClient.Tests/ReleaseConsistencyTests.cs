using System.Text.Json;
using System.Xml.Linq;
using OinksterClient.Models;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

public sealed class ReleaseConsistencyTests
{
    [Fact]
    public void ReleaseMetadata_AllSurfacesMatchAuthoritativeVersion()
    {
        var root = FindRepositoryRoot();
        var project = XDocument.Load(Path.Combine(root, "OinksterClient.csproj"));
        var applicationManifest = XDocument.Load(Path.Combine(root, "app.manifest"));
        var manifest = XDocument.Load(Path.Combine(root, "Package.appxmanifest"));
        using var release = JsonDocument.Parse(File.ReadAllText(Path.Combine(root, "website", "dist", "release.json")));

        Assert.Equal("1.1.1", project.Descendants("Version").Single().Value);
        Assert.Equal(AppVersion.SemVer, project.Descendants("InformationalVersion").Single().Value);
        Assert.Equal("1.1.1.0", manifest.Descendants().Single(element => element.Name.LocalName == "Identity").Attribute("Version")?.Value);
        Assert.Equal("1.1.1.0", applicationManifest.Descendants().Single(element => element.Name.LocalName == "assemblyIdentity").Attribute("version")?.Value);
        Assert.Contains(manifest.Descendants(), element =>
            element.Name.LocalName == "Capability" && element.Attribute("Name")?.Value == "globalMediaControl");
        Assert.Contains(project.Descendants("ProjectReference"), element =>
            element.Attribute("Include")?.Value == @"Oinkster.MediaBroker\Oinkster.MediaBroker.csproj");
        Assert.Equal(AppVersion.Display, release.RootElement.GetProperty("version").GetString());
        Assert.Contains("release-v1.1.1/OinksterClient-Setup-1.1.1.exe",
            release.RootElement.GetProperty("downloadUrl").GetString(), StringComparison.Ordinal);
    }

    [Fact]
    public void SupportedBuild_DevelopmentAndNativeConstantsMatch()
    {
        var root = FindRepositoryRoot();
        var native = File.ReadAllText(Path.Combine(root, "Native", "include", "Oinkster", "SupportedBuild.hpp"));
        Assert.Contains($"PackageVersion = L\"{DevelopmentClientBuildService.SupportedBedrockVersion}\"", native, StringComparison.Ordinal);
        Assert.Contains("NetworkProtocol = 2225", native, StringComparison.Ordinal);
    }

    private static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "OinksterClient.csproj")))
        {
            directory = directory.Parent;
        }
        return directory?.FullName ?? throw new DirectoryNotFoundException("Repository root was not found.");
    }
}
