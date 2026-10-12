#include "Oinkster/PipeWait.hpp"

namespace Oinkster::PipeWait {
namespace {

// Waits for an already-pending overlapped operation, honouring both the stop
// event and a timeout. On any path that does not complete normally the I/O is
// cancelled AND drained (GetOverlappedResult with bWait=TRUE) before returning,
// because `overlapped` lives on the caller's stack: returning while the kernel
// still owns it would corrupt that stack frame.
Result AwaitOverlapped(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stopEvent,
    DWORD timeoutMs, DWORD* transferred) noexcept {
    HANDLE waits[2]{overlapped.hEvent, stopEvent};
    const DWORD count = stopEvent ? 2U : 1U;
    const DWORD waited = WaitForMultipleObjects(count, waits, FALSE, timeoutMs);

    if (waited == WAIT_OBJECT_0) {
        DWORD moved{};
        const BOOL ok = GetOverlappedResult(pipe, &overlapped, &moved, FALSE);
        if (transferred) *transferred = moved;
        return ok ? Result::Completed : Result::Failed;
    }

    // Stopped, timed out, or the wait itself failed: cancel and drain.
    (void)CancelIoEx(pipe, &overlapped);
    DWORD moved{};
    (void)GetOverlappedResult(pipe, &overlapped, &moved, TRUE);
    if (transferred) *transferred = 0;
    return waited == WAIT_OBJECT_0 + 1 ? Result::Stopped : Result::Failed;
}

class ScopedEvent final {
public:
    ScopedEvent() noexcept : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent() { if (handle_) CloseHandle(handle_); }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr; }
private:
    HANDLE handle_;
};

}

Result AwaitConnection(HANDLE pipe, HANDLE stopEvent) noexcept {
    // Signalled before we even wait, e.g. Stop() raced us to the top of the
    // accept loop.
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return Result::Stopped;

    ScopedEvent connected;
    if (!connected.Valid()) return Result::Failed;
    OVERLAPPED overlapped{};
    overlapped.hEvent = connected.Get();

    if (ConnectNamedPipe(pipe, &overlapped)) return Result::Completed;
    switch (GetLastError()) {
    case ERROR_PIPE_CONNECTED:
        // A client connected between CreateNamedPipeW and here.
        return Result::Completed;
    case ERROR_IO_PENDING:
        return AwaitOverlapped(pipe, overlapped, stopEvent, INFINITE, nullptr);
    default:
        return Result::Failed;
    }
}

Result Read(HANDLE pipe, void* buffer, DWORD bytes, DWORD& bytesRead,
    HANDLE stopEvent, DWORD timeoutMs) noexcept {
    bytesRead = 0;
    ScopedEvent done;
    if (!done.Valid()) return Result::Failed;
    OVERLAPPED overlapped{};
    overlapped.hEvent = done.Get();

    if (ReadFile(pipe, buffer, bytes, &bytesRead, &overlapped)) return Result::Completed;
    if (GetLastError() != ERROR_IO_PENDING) return Result::Failed;
    return AwaitOverlapped(pipe, overlapped, stopEvent, timeoutMs, &bytesRead);
}

Result Write(HANDLE pipe, const void* buffer, DWORD bytes,
    HANDLE stopEvent, DWORD timeoutMs) noexcept {
    if (bytes == 0) return Result::Completed;
    ScopedEvent done;
    if (!done.Valid()) return Result::Failed;
    OVERLAPPED overlapped{};
    overlapped.hEvent = done.Get();

    DWORD written{};
    if (WriteFile(pipe, buffer, bytes, &written, &overlapped)) return Result::Completed;
    if (GetLastError() != ERROR_IO_PENDING) return Result::Failed;
    return AwaitOverlapped(pipe, overlapped, stopEvent, timeoutMs, &written);
}

}
