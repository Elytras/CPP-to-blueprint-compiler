#include "Command.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace Uasset
{
#ifdef _WIN32
/* An inheritable copy of one of this process's standard handles, for the command; null when there is none. */
static HANDLE InheritableStd(DWORD Which)
{
    const HANDLE H = GetStdHandle(Which);
    HANDLE Copy = nullptr;
    if (H && H != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), H, GetCurrentProcess(), &Copy, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return Copy;
}

bool OpenCommand(const std::string& Cmd, FCommandPipe& Pipe)
{
    SECURITY_ATTRIBUTES Inherit{ sizeof Inherit, nullptr, TRUE };
    HANDLE Read = nullptr, Write = nullptr;
    if (!CreatePipe(&Read, &Write, &Inherit, 0)) return false;
    /* The command must not hold the read end too: the pipe would outlive our close of it. */
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA Start{};
    Start.cb = sizeof Start;
    Start.dwFlags = STARTF_USESTDHANDLES;
    Start.hStdInput = InheritableStd(STD_INPUT_HANDLE);
    Start.hStdOutput = Write;
    Start.hStdError = InheritableStd(STD_ERROR_HANDLE);
    PROCESS_INFORMATION Info{};
    std::string Line = Cmd;     // CreateProcess may write to the command line it is given
    const bool bStarted = CreateProcessA(nullptr, Line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &Start, &Info);
    /* The command has its own copies now; ours of the write end would keep the pipe from ever reaching its end. */
    for (HANDLE H : { Start.hStdInput, Write, Start.hStdError })
        if (H) CloseHandle(H);
    if (!bStarted) { CloseHandle(Read); return false; }
    CloseHandle(Info.hThread);
    const int Fd = _open_osfhandle(intptr_t(Read), _O_RDONLY | _O_BINARY);
    Pipe.Out = Fd == -1 ? nullptr : _fdopen(Fd, "rb");
    if (!Pipe.Out)
    {
        if (Fd != -1) _close(Fd); else CloseHandle(Read);   // the command's writes fail, so it exits
        WaitForSingleObject(Info.hProcess, INFINITE);
        CloseHandle(Info.hProcess);
        return false;
    }
    Pipe.Process = Info.hProcess;
    return true;
}

int CloseCommand(FCommandPipe& Pipe)
{
    fclose(Pipe.Out);
    DWORD Code = 1;
    if (WaitForSingleObject(Pipe.Process, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(Pipe.Process, &Code)) Code = 1;
    CloseHandle(Pipe.Process);
    Pipe = {};
    return int(Code);
}
#else
bool OpenCommand(const std::string& Cmd, FCommandPipe& Pipe)
{
    Pipe.Out = popen(Cmd.c_str(), "r");
    return Pipe.Out != nullptr;
}

int CloseCommand(FCommandPipe& Pipe)
{
    const int Status = pclose(Pipe.Out);
    Pipe = {};
    return Status;
}
#endif

}   // namespace Uasset
