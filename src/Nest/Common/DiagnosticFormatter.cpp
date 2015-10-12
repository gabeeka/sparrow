#include <StdInc.hpp>
#include "DiagnosticFormatter.hpp"

using namespace Nest;
using namespace Nest::Common;

DiagnosticFormatter::DiagnosticFormatter(DiagnosticSeverity severity, const char* fmt, bool dontThrow)
    : severity_(severity)
    , fmt_(fmt)
    , location_()
    , dontThrow_(dontThrow)
{
}

DiagnosticFormatter::DiagnosticFormatter(DiagnosticSeverity severity, const char* fmt, Location loc, bool dontThrow)
    : severity_(severity)
    , fmt_(fmt)
    , location_(loc)
    , dontThrow_(dontThrow)
{
}
