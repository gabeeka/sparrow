#pragma once

/// A reference to a string, or, range of characters
struct Nest_StringRef {
    const char* begin; ///< The beginning of the string
    const char* end;   ///< One past the last character of the string
};
typedef struct Nest_StringRef Nest_StringRef;
