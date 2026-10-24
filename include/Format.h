#pragma once
/*
Format.h - fmt-style text formatting: `Format("{Name}:{Value}", Key, Map[Key])`.

The pattern is UE's (FText::Format): each `{Name}` is filled by an argument, a backtick escapes the next character.
The arguments fill the placeholders in the order their names first appear, so `{A} {B} {A}` takes two. The call
is the Format node over one FFormatArgumentData per argument, typed by the argument's C++ type: an int32 (or
smaller) is an Int argument, a float or double a Float, an FText a Text, an ETextGender a Gender, and an FString,
FName, bool or int64 the Text of it (Conv_*ToText).

clang counts the placeholders while it compiles the mod: a pattern that names more or fewer of them than there
are arguments is a compile error, which names the function Format_PlaceholdersAndArgumentsDiffer. The pattern
must be a string literal; AssetGen reads the names out of it at build time (__FormatPattern__ and
__FormatArgName__, lowered by Cpp.cpp).

  FString Line = Format("{Name}:{Value}", Key, Map[Key]);
  FText Score  = Format("{0} of {1}", Hits, Shots);
*/
#include "UeApi/Types.h"

#include "UeApi/Engine.h"

/* Not constexpr, so a consteval call that reaches it fails: the name is the message. */
inline void Format_PlaceholdersAndArgumentsDiffer() {}

/* How many different names the pattern's `{...}` placeholders have. */
consteval int32 FormatPlaceholderCount(const char* P)
{
    int32 Count = 0;
    for (int32 I = 0; P[I]; ++I)
    {
        if (P[I] == '`') { if (P[I + 1]) ++I; continue; }
        if (P[I] != '{') continue;
        int32 End = I + 1;
        while (P[End] && P[End] != '}') ++End;
        if (!P[End]) break;
        /* A name seen before is the same argument again. */
        bool bSeen = false;
        for (int32 J = 0; J < I && !bSeen; ++J)
        {
            if (P[J] == '`') { ++J; continue; }
            if (P[J] != '{') continue;
            int32 K = 0;
            while (J + 1 + K < I && P[J + 1 + K] == P[I + 1 + K] && P[I + 1 + K] != '}') ++K;
            bSeen = P[J + 1 + K] == '}' && P[I + 1 + K] == '}';
        }
        Count += bSeen ? 0 : 1;
        I = End;
    }
    return Count;
}

template <class T> struct TFormatIdentity { using Type = T; };

/* The pattern of a Format call with arguments A...: built from the literal by a consteval constructor, which is
   where clang checks the count. */
template <class... A>
struct TFormatPattern
{
    const char* S;
    template <int32 N>
    consteval TFormatPattern(const char (&P)[N]) : S(P)
    {
        if (FormatPlaceholderCount(P) != int32(sizeof...(A))) Format_PlaceholdersAndArgumentsDiffer();
    }
};

FText __FormatPattern__(const char* Pattern);                 // the pattern as text
template <class T> FString __FormatArgName__(const char* Pattern, const T& Arg);   // the name Arg fills, by its position

inline FFormatArgumentData FormatArg(const FString& Name, int32 V) {
    return FFormatArgumentData(Name, EFormatArgumentType::Int, FText(), V, 0.0f, ETextGender::Masculine);
}
inline FFormatArgumentData FormatArg(const FString& Name, float V) {
    return FFormatArgumentData(Name, EFormatArgumentType::Float, FText(), 0, V, ETextGender::Masculine);
}
inline FFormatArgumentData FormatArg(const FString& Name, double V) { return FormatArg(Name, float(V)); }
inline FFormatArgumentData FormatArg(const FString& Name, FText V) {
    return FFormatArgumentData(Name, EFormatArgumentType::Text, V, 0, 0.0f, ETextGender::Masculine);
}
inline FFormatArgumentData FormatArg(const FString& Name, ETextGender V) {
    return FFormatArgumentData(Name, EFormatArgumentType::Gender, FText(), 0, 0.0f, V);
}
inline FFormatArgumentData FormatArg(const FString& Name, const FString& V) {
    return FormatArg(Name, UKismetTextLibrary::Conv_StringToText(V));
}
inline FFormatArgumentData FormatArg(const FString& Name, FName V) {
    return FormatArg(Name, UKismetTextLibrary::Conv_NameToText(V));
}
inline FFormatArgumentData FormatArg(const FString& Name, bool V) {
    return FormatArg(Name, UKismetTextLibrary::Conv_BoolToText(V));
}
inline FFormatArgumentData FormatArg(const FString& Name, int64 V) {
    return FormatArg(Name, UKismetTextLibrary::Conv_Int64ToText(V, false, true, 1, 324));
}

template <class... A>
inline FText Format(TFormatPattern<typename TFormatIdentity<A>::Type...> Pattern, const A&... Args)
{
    return UKismetTextLibrary::Format(__FormatPattern__(Pattern.S), { FormatArg(__FormatArgName__(Pattern.S, Args), Args)... });
}
