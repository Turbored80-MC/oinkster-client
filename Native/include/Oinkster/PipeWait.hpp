#pragma once

#include <Windows.h>

// ---------------------------------------------------------------------------
// Cancellable named-pipe server primitives.
//
// A blocking (non-overlapped) ConnectNamedPipe cannot be cancelled, which made
// shutdown racy: StatusPipe/ControlPipe used to wake their worker by connecting
// a throwaway client and then join() with no timeout. If that wake connect
// landed in the window between the worker closing one pipe instance and
// creating the next, the connect failed, the worker then entered an
// uncancellable ConnectNamedPipe, and join() never returned - eject hung
// forever.
//
// These helpers run the pipe handle in OVERLAPPED mode and wait on both the
// I/O completion and a caller-owned stop event, so shutdown is deterministic
// and needs no wake-client trick. Note that a handle created with
// FILE_FLAG_OVERLAPPED must use the read/write helpers here too - passing a
// null OVERLAPPED to ReadFile/WriteFile on such a handle is not valid.
// ---------------------------------------------------------------------------
namespace Oinkster::PipeWait {

// Completed = the operation finished (a client connected, or the I/O
// transferred). Stopped = the stop event fired first. Failed = an error, or a
// timeout on a read/write.
enum class Result { Completed, Stopped, Failed };

// Waits for a client to connect to `pipe`, or for `stopEvent` to be signalled.
// `pipe` must have been created with FILE_FLAG_OVERLAPPED.
[[nodiscard]] Result AwaitConnection(HANDLE pipe, HANDLE stopEvent) noexcept;

// Reads one message, giving up after `timeoutMs` or when `stopEvent` fires, so
// a client that connects and then stalls cannot hold shutdown open.
[[nodiscard]] Result Read(HANDLE pipe, void* buffer, DWORD bytes, DWORD& bytesRead,
    HANDLE stopEvent, DWORD timeoutMs) noexcept;

[[nodiscard]] Result Write(HANDLE pipe, const void* buffer, DWORD bytes,
    HANDLE stopEvent, DWORD timeoutMs) noexcept;

}
