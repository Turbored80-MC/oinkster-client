using OinksterClient.Models;
using Windows.Storage;

namespace OinksterClient.Services;

public sealed class LauncherSettingsService
{
    private const string UpdateChannelKey = "UpdateChannel";
    private const string CheckForUpdatesKey = "CheckForUpdatesOnStartup";
    private const string CloseAfterLaunchKey = "CloseAfterLaunch";

    private readonly ApplicationDataContainer localSettings = ApplicationData.Current.LocalSettings;

    public LauncherSettings Load()
    {
        return new LauncherSettings(
            localSettings.Values[UpdateChannelKey] as string ?? "Release",
            localSettings.Values[CheckForUpdatesKey] as bool? ?? true,
            localSettings.Values[CloseAfterLaunchKey] as bool? ?? false);
    }

    public void Save(LauncherSettings settings)
    {
        localSettings.Values[UpdateChannelKey] = settings.UpdateChannel;
        localSettings.Values[CheckForUpdatesKey] = settings.CheckForUpdatesOnStartup;
        localSettings.Values[CloseAfterLaunchKey] = settings.CloseAfterLaunch;
    }
}
