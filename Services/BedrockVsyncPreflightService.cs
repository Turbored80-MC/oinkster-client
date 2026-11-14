using System.Text;

namespace OinksterClient.Services;

/// <summary>
/// Temporarily starts a launcher-created Bedrock session with gfx_vsync off so
/// its first flip-model swap chain can legally opt into tearing. The player's
/// original options file is restored after injection completes.
/// </summary>
public sealed class BedrockVsyncPreflightService
{
    private const int MaximumOptionsBytes = 2 * 1024 * 1024;
    private const string DisableVsyncSetting = "click-gui.disable-vsync";
    private readonly IReadOnlyList<string> gameRoots;

    public BedrockVsyncPreflightService(IReadOnlyList<string>? gameRoots = null)
    {
        this.gameRoots = gameRoots ?? DefaultGameRoots();
    }

    public IDisposable Prepare()
    {
        try
        {
            var path = gameRoots.Select(root => Path.Combine(root, "minecraftpe", "options.txt"))
                .Where(File.Exists)
                .OrderByDescending(File.GetLastWriteTimeUtc)
                .FirstOrDefault();
            if (path is null) return EmptyLease.Instance;
            var original = File.ReadAllBytes(path);
            if (original.Length == 0 || original.Length > MaximumOptionsBytes) return EmptyLease.Instance;
            var hasBom = original.AsSpan().StartsWith(Encoding.UTF8.Preamble);
            var offset = hasBom ? Encoding.UTF8.Preamble.Length : 0;
            var text = Encoding.UTF8.GetString(original, offset, original.Length - offset);
            var updated = SetOption(text, "gfx_vsync", "0");
            if (updated == text) return EmptyLease.Instance;
            WriteAtomically(path, Encode(updated, hasBom));
            return new RestoreLease(path, original);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or DecoderFallbackException)
        {
            return EmptyLease.Instance;
        }
    }

    internal static bool IsRequested(IReadOnlyDictionary<string, string> settings) =>
        settings.TryGetValue(DisableVsyncSetting, out var value)
        && (value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase));

    internal static string SetOption(string text, string key, string value)
    {
        var prefix = key + ":";
        for (var start = 0; start <= text.Length;)
        {
            var end = text.IndexOf('\n', start);
            if (end < 0) end = text.Length;
            var contentEnd = end > start && text[end - 1] == '\r' ? end - 1 : end;
            if (contentEnd - start >= prefix.Length
                && text.AsSpan(start, prefix.Length).SequenceEqual(prefix.AsSpan()))
            {
                return string.Concat(text.AsSpan(0, start + prefix.Length), value,
                    text.AsSpan(contentEnd));
            }
            if (end == text.Length) break;
            start = end + 1;
        }
        return text;
    }

    private static byte[] Encode(string text, bool bom)
    {
        var content = Encoding.UTF8.GetBytes(text);
        if (!bom) return content;
        var result = new byte[Encoding.UTF8.Preamble.Length + content.Length];
        Encoding.UTF8.GetPreamble().CopyTo(result, 0);
        content.CopyTo(result, Encoding.UTF8.Preamble.Length);
        return result;
    }

    private static void WriteAtomically(string path, byte[] content)
    {
        var temporary = path + ".oinkster.partial";
        File.WriteAllBytes(temporary, content);
        File.Move(temporary, path, true);
    }

    private static IReadOnlyList<string> DefaultGameRoots()
    {
        var roots = new List<string>();
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var users = Path.Combine(roaming, "Minecraft Bedrock", "Users");
        try
        {
            if (Directory.Exists(users))
            {
                roots.AddRange(Directory.EnumerateDirectories(users)
                    .Select(user => Path.Combine(user, "games", "com.mojang")));
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        roots.Add(Path.Combine(local, "Packages", "Microsoft.MinecraftUWP_8wekyb3d8bbwe", "LocalState", "games", "com.mojang"));
        return roots;
    }

    private sealed class RestoreLease(string path, byte[] original) : IDisposable
    {
        private byte[]? originalBytes = original;

        public void Dispose()
        {
            var bytes = Interlocked.Exchange(ref originalBytes, null);
            if (bytes is null) return;
            try { WriteAtomically(path, bytes); }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        }
    }

    private sealed class EmptyLease : IDisposable
    {
        public static EmptyLease Instance { get; } = new();
        public void Dispose() { }
    }
}
