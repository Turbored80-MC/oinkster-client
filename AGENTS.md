# Repository Guidelines

## Project Structure & Module Organization

Oinkster Client is a C# WinUI 3 launcher prototype targeting .NET 10 and Windows 10 build 17763 or later.

- `App.xaml` and `App.xaml.cs`: application resources and startup lifecycle.
- `MainWindow.xaml` and `.xaml.cs`: window chrome, title bar, and root frame.
- `MainPage.xaml` and `.xaml.cs`: launcher UI and prototype interactions.
- `Assets/`: package artwork, generated branding, and skin reference images. The active launcher logo is `Assets/OinksterLogo.png`.
- `Package.appxmanifest` and `app.manifest`: MSIX identity, capabilities, and Windows metadata.
- `Properties/launchSettings.json`: local launch configuration.

Generated `bin/` and `obj/` directories must not be committed. Add new services or models in focused top-level folders such as `Services/` or `Models/` rather than expanding page code-behind indefinitely.

## Build, Test, and Development Commands

Run commands from the repository root in PowerShell:

```powershell
dotnet restore
dotnet build OinksterClient.csproj -p:Platform=x64
dotnet run --project OinksterClient.csproj -p:Platform=x64
dotnet clean OinksterClient.csproj -p:Platform=x64
```

`build` compiles XAML and C# for the primary development architecture. `run` registers the debug package and launches it. Use Visual Studio 2022 with the WinUI application development workload for XAML debugging.

## Coding Style & Naming Conventions

Use four-space indentation, file-scoped C# namespaces, nullable reference types, and implicit usings. Name types and public members with `PascalCase`; use `camelCase` for locals and parameters. Name UI handlers by action, for example `SettingsButton_Click`. Give interactive XAML controls descriptive `x:Name` values and centralize repeated colors or styles in resource dictionaries. Keep UI text sentence-cased and concise.

## Testing Guidelines

Launcher service and model logic is covered by the `OinksterClient.Tests` xUnit project (`dotnet test OinksterClient.Tests/OinksterClient.Tests.csproj`); it links individual `Services`/`Models` files rather than referencing the WinUI project, so add new files to its `.csproj` when you put testable logic in one. Name tests `MethodName_Condition_ExpectedResult`. Native tests run via `ctest --preset debug`.

Both C# projects set `TreatWarningsAsErrors` for Debug, so every change must build warning-free in Debug. Release is not trimmed, because trimming breaks the reflection-based JSON the launcher uses at runtime. The WinUI layer has no automated coverage, so UI changes also need a manual smoke test: verify Play, Settings, Back, resizing, keyboard focus, and asset loading.

## Commit & Pull Request Guidelines

The repository has no established commit history. Use short imperative commits such as `Refine launcher settings layout`. Pull requests should explain user-visible behavior, list verification commands, link relevant issues, and include screenshots for visual changes. Keep unrelated refactors out of feature PRs.

## Security & Scope

Never commit credentials, signing certificates, tokens, packaged binaries, or downloaded DLLs. Keep the launcher prototype free of injection, account authentication, and game-file modification unless those capabilities are explicitly designed and reviewed in a separate change.
