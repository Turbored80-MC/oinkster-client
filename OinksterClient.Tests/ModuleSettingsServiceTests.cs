using System.Text.Json;
using OinksterClient.Models;
using OinksterClient.Services;
using Xunit;

namespace OinksterClient.Tests;

public sealed class ModuleSettingsServiceTests
{
    [Fact]
    public async Task SaveAsync_ThenLoadAsync_RoundTripsVersionTwoSettings()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            var document = new ModuleSettingsDocument { MenuKey = 'M', UnloadKey = 0xDC };
            document.Modules["fps"] = new ModuleClientSettings { Enabled = true, Keybind = 'F', Hud = new() { Scale = 1.25F, Opacity = 0.7F } };
            document.ModuleSettings["click-gui.favorite.fps"] = "true";

            await service.SaveAsync(document);
            var loaded = await service.LoadAsync();

            Assert.Equal(2, loaded.SchemaVersion);
            Assert.Equal((uint)'M', loaded.MenuKey);
            Assert.True(loaded.Modules["fps"].Enabled);
            Assert.Equal(1.25F, loaded.Modules["fps"].Hud.Scale);
            Assert.Equal("true", loaded.ModuleSettings["click-gui.favorite.fps"]);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public async Task LoadAsync_VersionOne_MigratesModuleStateBindingAndLayout()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            await File.WriteAllTextAsync(service.SettingsPath, """{"schemaVersion":1,"clickGuiKey":77,"modules":{"fps":true},"keybinds":{"bind.fps":70},"layout":{"layout.fps":"0.2,0.3,1.4,0"}}""");

            var loaded = await service.LoadAsync();

            Assert.Equal((uint)77, loaded.MenuKey);
            Assert.True(loaded.Modules["fps"].Enabled);
            Assert.Equal((uint)70, loaded.Modules["fps"].Keybind);
            Assert.False(loaded.Modules["fps"].Hud.Background);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public async Task LoadAsync_CorruptJson_QuarantinesFileAndReturnsDefaults()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            await File.WriteAllTextAsync(service.SettingsPath, "not json");
            var loaded = await service.LoadAsync();
            Assert.Empty(loaded.Modules);
            Assert.Single(Directory.GetFiles(directory, "settings.json.corrupt.*"));
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public async Task SaveAsync_WhenSettingsPathIsADirectory_ReturnsFalseAndKeepsRevision()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            // Occupying settings.json with a directory makes File.Move fail the
            // same way a locked or read-only file does. This used to throw out
            // of an async void handler and take the launcher down.
            Directory.CreateDirectory(service.SettingsPath);

            var document = new ModuleSettingsDocument();
            var saved = await service.SaveAsync(document);

            Assert.False(saved);
            // The revision is rolled back so the in-memory document still
            // matches what is actually on disk.
            Assert.Equal(0UL, document.Revision);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public async Task SaveAsync_OnSuccess_ReturnsTrueAndAdvancesRevision()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            var document = new ModuleSettingsDocument();

            Assert.True(await service.SaveAsync(document));
            Assert.Equal(1UL, document.Revision);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public async Task LoadAsync_VersionOneModuleSettings_MigratesLegacyOptionKeys()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var service = new ModuleSettingsService(directory);
            // The "moduleSettings"/"setting." branch of MigrateVersionOne had
            // no coverage, and the native SettingsStore depends on these
            // landing under the "legacyModuleSettings" JSON name.
            await File.WriteAllTextAsync(service.SettingsPath,
                """{"schemaVersion":1,"moduleSettings":{"setting.custom-crosshair.size":"9","click-gui.scale":"0.8"}}""");

            var loaded = await service.LoadAsync();

            Assert.Equal("9", loaded.ModuleSettings["custom-crosshair.size"]);
            Assert.Equal("0.8", loaded.ModuleSettings["click-gui.scale"]);

            // Round-trip through Save and confirm the on-disk property name the
            // native side reads is preserved.
            Assert.True(await service.SaveAsync(loaded));
            var json = await File.ReadAllTextAsync(service.SettingsPath);
            Assert.Contains("\"legacyModuleSettings\"", json);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public void ModuleDefinition_MissingRequiredProperty_FailsToDeserialize()
    {
        // Id/Name/Description/Category are non-nullable but System.Text.Json
        // would leave them null for a catalog entry that omits them, which then
        // threw an NRE inside the module list's search filter.
        Assert.Throws<JsonException>(() => JsonSerializer.Deserialize<ModuleDefinition>(
            """{"id":"fps","description":"d","category":"Performance","available":true,"hud":true}""",
            new JsonSerializerOptions(JsonSerializerDefaults.Web)));
    }

    private static string CreateTemporaryDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "OinksterModuleSettingsTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path); return path;
    }
}
