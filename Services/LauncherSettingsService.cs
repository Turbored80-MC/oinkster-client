using System.Text.Json;
using OinksterClient.Models;

namespace OinksterClient.Services;

// Stored as a plain file rather than in ApplicationData so the launcher works
// the same whether it runs as a packaged dev build or as the unpackaged
// release download, which has no package identity.
public sealed class LauncherSettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private static readonly LauncherSettings Defaults = new("Release", true, false);

    private readonly string settingsPath;

    public LauncherSettingsService(string? settingsDirectory = null)
    {
        settingsPath = Path.Combine(
            settingsDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Oinkster"),
            "launcher-settings.json");
    }

    public LauncherSettings Load()
    {
        try
        {
            if (!File.Exists(settingsPath)) return Defaults;
            var settings = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(settingsPath), JsonOptions);
            return settings is null || string.IsNullOrWhiteSpace(settings.UpdateChannel) ? Defaults : settings;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            return Defaults;
        }
    }

    public void Save(LauncherSettings settings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
        var temporaryPath = settingsPath + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, JsonOptions));
        File.Move(temporaryPath, settingsPath, true);
    }
}
