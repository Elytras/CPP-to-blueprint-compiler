#pragma once
#include <cstdio>
#include <string>

namespace Uasset
{
/* A command running with its stdout on a pipe (clang's AST dump, Cpp.cpp's RunClang). */
struct FCommandPipe
{
    FILE* Out = nullptr;        // the command's stdout, read in binary
    void* Process = nullptr;    // Windows: its process handle
};

/* Starts the command line Cmd, program first; its stdin and stderr are this process's. On Windows the program runs
   directly, not under cmd.exe as _popen runs it: one process per compile instead of two or three (cmd.exe, and any
   AutoRun command it runs first, such as a chcp), which matters where exited processes are not freed until a reboot.
   So Cmd is quoted as the program's own argument parser reads it, with no shell syntax. False if it could not start. */
bool OpenCommand(const std::string& Cmd, FCommandPipe& Pipe);

/* Closes the pipe, waits for the command to exit and returns its exit status: 0 for success. */
int CloseCommand(FCommandPipe& Pipe);

}   // namespace Uasset
