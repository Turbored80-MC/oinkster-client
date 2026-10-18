using OinksterClient.Models;
using Microsoft.Win32;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Management.Deployment;
using Windows.System;

namespace OinksterClient.Services;

public sealed class BedrockService
{
    private const string LegacyPackageFamilyName = "Microsoft.MinecraftUWP_8wekyb3d8bbwe";

    public Task<BedrockInstallation> DetectAsync()
    {
        return Task.Run(() =>
        {
            try
            {
                var packageManager = new PackageManager();
                var package = packageManager.FindPackagesForUser(string.Empty, LegacyPackageFamilyName).FirstOrDefault()
                    ?? packageManager.FindPackagesForUser(string.Empty).FirstOrDefault(candidate =>
                        (candidate.Id.Name.Contains("Minecraft", StringComparison.OrdinalIgnoreCase)
                            || candidate.DisplayName.Contains("Minecraft", StringComparison.OrdinalIgnoreCase))
                        && !candidate.Id.Name.Contains("Dungeons", StringComparison.OrdinalIgnoreCase)
                        && !candidate.DisplayName.Contains("Dungeons", StringComparison.OrdinalIgnoreCase)
                        && !candidate.Id.Name.Contains("Launcher", StringComparison.OrdinalIgnoreCase));

                if (package is null)
                {
                    return DetectFromProtocolRegistration();
                }

                var version = package.Id.Version;
                return new BedrockInstallation(
                    true,
                    $"{version.Major}.{version.Minor}.{version.Build}.{version.Revision}");
            }
            catch (Exception)
            {
                return DetectFromProtocolRegistration();
            }
        });
    }

    private static BedrockInstallation DetectFromProtocolRegistration()
    {
        try
        {
            using var commandKey = Registry.ClassesRoot.OpenSubKey(@"minecraft\shell\open\command");
            var command = commandKey?.GetValue(null) as string;
            if (string.IsNullOrWhiteSpace(command))
            {
                return BedrockInstallation.NotInstalled;
            }

            var executable = command.Trim().Trim('"').Split('"', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
            if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable))
            {
                return BedrockInstallation.NotInstalled;
            }

            var version = FileVersionInfo.GetVersionInfo(executable).FileVersion;
            return Version.TryParse(version, out var parsed)
                ? new BedrockInstallation(true, parsed.ToString())
                : BedrockInstallation.NotInstalled;
        }
        catch (Exception)
        {
            return BedrockInstallation.NotInstalled;
        }
    }

    public Task<bool> LaunchAsync()
    {
        return Launcher.LaunchUriAsync(new Uri("minecraft:")).AsTask();
    }

    public async Task<int?> WaitForProcessAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        int? stableProcessId = null;
        var stableChecks = 0;
        do
        {
            cancellationToken.ThrowIfCancellationRequested();
            var readyProcessId = default(int?);
            foreach (var process in Process.GetProcessesByName("Minecraft.Windows"))
            {
                using (process)
                {
                    try
                    {
                        if (IsReady(process))
                        {
                            readyProcessId = process.Id;
                            break;
                        }
                    }
                    catch (InvalidOperationException)
                    {
                    }
                }
            }

            if (readyProcessId is not null && readyProcessId == stableProcessId)
            {
                stableChecks++;
                if (stableChecks >= 12)
                {
                    return readyProcessId;
                }
            }
            else
            {
                stableProcessId = readyProcessId;
                stableChecks = readyProcessId is null ? 0 : 1;
            }

            await Task.Delay(250, cancellationToken);
        }
        while (DateTimeOffset.UtcNow < deadline);

        return null;
    }

    public int? TryGetRunningProcessId()
    {
        foreach (var process in Process.GetProcessesByName("Minecraft.Windows"))
        {
            using (process)
            {
                try
                {
                    if (IsReady(process))
                    {
                        return process.Id;
                    }
                }
                catch (InvalidOperationException)
                {
                }
            }
        }

        return null;
    }

    private static bool IsReady(Process process)
    {
        if (!process.Responding || process.HasExited || DateTime.Now - process.StartTime < TimeSpan.FromSeconds(5))
        {
            return false;
        }

        var found = false;
        EnumWindows((window, parameter) =>
        {
            _ = parameter;
            GetWindowThreadProcessId(window, out var processId);
            if (processId != process.Id || !IsWindowVisible(window))
            {
                return true;
            }

            var className = new StringBuilder(64);
            if (GetClassName(window, className, className.Capacity) == 0
                || !className.ToString().Equals("Bedrock", StringComparison.Ordinal))
            {
                return true;
            }

            if (GetClientRect(window, out var rectangle)
                && rectangle.Right - rectangle.Left >= 640
                && rectangle.Bottom - rectangle.Top >= 360)
            {
                found = true;
                return false;
            }

            return true;
        }, IntPtr.Zero);
        return found;
    }

    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRectangle
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out int processId);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder className, int maximumCount);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out NativeRectangle rectangle);
}
