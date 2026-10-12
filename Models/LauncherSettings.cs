namespace OinksterClient.Models;

public sealed record LauncherSettings(
    string UpdateChannel,
    bool CheckForUpdatesOnStartup,
    bool CloseAfterLaunch);
