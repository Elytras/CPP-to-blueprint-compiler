#pragma once
/*
Types.h — the project's integer type aliases, UE/Dumper-7 spellings (uint32, int64, …).

Lives in core/ (L0) so every layer — including the game-agnostic IPC/string utilities and
the CLI — can name integers the same way, without depending on the game SDK or the game/
layer. The SDK declares its own (namespaced) SDK::uint32 etc.; these are the global ones.
*/
using uint64 = unsigned long long;
using int64 = signed long long;
using uint32 = unsigned int;
using int32 = signed int;
using uint16 = unsigned short;
using int16 = signed short;
using uint8 = unsigned char;
using int8 = signed char;
using wchar = wchar_t;
using uchar = unsigned char;