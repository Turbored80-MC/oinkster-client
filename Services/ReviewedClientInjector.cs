using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using OinksterClient.Models;

namespace OinksterClient.Services;

public sealed class ReviewedClientInjector : IClientInjector
{
    private static readonly bool LiveActivationEnabled = true;
    private const uint ProcessCreateThread = 0x0002;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint ProcessVmOperation = 0x0008;
    private const uint ProcessVmWrite = 0x0020;
    private const uint ProcessVmRead = 0x0010;
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint MemRelease = 0x8000;
    private const uint PageReadWrite = 0x04;
    private const uint WaitObject0 = 0;
    private const uint WaitFailed = 0xFFFFFFFF;
    private const uint ImageFileMachineUnknown = 0;
    private const uint ImageFileMachineAmd64 = 0x8664;

    // Appends the calling thread's last Win32 error to a message. Must be used
    // immediately after the failing call, before any other P/Invoke.
    private static string Win32(string message)
    {
        var code = Marshal.GetLastWin32Error();
        if (code == 0) return message;
        return $"{message} (Win32 {code}: {new Win32Exception(code).Message})";
    }

    public Task<InjectionResult> InjectAsync(
        int processId,
        InstalledClientBuild clientBuild,
        CancellationToken cancellationToken = default)
    {
        if (!LiveActivationEnabled)
        {
            return Task.FromResult(new InjectionResult(
                InjectionStatus.NotImplemented,
                "Live activation is temporarily disabled while the corrected D3D12 adapter completes validation."));
        }

        return Task.Run(() => Inject(processId, clientBuild, cancellationToken), cancellationToken);
    }

    private static InjectionResult Inject(int processId, InstalledClientBuild clientBuild, CancellationToken cancellationToken)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var dllPath = Path.GetFullPath(clientBuild.DllPath);
            if (!File.Exists(dllPath) || !Path.GetFileName(dllPath).Equals("oinkster-client-x64.dll", StringComparison.Ordinal))
            {
                return Failure(InjectionStatus.SecurityValidationFailed, "The installed client DLL is missing or has an unexpected name.");
            }

            // Hashing reads the whole DLL; the token was checked once at the
            // top and then nothing else honoured it for the rest of the method.
            cancellationToken.ThrowIfCancellationRequested();
            var actualHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(dllPath)));
            if (!actualHash.Equals(clientBuild.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                return Failure(InjectionStatus.SecurityValidationFailed, "The client DLL failed its integrity check.");
            }
            cancellationToken.ThrowIfCancellationRequested();

            using var target = Process.GetProcessById(processId);
            if (!target.ProcessName.Equals("Minecraft.Windows", StringComparison.OrdinalIgnoreCase))
            {
                return Failure(InjectionStatus.SecurityValidationFailed, "The target is not Minecraft for Windows.");
            }

            var queryHandle = OpenProcess(ProcessQueryLimitedInformation, false, processId);
            if (queryHandle == IntPtr.Zero)
            {
                return Failure(InjectionStatus.SecurityValidationFailed, "Minecraft could not be inspected safely.");
            }

            try
            {
                if (!IsWow64Process2(queryHandle, out var processMachine, out var nativeMachine)
                    || (processMachine != ImageFileMachineUnknown && processMachine != ImageFileMachineAmd64)
                    || nativeMachine != ImageFileMachineAmd64)
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, "Minecraft is not running as native x64.");
                }

                var packageName = GetPackageName(queryHandle);
                var expectedMarker = $"_{clientBuild.Bedrock.ExactVersion}_x64__";
                if (packageName is null || !packageName.Contains(expectedMarker, StringComparison.OrdinalIgnoreCase))
                {
                    return Failure(InjectionStatus.UnsupportedBedrock, $"This DLL supports only Bedrock {clientBuild.Bedrock.ExactVersion} x64.");
                }
            }
            finally
            {
                CloseHandle(queryHandle);
            }

            var rights = ProcessCreateThread | ProcessQueryInformation | ProcessVmOperation | ProcessVmWrite | ProcessVmRead;
            var processHandle = OpenProcess(rights, false, processId);
            if (processHandle == IntPtr.Zero)
            {
                // Every P/Invoke here declares SetLastError, but the code never
                // read it - so an access-denied open was indistinguishable from
                // an anti-cheat or CFG policy refusal further down. Reporting
                // the code is the difference between a diagnosable failure and
                // "it didn't work".
                return Failure(InjectionStatus.SecurityValidationFailed, Win32("Minecraft denied client activation."));
            }

            IntPtr remotePath = IntPtr.Zero;
            IntPtr threadHandle = IntPtr.Zero;
            try
            {
                var pathBytes = Encoding.Unicode.GetBytes(dllPath + '\0');
                remotePath = VirtualAllocEx(processHandle, IntPtr.Zero, (nuint)pathBytes.Length, MemCommit | MemReserve, PageReadWrite);
                if (remotePath == IntPtr.Zero
                    || !WriteProcessMemory(processHandle, remotePath, pathBytes, (nuint)pathBytes.Length, out var written)
                    || written != (nuint)pathBytes.Length)
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, Win32("The verified DLL path could not be prepared in Minecraft."));
                }

                var kernel32 = GetModuleHandle("kernel32.dll");
                var loadLibrary = kernel32 == IntPtr.Zero ? IntPtr.Zero : GetProcAddress(kernel32, "LoadLibraryW");
                if (loadLibrary == IntPtr.Zero)
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, Win32("Windows loader validation failed."));
                }

                cancellationToken.ThrowIfCancellationRequested();
                threadHandle = CreateRemoteThread(processHandle, IntPtr.Zero, 0, loadLibrary, remotePath, 0, out _);
                if (threadHandle == IntPtr.Zero)
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, Win32("Minecraft rejected the activation thread."));
                }

                // WAIT_TIMEOUT, WAIT_FAILED and WAIT_ABANDONED all used to be
                // reported as "timed out", which is only true of the first.
                var wait = WaitForSingleObject(threadHandle, 10_000);
                if (wait == WaitFailed)
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, Win32("Waiting on the activation thread failed."));
                }

                if (wait != WaitObject0)
                {
                    return Failure(InjectionStatus.TimedOut,
                        "Client activation did not finish within 10 seconds. Minecraft may be busy or blocking the load.");
                }

                if (!GetExitCodeThread(threadHandle, out var exitCode))
                {
                    return Failure(InjectionStatus.SecurityValidationFailed, Win32("The activation thread result could not be read."));
                }

                if (exitCode == 0)
                {
                    // LoadLibraryW returned NULL in the target process.
                    return Failure(InjectionStatus.SecurityValidationFailed,
                        "Windows did not load the client DLL. Check oinkster-native.log for details.");
                }

                return new InjectionResult(InjectionStatus.Success, "Oinkster was activated.");
            }
            finally
            {
                if (threadHandle != IntPtr.Zero) CloseHandle(threadHandle);
                if (remotePath != IntPtr.Zero) VirtualFreeEx(processHandle, remotePath, 0, MemRelease);
                CloseHandle(processHandle);
            }
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (ArgumentException)
        {
            return Failure(InjectionStatus.TargetExited, "Minecraft exited before Oinkster could activate.");
        }
        catch (InvalidOperationException)
        {
            return Failure(InjectionStatus.TargetExited, "Minecraft exited before Oinkster could activate.");
        }
        catch (Win32Exception exception)
        {
            return Failure(InjectionStatus.SecurityValidationFailed, exception.Message);
        }
        catch (IOException exception)
        {
            return Failure(InjectionStatus.SecurityValidationFailed, exception.Message);
        }
        // UnauthorizedAccessException derives from SystemException, not
        // IOException, so reading a DLL the process cannot open escaped every
        // filter here. The returned Task faulted, PlayButton_Click had no
        // try, and the launcher died with Play left permanently disabled.
        // NotSupportedException covers Path.GetFullPath on a malformed path
        // from installed.json.
        catch (Exception exception) when (exception is UnauthorizedAccessException or NotSupportedException)
        {
            return Failure(InjectionStatus.SecurityValidationFailed,
                $"The installed client DLL could not be read: {exception.Message}");
        }
    }

    private static string? GetPackageName(IntPtr processHandle)
    {
        uint length = 0;
        if (GetPackageFullName(processHandle, ref length, null) != 122 || length == 0)
        {
            return null;
        }

        var builder = new StringBuilder((int)length);
        return GetPackageFullName(processHandle, ref length, builder) == 0 ? builder.ToString() : null;
    }

    private static InjectionResult Failure(InjectionStatus status, string message) => new(status, message);

    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inheritHandle, int processId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackageFullName(IntPtr process, ref uint packageFullNameLength, StringBuilder? packageFullName);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, nuint size, uint allocationType, uint protection);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, nuint size, uint freeType);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, nuint size, out nuint written);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string moduleName);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes, nuint stackSize, IntPtr startAddress, IntPtr parameter, uint flags, out uint threadId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);
}
