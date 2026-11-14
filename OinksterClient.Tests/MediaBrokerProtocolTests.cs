using Oinkster.MediaBroker;
using Xunit;

namespace OinksterClient.Tests;

public sealed class MediaBrokerProtocolTests
{
    [Fact]
    public async Task HandleAsync_MalformedMessage_ReturnsUnavailableWithoutDispatching()
    {
        var provider = new FakeMediaSessionProvider(MediaStatus.Unavailable());

        var result = await MediaRequestProcessor.HandleAsync("{bad", provider, CancellationToken.None);

        Assert.False(result.Available);
        Assert.Contains("Malformed", result.Error, StringComparison.Ordinal);
        Assert.Empty(provider.Commands);
    }

    [Fact]
    public async Task HandleAsync_UnsupportedVersion_ReturnsUnavailableWithoutDispatching()
    {
        var provider = new FakeMediaSessionProvider(MediaStatus.Unavailable());

        var result = await MediaRequestProcessor.HandleAsync(
            "{\"protocolVersion\":2,\"command\":\"next\"}", provider, CancellationToken.None);

        Assert.False(result.Available);
        Assert.Contains("Unsupported", result.Error, StringComparison.Ordinal);
        Assert.Empty(provider.Commands);
    }

    [Theory]
    [InlineData("previous")]
    [InlineData("toggle-play-pause")]
    [InlineData("next")]
    public async Task HandleAsync_Action_DispatchesAndReturnsFreshStatus(string command)
    {
        var expected = new MediaStatus(1, true, "Spotify.exe", "Track", "Artist", "playing", true, true, true, "");
        var provider = new FakeMediaSessionProvider(expected);

        var result = await MediaRequestProcessor.HandleAsync(
            $"{{\"protocolVersion\":1,\"command\":\"{command}\"}}", provider, CancellationToken.None);

        Assert.Equal(expected, result);
        Assert.Equal([command], provider.Commands);
    }

    [Fact]
    public async Task HandleAsync_Status_DoesNotDispatchAnAction()
    {
        var expected = MediaStatus.Unavailable();
        var provider = new FakeMediaSessionProvider(expected);

        var result = await MediaRequestProcessor.HandleAsync(
            "{\"protocolVersion\":1,\"command\":\"status\"}", provider, CancellationToken.None);

        Assert.Equal(expected, result);
        Assert.Empty(provider.Commands);
    }

    private sealed class FakeMediaSessionProvider(MediaStatus status) : IMediaSessionProvider
    {
        public List<string> Commands { get; } = [];

        public Task<MediaStatus> GetStatusAsync(CancellationToken cancellationToken) => Task.FromResult(status);

        public Task<bool> ExecuteAsync(string command, CancellationToken cancellationToken)
        {
            Commands.Add(command);
            return Task.FromResult(true);
        }
    }
}
