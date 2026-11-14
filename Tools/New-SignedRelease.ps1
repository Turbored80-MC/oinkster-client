param(
    [Parameter(Mandatory = $true)][string]$ClientDllPath,
    [Parameter(Mandatory = $true)][version]$Version,
    [Parameter(Mandatory = $true)][ValidateSet('Release', 'Beta', 'Nightly')][string]$Channel,
    [Parameter(Mandatory = $true)][version]$ExactBedrockVersion,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Optional: sign the launcher installer into the manifest so installed
    # launchers update themselves. Pass the version of the launcher inside it.
    [string]$LauncherInstallerPath,
    [version]$LauncherVersion
)

$ErrorActionPreference = 'Stop'
$arguments = @('sign', $ClientDllPath, $Version, $Channel, $ExactBedrockVersion, $OutputDirectory)
if ($LauncherInstallerPath) {
    if (-not $LauncherVersion) { throw "-LauncherVersion is required with -LauncherInstallerPath." }
    $arguments += @($LauncherInstallerPath, $LauncherVersion)
}
dotnet run --project "$PSScriptRoot\ReleaseTool\Oinkster.ReleaseTool.csproj" -- @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
