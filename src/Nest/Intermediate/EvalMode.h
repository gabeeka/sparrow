#pragma once

/// Enumeration that determines the evaluation mode of nodes: rt, ct, or rtct
enum Nest_EvalMode
{
    modeUnspecified = 0,
    modeRt,             ///< Only available at run-time
    modeCt,             ///< Only available at compile-time
    modeRtCt,           ///< Available both at run-time and compile-time, depending on the context in which invoked
};

typedef enum Nest_EvalMode Nest_EvalMode;
typedef enum Nest_EvalMode EvalMode;

inline const char* Nest_evalModeToString(EvalMode mode)
{
    switch ( mode )
    {
    case modeRt:        return "rt";
    case modeCt:        return "ct";
    case modeRtCt:      return "rtct";
    default:            return "unspecified";
    }
}

#ifdef __cplusplus
    template <typename T>
    basic_ostream<T>& operator << (basic_ostream<T>& os, EvalMode mode)
    {
        os << Nest_evalModeToString(mode);
        return os;
    }
#endif
