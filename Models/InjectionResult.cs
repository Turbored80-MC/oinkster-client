namespace OinksterClient.Models;

public enum InjectionStatus
{
    Success,
    AlreadyActive,
    UnsupportedBedrock,
    UpdateRequired,
    TargetExited,
    TimedOut,
    SecurityValidationFailed,
    NotImplemented
}

public sealed record InjectionResult(InjectionStatus Status, string Message);
