using System.Text.Json;
using System.IO.Pipes;
using System.Text;
using OinksterClient.Models;

namespace OinksterClient.Services;

public sealed class ModuleSettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private readonly bool synchronizeWithRuntime;

    public ModuleSettingsService(string? settingsDirectory = null)
    {
        synchronizeWithRuntime = settingsDirectory is null;
        SettingsDirectory = settingsDirectory ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Oinkster", "Client");
    }

    public string SettingsDirectory { get; }

    public string SettingsPath => Path.Combine(SettingsDirectory, "settings.json");

    public async Task<ModuleSettingsDocument> LoadAsync()
    {
        var runtimeJson = synchronizeWithRuntime ? await QueryRuntimeAsync() : null;
        if (!string.IsNullOrWhiteSpace(runtimeJson))
        {
            try
            {
                var live = JsonSerializer.Deserialize<ModuleSettingsDocument>(runtimeJson, JsonOptions);
                if (live?.SchemaVersion == 2) return live;
            }
            catch (JsonException) { }
        }
        if (!File.Exists(SettingsPath))
        {
            return new ModuleSettingsDocument();
        }

        try
        {
            await using var stream = File.OpenRead(SettingsPath);
            using var json = await JsonDocument.ParseAsync(stream);
            var version = json.RootElement.TryGetProperty("schemaVersion", out var schema) ? schema.GetInt32() : 0;
            if (version == 2)
            {
                return json.RootElement.Deserialize<ModuleSettingsDocument>(JsonOptions) ?? new ModuleSettingsDocument();
            }

            if (version == 1)
            {
                return MigrateVersionOne(json.RootElement);
            }

            throw new JsonException("Unsupported module settings schema.");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            var corruptPath = $"{SettingsPath}.corrupt.{DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()}";
            // UnauthorizedAccessException escaped this quarantine handler and
            // propagated out of LoadAsync, defeating the point of the catch.
            try { File.Move(SettingsPath, corruptPath, false); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return new ModuleSettingsDocument();
        }
    }

    /// <summary>
    /// Persists the settings and pushes them to a running client. Returns false
    /// if they could not be written to disk, so callers can report it; the
    /// revision is rolled back in that case so the in-memory document still
    /// matches what is actually stored.
    /// </summary>
    public async Task<bool> SaveAsync(ModuleSettingsDocument settings)
    {
        settings.SchemaVersion = 2;
        settings.Revision++;
        var temporaryPath = SettingsPath + ".partial";
        var json = JsonSerializer.Serialize(settings, JsonOptions);

        try
        {
            Directory.CreateDirectory(SettingsDirectory);
            await File.WriteAllTextAsync(temporaryPath, json);
            File.Move(temporaryPath, SettingsPath, true);
        }
        // Previously unhandled, and every caller is an async void event
        // handler - so a locked or read-only settings file killed the launcher.
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            settings.Revision--;
            try { if (File.Exists(temporaryPath)) File.Delete(temporaryPath); } catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return false;
        }

        if (synchronizeWithRuntime) await SendRuntimeAsync("set\n" + json);
        return true;
    }

    private static async Task<string?> SendRuntimeAsync(string request)
    {
        try
        {
            await using var pipe = new NamedPipeClientStream(".", @"LOCAL\Oinkster.Client.Control.v1", PipeDirection.InOut, PipeOptions.Asynchronous);
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
            await pipe.ConnectAsync(timeout.Token);
            await pipe.WriteAsync(Encoding.UTF8.GetBytes(request), timeout.Token);
            await pipe.FlushAsync(timeout.Token);
            using var reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
            return await reader.ReadToEndAsync(timeout.Token);
        }
        catch (Exception exception) when (exception is IOException or OperationCanceledException or UnauthorizedAccessException) { return null; }
    }

    private static Task<string?> QueryRuntimeAsync() => SendRuntimeAsync("get");

    /// <summary>
    /// Supported schema version of Assets/module-catalog.json.
    /// </summary>
    public const int CatalogSchemaVersion = 1;

    /// <summary>
    /// Thrown when the module catalog asset is missing or unreadable. The
    /// catalog ships with the app, so this is a packaging fault rather than
    /// something the user can fix - but it must surface as an error in the UI
    /// rather than an unhandled exception out of an async void handler.
    /// </summary>
    public sealed class CatalogUnavailableException(string message, Exception? inner = null)
        : Exception(message, inner);

    public static async Task<IReadOnlyList<ModuleDefinition>> LoadCatalogAsync()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Assets", "module-catalog.json");
        if (!File.Exists(path))
        {
            throw new CatalogUnavailableException(
                "The module catalog (Assets/module-catalog.json) is missing from this installation.");
        }

        ModuleCatalogDocument? catalog;
        try
        {
            await using var stream = File.OpenRead(path);
            catalog = await JsonSerializer.DeserializeAsync<ModuleCatalogDocument>(stream, JsonOptions);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            throw new CatalogUnavailableException("The module catalog could not be read.", exception);
        }

        if (catalog is null)
        {
            throw new CatalogUnavailableException("The module catalog is empty.");
        }

        // The version was parsed and then ignored, so a future catalog format
        // would have been silently misread as the current one.
        if (catalog.SchemaVersion != CatalogSchemaVersion)
        {
            throw new CatalogUnavailableException(
                $"The module catalog uses schema version {catalog.SchemaVersion}, but this launcher supports version {CatalogSchemaVersion}. Update Oinkster.");
        }

        return catalog.Modules ?? [];
    }

    private static ModuleSettingsDocument MigrateVersionOne(JsonElement root)
    {
        var migrated = new ModuleSettingsDocument();
        if (root.TryGetProperty("clickGuiKey", out var menuKey)) migrated.MenuKey = menuKey.GetUInt32();
        if (root.TryGetProperty("modules", out var modules))
        {
            foreach (var property in modules.EnumerateObject())
            {
                migrated.Modules[property.Name] = new ModuleClientSettings { Enabled = property.Value.GetBoolean() };
            }
        }
        if (root.TryGetProperty("keybinds", out var keybinds))
        {
            foreach (var property in keybinds.EnumerateObject())
            {
                var id = property.Name.StartsWith("bind.", StringComparison.Ordinal) ? property.Name[5..] : property.Name;
                if (!migrated.Modules.TryGetValue(id, out var module)) migrated.Modules[id] = module = new();
                module.Keybind = property.Value.GetUInt32();
            }
        }
        if (root.TryGetProperty("layout", out var layouts))
        {
            foreach (var property in layouts.EnumerateObject())
            {
                var id = property.Name.StartsWith("layout.", StringComparison.Ordinal) ? property.Name[7..] : property.Name;
                if (!migrated.Modules.TryGetValue(id, out var module)) migrated.Modules[id] = module = new();
                var parts = property.Value.GetString()?.Split(',');
                if (parts is { Length: >= 3 }
                    && float.TryParse(parts[0], System.Globalization.CultureInfo.InvariantCulture, out var x)
                    && float.TryParse(parts[1], System.Globalization.CultureInfo.InvariantCulture, out var y)
                    && float.TryParse(parts[2], System.Globalization.CultureInfo.InvariantCulture, out var scale))
                {
                    module.Hud.X = x; module.Hud.Y = y; module.Hud.Scale = scale;
                    module.Hud.Background = parts.Length < 4 || parts[3] != "0";
                }
            }
        }
        if (root.TryGetProperty("moduleSettings", out var options))
        {
            foreach (var property in options.EnumerateObject())
            {
                var key = property.Name.StartsWith("setting.", StringComparison.Ordinal) ? property.Name[8..] : property.Name;
                migrated.ModuleSettings[key] = property.Value.GetString() ?? string.Empty;
            }
        }
        return migrated;
    }
}
