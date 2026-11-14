using System.Globalization;

namespace OinksterClient.Models;

/// <summary>
/// Launcher mirror of the native custom-crosshair settings (see the settings
/// table in Native/README.md). Values live as strings in
/// <see cref="ModuleSettingsDocument.ModuleSettings"/>; the native
/// ModuleManager normalizes them again on load, so these readers only decide
/// what the launcher shows for a missing, older or hand-edited entry - and
/// they fall back exactly the way the native descriptors do.
/// </summary>
public static class CrosshairSettings
{
    public const string ModuleId = "custom-crosshair";

    public const string SourceKey = ModuleId + ".source";
    public const string SourceCustom = "custom";
    public const string SourceDrawn = "drawn";
    public const string SourceGame = "game";

    /// <summary>The drawn 16x16 grid; it is edited in game (ClickGUI), never by the launcher.</summary>
    public const string PixelsKey = ModuleId + ".pixels";

    public const string PerspectiveKey = ModuleId + ".perspective";
    public const string PerspectiveFirstPerson = "first-person";
    public const string PerspectiveAll = "all";

    public const string ColorKey = ModuleId + ".color";
    public const string DefaultColor = "#FFFFFFFF";

    public sealed record NumberSetting(string Key, double Default, double Minimum, double Maximum);
    public sealed record BoolSetting(string Key, bool Default);

    public static readonly NumberSetting Size = new(ModuleId + ".size", 6, 0, 24);
    public static readonly NumberSetting Gap = new(ModuleId + ".gap", 3, 0, 16);
    public static readonly NumberSetting Thickness = new(ModuleId + ".thickness", 2, 1, 6);
    public static readonly NumberSetting Opacity = new(ModuleId + ".opacity", 1, 0.1, 1);
    public static readonly NumberSetting OutlineThickness = new(ModuleId + ".outline-thickness", 1, 0.5, 3);
    public static readonly NumberSetting DotSize = new(ModuleId + ".dot-size", 1.5, 0.5, 6);
    public static readonly NumberSetting PixelScale = new(ModuleId + ".pixel-scale", 2, 1, 8);
    public static readonly NumberSetting GameScale = new(ModuleId + ".game-scale", 2, 1, 8);
    public static readonly BoolSetting Outline = new(ModuleId + ".outline", true);
    public static readonly BoolSetting CenterDot = new(ModuleId + ".center-dot", false);
    public static readonly BoolSetting ThirdPerson = new(ModuleId + ".third-person", true);

    /// <summary>"game" and "drawn" as stored; anything else - including no value at all - is the custom crosshair.</summary>
    public static string ReadSource(IReadOnlyDictionary<string, string> settings) =>
        settings.TryGetValue(SourceKey, out var value) && value is SourceGame or SourceDrawn ? value : SourceCustom;

    public static string ReadPerspective(IReadOnlyDictionary<string, string> settings) =>
        settings.TryGetValue(PerspectiveKey, out var value) && value == PerspectiveAll ? PerspectiveAll : PerspectiveFirstPerson;

    public static double ReadNumber(IReadOnlyDictionary<string, string> settings, NumberSetting setting) =>
        settings.TryGetValue(setting.Key, out var value)
        && double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) && double.IsFinite(parsed)
            ? Math.Clamp(parsed, setting.Minimum, setting.Maximum)
            : setting.Default;

    public static bool ReadBool(IReadOnlyDictionary<string, string> settings, BoolSetting setting)
    {
        if (!settings.TryGetValue(setting.Key, out var value)) return setting.Default;
        return value.Trim().ToLowerInvariant() switch
        {
            "true" or "1" or "yes" or "on" => true,
            "false" or "0" or "no" or "off" => false,
            _ => setting.Default
        };
    }

    /// <summary>"#AARRGGBB" as stored; "#RRGGBB" reads as opaque; anything else is the default.</summary>
    public static string ReadColor(IReadOnlyDictionary<string, string> settings) =>
        settings.TryGetValue(ColorKey, out var value) && NormalizeColor(value) is { } color ? color : DefaultColor;

    public static string? NormalizeColor(string? value)
    {
        if (value is null) return null;
        var text = value.Trim();
        if (text.Length < 1 || text[0] != '#' || !text[1..].All(Uri.IsHexDigit)) return null;
        return text.Length switch
        {
            9 => text.ToUpperInvariant(),
            7 => ("#FF" + text[1..]).ToUpperInvariant(),
            _ => null
        };
    }

    public static string FormatNumber(double value) => value.ToString("0.###", CultureInfo.InvariantCulture);
}
