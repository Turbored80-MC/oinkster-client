param(
    [Parameter(Mandatory = $true)][version]$Version,
    [Parameter(Mandatory = $true)][string]$VersionLabel,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$InnoCompiler = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $InnoCompiler)) {
    throw "Inno Setup was not found at $InnoCompiler. Install it with: winget install JRSoftware.InnoSetup"
}

$root = Split-Path $PSScriptRoot -Parent
$publishDir = Join-Path ([IO.Path]::GetTempPath()) "oinkster-installer-publish-$Version"
if (Test-Path $publishDir) { Remove-Item -Recurse -Force $publishDir }

dotnet publish "$root\OinksterClient.csproj" -c Release -p:Platform=x64 -r win-x64 -o $publishDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Test-Path "$publishDir\Assets\OinksterReleasePublicKey.pem")) {
    throw "The published launcher is missing the release public key."
}

New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
& $InnoCompiler /Q "/DSourceDir=$publishDir" "/DAppVersion=$Version" "/DVersionLabel=$VersionLabel" "/DOutputDir=$((Resolve-Path $OutputDirectory).Path)" "$PSScriptRoot\Installer\OinksterClient.iss"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Created $OutputDirectory\OinksterClient-Setup-$Version.exe"
