using System.Diagnostics;

namespace OinksterClient.Services;

public sealed class MediaBrokerService
{
    public bool TryStart(int minecraftProcessId)
    {
        var brokerPath = Path.Combine(AppContext.BaseDirectory, "Oinkster.MediaBroker.exe");
        if (!File.Exists(brokerPath))
        {
            App.Log("media_broker_missing", brokerPath);
            return false;
        }

        try
        {
            Process.Start(new ProcessStartInfo(brokerPath, $"--minecraft-pid {minecraftProcessId}")
            {
                UseShellExecute = true,
                WindowStyle = ProcessWindowStyle.Hidden
            });
            return true;
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            App.Log("media_broker_start_failed", exception.ToString());
            return false;
        }
    }
}
