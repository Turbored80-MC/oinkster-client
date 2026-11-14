using Microsoft.UI.Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace OinksterClient;

/// <summary>
/// The application window. This hosts a Frame that displays pages. Add your
/// UI and logic to MainPage.xaml / MainPage.xaml.cs instead of here so you
/// can use Page features such as navigation events and the Loaded lifecycle.
/// </summary>
public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        // Absolute path: the taskbar button and its hover thumbnail both come
        // from this window icon, and a relative path only resolves when the
        // working directory happens to be the app folder.
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "OinksterAppIcon.ico"));
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1180, 760));

        // Navigate the root frame to the main page on startup.
        // Main content is attached after the window is initialized.
        RootFrame.Content = new MainPage();
    }
}
