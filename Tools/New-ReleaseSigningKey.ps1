param(
    [Parameter(Mandatory = $true)]
    [string]$PrivateKeyPath
)

$ErrorActionPreference = 'Stop'
$publicKeyPath = Join-Path $PSScriptRoot '..\Assets\OinksterReleasePublicKey.pem'
dotnet run --project "$PSScriptRoot\ReleaseTool\Oinkster.ReleaseTool.csproj" -- `
    keygen $PrivateKeyPath $publicKeyPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
