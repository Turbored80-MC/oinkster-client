using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using OinksterClient.Models;
using OinksterClient.Services;
using Windows.Storage;
using Windows.System;

namespace OinksterClient;

public sealed partial class MainPage : Page
{
    private readonly BedrockService bedrockService = new();
    private readonly LauncherSettingsService settingsService = new();
    private readonly IClientInjector clientInjector = new ReviewedClientInjector();
    private readonly NativeClientStatusService nativeStatusService = new();
    private readonly ModuleSettingsService moduleSettingsService = new();
    private readonly IClientUpdateService updateService = new GitHubClientUpdateService(
        new HttpClient { Timeout = TimeSpan.FromSeconds(15) },
        ReleaseTrustOptions.Production);
    private BedrockInstallation bedrockInstallation = BedrockInstallation.NotInstalled;
    private ClientUpdateResult? clientUpdate;
    private bool settingsLoaded;
    private CancellationTokenSource? updateCheckCancellation;
    private IReadOnlyList<ModuleDefinition> moduleCatalog = [];
    private ModuleSettingsDocument moduleSettings = new();
    private ModuleDefinition? selectedModule;
    private NativeClientStatus? moduleNativeStatus;
    private bool moduleUiLoading;
    private bool moduleRefreshInFlight;
    private readonly DispatcherTimer moduleRefreshTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };

    public MainPage()
    {
        InitializeComponent();
        moduleRefreshTimer.Tick += ModuleRefreshTimer_Tick;
        // Deliberately NOT started here: the tick handler reads moduleCatalog
        // and moduleSettings, which Page_Loaded is what initialises. Starting
        // in the constructor let ticks run against an empty workspace.
        Unloaded += MainPage_Unloaded;
    }

    private void MainPage_Unloaded(object sender, RoutedEventArgs e)
    {
        // Without this the timer keeps firing after the page is gone (including
        // down the CloseAfterLaunch path, which closes the window outright), so
        // a continuation could touch torn-down XAML.
        moduleRefreshTimer.Stop();
        moduleRefreshTimer.Tick -= ModuleRefreshTimer_Tick;
        updateCheckCancellation?.Cancel();
    }

    // Every event handler in this file is `async void`, which in WinUI means an
    // escaped exception kills the process with no diagnostic. Routing them all
    // through here turns that into a visible error instead. Handlers stay
    // `async void` (the XAML event signature requires it) but their bodies move
    // into a Task-returning method called through this.
    private async void RunGuarded(Func<Task> action)
    {
        try
        {
            await action();
        }
        catch (OperationCanceledException)
        {
            // Expected during teardown or a superseded refresh.
        }
        catch (Exception exception)
        {
            App.Log("handler_exception", exception.ToString());
            ShowError("Something went wrong", exception.Message);
        }
    }

    private void ShowError(string title, string message)
    {
        ActionInfo.Title = title;
        ActionInfo.Message = message;
        ActionInfo.Severity = InfoBarSeverity.Error;
        ActionInfo.IsOpen = true;
    }

    private void Page_Loaded(object sender, RoutedEventArgs e) => RunGuarded(async () =>
    {
        LoadSettings();
        await LoadModuleWorkspaceAsync();
        await RefreshBedrockStatusAsync();
        if (bedrockInstallation.IsInstalled && CheckForUpdatesToggle.IsOn)
        {
            await RefreshClientStatusAsync();
        }
        else if (bedrockInstallation.IsInstalled)
        {
            ClientBuildText.Text = "Automatic checks disabled";
            PlayButtonText.Text = "Check for client";
            PlayButton.IsEnabled = true;
        }

        // Safe to start polling only now that the workspace is populated.
        moduleRefreshTimer.Start();
    });

    private void LoadSettings()
    {
        var settings = settingsService.Load();
        UpdateChannelComboBox.SelectedIndex = settings.UpdateChannel switch
        {
            "Beta" => 1,
            "Nightly" => 2,
            _ => 0
        };
        CheckForUpdatesToggle.IsOn = settings.CheckForUpdatesOnStartup;
        CloseAfterLaunchToggle.IsOn = settings.CloseAfterLaunch;
        settingsLoaded = true;
    }

    private async Task RefreshBedrockStatusAsync()
    {
        bedrockInstallation = await bedrockService.DetectAsync();

        if (bedrockInstallation.IsInstalled)
        {
            var version = bedrockInstallation.Version ?? "Unknown version";
            ServiceStatusText.Text = "Bedrock available";
            BedrockVersionText.Text = $"Bedrock {version}";
            BedrockReadyText.Text = "Ready";
            FooterStatusText.Text = "Bedrock detected";
            PlayButtonText.Text = "Launch Oinkster";
            PlayButton.IsEnabled = true;
            SetStatusColor(Colors.LightGreen);
            return;
        }

        ServiceStatusText.Text = "Bedrock unavailable";
        BedrockVersionText.Text = "Not installed";
        BedrockReadyText.Text = "Action needed";
        FooterStatusText.Text = "Minecraft for Windows not found";
        PlayButtonText.Text = "Bedrock required";
        PlayButton.IsEnabled = false;
        SetStatusColor(Colors.IndianRed);
    }

    private void SetStatusColor(Windows.UI.Color color)
    {
        var brush = new SolidColorBrush(color);
        ServiceStatusDot.Fill = brush;
        FooterStatusDot.Fill = brush;
        BedrockReadyText.Foreground = brush;
    }

    private async Task RefreshClientStatusAsync()
    {
#if DEBUG
        var developmentBuild = DevelopmentClientBuildService.TryLoad();
        if (developmentBuild is not null)
        {
            clientUpdate = new ClientUpdateResult(UpdateStatus.Ready, null, developmentBuild, "Local reviewed Debug build.");
            ClientBuildText.Text = $"Development build {developmentBuild.Version}";
            PlayButtonText.Text = "Launch Oinkster";
            PlayButton.IsEnabled = true;
            return;
        }
#endif
        // Cancel the previous check but do NOT dispose it here: a still-running
        // CheckAsync holds its token, and disposing the source out from under
        // it can throw ObjectDisposedException. The superseded call disposes
        // its own source in the finally below.
        updateCheckCancellation?.Cancel();
        var cancellation = new CancellationTokenSource();
        updateCheckCancellation = cancellation;
        var cancellationToken = cancellation.Token;
        var channel = (UpdateChannelComboBox.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "Release";
        ClientBuildText.Text = $"Checking {channel.ToLowerInvariant()} channel…";

        ClientUpdateResult result;
        try
        {
            result = await updateService.CheckAsync(channel, bedrockInstallation.Version, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            cancellation.Dispose();
            return;
        }

        // Three callers race here (Page_Loaded, the fire-and-forget in
        // Setting_Changed, and the Check-now button), and CheckAsync turns a
        // network failure into an Offline *result* rather than throwing - so a
        // superseded call returns normally and used to overwrite the newer
        // one's result along with all the button text. If a newer check has
        // replaced us, drop this result on the floor.
        if (!ReferenceEquals(updateCheckCancellation, cancellation))
        {
            cancellation.Dispose();
            return;
        }

        updateCheckCancellation = null;
        cancellation.Dispose();
        clientUpdate = result;

        switch (clientUpdate.Status)
        {
            case UpdateStatus.NotInstalled:
                ClientBuildText.Text = $"Version {clientUpdate.Release!.Manifest.Version} available";
                PlayButtonText.Text = "Download Oinkster";
                PlayButton.IsEnabled = true;
                break;
            case UpdateStatus.UpdateAvailable:
                ClientBuildText.Text = $"Update {clientUpdate.Release!.Manifest.Version} available";
                PlayButtonText.Text = "Update Oinkster";
                PlayButton.IsEnabled = true;
                break;
            case UpdateStatus.Ready:
                ClientBuildText.Text = $"Build {clientUpdate.InstalledBuild!.Version}";
                PlayButtonText.Text = "Launch Oinkster";
                PlayButton.IsEnabled = true;
                break;
            case UpdateStatus.Incompatible:
                ClientBuildText.Text = "Waiting for compatible update";
                PlayButtonText.Text = "Waiting for update";
                PlayButton.IsEnabled = false;
                break;
            case UpdateStatus.TrustNotConfigured:
                ClientBuildText.Text = "Release signing setup required";
                PlayButtonText.Text = "Signing setup required";
                PlayButton.IsEnabled = false;
                break;
            default:
                ClientBuildText.Text = clientUpdate.InstalledBuild is null ? "Client unavailable" : $"Build {clientUpdate.InstalledBuild.Version}";
                PlayButtonText.Text = clientUpdate.InstalledBuild is null ? "Client unavailable" : "Launch Oinkster";
                PlayButton.IsEnabled = clientUpdate.InstalledBuild is not null;
                break;
        }
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        LaunchPanel.Visibility = Visibility.Collapsed;
        ModulesPanel.Visibility = Visibility.Collapsed;
        SettingsPanel.Visibility = Visibility.Visible;
        SetRailSelection(SettingsButton);
    }

    private void ModulesButton_Click(object sender, RoutedEventArgs e)
    {
        LaunchPanel.Visibility = Visibility.Collapsed;
        SettingsPanel.Visibility = Visibility.Collapsed;
        ModulesPanel.Visibility = Visibility.Visible;
        SetRailSelection(ModulesButton);
        RenderModuleList();
    }

    private void BackButton_Click(object sender, RoutedEventArgs e) => ShowLaunchPanel();

    private void HomeButton_Click(object sender, RoutedEventArgs e) => ShowLaunchPanel();

    private void ShowLaunchPanel()
    {
        SettingsPanel.Visibility = Visibility.Collapsed;
        ModulesPanel.Visibility = Visibility.Collapsed;
        LaunchPanel.Visibility = Visibility.Visible;
        SetRailSelection(HomeButton);
    }

    private void SetRailSelection(Button selectedButton)
    {
        foreach (var button in new[] { HomeButton, ModulesButton, SettingsButton })
        {
            var active = ReferenceEquals(button, selectedButton);
            button.Background = new SolidColorBrush(active
                ? Windows.UI.Color.FromArgb(255, 41, 35, 42) : Colors.Transparent);
            button.Foreground = new SolidColorBrush(active
                ? Windows.UI.Color.FromArgb(255, 255, 177, 205)
                : Windows.UI.Color.FromArgb(255, 141, 135, 143));
        }
    }

    private async Task LoadModuleWorkspaceAsync()
    {
        moduleCatalog = await ModuleSettingsService.LoadCatalogAsync();
        moduleSettings = await moduleSettingsService.LoadAsync();
        UpdateControlKeyLabels();
        ModuleCategoryComboBox.SelectedIndex = 0;
        RenderModuleList();
        SelectModule(moduleCatalog.FirstOrDefault(item => item.Available));
    }

    private void ModuleRefreshTimer_Tick(object? sender, object e) => RunGuarded(async () =>
    {
        if (ModulesPanel.Visibility != Visibility.Visible || moduleUiLoading) return;
        // LoadAsync does a named-pipe round trip plus file I/O and can easily
        // outlast the 500 ms interval, so without this guard ticks overlapped
        // and interleaved their RenderModuleList calls - which could also drop
        // an edit saved between a tick's load and its render.
        if (moduleRefreshInFlight) return;
        moduleRefreshInFlight = true;
        try
        {
            var latest = await moduleSettingsService.LoadAsync();
            var latestNativeStatus = await nativeStatusService.QueryAsync();
            var statusChanged = !NativeStatusEquivalent(moduleNativeStatus, latestNativeStatus);
            if (latest.Revision <= moduleSettings.Revision && !statusChanged) return;
            var selectedId = selectedModule?.Id;
            if (latest.Revision > moduleSettings.Revision) moduleSettings = latest;
            moduleNativeStatus = latestNativeStatus;
            UpdateControlKeyLabels();
            RenderModuleList();
            SelectModule(moduleCatalog.FirstOrDefault(item => item.Id == selectedId));
        }
        finally
        {
            moduleRefreshInFlight = false;
        }
    });

    private void ModuleFilter_Changed(object sender, object e) => RenderModuleList();

    private void RenderModuleList()
    {
        if (ReadyModulesPanel is null || RoadmapModulesPanel is null) return;
        ReadyModulesPanel.Children.Clear();
        RoadmapModulesPanel.Children.Clear();
        var query = ModuleSearchBox?.Text?.Trim() ?? string.Empty;
        var category = (ModuleCategoryComboBox?.SelectedItem as ComboBoxItem)?.Content?.ToString();
        var filtered = moduleCatalog.Where(item =>
            (string.IsNullOrEmpty(query) || item.Name.Contains(query, StringComparison.OrdinalIgnoreCase)
                || item.Description.Contains(query, StringComparison.OrdinalIgnoreCase))
            && (category is null or "All categories" || item.Category == category));
        foreach (var definition in filtered)
        {
            var state = GetModuleSettings(definition.Id);
            var button = new Button
            {
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Padding = new Thickness(14, 11, 14, 11),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 24, 23, 27)),
                BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 45, 41, 49)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(10),
                Tag = definition
            };
            var grid = new Grid { ColumnSpacing = 12 };
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var copy = new StackPanel { Spacing = 2 };
            copy.Children.Add(new TextBlock { Text = definition.Name, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 235, 239)), FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
            copy.Children.Add(new TextBlock { Text = definition.Description, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 119, 113, 121)), FontSize = 11, TextWrapping = TextWrapping.Wrap });
            grid.Children.Add(copy);
            var runtimeIssue = RuntimeIssue(definition);
            var statusText = !definition.Available ? "Later"
                : runtimeIssue is not null ? (state.Enabled ? "Waiting" : "Off")
                : state.Enabled ? "On" : "Off";
            var status = new TextBlock { Text = statusText, VerticalAlignment = VerticalAlignment.Center, Foreground = new SolidColorBrush(definition.Available && state.Enabled && runtimeIssue is null ? Colors.LightGreen : Windows.UI.Color.FromArgb(255, 141, 135, 143)) };
            Grid.SetColumn(status, 1); grid.Children.Add(status); button.Content = grid;
            button.Click += (_, _) => SelectModule((ModuleDefinition)button.Tag);
            (definition.Available ? ReadyModulesPanel : RoadmapModulesPanel).Children.Add(button);
        }
        var enabled = moduleCatalog.Count(item => item.Available && GetModuleSettings(item.Id).Enabled);
        ModuleSummaryText.Text = $"{moduleCatalog.Count(item => item.Available)} ready · {enabled} enabled · {moduleCatalog.Count(item => !item.Available)} on the roadmap";
        EnabledModulesText.Text = enabled == 0 ? "No modules enabled" : enabled == 1 ? "1 module enabled" : $"{enabled} modules enabled";
    }

    private ModuleClientSettings GetModuleSettings(string id)
    {
        if (!moduleSettings.Modules.TryGetValue(id, out var settings))
        {
            moduleSettings.Modules[id] = settings = new ModuleClientSettings();
        }
        return settings;
    }

    private string? RuntimeIssue(ModuleDefinition definition)
    {
        if (!definition.Available || moduleNativeStatus?.State != "ready"
            || moduleNativeStatus.Capabilities is null || definition.Capabilities is null)
        {
            return null;
        }

        foreach (var capability in definition.Capabilities)
        {
            if (!moduleNativeStatus.Capabilities.TryGetValue(capability, out var state)
                || state.Equals("available", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (moduleNativeStatus.CapabilityReasons?.TryGetValue(capability, out var reason) == true
                && !string.IsNullOrWhiteSpace(reason))
            {
                return reason;
            }

            return state.Equals("pending", StringComparison.OrdinalIgnoreCase)
                ? "The native client is still validating this module's game integration."
                : "The native client could not validate this module for the running game session.";
        }

        return null;
    }

    private static bool NativeStatusEquivalent(NativeClientStatus? left, NativeClientStatus? right)
    {
        if (ReferenceEquals(left, right)) return true;
        if (left is null || right is null || left.State != right.State) return false;
        return DictionaryEqual(left.Capabilities, right.Capabilities)
            && DictionaryEqual(left.CapabilityReasons, right.CapabilityReasons);
    }

    private static bool DictionaryEqual(
        IReadOnlyDictionary<string, string>? left,
        IReadOnlyDictionary<string, string>? right)
    {
        if (ReferenceEquals(left, right)) return true;
        if (left is null || right is null || left.Count != right.Count) return false;
        return left.All(pair => right.TryGetValue(pair.Key, out var value) && value == pair.Value);
    }

    private void SelectModule(ModuleDefinition? definition)
    {
        if (definition is null) return;
        selectedModule = definition;
        var settings = GetModuleSettings(definition.Id);
        var runtimeIssue = RuntimeIssue(definition);
        moduleUiLoading = true;
        InspectorCategoryText.Text = !definition.Available ? $"Roadmap · {definition.Category}"
            : runtimeIssue is not null ? $"Waiting · {definition.Category}" : definition.Category;
        InspectorNameText.Text = definition.Name;
        InspectorDescriptionText.Text = definition.Description;
        ModuleEnabledToggle.IsEnabled = definition.Available;
        ModuleEnabledToggle.IsOn = definition.Available && settings.Enabled;
        ModuleControlsPanel.Visibility = definition.Available ? Visibility.Visible : Visibility.Collapsed;
        InspectorRoadmapText.Visibility = definition.Available && runtimeIssue is null ? Visibility.Collapsed : Visibility.Visible;
        InspectorRoadmapText.Text = runtimeIssue ?? definition.UnavailableReason ?? "This module needs additional validated game integration.";
        ModuleKeybindButton.Content = settings.Keybind is null ? "Set keybind" : $"Keybind: {FormatKey(settings.Keybind.Value)}";
        ModuleScaleSlider.IsEnabled = definition.Hud;
        ModuleBackgroundToggle.IsEnabled = definition.Hud;
        ModuleOpacitySlider.IsEnabled = definition.Hud;
        ModuleScaleSlider.Value = settings.Hud.Scale;
        ModuleBackgroundToggle.IsOn = settings.Hud.Background;
        ModuleOpacitySlider.Value = settings.Hud.Opacity;
        ModuleBackgroundColorBox.Text = settings.Hud.BackgroundColor;
        ModuleTextColorBox.Text = settings.Hud.TextColor;
        ModuleAccentColorBox.Text = settings.Hud.AccentColor;
        var isCrosshair = definition.Id == "custom-crosshair";
        CrosshairControlsPanel.Visibility = isCrosshair ? Visibility.Visible : Visibility.Collapsed;
        if (isCrosshair)
        {
            CrosshairSizeSlider.Value = ReadModuleNumber("custom-crosshair.size", 6);
            CrosshairGapSlider.Value = ReadModuleNumber("custom-crosshair.gap", 3);
            CrosshairThicknessSlider.Value = ReadModuleNumber("custom-crosshair.thickness", 2);
            CrosshairOutlineToggle.IsOn = !moduleSettings.ModuleSettings.TryGetValue("custom-crosshair.outline", out var outline) || outline == "true";
        }
        moduleUiLoading = false;
    }

    private void ModuleSetting_Changed(object sender, object e) => RunGuarded(async () =>
    {
        if (moduleUiLoading || selectedModule is null || !selectedModule.Available) return;
        var settings = GetModuleSettings(selectedModule.Id);
        var invariant = System.Globalization.CultureInfo.InvariantCulture;
        var isCrosshair = selectedModule.Id == "custom-crosshair";

        // Write only the control that raised this event. Re-reading every
        // control on every change meant a LostFocus on one colour box also
        // rewrote Enabled, the HUD sliders and all five crosshair keys from
        // whatever they happened to hold - so selecting a non-crosshair module
        // persisted the PREVIOUS module's crosshair values as global settings.
        switch (sender)
        {
            case var _ when ReferenceEquals(sender, ModuleEnabledToggle):
                settings.Enabled = ModuleEnabledToggle.IsOn;
                break;
            case var _ when ReferenceEquals(sender, ModuleScaleSlider) && selectedModule.Hud:
                settings.Hud.Scale = (float)ModuleScaleSlider.Value;
                break;
            case var _ when ReferenceEquals(sender, ModuleBackgroundToggle) && selectedModule.Hud:
                settings.Hud.Background = ModuleBackgroundToggle.IsOn;
                break;
            case var _ when ReferenceEquals(sender, ModuleOpacitySlider) && selectedModule.Hud:
                settings.Hud.Opacity = (float)ModuleOpacitySlider.Value;
                break;
            case var _ when ReferenceEquals(sender, ModuleBackgroundColorBox):
                if (!TryApplyColor(ModuleBackgroundColorBox, value => settings.Hud.BackgroundColor = value)) return;
                break;
            case var _ when ReferenceEquals(sender, ModuleTextColorBox):
                if (!TryApplyColor(ModuleTextColorBox, value => settings.Hud.TextColor = value)) return;
                if (isCrosshair) moduleSettings.ModuleSettings["custom-crosshair.color"] = settings.Hud.TextColor;
                break;
            case var _ when ReferenceEquals(sender, ModuleAccentColorBox):
                if (!TryApplyColor(ModuleAccentColorBox, value => settings.Hud.AccentColor = value)) return;
                break;
            case var _ when ReferenceEquals(sender, CrosshairSizeSlider) && isCrosshair:
                moduleSettings.ModuleSettings["custom-crosshair.size"] = CrosshairSizeSlider.Value.ToString(invariant);
                break;
            case var _ when ReferenceEquals(sender, CrosshairGapSlider) && isCrosshair:
                moduleSettings.ModuleSettings["custom-crosshair.gap"] = CrosshairGapSlider.Value.ToString(invariant);
                break;
            case var _ when ReferenceEquals(sender, CrosshairThicknessSlider) && isCrosshair:
                moduleSettings.ModuleSettings["custom-crosshair.thickness"] = CrosshairThicknessSlider.Value.ToString(invariant);
                break;
            case var _ when ReferenceEquals(sender, CrosshairOutlineToggle) && isCrosshair:
                moduleSettings.ModuleSettings["custom-crosshair.outline"] = CrosshairOutlineToggle.IsOn ? "true" : "false";
                break;
            default:
                return;
        }

        if (!await SaveModuleSettingsAsync()) return;
        RenderModuleList();
    });

    // SaveAsync returns false when settings could not be written (locked or
    // read-only file) rather than throwing. Report that instead of leaving the
    // UI showing a change that was never persisted.
    private async Task<bool> SaveModuleSettingsAsync()
    {
        if (await moduleSettingsService.SaveAsync(moduleSettings)) return true;
        ShowError("Settings not saved",
            $"Could not write {moduleSettingsService.SettingsPath}. Check that the file is not read-only or in use.");
        return false;
    }

    // Invalid colour text used to be discarded in silence, leaving the box
    // showing a value that was never saved.
    private bool TryApplyColor(TextBox box, Action<string> apply)
    {
        if (IsColor(box.Text))
        {
            apply(box.Text.ToUpperInvariant());
            ActionInfo.IsOpen = false;
            return true;
        }

        ActionInfo.Title = "Colour not applied";
        ActionInfo.Message = $"\"{box.Text}\" is not a colour. Use #AARRGGBB, for example #FFF28FB5.";
        ActionInfo.Severity = InfoBarSeverity.Warning;
        ActionInfo.IsOpen = true;
        return false;
    }

    private void ModuleKeybindButton_Click(object sender, RoutedEventArgs e) => RunGuarded(async () =>
    {
        if (selectedModule is null) return;
        var input = new TextBox { PlaceholderText = "A-Z, 0-9, or virtual-key number", MaxLength = 5 };
        var dialog = new ContentDialog { XamlRoot = XamlRoot, Title = $"Bind {selectedModule.Name}", Content = input, PrimaryButtonText = "Save", SecondaryButtonText = "Clear", CloseButtonText = "Cancel", DefaultButton = ContentDialogButton.Primary };
        var result = await dialog.ShowAsync();

        // Cancel/Esc must change nothing. This used to fall through to
        // SaveAsync on every outcome, so each cancel bumped Revision, rewrote
        // settings.json and pushed a "set" over the control pipe. Resolving
        // the outcome before touching GetModuleSettings also stops a cancel
        // from silently inserting a default entry for the module.
        if (result is not (ContentDialogResult.Primary or ContentDialogResult.Secondary)) return;

        var settings = GetModuleSettings(selectedModule.Id);
        if (result == ContentDialogResult.Secondary)
        {
            settings.Keybind = null;
        }
        else
        {
            if (!TryParseKey(input.Text, out var key))
            {
                ActionInfo.Title = "Key not recognised";
                ActionInfo.Message = "Enter a single letter or digit, or a virtual-key number from 1 to 255.";
                ActionInfo.Severity = InfoBarSeverity.Warning; ActionInfo.IsOpen = true; return;
            }
            if (key == moduleSettings.MenuKey || key == moduleSettings.UnloadKey)
            {
                ActionInfo.Title = "Key is reserved"; ActionInfo.Message = "Choose a key other than the menu or unload key."; ActionInfo.Severity = InfoBarSeverity.Warning; ActionInfo.IsOpen = true; return;
            }
            settings.Keybind = key;
        }
        if (!await SaveModuleSettingsAsync()) return;
        SelectModule(selectedModule);
    });

    private static bool TryParseKey(string value, out uint key)
    {
        value = value.Trim();
        if (value.Length == 1 && char.IsLetterOrDigit(value[0])) { key = char.ToUpperInvariant(value[0]); return true; }
        return uint.TryParse(value, out key) && key is > 0 and < 256;
    }

    // 0x30-0x5A is not a contiguous run of printable virtual keys: 0x3A-0x40
    // sits in the middle of it and has no VK assignment, so the old range test
    // rendered those as ":;<=>?@". Named keys (the 0xDC default for unload, for
    // instance) showed as a bare "VK 220"; the native ClickGUI already prints
    // real names via KeyName(), so the two surfaces disagreed.
    private static string FormatKey(uint key) => key switch
    {
        >= 0x30 and <= 0x39 => ((char)key).ToString(),
        >= 0x41 and <= 0x5A => ((char)key).ToString(),
        0x08 => "Backspace",
        0x09 => "Tab",
        0x0D => "Enter",
        0x10 => "Shift",
        0x11 => "Ctrl",
        0x12 => "Alt",
        0x14 => "Caps Lock",
        0x1B => "Esc",
        0x20 => "Space",
        0x21 => "Page Up",
        0x22 => "Page Down",
        0x23 => "End",
        0x24 => "Home",
        0x25 => "Left",
        0x26 => "Up",
        0x27 => "Right",
        0x28 => "Down",
        0x2D => "Insert",
        0x2E => "Delete",
        >= 0x60 and <= 0x69 => $"Numpad {key - 0x60}",
        >= 0x70 and <= 0x87 => $"F{key - 0x6F}",
        0xBA => ";",
        0xBB => "+",
        0xBC => ",",
        0xBD => "-",
        0xBE => ".",
        0xBF => "/",
        0xC0 => "`",
        0xDB => "[",
        0xDC => "\\",
        0xDD => "]",
        0xDE => "'",
        _ => $"VK {key}"
    };
    private static bool IsColor(string value) => value.Length == 9 && value[0] == '#' && value[1..].All(Uri.IsHexDigit);
    private double ReadModuleNumber(string key, double fallback) => moduleSettings.ModuleSettings.TryGetValue(key, out var value)
        && double.TryParse(value, System.Globalization.CultureInfo.InvariantCulture, out var parsed) ? parsed : fallback;

    private void UpdateControlKeyLabels()
    {
        MenuKeyButton.Content = $"Menu: {FormatKey(moduleSettings.MenuKey)}";
        UnloadKeyButton.Content = $"Unload: {FormatKey(moduleSettings.UnloadKey)}";
    }

    private void MenuKeyButton_Click(object sender, RoutedEventArgs e) => RunGuarded(() => ChangeControlKeyAsync(true));
    private void UnloadKeyButton_Click(object sender, RoutedEventArgs e) => RunGuarded(() => ChangeControlKeyAsync(false));

    private async Task ChangeControlKeyAsync(bool menu)
    {
        var input = new TextBox { PlaceholderText = "A-Z, 0-9, or virtual-key number", MaxLength = 5 };
        var dialog = new ContentDialog { XamlRoot = XamlRoot, Title = menu ? "Rebind menu" : "Rebind unload", Content = input, PrimaryButtonText = "Save", CloseButtonText = "Cancel", DefaultButton = ContentDialogButton.Primary };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary || !TryParseKey(input.Text, out var key)) return;
        var other = menu ? moduleSettings.UnloadKey : moduleSettings.MenuKey;
        if (key == other || moduleSettings.Modules.Values.Any(value => value.Keybind == key))
        {
            ActionInfo.Title = "Key is already reserved";
            ActionInfo.Message = "System keys must be distinct and cannot overlap a module keybind.";
            ActionInfo.Severity = InfoBarSeverity.Warning; ActionInfo.IsOpen = true; return;
        }
        if (menu) moduleSettings.MenuKey = key; else moduleSettings.UnloadKey = key;
        if (!await SaveModuleSettingsAsync()) return;
        UpdateControlKeyLabels();
    }

    private void PlayButton_Click(object sender, RoutedEventArgs e) => RunGuarded(PlayAsync);

    private async Task PlayAsync()
    {
        if (!bedrockInstallation.IsInstalled)
        {
            await RefreshBedrockStatusAsync();
            return;
        }

        if (clientUpdate?.Status is UpdateStatus.NotInstalled or UpdateStatus.UpdateAvailable)
        {
            await ConfirmAndInstallUpdateAsync(clientUpdate.Release!);
            return;
        }

        if (clientUpdate?.InstalledBuild is null)
        {
            await RefreshClientStatusAsync();
            return;
        }

        PlayButton.IsEnabled = false;
        // From here on the button is disabled, so restoring it has to happen in
        // a finally: an exception escaping this stretch used to leave Play
        // permanently dead for the rest of the session.
        var closingAfterLaunch = false;
        try
        {
            closingAfterLaunch = await LaunchAndInjectAsync();
        }
        finally
        {
            // Skipped when the window is closing - touching XAML then is
            // pointless and the controls may already be torn down.
            if (!closingAfterLaunch)
            {
                PlayButtonText.Text = "Launch Oinkster";
                PlayButton.IsEnabled = true;
            }
        }
    }

    // Returns true if the launcher window is being closed as a result (the
    // CloseAfterLaunch setting), in which case the caller must not touch the UI.
    private async Task<bool> LaunchAndInjectAsync()
    {
        var processId = bedrockService.TryGetRunningProcessId();
        var launched = processId is not null;
        if (!launched)
        {
            PlayButtonText.Text = "Starting Bedrock…";
            launched = await bedrockService.LaunchAsync();
        }

        if (!launched)
        {
            ActionInfo.Title = "Bedrock did not start";
            ActionInfo.Message = "Windows could not open the Minecraft protocol. Repair or reinstall Minecraft for Windows, then try again.";
            ActionInfo.Severity = InfoBarSeverity.Error;
            ActionInfo.IsOpen = true;
            return false;
        }

        PlayButtonText.Text = "Activating Oinkster…";
        processId ??= await bedrockService.WaitForProcessAsync(TimeSpan.FromSeconds(45));
        var existingStatus = await nativeStatusService.QueryAsync();
        var injection = existingStatus?.State == "ready" && existingStatus.HooksInstalled == true
            ? new InjectionResult(InjectionStatus.Success, $"Oinkster is already active. Press {FormatKey(moduleSettings.MenuKey)} in-game to open it.")
            : processId is null
                ? new InjectionResult(InjectionStatus.TargetExited, "Minecraft did not become ready in time.")
                : await clientInjector.InjectAsync(processId.Value, clientUpdate!.InstalledBuild!);
        NativeClientStatus? nativeStatus = existingStatus?.State == "ready" ? existingStatus : null;
        if (injection.Status == InjectionStatus.Success)
        {
            nativeStatus ??= await WaitForNativeReadyAsync();
            if (nativeStatus?.State != "ready" || nativeStatus.HooksInstalled != true)
            {
                injection = new InjectionResult(
                    InjectionStatus.SecurityValidationFailed,
                    "The DLL loaded, but its graphics adapter did not report ready.");
            }
        }
        // The native client reports the Bedrock build it was compiled against.
        // That value was deserialized and never looked at, even though it is
        // the one thing that can catch the launcher's recorded version and the
        // DLL's own supported version drifting apart (they are maintained in
        // two separate toolchains - see
        // DevelopmentClientBuildService.SupportedBedrockVersion).
        if (injection.Status == InjectionStatus.Success
            && nativeStatus?.SupportedBedrock is { Length: > 0 } reported
            && !reported.Equals(clientUpdate!.InstalledBuild!.Bedrock.ExactVersion, StringComparison.OrdinalIgnoreCase))
        {
            App.Log("bedrock_version_mismatch",
                $"launcher recorded {clientUpdate.InstalledBuild.Bedrock.ExactVersion}, client reported {reported}");
        }

        ActionInfo.Title = injection.Status == InjectionStatus.Success ? "Oinkster started" : "Oinkster did not start";
        ActionInfo.Message = injection.Status == InjectionStatus.Success
            ? $"{injection.Message} Press {FormatKey(moduleSettings.MenuKey)} while playing to open the ClickGUI."
            : injection.Message;
        ActionInfo.Severity = injection.Status != InjectionStatus.Success
            ? InfoBarSeverity.Error
            : InfoBarSeverity.Success;
        ActionInfo.IsOpen = true;

        if (injection.Status == InjectionStatus.Success && CloseAfterLaunchToggle.IsOn)
        {
            App.Window.Close();
            return true;
        }

        return false;
    }

    private async Task<NativeClientStatus?> WaitForNativeReadyAsync()
    {
        for (var attempt = 0; attempt < 48; attempt++)
        {
            var status = await nativeStatusService.QueryAsync();
            if (status is not null && string.Equals(status.State, "ready", StringComparison.OrdinalIgnoreCase))
            {
                return status;
            }

            await Task.Delay(250);
        }

        return null;
    }

    private async Task ConfirmAndInstallUpdateAsync(ClientRelease release)
    {
        var sizeInMegabytes = release.Manifest.Dll.Size / 1024d / 1024d;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Download Oinkster update?",
            Content = $"Version {release.Manifest.Version} · {release.Manifest.Channel} · {sizeInMegabytes:F1} MB\nBuilt for Bedrock {release.Manifest.Bedrock.ExactVersion}.",
            PrimaryButtonText = "Download",
            CloseButtonText = "Not now",
            DefaultButton = ContentDialogButton.Primary
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        PlayButton.IsEnabled = false;
        var progress = new Progress<DownloadProgress>(value =>
        {
            PlayButtonText.Text = $"Downloading {value.Percentage:F0}%";
        });

        try
        {
            PlayButtonText.Text = "Downloading 0%";
            var installed = await updateService.InstallAsync(release, progress);
            ClientBuildText.Text = $"Build {installed.Version}";
            ActionInfo.Title = "Oinkster updated";
            ActionInfo.Message = $"Version {installed.Version} was downloaded and verified.";
            ActionInfo.Severity = InfoBarSeverity.Success;
            ActionInfo.IsOpen = true;
            await RefreshClientStatusAsync();
        }
        // TaskCanceledException covers the download exceeding HttpClient's
        // timeout (which applies to reading the response body, not just the
        // headers), and InvalidDataException is what the update service throws
        // on a size or hash mismatch. Neither was caught, so either one
        // escaped this async void handler and killed the process mid-update.
        catch (Exception exception) when (exception is HttpRequestException or IOException
            or UnauthorizedAccessException or InvalidDataException or TaskCanceledException)
        {
            ActionInfo.Title = "Update failed";
            ActionInfo.Message = exception is TaskCanceledException
                ? "The download timed out. Check your connection and try again."
                : exception.Message;
            ActionInfo.Severity = InfoBarSeverity.Error;
            ActionInfo.IsOpen = true;
            await RefreshClientStatusAsync();
        }
    }

    private void Setting_Changed(object sender, object e)
    {
        if (!settingsLoaded || UpdateChannelComboBox.SelectedItem is not ComboBoxItem selectedChannel)
        {
            return;
        }

        settingsService.Save(new LauncherSettings(
            selectedChannel.Content?.ToString() ?? "Release",
            CheckForUpdatesToggle.IsOn,
            CloseAfterLaunchToggle.IsOn));

        if (ReferenceEquals(sender, UpdateChannelComboBox) && bedrockInstallation.IsInstalled)
        {
            _ = RefreshClientStatusAsync();
        }
    }

    private void CheckForUpdatesButton_Click(object sender, RoutedEventArgs e) => RunGuarded(async () =>
    {
        if (!bedrockInstallation.IsInstalled)
        {
            await RefreshBedrockStatusAsync();
        }

        if (bedrockInstallation.IsInstalled)
        {
            await RefreshClientStatusAsync();
        }
    });

    private void OpenFolderButton_Click(object sender, RoutedEventArgs e) => RunGuarded(async () =>
    {
        Directory.CreateDirectory(moduleSettingsService.SettingsDirectory);
        var folder = await StorageFolder.GetFolderFromPathAsync(moduleSettingsService.SettingsDirectory);
        var opened = await Launcher.LaunchFolderAsync(folder);
        if (!opened)
        {
            ShowError("Folder could not be opened", "Open the Oinkster local application data folder manually.");
        }
    });

    private void PreviewAction_Click(object sender, RoutedEventArgs e)
    {
        ActionInfo.Title = "Coming soon";
        ActionInfo.Message = "This control is ready for its client integration.";
        ActionInfo.Severity = InfoBarSeverity.Informational;
        ActionInfo.IsOpen = true;
    }
}
