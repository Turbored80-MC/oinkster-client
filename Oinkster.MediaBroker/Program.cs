using System.Diagnostics;
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using Windows.Media.Control;

namespace Oinkster.MediaBroker;

internal static class Program
{
    private const string PipeName = @"LOCAL\Oinkster.Media.v1";
    private const int MaximumRequestBytes = 4096;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public static async Task<int> Main(string[] args)
    {
        if (!TryReadMinecraftProcess(args, out var minecraft)) return 2;
        using (minecraft)
        using (var singleton = new Mutex(true, @"Local\Oinkster.MediaBroker.v1", out var ownsMutex))
        {
            if (!ownsMutex) return 0;

            var provider = await WindowsMediaSessionProvider.CreateAsync();
            while (!minecraft.HasExited)
            {
                await ServeOnceAsync(provider, minecraft);
            }
        }

        return 0;
    }

    private static bool TryReadMinecraftProcess(string[] args, out Process process)
    {
        process = null!;
        if (args.Length != 2 || args[0] != "--minecraft-pid" || !int.TryParse(args[1], out var processId)) return false;
        try
        {
            process = Process.GetProcessById(processId);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    private static async Task ServeOnceAsync(IMediaSessionProvider provider, Process minecraft)
    {
        var pipeSecurity = new PipeSecurity();
        var currentUser = WindowsIdentity.GetCurrent().User
            ?? throw new InvalidOperationException("The current Windows user has no security identifier.");
        pipeSecurity.AddAccessRule(new PipeAccessRule(
            currentUser,
            PipeAccessRights.FullControl,
            AccessControlType.Allow));
        pipeSecurity.AddAccessRule(new PipeAccessRule(
            new SecurityIdentifier("S-1-15-2-1"), // ALL APPLICATION PACKAGES
            PipeAccessRights.ReadWrite,
            AccessControlType.Allow));

        await using var pipe = NamedPipeServerStreamAcl.Create(
            PipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Message,
            PipeOptions.Asynchronous,
            MaximumRequestBytes,
            16 * 1024,
            pipeSecurity);

        using var acceptTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        try
        {
            await pipe.WaitForConnectionAsync(acceptTimeout.Token);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        using var exchangeTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        try
        {
            var requestText = await ReadBoundedLineAsync(pipe, exchangeTimeout.Token);
            var response = await MediaRequestProcessor.HandleAsync(requestText, provider, exchangeTimeout.Token);

            var json = JsonSerializer.Serialize(response, JsonOptions) + "\n";
            var bytes = Encoding.UTF8.GetBytes(json);
            await pipe.WriteAsync(bytes, exchangeTimeout.Token);
            await pipe.FlushAsync(exchangeTimeout.Token);
        }
        catch (Exception exception) when (exception is IOException or JsonException or OperationCanceledException)
        {
            // The injected client may unload between connect and response.
        }
    }

    private static async Task<string?> ReadBoundedLineAsync(Stream stream, CancellationToken cancellationToken)
    {
        var bytes = new List<byte>(256);
        var one = new byte[1];
        while (bytes.Count < MaximumRequestBytes)
        {
            var read = await stream.ReadAsync(one, cancellationToken);
            if (read == 0 || one[0] == (byte)'\n') break;
            bytes.Add(one[0]);
        }
        return bytes.Count == MaximumRequestBytes ? null : Encoding.UTF8.GetString(bytes.ToArray());
    }
}

internal static class MediaProtocol
{
    public const int Version = 1;
}

internal sealed record MediaRequest(int ProtocolVersion, string Command);

internal static class MediaRequestProcessor
{
    public static async Task<MediaStatus> HandleAsync(
        string? requestText,
        IMediaSessionProvider provider,
        CancellationToken cancellationToken)
    {
        MediaRequest? request;
        try
        {
            request = requestText is null
                ? null
                : JsonSerializer.Deserialize<MediaRequest>(requestText, new JsonSerializerOptions(JsonSerializerDefaults.Web));
        }
        catch (JsonException)
        {
            return MediaStatus.Unavailable("Malformed media-control request.");
        }

        if (request is null || request.ProtocolVersion != MediaProtocol.Version)
        {
            return MediaStatus.Unavailable("Unsupported media-control request.");
        }

        if (!string.Equals(request.Command, "status", StringComparison.Ordinal))
        {
            await provider.ExecuteAsync(request.Command, cancellationToken);
        }
        return await provider.GetStatusAsync(cancellationToken);
    }
}

internal sealed record MediaStatus(
    int ProtocolVersion,
    bool Available,
    string SourceAppId,
    string Title,
    string Artist,
    string PlaybackStatus,
    bool CanToggle,
    bool CanPrevious,
    bool CanNext,
    string Error)
{
    public static MediaStatus Unavailable(string error = "") =>
        new(MediaProtocol.Version, false, "", "", "", "unavailable", false, false, false, error);
}

internal interface IMediaSessionProvider
{
    Task<MediaStatus> GetStatusAsync(CancellationToken cancellationToken);
    Task<bool> ExecuteAsync(string command, CancellationToken cancellationToken);
}

internal sealed class WindowsMediaSessionProvider : IMediaSessionProvider
{
    private readonly GlobalSystemMediaTransportControlsSessionManager? manager;
    private readonly string initializationError;

    private WindowsMediaSessionProvider(GlobalSystemMediaTransportControlsSessionManager? manager, string initializationError)
    {
        this.manager = manager;
        this.initializationError = initializationError;
    }

    public static async Task<WindowsMediaSessionProvider> CreateAsync()
    {
        try
        {
            return new(await GlobalSystemMediaTransportControlsSessionManager.RequestAsync(), "");
        }
        catch (Exception exception) when (exception is UnauthorizedAccessException or InvalidOperationException)
        {
            return new(null, exception.Message);
        }
    }

    public async Task<MediaStatus> GetStatusAsync(CancellationToken cancellationToken)
    {
        var session = manager?.GetCurrentSession();
        if (session is null) return MediaStatus.Unavailable(initializationError);
        try
        {
            var properties = await session.TryGetMediaPropertiesAsync().AsTask(cancellationToken);
            var playback = session.GetPlaybackInfo();
            var controls = playback.Controls;
            return new(
                MediaProtocol.Version,
                true,
                session.SourceAppUserModelId ?? "",
                properties?.Title ?? "",
                properties?.Artist ?? "",
                playback.PlaybackStatus.ToString().ToLowerInvariant(),
                controls.IsPlayEnabled || controls.IsPauseEnabled,
                controls.IsPreviousEnabled,
                controls.IsNextEnabled,
                "");
        }
        catch (Exception exception) when (exception is UnauthorizedAccessException or InvalidOperationException)
        {
            return MediaStatus.Unavailable(exception.Message);
        }
    }

    public async Task<bool> ExecuteAsync(string command, CancellationToken cancellationToken)
    {
        var session = manager?.GetCurrentSession();
        if (session is null) return false;
        try
        {
            var controls = session.GetPlaybackInfo().Controls;
            return command switch
            {
                "previous" when controls.IsPreviousEnabled => await session.TrySkipPreviousAsync().AsTask(cancellationToken),
                "toggle-play-pause" when controls.IsPlayEnabled || controls.IsPauseEnabled =>
                    await session.TryTogglePlayPauseAsync().AsTask(cancellationToken),
                "next" when controls.IsNextEnabled => await session.TrySkipNextAsync().AsTask(cancellationToken),
                _ => false
            };
        }
        catch (Exception exception) when (exception is UnauthorizedAccessException or InvalidOperationException)
        {
            return false;
        }
    }
}
