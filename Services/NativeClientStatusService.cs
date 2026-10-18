using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using OinksterClient.Models;

namespace OinksterClient.Services;

public sealed class NativeClientStatusService
{
    private const string PipeName = @"LOCAL\Oinkster.Client.Status.v1";
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public async Task<NativeClientStatus?> QueryAsync(CancellationToken cancellationToken = default)
    {
        await using var pipe = new NamedPipeClientStream(
            ".",
            PipeName,
            PipeDirection.In,
            PipeOptions.Asynchronous,
            System.Security.Principal.TokenImpersonationLevel.Identification);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromMilliseconds(750));

        try
        {
            await pipe.ConnectAsync(timeout.Token);
            using var reader = new StreamReader(pipe, Encoding.Unicode, false, 4096, leaveOpen: true);
            var json = await reader.ReadToEndAsync(timeout.Token);
            var status = JsonSerializer.Deserialize<NativeClientStatus>(json, JsonOptions);
            return status?.InterfaceVersion == 2 ? status : null;
        }
        // UnauthorizedAccessException is what ConnectAsync throws when the pipe
        // exists but its DACL denies us - exactly the case the native side's
        // SDDL produces for a caller that is neither the pipe's owner nor an
        // AppContainer (an elevated launcher against a non-elevated game, say).
        // It was missing here, so that scenario threw out of QueryAsync into an
        // async void handler. ModuleSettingsService's pipe client already
        // caught it; this one did not.
        catch (Exception exception) when (exception is IOException or OperationCanceledException
            or JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }
}
