using OinksterClient.Models;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

public sealed class CrosshairSettingsTests
{
    [Fact]
    public void ReadSource_MissingKey_DefaultsToCustom()
    {
        Assert.Equal(CrosshairSettings.SourceCustom, CrosshairSettings.ReadSource(new Dictionary<string, string>()));
    }

    [Theory]
    [InlineData("game", "game")]
    [InlineData("drawn", "drawn")]
    [InlineData("custom", "custom")]
    [InlineData("vanilla", "custom")]
    [InlineData("", "custom")]
    public void ReadSource_StoredValue_MatchesNativeNormalization(string stored, string expected)
    {
        var settings = new Dictionary<string, string> { [CrosshairSettings.SourceKey] = stored };
        Assert.Equal(expected, CrosshairSettings.ReadSource(settings));
    }

    [Theory]
    [InlineData(null, "first-person")]
    [InlineData("all", "all")]
    [InlineData("first-person", "first-person")]
    [InlineData("third-person", "first-person")]
    public void ReadPerspective_StoredValue_FallsBackToFirstPerson(string? stored, string expected)
    {
        var settings = new Dictionary<string, string>();
        if (stored is not null) settings[CrosshairSettings.PerspectiveKey] = stored;
        Assert.Equal(expected, CrosshairSettings.ReadPerspective(settings));
    }

    [Fact]
    public void ReadNumber_OutOfRangeOrMalformed_ClampsOrDefaults()
    {
        var settings = new Dictionary<string, string>
        {
            [CrosshairSettings.Size.Key] = "999",
            [CrosshairSettings.Gap.Key] = "not a number",
            [CrosshairSettings.Opacity.Key] = "0.5"
        };
        Assert.Equal(24, CrosshairSettings.ReadNumber(settings, CrosshairSettings.Size));
        Assert.Equal(3, CrosshairSettings.ReadNumber(settings, CrosshairSettings.Gap));
        Assert.Equal(0.5, CrosshairSettings.ReadNumber(settings, CrosshairSettings.Opacity));
        Assert.Equal(1.5, CrosshairSettings.ReadNumber(settings, CrosshairSettings.DotSize));
    }

    [Fact]
    public void ReadBool_MissingOrUnknown_UsesDescriptorDefault()
    {
        var settings = new Dictionary<string, string> { [CrosshairSettings.CenterDot.Key] = "on" };
        Assert.True(CrosshairSettings.ReadBool(settings, CrosshairSettings.Outline));
        Assert.True(CrosshairSettings.ReadBool(settings, CrosshairSettings.CenterDot));
        settings[CrosshairSettings.Outline.Key] = "maybe";
        Assert.True(CrosshairSettings.ReadBool(settings, CrosshairSettings.Outline));
        settings[CrosshairSettings.Outline.Key] = "false";
        Assert.False(CrosshairSettings.ReadBool(settings, CrosshairSettings.Outline));
    }

    [Fact]
    public void ReadBool_ThirdPersonMissing_DefaultsToShown()
    {
        var settings = new Dictionary<string, string>();
        Assert.True(CrosshairSettings.ReadBool(settings, CrosshairSettings.ThirdPerson));
        settings[CrosshairSettings.ThirdPerson.Key] = "false";
        Assert.False(CrosshairSettings.ReadBool(settings, CrosshairSettings.ThirdPerson));
    }

    [Fact]
    public void ReadNumber_DrawnAndGameScales_ClampToNativeRanges()
    {
        var settings = new Dictionary<string, string>
        {
            [CrosshairSettings.PixelScale.Key] = "0",
            [CrosshairSettings.GameScale.Key] = "100"
        };
        Assert.Equal(1, CrosshairSettings.ReadNumber(settings, CrosshairSettings.PixelScale));
        Assert.Equal(8, CrosshairSettings.ReadNumber(settings, CrosshairSettings.GameScale));
        Assert.Equal(2, CrosshairSettings.ReadNumber(new Dictionary<string, string>(), CrosshairSettings.PixelScale));
    }

    [Theory]
    [InlineData("#ff00ff00", "#FF00FF00")]
    [InlineData("#00ff00", "#FF00FF00")]
    [InlineData("green", null)]
    [InlineData("#12345", null)]
    public void NormalizeColor_Input_AcceptsNativeFormats(string input, string? expected)
    {
        Assert.Equal(expected, CrosshairSettings.NormalizeColor(input));
    }

    [Fact]
    public async Task SaveAsync_CrosshairChoices_RoundTripThroughSettingsFile()
    {
        var directory = Path.Combine(Path.GetTempPath(), "oinkster-crosshair-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var service = new ModuleSettingsService(directory);
            var document = new ModuleSettingsDocument();
            document.ModuleSettings[CrosshairSettings.SourceKey] = CrosshairSettings.SourceGame;
            document.ModuleSettings[CrosshairSettings.PerspectiveKey] = CrosshairSettings.PerspectiveAll;
            document.ModuleSettings[CrosshairSettings.DotSize.Key] = CrosshairSettings.FormatNumber(2.5);

            Assert.True(await service.SaveAsync(document));
            var loaded = await service.LoadAsync();

            Assert.Equal(CrosshairSettings.SourceGame, CrosshairSettings.ReadSource(loaded.ModuleSettings));
            Assert.Equal(CrosshairSettings.PerspectiveAll, CrosshairSettings.ReadPerspective(loaded.ModuleSettings));
            Assert.Equal(2.5, CrosshairSettings.ReadNumber(loaded.ModuleSettings, CrosshairSettings.DotSize));
        }
        finally { Directory.Delete(directory, true); }
    }
}
