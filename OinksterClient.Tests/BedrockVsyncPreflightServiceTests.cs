using System.Text;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

public sealed class BedrockVsyncPreflightServiceTests : IDisposable
{
    private readonly string directory = Path.Combine(Path.GetTempPath(), "oinkster-vsync-" + Guid.NewGuid().ToString("N"));

    [Fact]
    public void SetOption_ExistingVsync_PreservesOtherOptionsAndLineEndings()
    {
        const string input = "gfx_gamma:0.5\r\ngfx_vsync:1\r\ngfx_viewdistance:16\r\n";
        Assert.Equal("gfx_gamma:0.5\r\ngfx_vsync:0\r\ngfx_viewdistance:16\r\n",
            BedrockVsyncPreflightService.SetOption(input, "gfx_vsync", "0"));
    }

    [Fact]
    public void Prepare_Dispose_RestoresExactOptionsBytes()
    {
        var minecraftPe = Path.Combine(directory, "minecraftpe");
        Directory.CreateDirectory(minecraftPe);
        var path = Path.Combine(minecraftPe, "options.txt");
        var original = Encoding.UTF8.GetPreamble().Concat(Encoding.UTF8.GetBytes("gfx_vsync:1\r\ncustom:value\r\n")).ToArray();
        File.WriteAllBytes(path, original);
        var service = new BedrockVsyncPreflightService([directory]);

        using (service.Prepare())
        {
            Assert.Contains("gfx_vsync:0", File.ReadAllText(path));
        }

        Assert.Equal(original, File.ReadAllBytes(path));
    }

    [Theory]
    [InlineData(null, false)]
    [InlineData("0", false)]
    [InlineData("false", false)]
    [InlineData("1", true)]
    [InlineData("true", true)]
    public void IsRequested_SettingValue_OnlyEnablesExplicitOptIn(string? value, bool expected)
    {
        var settings = new Dictionary<string, string>();
        if (value is not null) settings["click-gui.disable-vsync"] = value;

        Assert.Equal(expected, BedrockVsyncPreflightService.IsRequested(settings));
    }

    public void Dispose()
    {
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
    }
}
