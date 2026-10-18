using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using OinksterClient.Models;
using OinksterClient.Services;
using Windows.Storage;
using Windows.System;

namespace OinksterClient;

public sealed partial class MainPage : Page
{
    private enum KeyCaptureAction { Cancelled, Captured, Cleared }
    private readonly record struct KeyCaptureResult(KeyCaptureAction Action, uint Key = 0);

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
        AppVersionText.Text = $"Oinkster {AppVersion.Display}";
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
            && DictionaryEqual(left.CapabilityReasons, right.CapabilityReasons)
            && left.ServerNetwork == right.ServerNetwork
            && DictionaryEqual(left.CompetitivePolicies, right.CompetitivePolicies);
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
        var policyState = CompetitivePolicyState(definition);
        moduleUiLoading = true;
        InspectorCategoryText.Text = !definition.Available ? $"Roadmap · {definition.Category}"
            : runtimeIssue is not null ? $"Waiting · {definition.Category}" : definition.Category;
        InspectorNameText.Text = definition.Name;
        InspectorDescriptionText.Text = definition.Description;
        ModuleEnabledToggle.IsEnabled = definition.Available && policyState != "blocked";
        ModuleEnabledToggle.IsOn = definition.Available && settings.Enabled;
        ModuleControlsPanel.Visibility = definition.Available ? Visibility.Visible : Visibility.Collapsed;
        var policySummary = CompetitivePolicySummary(definition, policyState);
        InspectorRoadmapText.Visibility = definition.Available && runtimeIssue is null && policySummary is null
            ? Visibility.Collapsed : Visibility.Visible;
        InspectorRoadmapText.Text = runtimeIssue ?? policySummary ?? definition.UnavailableReason
            ?? "This module needs additional validated game integration.";
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
        HudAppearancePanel.Visibility = definition.Hud ? Visibility.Visible : Visibility.Collapsed;
        var isCrosshair = definition.Id == CrosshairSettings.ModuleId;
        CrosshairControlsPanel.Visibility = isCrosshair ? Visibility.Visible : Visibility.Collapsed;
        if (isCrosshair) LoadCrosshairControls();
        moduleUiLoading = false;
    }

    private string? CompetitivePolicyState(ModuleDefinition definition)
    {
        if (definition.Id is not ("dc-prevent" or "cps-cap")) return null;
        return moduleNativeStatus?.CompetitivePolicies?.TryGetValue(definition.Id, out var state) == true ? state : null;
    }

    private string? CompetitivePolicySummary(ModuleDefinition definition, string? state)
    {
        if (definition.Id is not ("dc-prevent" or "cps-cap") || state is null) return null;
        var network = moduleNativeStatus?.ServerNetwork ?? "unknown-remote";
        var label = network switch
        {
            "local" => "local or single-player sessions",
            "unknown-remote" => "an unrecognized remote server",
            _ => network
        };
        return $"Server policy: {state} on {label}. Rules last reviewed October 16, 2026.";
    }

    private void ModuleSetting_Changed(object sender, object e) => RunGuarded(async () =>
    {
        if (moduleUiLoading || selectedModule is null || !selectedModule.Available) return;
        var settings = GetModuleSettings(selectedModule.Id);
        var isCrosshair = selectedModule.Id == CrosshairSettings.ModuleId;
        var stored = moduleSettings.ModuleSettings;

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
                break;
            case var _ when ReferenceEquals(sender, ModuleAccentColorBox):
                if (!TryApplyColor(ModuleAccentColorBox, value => settings.Hud.AccentColor = value)) return;
                break;
            case var _ when ReferenceEquals(sender, CrosshairSourceComboBox) && isCrosshair:
                if (SelectedTag(CrosshairSourceComboBox) is not { } source) return;
                stored[CrosshairSettings.SourceKey] = source;
                UpdateCrosshairSourceVisibility(source);
                break;
            case var _ when ReferenceEquals(sender, CrosshairPerspectiveComboBox) && isCrosshair:
                if (SelectedTag(CrosshairPerspectiveComboBox) is not { } perspective) return;
                stored[CrosshairSettings.PerspectiveKey] = perspective;
                break;
            case var _ when ReferenceEquals(sender, CrosshairColorBox) && isCrosshair:
                if (CrosshairSettings.NormalizeColor(CrosshairColorBox.Text) is not { } color)
                {
                    ShowError("Invalid color", "Use #AARRGGBB or #RRGGBB, for example #FFFFFFFF.");
                    return;
                }
                ActionInfo.IsOpen = false;
                CrosshairColorBox.Text = color;
                stored[CrosshairSettings.ColorKey] = color;
                break;
            case var _ when isCrosshair && CrosshairNumberFor(sender) is { } number:
                stored[number.Setting.Key] = CrosshairSettings.FormatNumber(number.Slider.Value);
                break;
            case var _ when ReferenceEquals(sender, CrosshairOutlineToggle) && isCrosshair:
                stored[CrosshairSettings.Outline.Key] = CrosshairOutlineToggle.IsOn ? "true" : "false";
                break;
            case var _ when ReferenceEquals(sender, CrosshairCenterDotToggle) && isCrosshair:
                stored[CrosshairSettings.CenterDot.Key] = CrosshairCenterDotToggle.IsOn ? "true" : "false";
                break;
            case var _ when ReferenceEquals(sender, CrosshairThirdPersonToggle) && isCrosshair:
                stored[CrosshairSettings.ThirdPerson.Key] = CrosshairThirdPersonToggle.IsOn ? "true" : "false";
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
        var moduleId = selectedModule.Id;
        var moduleName = selectedModule.Name;
        var currentKey = GetModuleSettings(moduleId).Keybind;
        var result = await CaptureKeyAsync($"Bind {moduleName}", currentKey, true, key =>
        {
            if (key == moduleSettings.MenuKey || key == moduleSettings.UnloadKey)
            {
                return "That key opens or unloads Oinkster. Press a different key.";
            }

            var conflict = moduleSettings.Modules.FirstOrDefault(pair =>
                pair.Key != moduleId && pair.Value.Keybind == key);
            if (conflict.Key is not null)
            {
                var conflictName = moduleCatalog.FirstOrDefault(item => item.Id == conflict.Key)?.Name ?? conflict.Key;
                return $"{FormatKey(key)} is already assigned to {conflictName}.";
            }

            return null;
        });
        if (result.Action == KeyCaptureAction.Cancelled) return;

        var settings = GetModuleSettings(moduleId);
        settings.Keybind = result.Action == KeyCaptureAction.Cleared ? null : result.Key;
        if (!await SaveModuleSettingsAsync()) return;
        SelectModule(moduleCatalog.FirstOrDefault(item => item.Id == moduleId));
    });

    private async Task<KeyCaptureResult> CaptureKeyAsync(
        string title,
        uint? currentKey,
        bool allowClear,
        Func<uint, string?> validate)
    {
        var promptTitle = new TextBlock
        {
            Text = "Press a key",
            HorizontalAlignment = HorizontalAlignment.Center,
            Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 247, 242, 245)),
            FontSize = 18,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
        };
        var promptDetail = new TextBlock
        {
            Text = currentKey is null ? "No key is assigned" : $"Currently {FormatKey(currentKey.Value)}",
            HorizontalAlignment = HorizontalAlignment.Center,
            Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 155, 148, 156)),
            FontSize = 12,
            TextWrapping = TextWrapping.Wrap
        };
        var prompt = new Button
        {
            MinHeight = 112,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            Padding = new Thickness(22),
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 41, 35, 42)),
            BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 159, 194)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12),
            Content = new StackPanel
            {
                Spacing = 6,
                Children = { promptTitle, promptDetail }
            }
        };
        var instruction = new TextBlock
        {
            Text = "Keyboard keys only. Escape cancels.",
            Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 119, 113, 121)),
            FontSize = 12,
            TextWrapping = TextWrapping.Wrap
        };
        var content = new StackPanel { Spacing = 12, Children = { prompt, instruction } };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = content,
            PrimaryButtonText = allowClear ? "Clear binding" : string.Empty,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.None
        };

        uint? capturedKey = null;
        void Prompt_KeyDown(object sender, KeyRoutedEventArgs args)
        {
            args.Handled = true;
            if (args.Key == VirtualKey.Escape)
            {
                dialog.Hide();
                return;
            }

            var key = (uint)args.Key;
            var issue = validate(key);
            if (issue is not null)
            {
                promptTitle.Text = "Choose another key";
                promptDetail.Text = issue;
                promptDetail.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 224, 173, 98));
                return;
            }

            capturedKey = key;
            dialog.Hide();
        }

        void Dialog_Opened(ContentDialog sender, ContentDialogOpenedEventArgs args) =>
            prompt.Focus(FocusState.Programmatic);

        prompt.KeyDown += Prompt_KeyDown;
        dialog.Opened += Dialog_Opened;
        var dialogResult = await dialog.ShowAsync();
        prompt.KeyDown -= Prompt_KeyDown;
        dialog.Opened -= Dialog_Opened;

        if (capturedKey is uint key) return new KeyCaptureResult(KeyCaptureAction.Captured, key);
        if (allowClear && dialogResult == ContentDialogResult.Primary) return new KeyCaptureResult(KeyCaptureAction.Cleared);
        return new KeyCaptureResult(KeyCaptureAction.Cancelled);
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

    // Each slider and the native setting it writes. CrosshairSettings holds
    // the same defaults and ranges as the native descriptors.
    private IEnumerable<(Slider Slider, CrosshairSettings.NumberSetting Setting)> CrosshairNumbers()
    {
        yield return (CrosshairSizeSlider, CrosshairSettings.Size);
        yield return (CrosshairGapSlider, CrosshairSettings.Gap);
        yield return (CrosshairThicknessSlider, CrosshairSettings.Thickness);
        yield return (CrosshairOpacitySlider, CrosshairSettings.Opacity);
        yield return (CrosshairOutlineThicknessSlider, CrosshairSettings.OutlineThickness);
        yield return (CrosshairDotSizeSlider, CrosshairSettings.DotSize);
        yield return (CrosshairPixelScaleSlider, CrosshairSettings.PixelScale);
        yield return (CrosshairGameScaleSlider, CrosshairSettings.GameScale);
    }

    private (Slider Slider, CrosshairSettings.NumberSetting Setting)? CrosshairNumberFor(object sender)
    {
        foreach (var number in CrosshairNumbers())
        {
            if (ReferenceEquals(number.Slider, sender)) return number;
        }
        return null;
    }

    private void LoadCrosshairControls()
    {
        var stored = moduleSettings.ModuleSettings;
        var source = CrosshairSettings.ReadSource(stored);
        SelectByTag(CrosshairSourceComboBox, source);
        SelectByTag(CrosshairPerspectiveComboBox, CrosshairSettings.ReadPerspective(stored));
        foreach (var (slider, setting) in CrosshairNumbers()) slider.Value = CrosshairSettings.ReadNumber(stored, setting);
        CrosshairColorBox.Text = CrosshairSettings.ReadColor(stored);
        CrosshairOutlineToggle.IsOn = CrosshairSettings.ReadBool(stored, CrosshairSettings.Outline);
        CrosshairCenterDotToggle.IsOn = CrosshairSettings.ReadBool(stored, CrosshairSettings.CenterDot);
        CrosshairThirdPersonToggle.IsOn = CrosshairSettings.ReadBool(stored, CrosshairSettings.ThirdPerson);
        UpdateCrosshairSourceVisibility(source);
    }

    private void UpdateCrosshairSourceVisibility(string source)
    {
        static Visibility VisibleWhen(bool shown) => shown ? Visibility.Visible : Visibility.Collapsed;
        var game = source == CrosshairSettings.SourceGame;
        var drawn = source == CrosshairSettings.SourceDrawn;
        CrosshairGamePanel.Visibility = VisibleWhen(game);
        CrosshairOwnPanel.Visibility = VisibleWhen(!game);
        CrosshairDrawnPanel.Visibility = VisibleWhen(drawn);
        CrosshairLinePanel.Visibility = VisibleWhen(!drawn);
        CrosshairDotPanel.Visibility = VisibleWhen(!drawn);
        CrosshairSourceHelpText.Text = source switch
        {
            CrosshairSettings.SourceGame =>
                "The game's own crosshair (or your texture pack's) stays as it is. Bedrock hides it in 3rd person; Oinkster can redraw it there.",
            CrosshairSettings.SourceDrawn =>
                "Draw it pixel by pixel in game: open the menu, then Custom crosshair. Oinkster draws it on top of the game's own.",
            _ => "Oinkster draws this crosshair on top of the game's own."
        };
    }

    private static string? SelectedTag(ComboBox box) => (box.SelectedItem as ComboBoxItem)?.Tag as string;

    private static void SelectByTag(ComboBox box, string tag) =>
        box.SelectedItem = box.Items.OfType<ComboBoxItem>().FirstOrDefault(item => item.Tag as string == tag) ?? box.Items[0];

    private void UpdateControlKeyLabels()
    {
        MenuKeyButton.Content = $"Menu: {FormatKey(moduleSettings.MenuKey)}";
        UnloadKeyButton.Content = $"Unload: {FormatKey(moduleSettings.UnloadKey)}";
    }

    private void MenuKeyButton_Click(object sender, RoutedEventArgs e) => RunGuarded(() => ChangeControlKeyAsync(true));
    private void UnloadKeyButton_Click(object sender, RoutedEventArgs e) => RunGuarded(() => ChangeControlKeyAsync(false));

    private async Task ChangeControlKeyAsync(bool menu)
    {
        var current = menu ? moduleSettings.MenuKey : moduleSettings.UnloadKey;
        var result = await CaptureKeyAsync(menu ? "Rebind menu" : "Rebind unload", current, false, key =>
        {
            var other = menu ? moduleSettings.UnloadKey : moduleSettings.MenuKey;
            if (key == other) return "The menu and unload actions need different keys.";

            var conflict = moduleSettings.Modules.FirstOrDefault(pair => pair.Value.Keybind == key);
            if (conflict.Key is null) return null;
            var conflictName = moduleCatalog.FirstOrDefault(item => item.Id == conflict.Key)?.Name ?? conflict.Key;
            return $"{FormatKey(key)} is already assigned to {conflictName}.";
        });
        if (result.Action != KeyCaptureAction.Captured) return;

        var key = result.Key;
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
            // A running game keeps the client it already loaded until restart.
            ActionInfo.Message = bedrockService.TryGetRunningProcessId() is null
                ? $"Version {installed.Version} was downloaded and verified."
                : $"Version {installed.Version} was downloaded and verified. Restart Minecraft to load it.";
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
