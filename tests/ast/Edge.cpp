/*
Edge.cpp - not a mod: the shapes of clang's AST dump that the filter in front of the compile's parser (FDumpFilter)
could get wrong, for `assetgen astcheck`, which the suite runs on this file (tools/test_bytecode.py). It sits in a
folder of its own, outside the test mods the suite cooks; astcheck parses it with the command a mod gets.

- escapes and non-ASCII text: in string and character literals, in a name, and in a doc comment that holds a raw
  U+0001 and U+007F (the dump writes the first as \u0001 and the second as it is)
- a string that ends in a backslash, so its closing quote comes right after an escaped one (\\"): kept (a deprecation
  message) and dropped (a #line file name, inside the locations after it)
- DeclRefExprs and a MemberExpr written through macros: their range's end has a spellingLoc, which FAstSax keeps
- the largest u64, the smallest i64, negative enumerators and '\xff'
- friend templates, and operator= declared defaulted, user-defined, and implicit (in a class with a virtual)
- a type alias (typeAliasDeclId) and a declaration from an included file (includedFrom): this file, included again
- unbalanced brackets inside a string in a dropped location (a #line file name): a skip that counted them would run
  past the end of the location
- a string literal longer than the reader's 256 KB chunk, built from adjacent literals in macros so the file stays small
*/
#ifndef EDGE_SECOND_PASS
#define EDGE_SECOND_PASS
#include "Edge.cpp"     // itself: the #else branch at the end, whose locations come from an included file

#define M(x) x
#define Q(x) (x + 1)
#define QUAL S::V
#define FIELD(o) o.F

/// A doc comment with non-ASCII text, é, € and 😀, and two control characters:  and .
int g = 5;
const char* s = "tab\t quote\" backslash\\ slash/ é \U0001F600 ctl \x01 del \x7f nl\n";
const char16_t* w = u"é\U0001F600";
const wchar_t* ww = L"\x01\x7f\xff€";
char c = '\xff';
wchar_t wc = L'€';
int Café = 1;
unsigned long long big = 18446744073709551615ull;
long long neg = -9223372036854775807ll - 1;
double d = 1.5e300, e = -0.0, f2 = 3.14159265358979323846;
float fl = 1e-45f;
enum E : int { A = -1, B = 0x7fffffff, C = -2147483647 - 1 };
enum class EU : unsigned long long { X = 18446744073709551615ull };
template <long long Lo, unsigned long long Hi> struct Extremes { static constexpr long long Low = Lo; };
Extremes<-9223372036854775807ll - 1, 18446744073709551615ull> Ex;
constexpr long long Lowest = Extremes<-9223372036854775807ll - 1, 18446744073709551615ull>::Low;
using Int32 = int;
Int32 Aliased = -3;
[[deprecated("ends in a backslash\\")]] int Deprecated = 0;

struct S
{
    static int V;
    int F = 3;
    int Get() const;
    S& operator=(const S&) = default;
};
int S::V = 7;
inline int S::Get() const { return F + M(g) + QUAL; }

struct T
{
    int X = 0;
    T& operator=(const T& O) { X = O.X + 1; return *this; }
};

/* clang declares the implicit operator= of a class with a virtual up front, and marks it isImplicit. */
struct WithVirtual
{
    virtual ~WithVirtual() = default;
    int X = 0;
};

template <class T> inline T Twice(T X) { return X + X; }

template <class U> struct Peer;
template <class T> struct Box
{
    T Val;
    friend bool operator==(const Box& A, const Box& B) { return A.Val == B.Val; }
    template <class U> friend struct Peer;
    template <class U> friend U Peek(const Box<U>& B);
};
template <class U> U Peek(const Box<U>& B) { return B.Val; }

int h()
{
    S a, b;
    a = b;
    T ta, tb;
    ta = tb;
    WithVirtual va, vb;
    va = vb;
    Box<int> x{ 1 }, y{ 2 };
    const bool bEq = x == y;
    return M(g) + Q(g) - -3 + Twice(4) + int(Twice(2.5)) + bEq + QUAL + a.Get() + FIELD(a) + Peek(x) + FromInclude(Café);
}

/* Longer than the 256 KB chunks the reader hands the parser, so the literal's dump spans chunks, escapes and all.
   S16 is 16 bytes: two adjacent literals, since a hex escape would run on into the letters after it. */
#define S16 "ab\\\"\t\x01\x7f\xff" "cd efgh "
#define S64 S16 S16 S16 S16
#define S256 S64 S64 S64 S64
#define S1K S256 S256 S256 S256
#define S4K S1K S1K S1K S1K
#define S16K S4K S4K S4K S4K
#define S64K S16K S16K S16K S16K
#define S256K S64K S64K S64K S64K
const char Long[] = S256K S16K;

#else
/* The locations after a #line carry its file name as a string inside the dropped loc. */
#line 1 "Edge{[.h"
/// From an included file.
inline int FromInclude(int X) { return X + 1; }
#line 1 "dir\\"
inline int FromDir(int X) { return X - 1; }
#endif
