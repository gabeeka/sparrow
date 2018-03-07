#include <StdInc.h>
#include "IntMods.h"
#include "Mods.h"
#include "SprDebug.h"

#include <Nodes/Builder.h>
#include <Nodes/SparrowNodesAccessors.h>
#include <Helpers/SprTypeTraits.h>
#include <Helpers/DeclsHelpers.h>
#include <Helpers/StdDef.h>
#include <Helpers/Convert.h>
#include "Feather/Api/Feather.h"
#include "Feather/Utils/FeatherUtils.hpp"

#include "Nest/Api/Modifier.h"
#include "Nest/Api/SourceCode.h"
#include "Nest/Utils/Diagnostic.hpp"

using namespace SprFrontend;
using namespace std;

namespace
{
    const int generatedOverloadPrio = -100;
    const int defaultOverloadPrio = 0;

    // Add to a local space an operator call
    void addOperatorCall(Node* dest, bool reverse, Node* operand1, const string& op, Node* operand2)
    {
        Node* call = mkOperatorCall(dest->location, operand1, fromString(op), operand2);
        if ( !reverse )
            Nest_appendNodeToArray(&dest->children, call);
        else
            Nest_insertNodeIntoArray(&dest->children, 0, call);
    }

    // Add an associated function with the given body and given arguments, near the parent class
    // If autoCt==true, and mode==modeRtCt, we will also add the autoCt modifier
    // This is useful for things like the '==' operator
    Node* addAssociatedFun(Node* parent, const string& name, Node* body,
            vector<pair<TypeRef, string>> params, int overloadPrio, Node* resClass = nullptr,
            EvalMode mode = modeUnspecified, bool autoCt = false) {
        Location loc = parent->location;
        loc.end = loc.start;

        // The context in which we add the associated fun
        // If this is a ctor, and we have other ctors inside the class, then add it inside the class
        // Otherwise, add it near the class.
        bool shouldBeInner = false;
        if (name=="ctor") {
            NodeArray decls = Nest_symTabLookupCurrent(parent->childrenContext->currentSymTab, "ctor");
            shouldBeInner = size(decls) > 0;
            Nest_freeNodeArray(decls);
        }
        CompilationContext* ctx = shouldBeInner ? parent->childrenContext : classContext(parent);
        // TODO (classes): Remove this

        // Construct the parameters list, return type node
        NodeVector sprParams;
        sprParams.reserve(params.size());
        for ( auto param: params )
        {
            sprParams.push_back(mkSprParameter(loc, fromString(param.second), param.first));
        }
        Node* parameters = sprParams.empty() ? nullptr : Feather_mkNodeList(loc, all(sprParams));
        Node* ret = resClass ? createTypeNode(ctx, loc, Feather_getDataType(resClass, 0, modeRtCt)) : nullptr;

        // Add the function in the context of the parent
        Node* f = mkSprFunction(loc, fromString(name), parameters, ret, body);
        Nest_setPropertyInt(f, propNoDefault, 1);
        Nest_setPropertyInt(f, "spr.accessType", (int) protectedAccess);
        Nest_setPropertyExplInt(f, propOverloadPrio, generatedOverloadPrio);
        if ( mode == modeUnspecified )
            mode = Feather_effectiveEvalMode(parent);
        Feather_setEvalMode(f, mode);
        if ( mode == modeRtCt && autoCt )
            Nest_addModifier(f, SprFe_getAutoCtMod());
        Nest_setContext(f, ctx);
        if ( !Nest_computeType(f) )
            return nullptr;
        Nest_queueSemanticCheck(f);
        Nest_appendNodeToArray(&parent->additionalNodes, f);
        return f;
    }

    /// Generate an associated function with the given name, by calling 'op' for the base classes and fields
    Node* generateAssociatedFun(Node* parent, const string& name, const string& op, TypeRef otherParam, bool reverse = false, EvalMode mode = modeUnspecified)
    {
        Location loc = parent->location;
        loc.end = loc.start;
        Node* cls = Nest_explanation(parent);
        cls = cls && cls->nodeKind == nkFeatherDeclClass ? cls : nullptr;
        ASSERT(cls);

        Node* thisRef = mkIdentifier(loc, fromCStr("this"));

        Node* otherRef = nullptr;
        if ( otherParam )
        {
            otherRef = mkIdentifier(loc, fromCStr("other"));
            if ( otherParam->numReferences > 0 )
                otherRef = Feather_mkMemLoad(loc, otherRef);
        }

        // Construct the body
        Node* body = Feather_mkLocalSpace(loc, {});
        for ( Node* field: cls->children )
        {
            // Left-hand side: field-ref to the current field
            Node* lhs = Feather_mkFieldRef(loc, Feather_mkMemLoad(loc, thisRef), field);

            // Right-hand side
            Node* rhs = otherParam ? mkCompoundExp(loc, otherRef, Feather_getName(field))
                                   : (name == "ctor" ? Nest_getPropertyDefaultNode(
                                                               field, propVarInit, nullptr)
                                                     : nullptr);

            string oper = op;
            if ( field->type->numReferences > 0 )
            {
                if ( op == "=" || op == "ctor" )
                {
                    oper = ":=";    // Transform into ref assignment
                    if ( !rhs )
                        rhs = buildNullLiteral(loc);
                }
                else if ( op == "dtor" )
                    continue;       // Nothing to destruct on references
            }
            addOperatorCall(body, reverse, lhs, oper, rhs);
        }

        vector<pair<TypeRef, string>> params;
        params.reserve(2);
        params.push_back({Feather_addRef(cls->type), string("this")});
        if ( otherParam )
            params.push_back({otherParam, string("other")});

        return addAssociatedFun(parent, name, body, params, generatedOverloadPrio, nullptr, mode);
    }

    /// Generate an init ctor, that initializes all the members with data received as arguments
    /// Returns true if all fields were initialized with default values, and no params are needed.
    bool generateInitCtor(Node* parent)
    {
        vector<pair<TypeRef, string>> params;
        params.push_back({Feather_addRef(parent->type), "this"});

        Location loc = parent->location;
        loc.end = loc.start;
        Node* cls = Nest_explanation(parent);
        cls = cls && cls->nodeKind == nkFeatherDeclClass ? cls : nullptr;
        ASSERT(cls);

        Node* thisRef = mkIdentifier(loc, fromCStr("this"));

        // Construct the body
        Node* body = Feather_mkLocalSpace(loc, {});
        for ( Node* field: cls->children )
        {
            // Take in account only fields of the current class
            Node* cls2 = Feather_getParentClass(field->context);
            if ( cls2 != cls )
                continue;

            TypeRef t = field->type;

            // Do we have an initialization specified in the data struct?
            Node* init = Nest_getPropertyDefaultNode(field, propVarInit, nullptr);
            if (!init) {
                // Add a parameter for the base
                string paramName = "f"+toString(Feather_getName(field));
                params.push_back({t, paramName});
                init = mkIdentifier(loc, fromString(paramName));
                if ( t->numReferences > 0 )
                    init = Feather_mkMemLoad(loc, init);
            }


            // Left-hand side: field-ref to the current field
            Node* lhs = Feather_mkFieldRef(loc, Feather_mkMemLoad(loc, thisRef), field);

            string oper = t->numReferences > 0 ? ":=" : "ctor";
            addOperatorCall(body, false, lhs, oper, init);
        }

        (void) addAssociatedFun(parent, "ctor", body, params, defaultOverloadPrio);
        return params.size() == 1;
    }

    /// Generate the equality check method for the given class
    Node* generateEqualityCheckFun(Node* parent)
    {
        Location loc = parent->location;
        loc.end = loc.start;
        Node* cls = Nest_explanation(parent);
        cls = cls && cls->nodeKind == nkFeatherDeclClass ? cls : nullptr;
        ASSERT(cls);

        // Construct the equality check expression
        Node* exp = nullptr;
        for ( Node* field: cls->children )
        {
            // Take in account only fields of the current class
            Node* cls2 = Feather_getParentClass(field->context);
            if ( cls2 != cls )
                continue;

            Node* fieldRef = Feather_mkFieldRef(loc, Feather_mkMemLoad(loc, mkIdentifier(loc, fromCStr("this"))), field);
            Node* otherFieldRef = Feather_mkFieldRef(loc, Feather_mkMemLoad(loc, mkIdentifier(loc, fromCStr("other"))), field);

            const char* op = (field->type->numReferences == 0) ? "==" : "===";
            Node* curExp = mkOperatorCall(loc, fieldRef, fromCStr(op), otherFieldRef);
            if ( !exp )
                exp = curExp;
            else
                exp = mkOperatorCall(loc, exp, fromCStr("&&"), curExp);
        }
        if ( !exp )
            exp = buildBoolLiteral(loc, true);

        Node* body = Feather_mkLocalSpace(loc, {});
        Nest_appendNodeToArray(&body->children, mkReturnStmt(loc, exp));

        vector<pair<TypeRef, string>> params;
        params.reserve(2);
        TypeRef t = Feather_getDataType(cls, 1, modeUnspecified);
        params.push_back({t, string("this")});
        params.push_back({t, string("other")});
        Node* res = addAssociatedFun(parent, "==", body, params, generatedOverloadPrio, StdDef::clsBool, Feather_effectiveEvalMode(parent), true);
        return res;
    }

    //! Gets the type for the 'this' parameter of the given function
    Node* getThisTypeForFun(Node* fun) {
        // Get the class of the 'this' param
        int thisIdx = getThisParamIdx(fun);
        if (thisIdx != 0)
            REP_INTERNAL(fun->location, "'this' parameter should be the first parameter");
        Node* parameters = at(fun->children, 0);
        Node* thisParam = at(parameters->children, thisIdx);
        CHECK(fun->location, thisParam);
        if (!Nest_computeType(thisParam))
            return nullptr;
        ASSERT(thisParam->type);
        Node* cls = thisParam->type->referredNode;
        ASSERT(cls);
        return cls;
    }

    /// Search the given body for a constructor with the given properties.
    ///
    /// This can search for constructors of given classes, constructors called on this, or called for the given field.
    ///
    /// It will search only the instructions directly inside the given local space, or in a child local space
    /// It will not search inside conditionals, or other instructions
    bool hasCtorCall(Node* inSpace, bool checkThis, Node* forField)
    {
        // Check all the items in the local space
        for ( Node* n: inSpace->children )
        {
            if ( !Nest_computeType(n) )
                continue;
            n = Nest_explanation(n);
            if ( !n )
                continue;

            // Check inner local spaces
            if ( n->nodeKind == nkFeatherLocalSpace )
            {
                if ( hasCtorCall(n, checkThis, forField) )
                    return true;
                continue;
            }

            // We consider function calls for our checks
            if ( n->nodeKind != nkFeatherExpFunCall )
                continue;
            Node* funDecl = at(n->referredNodes, 0);
            if ( Feather_getName(funDecl) != "ctor" )
                continue;
            if ( Nest_nodeArraySize(n->children) == 0 )
                continue;
            Node* thisArg = at(n->children, 0);

            // Check for this to be passed as argument
            if ( checkThis )
            {
                // If we have a MemLoad, just ignore it
                while (thisArg && thisArg->nodeKind == nkFeatherExpMemLoad)
                    thisArg = Nest_explanation(at(thisArg->children, 0));

                if ( !thisArg || thisArg->nodeKind != nkFeatherExpVarRef
                    || Feather_getName(at(thisArg->referredNodes, 0)) != "this" )
                    continue;
            }

            // If a field is given, check that the this argument is a reference to our field
            if ( forField )
            {
                // If we have a Bitcast, just ignore it
                if ( thisArg->nodeKind == nkFeatherExpBitcast )
                    thisArg = Nest_explanation(at(thisArg->children, 0));

                if ( !thisArg || thisArg->nodeKind != nkFeatherExpFieldRef
                    || at(thisArg->referredNodes, 0) != forField )
                    continue;
            }

            return true;
        }
        return false;
    }
}

void _IntModClassMembers_afterComputeType(Modifier*, Node* node)
{
    // Check to apply only to classes
    if ( node->nodeKind != nkSparrowDeclSprClass )
        REP_INTERNAL(node->location, "IntModClassMembers modifier can be applied only to classes");
    Node* cls = node;
    if ( !cls->type )
        REP_INTERNAL(node->location, "Type was not computed for %1% when applying IntModClassMembers") % Feather_getName(node);

    Node* basicClass = Nest_explanation(node);
    basicClass = basicClass && basicClass->nodeKind == nkFeatherDeclClass ? basicClass : nullptr;
    ASSERT(basicClass);
    TypeRef paramType = Feather_getDataType(basicClass, 1, modeRtCt);

    // Initialization ctor
    bool skipDefaultCtor = false;
    if ( Nest_hasProperty(cls, propGenerateInitCtor) )
        skipDefaultCtor = generateInitCtor(cls);

    // Auto-generated functions
    if (!skipDefaultCtor)
        generateAssociatedFun(cls, "ctor", "ctor", nullptr);
    generateAssociatedFun(cls, "ctor", "ctor", paramType);
    generateAssociatedFun(cls, "ctorFromCt", "ctor", Feather_checkChangeTypeMode(Feather_getDataType(basicClass, 0, modeRtCt), modeCt, node->location), false, modeRt);
    generateAssociatedFun(cls, "dtor", "dtor", nullptr, true);
    generateAssociatedFun(cls, "=", "=", paramType);
    generateEqualityCheckFun(cls);
}

void IntModCtorMembers_beforeSemanticCheck(Modifier*, Node* fun) {
    /// Check to apply only to non-static constructors
    if ( fun->nodeKind != nkSparrowDeclSprFunction || Feather_getName(fun) != "ctor" )
        REP_INTERNAL(fun->location, "IntModCtorMembers modifier can be applied only to constructors");
    if ( !funHasThisParameters(fun) )
        REP_INTERNAL(fun->location, "IntModCtorMembers cannot be applied to static constructors");

    // If we have a body, make sure it's a local space
    Node* body = at(fun->children, 2);
    if ( !body )
        return; // nothing to do
    if ( body->nodeKind != nkFeatherLocalSpace )
        REP_INTERNAL(fun->location, "Constructor body is not a local space (needed by IntModCtorMembers)");

    // Get the class
    Node* cls = getThisTypeForFun(fun);
    if (!cls)
        return;

    // If we are calling other constructor of this class, don't add any initialization
    if ( hasCtorCall(body, true, nullptr) )
        return;

    // Generate the ctor calls in the order of the fields; add them to the body of the constructor
    const Location& loc = body->location;
    for ( int i = Nest_nodeArraySize(cls->children)-1; i>=0; --i )
    {
        Node* field = at(cls->children, i);

        // Make sure we initialize only fields of the current class
        Node* cls2 = Feather_getParentClass(field->context);
        if ( cls2 != cls )
            continue;

        if ( !hasCtorCall(body, false, field) )
        {
            Node* base = mkCompoundExp(loc, mkIdentifier(loc, fromCStr("this")), Feather_getName(field));
            Node* call = nullptr;
            if ( field->type->numReferences == 0 )
            {
                call = mkOperatorCall(loc, base, fromCStr("ctor"), nullptr);
            }
            else
            {
                call = mkOperatorCall(loc, base, fromCStr(":="), buildNullLiteral(loc));
            }
            Nest_setContext(call, Nest_childrenContext(body));
            Nest_insertNodeIntoArray(&body->children, 0, call);
        }
    }
}

void IntModDtorMembers_beforeSemanticCheck(Modifier*, Node* fun)
{
    /// Check to apply only to non-static destructors
    if ( fun->nodeKind != nkSparrowDeclSprFunction || Feather_getName(fun) != "dtor" )
        REP_INTERNAL(fun->location, "IntModDtorMembers modifier can be applied only to destructors");
    if ( !funHasThisParameters(fun) )
        REP_INTERNAL(fun->location, "IntModDtorMembers cannot be applied to static destructors");

    // If we have a body, make sure it's a local space
    Node* body = at(fun->children, 2);
    if ( !body )
        return; // nothing to do
    if ( body->nodeKind != nkFeatherLocalSpace )
        REP_INTERNAL(fun->location, "Destructor body is not a local space (needed by IntModDtorMembers)");

    Node* cls = getThisTypeForFun(fun);
    if (!cls)
        return;

    // Generate the dtor calls in reverse order of the fields; add them to the body of the destructor
    CompilationContext* context = Nest_childrenContext(body);
    const Location& loc = body->location;
    for ( int i = Nest_nodeArraySize(cls->children)-1; i>=0; --i )
    {
        Node* field = at(cls->children, i);

        // Make sure we destruct only fields of the current class
        // TODO (classes): Revisit this
        Node* cls2 = Feather_getParentClass(field->context);
        if ( cls2 != cls )
            continue;

        if ( field->type->numReferences == 0 )
        {
            Node* base = mkCompoundExp(loc, mkIdentifier(loc, fromCStr("this")), Feather_getName(field));
            Node* call = mkOperatorCall(loc, base, fromCStr("dtor"), nullptr);
            Nest_setContext(call, context);
            Nest_appendNodeToArray(&body->children, call);
        }

    }
}

Modifier _classMembersIntMod = { modTypeAfterComputeType, &_IntModClassMembers_afterComputeType };
Modifier _ctorMembersIntMod = { modTypeBeforeSemanticCheck, &IntModCtorMembers_beforeSemanticCheck };
Modifier _dtorMembersIntMod = { modTypeBeforeSemanticCheck, &IntModDtorMembers_beforeSemanticCheck };

Modifier* SprFe_getClassMembersIntMod()
{
    return &_classMembersIntMod;
}
Modifier* SprFe_getCtorMembersIntMod()
{
    return &_ctorMembersIntMod;
}
Modifier* SprFe_getDtorMembersIntMod()
{
    return &_dtorMembersIntMod;
}
