//===--- DerivedConformanceEquatableHashable.cpp - Derived Equatable & co. ===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements implicit derivation of the Equatable and Hashable
//  protocols. (Comparable is similar enough in spirit that it would make
//  sense to live here too when we implement its derivation.)
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "DerivedConformances.h"

using namespace swift;
using namespace DerivedConformance;

/// Common preconditions for Equatable and Hashable.
static bool canDeriveConformance(NominalTypeDecl *type) {
  // The type must be an enum.
  // TODO: Structs with Equatable/Hashable/Comparable members
  auto enumDecl = dyn_cast<EnumDecl>(type);
  if (!enumDecl)
    return false;
  
  // The enum must be simple.
  // TODO: Enums with Equatable/Hashable/Comparable payloads
  if (!enumDecl->isSimpleEnum())
    return false;
  
  return true;
}

static Expr *getBooleanLiteral(ASTContext &C, bool value) {
  // Build the boolean literal.
  Type i1Ty = BuiltinIntegerType::get(BuiltinIntegerWidth::fixed(1), C);
  auto lit = new (C) BooleanLiteralExpr(value, SourceLoc(), /*implicit*/ true);
  lit->setType(i1Ty);

  // Find Bool._convertFromBuiltinBooleanLiteral.
  // Note that we synthesize the type of this method so that we don't
  // need to rely on it already having been type-checked. This only
  // matters when building the standard library.
  auto boolDecl = C.getBoolDecl();
  auto convertFunc
    = boolDecl->lookupDirect(C.Id_ConvertFromBuiltinBooleanLiteral)[0];
  auto convertFuncAppliedTy = FunctionType::get(ParenType::get(C, i1Ty),
                                                boolDecl->getDeclaredType());
  auto convertFuncTy = FunctionType::get(boolDecl->getType(), 
                                         convertFuncAppliedTy);
  assert(!convertFunc->hasType() || 
         convertFunc->getType()->isEqual(convertFuncTy) &&
         "_convertFromBuiltinBooleanLiteral has unexpected type");
  auto convertRef = new (C) DeclRefExpr(convertFunc, SourceLoc(), 
                                        /*implicit*/ true, 
                                        /*direct access*/false,
                                        convertFuncTy);

  // Form Bool._convertFromBuiltinBooleanLiteral
  auto boolRef = TypeExpr::createImplicit(boolDecl->getDeclaredType(), C);
  Expr *call = new (C) DotSyntaxCallExpr(convertRef, SourceLoc(), boolRef);
  call->setImplicit(true);
  call->setType(convertFuncAppliedTy);

  // Call Bool._convertFromBuiltinBooleanLiteral.
  Expr *arg = new (C) ParenExpr(SourceLoc(), lit, SourceLoc(), 
                                /*has trailing closure*/ false,
                                lit->getType());
  arg->setImplicit();
  call = new (C) CallExpr(call, arg, /*implicit*/ true, 
                          boolDecl->getDeclaredType());
  return call;
}

static void deriveBodyEquatable_enum_eq(AbstractFunctionDecl *eqDecl) {
  auto args = cast<TuplePattern>(eqDecl->getBodyParamPatterns().back());
  auto aPattern = args->getFields()[0].getPattern();
  auto aParamPattern =
    cast<NamedPattern>(aPattern->getSemanticsProvidingPattern());
  auto aParam = aParamPattern->getDecl();
  auto bPattern = args->getFields()[1].getPattern();
  auto bParamPattern =
    cast<NamedPattern>(bPattern->getSemanticsProvidingPattern());
  auto bParam = bParamPattern->getDecl();

  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());
  ASTContext &C = enumDecl->getASTContext();
  auto enumTy = enumDecl->getDeclaredTypeInContext();

  SmallVector<CaseStmt*, 4> cases;
  SmallVector<CaseLabelItem, 4> caseLabelItems;
  
  for (auto elt : enumDecl->getAllElements()) {
    assert(!elt->hasArgumentType()
           && "enums with payloads not supported yet");
    auto aPat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumTy),
                                           SourceLoc(), SourceLoc(),
                                           Identifier(), elt,
                                           nullptr);
    aPat->setImplicit();
    auto bPat = aPat->clone(C, Pattern::Implicit);
    
    TuplePatternElt tupleElts[] = {
      TuplePatternElt(aPat),
      TuplePatternElt(bPat)
    };
    auto tuplePat = TuplePattern::create(C, SourceLoc(), tupleElts,
                                         SourceLoc());
    tuplePat->setImplicit();

    caseLabelItems.push_back(
        CaseLabelItem(/*IsDefault=*/false, tuplePat, SourceLoc(), nullptr));
  }
  {
    Expr *trueExpr = getBooleanLiteral(C, true);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), trueExpr);
    BraceStmt* body = BraceStmt::create(C, SourceLoc(),
                                        ASTNode(returnStmt), SourceLoc());
    cases.push_back(
        CaseStmt::create(C, SourceLoc(), caseLabelItems,
                         /*HasBoundDecls=*/false, SourceLoc(), body));
  }
  {
    auto any = new (C) AnyPattern(SourceLoc());
    any->setImplicit();

    auto labelItem =
        CaseLabelItem(/*IsDefault=*/true, any, SourceLoc(), nullptr);

    Expr *falseExpr = getBooleanLiteral(C, false);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), falseExpr);
    BraceStmt* body = BraceStmt::create(C, SourceLoc(),
                                        ASTNode(returnStmt), SourceLoc());
    cases.push_back(
        CaseStmt::create(C, SourceLoc(), labelItem, /*HasBoundDecls=*/false,
                         SourceLoc(), body));
  }
  
  auto aRef = new (C) DeclRefExpr(aParam, SourceLoc(), /*implicit*/ true);
  auto bRef = new (C) DeclRefExpr(bParam, SourceLoc(), /*implicit*/ true);
  
  Expr *ab[] = {aRef, bRef};
  
  TupleExpr *abTuple = TupleExpr::createImplicit(C, ab, { });
  
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(),
                                       SourceLoc(), abTuple, SourceLoc(),
                                       cases, SourceLoc(), C);
  BraceStmt *body
    = BraceStmt::create(C, SourceLoc(), ASTNode(switchStmt), SourceLoc());
  eqDecl->setBody(body);
}

/// Derive an '==' operator implementation for an enum.
static ValueDecl *
deriveEquatable_enum_eq(TypeChecker &tc, EnumDecl *enumDecl) {
  // enum SomeEnum<T...> {
  //   case A, B, C
  // }
  // @derived
  // func ==<T...>(a: SomeEnum<T...>, b: SomeEnum<T...>) -> Bool {
  //   switch (a, b) {
  //   case (.A, .A):
  //   case (.B, .B):
  //   case (.C, .C):
  //     return true
  //   case _:
  //     return false
  //   }
  // }
  
  ASTContext &C = tc.Context;
  
  auto enumTy = enumDecl->getDeclaredTypeInContext();
  
  auto getParamPattern = [&](StringRef s) -> std::pair<VarDecl*, Pattern*> {
    VarDecl *aDecl = new (C) ParamDecl(/*isLet*/ true,
                                       SourceLoc(),
                                       C.getIdentifier(s),
                                       SourceLoc(),
                                       C.getIdentifier(s),
                                       enumTy,
                                       enumDecl);
    aDecl->setImplicit();
    Pattern *aParam = new (C) NamedPattern(aDecl, /*implicit*/ true);
    aParam->setType(enumTy);
    aParam = new (C) TypedPattern(aParam, TypeLoc::withoutLoc(enumTy));
    aParam->setType(enumTy);
    aParam->setImplicit();
    return {aDecl, aParam};
  };
  
  auto aParam = getParamPattern("a");
  auto bParam = getParamPattern("b");
  
  TupleTypeElt typeElts[] = {
    TupleTypeElt(enumTy),
    TupleTypeElt(enumTy)
  };
  auto paramsTy = TupleType::get(typeElts, C);
  
  TuplePatternElt paramElts[] = {
    TuplePatternElt(aParam.second),
    TuplePatternElt(bParam.second),
  };
  auto params = TuplePattern::create(C, SourceLoc(),
                                     paramElts, SourceLoc());
  params->setImplicit();
  params->setType(paramsTy);
  
  auto genericParams = enumDecl->getGenericParamsOfContext();
  
  auto boolTy = C.getBoolDecl()->getDeclaredType();
  
  DeclName name(C, C.Id_EqualsOperator, { Identifier(), Identifier() });
  auto eqDecl = FuncDecl::create(C, SourceLoc(), StaticSpellingKind::None,
                           SourceLoc(), name,
                           SourceLoc(),
                           genericParams,
                           Type(), params,
                           TypeLoc::withoutLoc(boolTy),
                           &enumDecl->getModuleContext()->getDerivedFileUnit());
  eqDecl->setImplicit();
  eqDecl->getMutableAttrs().setAttr(AttrKind::AK_infix, SourceLoc());
  auto op = C.getStdlibModule()->lookupInfixOperator(C.Id_EqualsOperator);
  if (!op) {
    tc.diagnose(enumDecl->getLoc(),
                diag::broken_equatable_eq_operator);
    return nullptr;
  }
  eqDecl->setOperatorDecl(op);
  eqDecl->setDerivedForTypeDecl(enumDecl);
  eqDecl->setBodySynthesizer(&deriveBodyEquatable_enum_eq);

  // Compute the type and interface type.
  Type fnTy, interfaceTy;
  if (genericParams) {
    fnTy = PolymorphicFunctionType::get(paramsTy, boolTy, genericParams);
    
    auto enumIfaceTy = enumDecl->getDeclaredInterfaceType();
    TupleTypeElt ifaceParamElts[] = {
      enumIfaceTy, enumIfaceTy,
    };
    auto ifaceParamsTy = TupleType::get(ifaceParamElts, C);
    
    interfaceTy = GenericFunctionType::get(
                                     enumDecl->getGenericSignatureOfContext(),
                                     ifaceParamsTy, boolTy,
                                     AnyFunctionType::ExtInfo());
  } else {
    fnTy = interfaceTy = FunctionType::get(paramsTy, boolTy);
  }
  eqDecl->setType(fnTy);
  eqDecl->setInterfaceType(interfaceTy);

  // Since we can't insert the == operator into the same FileUnit as the enum,
  // itself, we have to give it at least internal access.
  eqDecl->setAccessibility(std::max(enumDecl->getAccessibility(),
                                    Accessibility::Internal));

  if (enumDecl->hasClangNode())
    tc.implicitlyDefinedFunctions.push_back(eqDecl);
  
  // Since it's an operator we insert the decl after the type at global scope.
  return insertOperatorDecl(enumDecl, eqDecl);
}

ValueDecl *DerivedConformance::deriveEquatable(TypeChecker &tc,
                                               NominalTypeDecl *type,
                                               ValueDecl *requirement) {
  // Check that we can actually derive Equatable for this type.
  if (!canDeriveConformance(type))
    return nullptr;
  
  // Build the necessary decl.
  if (requirement->getName().str() == "==") {
    if (auto theEnum = dyn_cast<EnumDecl>(type))
      return deriveEquatable_enum_eq(tc, theEnum);
    else
      llvm_unreachable("todo");
  }
  tc.diagnose(requirement->getLoc(),
              diag::broken_equatable_requirement);
  return nullptr;
}

static void
deriveBodyHashable_enum_hashValue(AbstractFunctionDecl *hashValueDecl) {
  auto enumDecl = cast<EnumDecl>(hashValueDecl->getDeclContext());
  ASTContext &C = enumDecl->getASTContext();
  auto enumType = enumDecl->getDeclaredTypeInContext();
  Type intType = C.getIntDecl()->getDeclaredType();

  auto indexVar = new (C) ParamDecl(/*let*/false,
                                    SourceLoc(),
                                    C.getIdentifier("index"),
                                    SourceLoc(),
                                    C.getIdentifier("index"),
                                    intType, hashValueDecl);
  indexVar->setImplicit();
  
  Pattern *indexPat = new (C) NamedPattern(indexVar, /*implicit*/ true);
  indexPat->setType(intType);
  indexPat = new (C) TypedPattern(indexPat, TypeLoc::withoutLoc(intType));
  indexPat->setType(intType);
  auto indexBind = new (C) PatternBindingDecl(SourceLoc(),
                                              StaticSpellingKind::None,
                                              SourceLoc(),
                                              indexPat, nullptr,
                                              /*conditional*/ false,
                                              enumDecl);
  
  unsigned index = 0;
  SmallVector<CaseStmt*, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    auto pat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                          SourceLoc(), SourceLoc(),
                                          Identifier(), elt, nullptr);
    pat->setImplicit();
    
    auto labelItem =
        CaseLabelItem(/*IsDefault=*/false, pat, SourceLoc(), nullptr);
    
    llvm::SmallString<8> indexVal;
    APInt(32, index++).toString(indexVal, 10, /*signed*/ false);
    auto indexStr = C.AllocateCopy(indexVal);
    
    auto indexExpr = new (C) IntegerLiteralExpr(
                     StringRef(indexStr.data(), indexStr.size()), SourceLoc(),
                     /*implicit*/ true);
    auto indexRef = new (C) DeclRefExpr(indexVar, SourceLoc(),
                                        /*implicit*/true);
    auto assignExpr = new (C) AssignExpr(indexRef, SourceLoc(),
                                         indexExpr, /*implicit*/ true);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(
        CaseStmt::create(C, SourceLoc(), labelItem, /*HasBoundDecls=*/false,
                         SourceLoc(), body));
  }

  Pattern *curriedArgs = hashValueDecl->getBodyParamPatterns().front();
  auto selfPattern =
    cast<NamedPattern>(curriedArgs->getSemanticsProvidingPattern());
  auto selfDecl = selfPattern->getDecl();
  auto selfRef = new (C) DeclRefExpr(selfDecl, SourceLoc(), /*implicit*/true);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(),
                                       SourceLoc(), selfRef,
                                       SourceLoc(), cases, SourceLoc(), C);
  
  auto indexRef = new (C) DeclRefExpr(indexVar, SourceLoc(),
                                      /*implicit*/ true);
  auto memberRef = new (C) UnresolvedDotExpr(indexRef, SourceLoc(),
                                             C.getIdentifier("hashValue"),
                                             SourceLoc(),
                                             /*implicit*/true);
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), memberRef);

  ASTNode bodyStmts[] = {
    indexBind,
    switchStmt,
    returnStmt,
  };

  auto body = BraceStmt::create(C, SourceLoc(), bodyStmts, SourceLoc());
  hashValueDecl->setBody(body);
}

/// Derive a 'hashValue' implementation for an enum.
static ValueDecl *
deriveHashable_enum_hashValue(TypeChecker &tc, EnumDecl *enumDecl) {
  // enum SomeEnum {
  //   case A, B, C
  //   @derived var hashValue: Int {
  //     var index: Int
  //     switch self {
  //     case A:
  //       index = 0
  //     case B:
  //       index = 1
  //     case C:
  //       index = 2
  //     }
  //     return index.hashValue
  //   }
  // }
  ASTContext &C = tc.Context;
  
  Type enumType = enumDecl->getDeclaredTypeInContext();
  Type intType = C.getIntDecl()->getDeclaredType();
  
  // We can't form a Hashable conformance if Int isn't Hashable or
  // IntegerLiteralConvertible.
  if (!tc.conformsToProtocol(intType, C.getProtocol(KnownProtocolKind::Hashable),
                             enumDecl->getModuleContext())) {
    tc.diagnose(enumDecl->getLoc(), diag::broken_int_hashable_conformance);
    return nullptr;
  }
  if (!tc.conformsToProtocol(intType,
                   C.getProtocol(KnownProtocolKind::IntegerLiteralConvertible),
                   enumDecl->getModuleContext())) {
    tc.diagnose(enumDecl->getLoc(),
                diag::broken_int_integer_literal_convertible_conformance);
    return nullptr;
  }
  
  VarDecl *selfDecl = new (C) ParamDecl(/*IsLet*/true,
                                        SourceLoc(),
                                        Identifier(),
                                        SourceLoc(),
                                        C.Id_self,
                                        enumType,
                                        enumDecl);
  selfDecl->setImplicit();
  Pattern *selfParam = new (C) NamedPattern(selfDecl, /*implicit*/ true);
  selfParam->setType(enumType);
  selfParam = new (C) TypedPattern(selfParam, TypeLoc::withoutLoc(enumType));
  selfParam->setType(enumType);
  Pattern *methodParam = TuplePattern::create(C, SourceLoc(),{},SourceLoc());
  methodParam->setType(TupleType::getEmpty(tc.Context));
  Pattern *params[] = {selfParam, methodParam};
  
  FuncDecl *getterDecl =
      FuncDecl::create(C, SourceLoc(), StaticSpellingKind::None, SourceLoc(),
                       Identifier(), SourceLoc(), nullptr, Type(),
                       params, TypeLoc::withoutLoc(intType), enumDecl);
  getterDecl->setImplicit();
  getterDecl->setBodySynthesizer(deriveBodyHashable_enum_hashValue);

  // Compute the type of hashValue().
  GenericParamList *genericParams = nullptr;
  Type methodType = FunctionType::get(TupleType::getEmpty(tc.Context), intType);
  Type selfType = getterDecl->computeSelfType(&genericParams);
  Type type;
  if (genericParams)
    type = PolymorphicFunctionType::get(selfType, methodType, genericParams);
  else
    type = FunctionType::get(selfType, methodType);
  getterDecl->setType(type);
  getterDecl->setBodyResultType(intType);
  
  // Compute the interface type of hashValue().
  Type interfaceType;
  Type selfIfaceType = getterDecl->computeInterfaceSelfType(false);
  if (auto sig = enumDecl->getGenericSignatureOfContext())
    interfaceType = GenericFunctionType::get(sig, selfIfaceType, methodType,
                                             AnyFunctionType::ExtInfo());
  else
    interfaceType = type;
  
  getterDecl->setInterfaceType(interfaceType);
  getterDecl->setAccessibility(enumDecl->getAccessibility());

  if (enumDecl->hasClangNode())
    tc.implicitlyDefinedFunctions.push_back(getterDecl);
  
  // Create the property.
  VarDecl *hashValueDecl = new (C) VarDecl(/*static*/ false,
                                           /*let*/ false,
                                           SourceLoc(), C.Id_hashValue,
                                           intType, enumDecl);
  hashValueDecl->setImplicit();
  hashValueDecl->makeComputed(SourceLoc(), getterDecl, nullptr, SourceLoc());
  hashValueDecl->setAccessibility(enumDecl->getAccessibility());

  Pattern *hashValuePat = new (C) NamedPattern(hashValueDecl, /*implicit*/true);
  hashValuePat->setType(intType);
  hashValuePat
    = new (C) TypedPattern(hashValuePat, TypeLoc::withoutLoc(intType),
                           /*implicit*/ true);
  hashValuePat->setType(intType);
  
  auto patDecl = new (C) PatternBindingDecl(SourceLoc(),
                              StaticSpellingKind::None,
                              SourceLoc(), hashValuePat, nullptr,
                              /*conditional*/true, enumDecl);
  patDecl->setImplicit();
  
  enumDecl->addMember(getterDecl);
  enumDecl->addMember(hashValueDecl);
  enumDecl->addMember(patDecl);
  return hashValueDecl;
}

ValueDecl *DerivedConformance::deriveHashable(TypeChecker &tc,
                                              NominalTypeDecl *type,
                                              ValueDecl *requirement) {
  // Check that we can actually derive Hashable for this type.
  if (!canDeriveConformance(type))
    return nullptr;
  
  // Build the necessary decl.
  if (requirement->getName().str() == "hashValue") {
    if (auto theEnum = dyn_cast<EnumDecl>(type))
      return deriveHashable_enum_hashValue(tc, theEnum);
    else
      llvm_unreachable("todo");
  }
  tc.diagnose(requirement->getLoc(),
              diag::broken_hashable_requirement);
  return nullptr;
}
