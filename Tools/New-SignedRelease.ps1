param(
    [Parameter(Mandatory = $true)][string]$ClientDllPath,
    [Parameter(Mandatory = $true)][version]$Version,
    [Parameter(Mandatory = $true)][ValidateSet('Release', 'Beta', 'Nightly')][string]$Channel,
    [Parameter(Mandatory = $true)][version]$ExactBedrockVersion,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
dotnet run --project "$PSScriptRoot\ReleaseTool\Oinkster.ReleaseTool.csproj" -- `
    sign $ClientDllPath $Version $Channel $ExactBedrockVersion $OutputDirectory
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
