using System.Text.Json;
using OinksterClient.Models;
using Xunit;

namespace OinksterClient.Tests;

public sealed class NativeClientStatusTests
{
    [Fact]
    public void Deserialize_TerminalStartupFailure_PreservesActionableMessage()
    {
        const string json = """
            {"interfaceVersion":2,"state":"error","hooksInstalled":false,"startupError":"Direct3D 12 Present hook failed."}
            """;

        var status = JsonSerializer.Deserialize<NativeClientStatus>(json, new JsonSerializerOptions(JsonSerializerDefaults.Web));

        Assert.NotNull(status);
        Assert.Equal("error", status.State);
        Assert.False(status.HooksInstalled);
        Assert.Equal("Direct3D 12 Present hook failed.", status.StartupError);
    }

    [Fact]
    public void Deserialize_LegacyReadyStatus_AllowsMissingStartupError()
    {
        const string json = """
            {"interfaceVersion":2,"state":"ready","hooksInstalled":true}
            """;

        var status = JsonSerializer.Deserialize<NativeClientStatus>(json, new JsonSerializerOptions(JsonSerializerDefaults.Web));

        Assert.NotNull(status);
        Assert.True(status.HooksInstalled);
        Assert.Null(status.StartupError);
    }
}
