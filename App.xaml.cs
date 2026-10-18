using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace OinksterClient;

/// <summary>
/// Provides application-specific behavior to supplement the default Application class.
/// </summary>
public partial class App : Application
{
    /// <summary>
    /// The main application window. Use <c>App.Window</c> from any class that needs
    /// the window reference (for dialogs, pickers, interop, etc.).
    /// </summary>
    public static Window Window { get; private set; } = null!;

    /// <summary>
    /// The UI thread dispatcher. Use <c>App.DispatcherQueue</c> to marshal calls
    /// to the UI thread. Fully qualified to avoid CS0104 ambiguity with
    /// <see cref="Windows.System.DispatcherQueue"/>.
    /// </summary>
    public static Microsoft.UI.Dispatching.DispatcherQueue DispatcherQueue { get; private set; } = null!;

    /// <summary>
    /// The native window handle (HWND). Use for file pickers,
    /// <c>DataTransferManager</c>, and any WinRT interop that requires
    /// <c>InitializeWithWindow</c>.
    /// </summary>
    public static nint WindowHandle =>
        WinRT.Interop.WindowNative.GetWindowHandle(Window);

    /// <summary>
    /// Initializes the singleton application object.
    /// </summary>
    public App()
    {
        InitializeComponent();

        // Last-resort diagnostics. Individual handlers are guarded at the call
        // site (MainPage.RunGuarded), but anything that still escapes would
        // otherwise terminate the process with nothing written down - which is
        // exactly the failure mode that makes these bugs hard to report.
        UnhandledException += (_, e) =>
        {
            Log("unhandled_exception", e.Exception.ToString());
            // Left unhandled: the process still fails fast, but now there is a
            // record of why.
        };

        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            Log("unobserved_task_exception", e.Exception.ToString());
            e.SetObserved();
        };

        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            Log("appdomain_unhandled_exception", e.ExceptionObject?.ToString() ?? "unknown");
    }

    /// <summary>
    /// Appends a JSON line to the same log directory the native client writes
    /// to, so launcher and client failures can be read together.
    /// </summary>
    internal static void Log(string eventName, string message)
    {
        try
        {
            var directory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Oinkster", "Client");
            Directory.CreateDirectory(directory);
            File.AppendAllText(
                Path.Combine(directory, "oinkster-launcher.log"),
                $"{{\"timestamp\":\"{DateTimeOffset.UtcNow:O}\",\"event\":\"{EscapeJson(eventName)}\",\"message\":\"{EscapeJson(message)}\"}}\n");
        }
        catch
        {
            // Logging must never be the thing that takes the app down.
        }
    }

    /// <summary>
    /// Minimal JSON string escaping. Hand-rolled rather than via
    /// <c>JsonSerializer</c> so the crash logger pulls in no reflection-based
    /// serialization (which the trim analyzer flags) and cannot itself fail on
    /// the path where something has already gone wrong.
    /// </summary>
    private static string EscapeJson(string value)
    {
        var builder = new System.Text.StringBuilder(value.Length + 16);
        foreach (var character in value)
        {
            switch (character)
            {
                case '"': builder.Append("\\\""); break;
                case '\\': builder.Append("\\\\"); break;
                case '\n': builder.Append("\\n"); break;
                case '\r': builder.Append("\\r"); break;
                case '\t': builder.Append("\\t"); break;
                default:
                    if (character < ' ') builder.Append("\\u").Append(((int)character).ToString("x4"));
                    else builder.Append(character);
                    break;
            }
        }

        return builder.ToString();
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="args">Details about the launch request and process.</param>
    protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
    {
        Window = new MainWindow();
        DispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        Window.Activate();
    }
}
