#include <StdInc.h>
#include "SprTypeTraits.h"
#include "Overload.h"
#include "StdDef.h"
#include "Impl/Callable.h"
#include <SparrowFrontendTypes.h>
#include <NodeCommonsCpp.h>

#include "Feather/Api/Feather.h"
#include "Feather/Utils/FeatherUtils.hpp"


using namespace SprFrontend;

namespace
{
    bool getNumericProperties(TypeRef t, int& numBits, bool& isUnsigned, bool& isFloating)
    {
        if ( !t->hasStorage )
            return false;
        Node* cls = Feather_classForType(t);
        ASSERT(cls);
        
        const StringRef* nativeName = Nest_getPropertyString(cls, propNativeName);
        if ( !nativeName || size(*nativeName) <= 1 || !islower(nativeName->begin[0]) )
            return false;
        
        if ( *nativeName == "double" )
        {
            numBits = 64;
            isUnsigned = false;
            isFloating = true;
            return true;
        }
        else if ( *nativeName == "float" )
        {
            numBits = 32;
            isUnsigned = false;
            isFloating = true;
            return true;
        }
        else if ( size(*nativeName) > 1 && (nativeName->begin[0] == 'i' || nativeName->begin[0] == 'u') )
        {
            try
            {
                numBits = boost::lexical_cast<int>(nativeName->begin+1);
                isUnsigned = nativeName->begin[0] == 'u';
                isFloating = false;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        return false;
        
    }
}

TypeRef SprFrontend::commonType(CompilationContext* context, TypeRef t1, TypeRef t2)
{
    // Check if the types are the same
    if ( t1 == t2 )
        return t1;
    
    // Check for numerics
    int numBits1, numBits2;
    bool isUnsigned1, isUnsigned2;
    bool isFloating1, isFloating2;
    if ( getNumericProperties(t1, numBits1, isUnsigned1, isFloating1)
        && getNumericProperties(t2, numBits2, isUnsigned2, isFloating2) )
    {
        // Floating & non-floating => return the floating point type
        if ( isFloating1 && !isFloating2 )
            return t1;
        if ( !isFloating1 && isFloating2 )
            return t2;
        
        // If the values are too small, return Int
        if ( numBits1 < 32 && numBits2 < 32 )
            return StdDef::typeInt;
        
        // Prefer the bigger type to the small type
        if ( numBits1 > numBits2 )
            return t1;
        if ( numBits1 < numBits2 )
            return t2;
        
        // Prefer unsigned, if the types have the same type
        if ( numBits1 == numBits2 && isUnsigned1 && !isUnsigned2 )
            return t1;
        if ( numBits1 == numBits2 && !isUnsigned1 && isUnsigned2 )
            return t2;
    }
    
    // If there is one conversion from one type to another (and not vice-versa) take the less specialized type
    ConversionFlags flags = flagDontAddReference;
    ConversionResult c1 = canConvertType(context, t1, t2, flags);
    ConversionResult c2 = canConvertType(context, t2, t1, flags);
    if ( c1 && !c2 )
    {
        return t2;
    }
    if ( !c1 && c2 )
    {
        return t1;
    }
    
    return StdDef::typeVoid;
}

TypeRef SprFrontend::doDereference1(Node* arg, Node*& cvt)
{
    cvt = arg;

    // If the base is an expression with a data type, treat this as a data access
    TypeRef t = arg->type;
    if ( !t->hasStorage )
        return t;

    // If we have N references apply N-1 dereferencing operations
    for ( size_t i=1; i<t->numReferences; ++i )
    {
        cvt = Feather_mkMemLoad(arg->location, cvt);
    }
    return Feather_getDataType(t->referredNode, 0, t->mode);  // Zero references
}

namespace
{
    Node* checkDataTypeConversion(Node* node)
    {
        const Location& loc = node->location;
        TypeRef t = node->type;
        Node* cls = Feather_classForType(t);
        if ( Feather_effectiveEvalMode(cls) != modeRtCt )
            REP_INTERNAL(loc, "Cannot convert ct to rt for non-rtct classes (%1%)") % cls;

        // Check if we have a ct-to-rt ctor
        Callable* call = selectCtToRtCtor(node->context, t);
        if ( !call )
            REP_ERROR_RET(nullptr, loc, "Cannot convert %1% from CT to RT (make sure 'ctorFromRt' method exists)") % t;

        // Generate the call to the ctor
        if ( !Nest_computeType(node) )
            return nullptr;
        auto cr = call->canCall(node->context, loc, fromIniList({node}), modeRt, true);
        ASSERT(cr);
        Node* res = call->generateCall(loc);
        res = Feather_mkMemLoad(loc, res);

        // Sanity check
        Nest_setContext(res, node->context);
        if ( !Nest_computeType(res) )
            return nullptr;
        if ( res->type != Feather_checkChangeTypeMode(node->type, modeRt, NOLOC) )
            REP_INTERNAL(loc, "Cannot convert %1% from CT to RT (invalid returned type)") % t;

        return res;
    }
}

Node* SprFrontend::convertCtToRt(Node* node)
{
    const Location& loc = node->location;

    TypeRef t = node->type;

    if ( t->typeKind == typeKindVoid )
    {
        Nest_ctEval(node);
        return Feather_mkNop(loc);
    }

    if ( !t->hasStorage )
        REP_ERROR_RET(nullptr, loc, "Cannot convert a non-storage type from CT to RT (%1%)") % t;

    if ( t->typeKind != typeKindData )
        REP_ERROR_RET(nullptr, loc, "Cannot convert from CT to RT a node of non-data type (%1%)") % t;

    if ( t->numReferences > 0 )
        REP_ERROR_RET(nullptr, loc, "Cannot convert references from CT to RT (%1%)") % t;

    if ( Feather_isBasicNumericType(t) || Feather_checkChangeTypeMode(t, modeRtCt, NOLOC) == StdDef::typeStringRef )
        return Nest_ctEval(node);
    else
        return checkDataTypeConversion(node);
}

TypeRef SprFrontend::getType(Node* typeNode)
{
    Nest_setPropertyExplInt(typeNode, propAllowDeclExp, 1);
    if ( !Nest_semanticCheck(typeNode) )
        return nullptr;
    if ( !typeNode->type )
        REP_ERROR_RET(nullptr, typeNode->location, "Invalid type name");
    
    TypeRef t = tryGetTypeValue(typeNode);
    if ( t )
        return t;
    
    REP_ERROR(typeNode->location, "Invalid type name (%1%)") % typeNode->type;
    return nullptr;
}

TypeRef SprFrontend::tryGetTypeValue(Node* typeNode)
{
    Nest_setPropertyExplInt(typeNode, propAllowDeclExp, 1);
    if ( !Nest_semanticCheck(typeNode) )
        return nullptr;

    // If this is a DeclExp, try to look at the declaration types
    Node* expl = Nest_explanation(typeNode);
    if ( expl->nodeKind == nkSparrowExpDeclExp )
    {
        TypeRef res = nullptr;
        NodeRange decls = { expl->referredNodes.beginPtr+1, expl->referredNodes.endPtr };
        for ( Node* decl: decls )
        {
            TypeRef t = decl->nodeKind == nkSparrowDeclSprClass ? decl->type : nullptr;
            if ( t ) {
                if ( res )
                    return nullptr; // multiple class decls; not a clear type
                res = t;
            }
        }
        return res;
    }
    
    TypeRef t = Feather_lvalueToRefIfPresent(typeNode->type);
    
    if ( t == StdDef::typeRefType )
    {
        Node* n = Nest_ctEval(typeNode);
        if ( n->nodeKind == nkFeatherExpCtValue )
        {
            TypeRef** t = Feather_getCtValueData<TypeRef*>(n);
            if ( !t || !*t || !**t )
                REP_ERROR_RET(nullptr, typeNode->location, "No type was set for node");
            return **t;
        }
    }
    else if ( t == StdDef::typeType )
    {
        Node* n = Nest_ctEval(typeNode);
        if ( n->nodeKind == nkFeatherExpCtValue )
        {
            TypeRef* t = Feather_getCtValueData<TypeRef>(n);
            if ( !t || !*t )
                REP_ERROR_RET(nullptr, typeNode->location, "No type was set for node");
            return *t;
        }
    }
    
    return nullptr;
}

TypeRef SprFrontend::evalTypeIfPossible(Node* typeNode)
{
    TypeRef t = tryGetTypeValue(typeNode);
    return t ? t : typeNode->type;
}

Node* SprFrontend::createTypeNode(CompilationContext* context, const Location& loc, TypeRef t)
{
    Node* res = Feather_mkCtValueT(loc, StdDef::typeType, &t);
    if ( context )
        Nest_setContext(res, context);
    return res;
}

TypeRef SprFrontend::getAutoType(Node* typeNode, bool addRef)
{
    TypeRef t = typeNode->type;
    
    // Nothing to do for function types
    if ( t->typeKind == typeKindFunction )
        return t;
    
    // Remove l-value if we have one
    if ( t->typeKind == typeKindLValue )
        t = Feather_baseType(t);
    
    // Dereference
    t = Feather_removeAllRef(t);
    
    if ( addRef )
        t = Feather_addRef(t);
    t = Feather_checkChangeTypeMode(t, modeRtCt, typeNode->location);
    return t;
}

bool SprFrontend::isConceptType(TypeRef t)
{
    return t->typeKind == typeKindConcept;
}

bool SprFrontend::isConceptType(TypeRef t, bool& isRefAuto)
{
    if ( t->typeKind == typeKindConcept )
    {
        isRefAuto = t->numReferences > 0;
        return true;
    }
    return false;
}

TypeRef SprFrontend::changeRefCount(TypeRef type, int numRef, const Location& loc)
{
    ASSERT(type);
    
    // If we have a LValue type, remove it
    while ( type->typeKind == typeKindLValue )
        type = Feather_baseType(type);

    if ( type->typeKind == typeKindData )
        type = Feather_getDataType(type->referredNode, numRef, type->mode);
    else if ( type->typeKind == typeKindConcept )
        type = getConceptType(conceptOfType(type), numRef, type->mode);
    else
        REP_INTERNAL(loc, "Cannot change reference count for type %1%") % type;
    return type;
}

