#include <StdInc.h>
#include "ModAutoCt.h"

#include <Feather/Util/Decl.h>
#include <Nest/Common/Diagnostic.h>

using namespace SprFrontend;


void ModAutoCt::beforeSetContext(Node* node)
{
    using namespace Feather;

    if ( node->nodeKind == nkSparrowDeclSprFunction )
    {
        setProperty(node, propAutoCt, 1);
        setEvalMode(node, modeRtCt);
    }
    else
        REP_INTERNAL(node->location, "'autoCt' modifier can be applied only to functions");
}
