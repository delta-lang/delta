#include "typecheck.h"
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/ErrorHandling.h>
#include "../ast/decl.h"
#include "../ast/expr.h"
#include "../ast/mangle.h"
#include "../ast/module.h"
#include "../ast/type.h"
#include "../support/utility.h"

using namespace delta;

static void checkNotMoved(const Decl& decl, const VarExpr& expr) {
    const Movable* movable;

    switch (decl.getKind()) {
        case DeclKind::ParamDecl: movable = &llvm::cast<ParamDecl>(decl); break;
        case DeclKind::VarDecl: movable = &llvm::cast<VarDecl>(decl); break;
        default: return;
    }

    if (movable->isMoved()) {
        std::string typeInfo;

        if (expr.getType()) {
            typeInfo = " of type '" + expr.getType().toString() + "'";
        }

        error(expr.getLocation(), "use of moved value '", expr.getIdentifier(), "'", typeInfo);
    }
}

Type TypeChecker::typecheckVarExpr(VarExpr& expr, bool useIsWriteOnly) const {
    Decl& decl = findDecl(expr.getIdentifier(), expr.getLocation());
    expr.setDecl(&decl);

    switch (decl.getKind()) {
        case DeclKind::VarDecl:
            if (!useIsWriteOnly) checkNotMoved(decl, expr);
            return llvm::cast<VarDecl>(decl).getType();
        case DeclKind::ParamDecl:
            if (!useIsWriteOnly) checkNotMoved(decl, expr);
            return llvm::cast<ParamDecl>(decl).getType();
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl: return llvm::cast<FunctionDecl>(decl).getFunctionType();
        case DeclKind::GenericParamDecl: llvm_unreachable("cannot refer to generic parameters yet");
        case DeclKind::InitDecl: llvm_unreachable("cannot refer to initializers yet");
        case DeclKind::DeinitDecl: llvm_unreachable("cannot refer to deinitializers yet");
        case DeclKind::FunctionTemplate: llvm_unreachable("cannot refer to generic functions yet");
        case DeclKind::TypeDecl: error(expr.getLocation(), "'", expr.getIdentifier(), "' is not a variable");
        case DeclKind::TypeTemplate: llvm_unreachable("cannot refer to generic types yet");
        case DeclKind::FieldDecl:
            if (currentFunction->isInitDecl() || currentFunction->isDeinitDecl()) {
                return llvm::cast<FieldDecl>(decl).getType().asMutable();
            } else if (currentFunction->isMutating()) {
                return llvm::cast<FieldDecl>(decl).getType();
            } else {
                return llvm::cast<FieldDecl>(decl).getType().asImmutable();
            }
        case DeclKind::ImportDecl:
            llvm_unreachable("import statement validation not implemented yet");
    }
    abort();
}

Type typecheckStringLiteralExpr(StringLiteralExpr&) {
    return BasicType::get("StringRef", {});
}

Type typecheckCharacterLiteralExpr(CharacterLiteralExpr&) {
    return Type::getChar();
}

Type typecheckIntLiteralExpr(IntLiteralExpr& expr) {
    if (expr.getValue() >= std::numeric_limits<int32_t>::min() &&
        expr.getValue() <= std::numeric_limits<int32_t>::max()) {
        return Type::getInt();
    } else if (expr.getValue() >= std::numeric_limits<int64_t>::min() &&
               expr.getValue() <= std::numeric_limits<int64_t>::max()) {
        return Type::getInt64();
    }
    error(expr.getLocation(), "integer literal is too large");
}

Type typecheckFloatLiteralExpr(FloatLiteralExpr&) {
    return Type::getFloat64();
}

Type typecheckBoolLiteralExpr(BoolLiteralExpr&) {
    return Type::getBool();
}

Type typecheckNullLiteralExpr(NullLiteralExpr&) {
    return Type::getNull();
}

Type TypeChecker::typecheckArrayLiteralExpr(ArrayLiteralExpr& array) const {
    Type firstType = typecheckExpr(*array.getElements()[0]);
    for (auto& element : llvm::drop_begin(array.getElements(), 1)) {
        Type type = typecheckExpr(*element);
        if (type != firstType) {
            error(element->getLocation(), "mixed element types in array literal (expected '",
                  firstType, "', found '", type, "')");
        }
    }
    return ArrayType::get(firstType, int64_t(array.getElements().size()));
}

Type TypeChecker::typecheckTupleExpr(TupleExpr& expr) const {
    std::vector<Type> elementTypes;
    elementTypes.reserve(expr.getElements().size());

    for (auto& element : expr.getElements()) {
        elementTypes.push_back(typecheckExpr(*element));
    }

    return TupleType::get(std::move(elementTypes));
}

Type TypeChecker::typecheckPrefixExpr(PrefixExpr& expr) const {
    Type operandType = typecheckExpr(expr.getOperand());

    if (expr.getOperator() == NOT) {
        if (!operandType.isBool()) {
            error(expr.getOperand().getLocation(), "invalid operand type '", operandType,
                  "' to logical not");
        }
        return operandType;
    }
    if (expr.getOperator() == STAR) { // Dereference operation
        if (operandType.isOptionalType() && operandType.getWrappedType().isPointerType()) {
            error(expr.getOperand().getLocation(), "cannot dereference possibly-null pointer of type '",
                  operandType, "' (unwrap the value with '!' to access the pointer anyway)");
        } else if (!operandType.isPointerType()) {
            error(expr.getOperand().getLocation(), "cannot dereference non-pointer type '",
                  operandType, "'");
        }
        return operandType.getPointee();
    }
    if (expr.getOperator() == AND) { // Address-of operation
        return PointerType::get(operandType);
    }
    return operandType;
}

static void invalidOperandsToBinaryExpr(const BinaryExpr& expr) {
    std::string hint;

    if ((expr.getRHS().isNullLiteralExpr() || expr.getLHS().isNullLiteralExpr())
        && (expr.getOperator() == EQ || expr.getOperator() == NE)) {
        hint += " (non-optional type '";
        if (expr.getRHS().isNullLiteralExpr()) {
            hint += expr.getLHS().getType().toString();
        } else {
            hint += expr.getRHS().getType().toString();
        }
        hint += "' cannot be null)";
    } else {
        hint = "";
    }

    error(expr.getLocation(), "invalid operands '", expr.getLHS().getType(), "' and '",
          expr.getRHS().getType(), "' to '", expr.getFunctionName(), "'", hint);
}

Type TypeChecker::typecheckBinaryExpr(BinaryExpr& expr) const {
    Type leftType = typecheckExpr(expr.getLHS());
    Type rightType = typecheckExpr(expr.getRHS());

    if (!expr.isBuiltinOp()) {
        return typecheckCallExpr((CallExpr&) expr);
    }

    if (expr.getOperator() == AND_AND || expr.getOperator() == OR_OR) {
        if (leftType.isBool() && rightType.isBool()) {
            return Type::getBool();
        }
        invalidOperandsToBinaryExpr(expr);
    }

    if (leftType.isPointerType() && rightType.isInteger() &&
        (expr.getOperator() == PLUS || expr.getOperator() == MINUS)) {
        return leftType;
    }

    if (expr.getOperator().isBitwiseOperator() &&
        (leftType.isFloatingPoint() || rightType.isFloatingPoint())) {
        invalidOperandsToBinaryExpr(expr);
    }

    Type convertedType;

    if (isImplicitlyConvertible(&expr.getRHS(), rightType, leftType, &convertedType)) {
        expr.getRHS().setType(convertedType ? convertedType : rightType);
    } else if (isImplicitlyConvertible(&expr.getLHS(), leftType, rightType, &convertedType)) {
        expr.getLHS().setType(convertedType ? convertedType : leftType);
    } else {
        invalidOperandsToBinaryExpr(expr);
    }

    return expr.getOperator().isComparisonOperator() ? Type::getBool() : leftType;
}

template<int bitWidth, bool isSigned>
bool checkRange(const Expr& expr, int64_t value, Type type, Type* convertedType) {
    if ((isSigned && !llvm::APSInt::get(value).isSignedIntN(bitWidth)) ||
        (!isSigned && !llvm::APSInt::get(value).isIntN(bitWidth))) {
        error(expr.getLocation(), value, " is out of range for type '", type, "'");
    }

    if (convertedType) *convertedType = type;
    return true;
}

bool TypeChecker::isInterface(Type type) const {
    if (!type.isBasicType() || type.isBuiltinType() || type.isVoid()) return false;
    auto* typeDecl = getTypeDecl(llvm::cast<BasicType>(*type));
    return typeDecl && typeDecl->isInterface();
}

static bool hasField(TypeDecl& type, const FieldDecl& field) {
    return llvm::any_of(type.getFields(), [&](const FieldDecl& ownField) {
        return ownField.getName() == field.getName() && ownField.getType() == field.getType();
    });
}

bool TypeChecker::hasMethod(TypeDecl& type, FunctionDecl& functionDecl) const {
    auto decls = findDecls(mangleFunctionDecl(type.getType(), functionDecl.getName()));
    for (Decl* decl : decls) {
        if (!decl->isFunctionDecl()) continue;
        if (!llvm::cast<FunctionDecl>(decl)->getTypeDecl()) continue;
        if (llvm::cast<FunctionDecl>(decl)->getTypeDecl()->getName() != type.getName()) continue;
        if (!llvm::cast<FunctionDecl>(decl)->signatureMatches(functionDecl, /* matchReceiver: */ false)) continue;
        return true;
    }
    return false;
}

bool TypeChecker::implementsInterface(TypeDecl& type, TypeDecl& interface) const {
    for (auto& fieldRequirement : interface.getFields()) {
        if (!hasField(type, fieldRequirement)) {
            return false;
        }
    }
    for (auto& requiredMethod : interface.getMethods()) {
        if (auto* functionDecl = llvm::dyn_cast<FunctionDecl>(requiredMethod.get())) {
            if (!hasMethod(type, *functionDecl)) {
                return false;
            }
        } else {
            fatalError("non-function interface member requirements are not supported yet");
        }
    }
    return true;
}

bool TypeChecker::isImplicitlyConvertible(const Expr* expr, Type source, Type target, Type* convertedType) const {
    if (target.isOptionalType()) {
        if (isImplicitlyConvertible(expr, source, target.getWrappedType(), convertedType)) {
            return true;
        }
    }

    switch (source.getKind()) {
        case TypeKind::BasicType:
            if (target.isBasicType() && source.getName() == target.getName()
                && source.getGenericArgs() == target.getGenericArgs()) {
                return true;
            }
            break;
        case TypeKind::ArrayType:
            if (target.isArrayType()
                && (source.getArraySize() == target.getArraySize() || target.isUnsizedArrayType())
                && isImplicitlyConvertible(nullptr, source.getElementType(), target.getElementType(), nullptr)) {
                return true;
            }
            break;
        case TypeKind::TupleType:
            if (target.isTupleType() && source.getSubtypes() == target.getSubtypes()) {
                return true;
            }
            break;
        case TypeKind::FunctionType:
            if (target.isFunctionType() && source.getReturnType() == target.getReturnType()
                && source.getParamTypes() == target.getParamTypes()) {
                return true;
            }
            break;
        case TypeKind::PointerType:
            if (target.isPointerType()
                && (source.getPointee().isMutable() || !target.getPointee().isMutable())
                && isImplicitlyConvertible(nullptr, source.getPointee(), target.getPointee(), nullptr)) {
                return true;
            }
            break;
        case TypeKind::OptionalType:
            if (target.isOptionalType()
                && (source.getWrappedType().isMutable() || !target.getWrappedType().isMutable())
                && isImplicitlyConvertible(nullptr, source.getWrappedType(), target.getWrappedType(), nullptr)) {
                return true;
            }
            break;
    }

    if (isInterface(target) && source.isBasicType()) {
        if (implementsInterface(*getTypeDecl(llvm::cast<BasicType>(*source)),
                                *getTypeDecl(llvm::cast<BasicType>(*target)))) {
            return true;
        }
    }

    if (expr) {
        // Autocast integer literals to parameter type if within range, error out if not within range.
        if ((expr->isIntLiteralExpr() || expr->isCharacterLiteralExpr()) && target.isBasicType()) {
            int64_t value;

            if (auto* intLiteral = llvm::dyn_cast<IntLiteralExpr>(expr)) {
                value = intLiteral->getValue();
            } else {
                auto* charLiteral = llvm::cast<CharacterLiteralExpr>(expr);
                value = static_cast<unsigned char>(charLiteral->getValue());
            }

            if (target.isInteger()) {
                if (target.isInt()) return checkRange<32, true>(*expr, value, target, convertedType);
                if (target.isUInt()) return checkRange<32, false>(*expr, value, target, convertedType);
                if (target.isInt8()) return checkRange<8, true>(*expr, value, target, convertedType);
                if (target.isInt16()) return checkRange<16, true>(*expr, value, target, convertedType);
                if (target.isInt32()) return checkRange<32, true>(*expr, value, target, convertedType);
                if (target.isInt64()) return checkRange<64, true>(*expr, value, target, convertedType);
                if (target.isUInt8()) return checkRange<8, false>(*expr, value, target, convertedType);
                if (target.isUInt16()) return checkRange<16, false>(*expr, value, target, convertedType);
                if (target.isUInt32()) return checkRange<32, false>(*expr, value, target, convertedType);
                if (target.isUInt64()) return checkRange<64, false>(*expr, value, target, convertedType);
            }

            if (target.isFloatingPoint() && expr->isIntLiteralExpr()) {
                // TODO: Check that the integer value is losslessly convertible to the target type?
                if (convertedType) *convertedType = target;
                return true;
            }
        } else if (expr->isNullLiteralExpr() && target.isOptionalType()) {
            if (convertedType) *convertedType = target;
            return true;
        } else if (expr->isStringLiteralExpr()
                   && target.removeOptional().isPointerType()
                   && target.removeOptional().getPointee().isChar()
                   && !target.removeOptional().getPointee().isMutable()) {
            // Special case: allow passing string literals as C-strings (const char*).
            if (convertedType) *convertedType = target;
            return true;
        }
    }

    if (source.isBasicType() && target.removeOptional().isPointerType()
        && isImplicitlyConvertible(expr, source, target.removeOptional().getPointee(), nullptr)) {
        if (convertedType) *convertedType = source;
        return true;
    } else if (source.isArrayType() && target.removeOptional().isPointerType()
               && target.removeOptional().getPointee().isArrayType()
               && isImplicitlyConvertible(nullptr, source.getElementType(),
                                          target.removeOptional().getPointee().getElementType(), nullptr)) {
        if (convertedType) *convertedType = source;
        return true;
    } else if (source.isTupleType()) {
        auto elements = llvm::cast<TupleExpr>(expr)->getElements();
        std::vector<Type> convertedSubtypes;

        for (size_t i = 0; i < elements.size(); ++i) {
            Type convertedType;

            if (!isImplicitlyConvertible(elements[i].get(), source.getSubtypes()[i],
                                         target.getSubtypes()[i], &convertedType)) {
                return false;
            }

            convertedSubtypes.push_back(convertedType ? convertedType : source.getSubtypes()[i]);
        }

        if (convertedType) *convertedType = TupleType::get(std::move(convertedSubtypes));
        return true;
    }

    return false;
}

static bool containsGenericParam(Type type, llvm::StringRef genericParam) {
    switch (type.getKind()) {
        case TypeKind::BasicType:
            for (Type genericArg : type.getGenericArgs()) {
                if (containsGenericParam(genericArg, genericParam)) {
                    return true;
                }
            }
            return type.getName() == genericParam;
        case TypeKind::ArrayType:
            return containsGenericParam(type.getElementType(), genericParam);
        case TypeKind::TupleType:
            llvm_unreachable("unimplemented");
        case TypeKind::FunctionType:
            llvm_unreachable("unimplemented");
        case TypeKind::PointerType:
            return containsGenericParam(type.getPointee(), genericParam);
        case TypeKind::OptionalType:
            return containsGenericParam(type.getWrappedType(), genericParam);
    }
    llvm_unreachable("all cases handled");
}

static Type findGenericArg(Type argType, Type paramType, llvm::StringRef genericParam) {
    if (paramType.isBasicType() && paramType.getName() == genericParam) {
        return argType;
    }

    switch (argType.getKind()) {
        case TypeKind::BasicType:
            if (!argType.getGenericArgs().empty() && paramType.isBasicType()
                && paramType.getName() == argType.getName()) {
                ASSERT(argType.getGenericArgs().size() == paramType.getGenericArgs().size());
                for (auto t : llvm::zip_first(argType.getGenericArgs(), paramType.getGenericArgs())) {
                    if (Type type = findGenericArg(std::get<0>(t), std::get<1>(t), genericParam)) {
                        return type;
                    }
                }
            }
            return nullptr;
        case TypeKind::ArrayType:
            if (paramType.isArrayType()) {
                return findGenericArg(argType.getElementType(), paramType.getElementType(), genericParam);
            }
            return nullptr;
        case TypeKind::TupleType:
            llvm_unreachable("unimplemented");
        case TypeKind::FunctionType:
            llvm_unreachable("unimplemented");
        case TypeKind::PointerType:
            if (paramType.isPointerType()) {
                return findGenericArg(argType.getPointee(), paramType.getPointee(), genericParam);
            }
            return nullptr;
        case TypeKind::OptionalType:
            if (paramType.isOptionalType()) {
                return findGenericArg(argType.getWrappedType(), paramType.getWrappedType(), genericParam);
            }
            return nullptr;
    }
    llvm_unreachable("all cases handled");
}

std::vector<Type> TypeChecker::inferGenericArgs(llvm::ArrayRef<GenericParamDecl> genericParams,
                                                const CallExpr& call,
                                                llvm::ArrayRef<ParamDecl> params) const {
    if (call.getArgs().size() != params.size()) return {};

    std::vector<Type> inferredGenericArgs;

    for (auto& genericParam : genericParams) {
        Type genericArg;
        Expr* genericArgValue;

        for (auto tuple : llvm::zip_first(params, call.getArgs())) {
            Type paramType = std::get<0>(tuple).getType();

            if (containsGenericParam(paramType, genericParam.getName())) {
                // FIXME: The args will also be typechecked by validateArgs()
                // after this function. Get rid of this duplicated typechecking.
                auto* argValue = std::get<1>(tuple).getValue();
                Type argType = typecheckExpr(*argValue);
                Type maybeGenericArg = findGenericArg(argType, paramType, genericParam.getName());
                if (!maybeGenericArg) continue;
                Type convertedType;

                if (!genericArg) {
                    genericArg = maybeGenericArg;
                    genericArgValue = argValue;
                } else if (isImplicitlyConvertible(argValue, maybeGenericArg, genericArg, &convertedType)) {
                    argValue->setType(convertedType ? convertedType : maybeGenericArg);
                    continue;
                } else if (isImplicitlyConvertible(genericArgValue, genericArg, maybeGenericArg, &convertedType)) {
                    argValue->setType(convertedType ? convertedType : genericArg);
                    genericArg = maybeGenericArg.asMutable(genericArg.isMutable());
                    genericArgValue = argValue;
                } else {
                    error(call.getLocation(), "couldn't infer generic parameter '", genericParam.getName(),
                          "' of '", call.getFunctionName(), "' because of conflicting argument types '",
                          genericArg, "' and '", maybeGenericArg, "'");
                }
            }
        }

        if (genericArg) {
            inferredGenericArgs.push_back(genericArg);
        } else {
            return {};
        }
    }

    return inferredGenericArgs;
}

void validateGenericArgCount(size_t genericParamCount, llvm::ArrayRef<Type> genericArgs,
                             llvm::StringRef name, SourceLocation location) {
    if (genericArgs.size() < genericParamCount) {
        error(location, "too few generic arguments to '", name, "', expected ",
              genericParamCount);
    } else if (genericArgs.size() > genericParamCount) {
        error(location, "too many generic arguments to '", name, "', expected ",
              genericParamCount);
    }
}

llvm::StringMap<Type> TypeChecker::getGenericArgsForCall(llvm::ArrayRef<GenericParamDecl> genericParams,
                                                         CallExpr& call, llvm::ArrayRef<ParamDecl> params) const {
    ASSERT(!genericParams.empty());

    if (call.getGenericArgs().empty()) {
        if (call.getArgs().empty()) {
            error(call.getLocation(), "can't infer generic parameters without function arguments");
        }

        auto inferredGenericArgs = inferGenericArgs(genericParams, call, params);
        if (inferredGenericArgs.empty()) return {};
        call.setGenericArgs(std::move(inferredGenericArgs));
        ASSERT(call.getGenericArgs().size() == genericParams.size());
    }

    llvm::StringMap<Type> genericArgs;
    auto genericArg = call.getGenericArgs().begin();

    for (const GenericParamDecl& genericParam : genericParams) {
        if (!genericParam.getConstraints().empty()) {
            ASSERT(genericParam.getConstraints().size() == 1, "cannot have multiple generic constraints yet");

            auto interfaces = findDecls(genericParam.getConstraints()[0]);
            ASSERT(interfaces.size() == 1);

            if (genericArg->isBasicType() &&
                !implementsInterface(*getTypeDecl(llvm::cast<BasicType>(**genericArg)),
                                     llvm::cast<TypeDecl>(*interfaces[0]))) {
                error(call.getLocation(), "type '", *genericArg, "' doesn't implement interface '",
                      genericParam.getConstraints()[0], "'");
            }
        }

        genericArgs.try_emplace(genericParam.getName(), *genericArg++);
    }

    return genericArgs;
}

Type TypeChecker::typecheckBuiltinConversion(CallExpr& expr) const {
    if (expr.getArgs().size() != 1) {
        error(expr.getLocation(), "expected single argument to converting initializer");
    }
    if (!expr.getGenericArgs().empty()) {
        error(expr.getLocation(), "expected no generic arguments to converting initializer");
    }
    if (!expr.getArgs().front().getName().empty()) {
        error(expr.getLocation(), "expected unnamed argument to converting initializer");
    }
    typecheckExpr(*expr.getArgs().front().getValue());
    expr.setType(BasicType::get(expr.getFunctionName(), {}));
    return expr.getType();
}

Decl& TypeChecker::resolveOverload(CallExpr& expr, llvm::StringRef callee) const {
    llvm::SmallVector<Decl*, 1> matches;
    bool isInitCall = false;
    bool atLeastOneFunction = false;
    TypeDecl* receiverTypeDecl;

    if (expr.getReceiverType() && expr.getReceiverType().removePointer().isBasicType()) {
        receiverTypeDecl = getTypeDecl(*llvm::cast<BasicType>(expr.getReceiverType().removePointer().get()));
    } else {
        receiverTypeDecl = nullptr;
    }

    auto decls = findDecls(callee, isPostProcessing, receiverTypeDecl);

    for (Decl* decl : decls) {
        switch (decl->getKind()) {
            case DeclKind::FunctionTemplate: {
                auto* functionTemplate = llvm::cast<FunctionTemplate>(decl);
                auto genericParams = functionTemplate->getGenericParams();

                if (!expr.getGenericArgs().empty()
                    && expr.getGenericArgs().size() != genericParams.size()) {
                    if (decls.size() == 1) {
                        validateGenericArgCount(genericParams.size(), expr.getGenericArgs(),
                                                expr.getFunctionName(), expr.getLocation());
                    }
                    continue;
                }

                auto params = functionTemplate->getFunctionDecl()->getParams();
                auto genericArgs = getGenericArgsForCall(genericParams, expr, params);
                if (genericArgs.empty()) continue; // Couldn't infer generic arguments.

                auto* functionDecl = functionTemplate->instantiate(genericArgs);
                declsToTypecheck.emplace_back(functionDecl);

                if (decls.size() == 1) {
                    validateArgs(expr, *functionDecl, callee, expr.getCallee().getLocation());
                    return *functionDecl;
                }
                if (validateArgs(expr, *functionDecl)) {
                    matches.push_back(functionDecl);
                }
                break;
            }
            case DeclKind::FunctionDecl: case DeclKind::MethodDecl: {
                auto& functionDecl = llvm::cast<FunctionDecl>(*decl);

                if (decls.size() == 1) {
                    validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                    validateArgs(expr, functionDecl, callee, expr.getCallee().getLocation());
                    return functionDecl;
                }
                if (validateArgs(expr, functionDecl)) {
                    matches.push_back(&functionDecl);
                }
                break;
            }
            case DeclKind::TypeDecl: {
                isInitCall = true;
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                auto mangledName = mangleFunctionDecl(llvm::cast<TypeDecl>(decl)->getType(), "init");
                auto initDecls = findDecls(mangledName);

                for (Decl* decl : initDecls) {
                    InitDecl& initDecl = llvm::cast<InitDecl>(*decl);

                    if (initDecls.size() == 1) {
                        validateArgs(expr, initDecl, callee, expr.getCallee().getLocation());
                        return initDecl;
                    }
                    if (validateArgs(expr, initDecl)) {
                        matches.push_back(&initDecl);
                    }
                }
                break;
            }
            case DeclKind::TypeTemplate: {
                auto* typeTemplate = llvm::cast<TypeTemplate>(decl);
                isInitCall = true;

                std::vector<InitDecl*> initDecls;
                std::vector<InitDecl*> instantiatedInitDecls;

                for (auto& method : typeTemplate->getTypeDecl()->getMethods()) {
                    auto* initDecl = llvm::dyn_cast<InitDecl>(method.get());
                    if (!initDecl) continue;
                    initDecls.push_back(initDecl);
                }

                for (auto* initDecl : initDecls) {
                    auto genericArgs = getGenericArgsForCall(typeTemplate->getGenericParams(), expr, initDecl->getParams());
                    if (genericArgs.empty()) continue; // Couldn't infer generic arguments.

                    TypeDecl* typeDecl = nullptr;

                    auto decls = findDecls(mangleTypeDecl(typeTemplate->getTypeDecl()->getName(), expr.getGenericArgs()));
                    if (decls.empty()) {
                        typeDecl = typeTemplate->instantiate(genericArgs);
                        addToSymbolTable(*typeDecl);
                        typecheckTypeDecl(*typeDecl);
                    } else {
                        typeDecl = llvm::cast<TypeDecl>(decls[0]);
                    }

                    for (auto& method : typeDecl->getMethods()) {
                        auto* initDecl = llvm::dyn_cast<InitDecl>(method.get());
                        if (!initDecl) continue;
                        instantiatedInitDecls.push_back(initDecl);
                    }
                }

                for (auto* instantiatedInitDecl : instantiatedInitDecls) {
                    if (initDecls.size() == 1) {
                        validateArgs(expr, *instantiatedInitDecl, callee, expr.getCallee().getLocation());
                        return *instantiatedInitDecl;
                    }
                    if (validateArgs(expr, *instantiatedInitDecl)) {
                        matches.push_back(instantiatedInitDecl);
                    }
                }
                break;
            }
            case DeclKind::VarDecl: {
                auto* varDecl = llvm::cast<VarDecl>(decl);

                if (auto* functionType = llvm::dyn_cast<FunctionType>(varDecl->getType().get())) {
                    auto paramDecls = functionType->getParamDecls(varDecl->getLocation());
                    if (validateArgs(expr, false, paramDecls, false)) {
                        matches.push_back(varDecl);
                    }
                }
                break;
            }
            case DeclKind::ParamDecl: {
                auto* paramDecl = llvm::cast<ParamDecl>(decl);

                if (auto* functionType = llvm::dyn_cast<FunctionType>(paramDecl->getType().get())) {
                    auto paramDecls = functionType->getParamDecls(paramDecl->getLocation());
                    if (validateArgs(expr, false, paramDecls, false)) {
                        matches.push_back(paramDecl);
                    }
                }
                break;
            }
            case DeclKind::FieldDecl: {
                auto* fieldDecl = llvm::cast<FieldDecl>(decl);

                if (auto* functionType = llvm::dyn_cast<FunctionType>(fieldDecl->getType().get())) {
                    auto paramDecls = functionType->getParamDecls(fieldDecl->getLocation());
                    if (validateArgs(expr, false, paramDecls, false)) {
                        matches.push_back(fieldDecl);
                    }
                }
                break;
            }
            default:
                continue;
        }

        if (!decl->isVarDecl() && !decl->isParamDecl() && !decl->isFieldDecl()) {
            atLeastOneFunction = true;
        }
    }

    switch (matches.size()) {
        case 1: return *matches.front();
        case 0:
            if (decls.size() == 0) {
                error(expr.getCallee().getLocation(), "unknown identifier '", callee, "'");
            } else if (atLeastOneFunction) {
                auto argTypeStrings = map(expr.getArgs(), [](const Argument& arg) {
                    auto type = arg.getValue()->getType();
                    return type ? type.toString() : "???";
                });

                error(expr.getCallee().getLocation(), "no matching ",
                      isInitCall ? "initializer for '" : "function for call to '", callee,
                      "' with argument list of type '(", llvm::join(argTypeStrings, ", "), ")'");
            } else {
                error(expr.getCallee().getLocation(), "'", callee, "' is not a function");
            }
        default:
            if (expr.getReceiver() && expr.getReceiverType().removePointer().isMutable()) {
                llvm::SmallVector<Decl*, 1> mutatingMatches;

                for (auto* match : matches) {
                    if (!match->isMethodDecl() || llvm::cast<MethodDecl>(match)->isMutating()) {
                        mutatingMatches.push_back(match);
                    }
                }

                if (mutatingMatches.size() == 1) {
                    return *mutatingMatches.front();
                }
            }

            bool allMatchesAreFromC = llvm::all_of(matches, [](Decl* match) {
                return match->getModule() && match->getModule()->getName().endswith_lower(".h");
            });

            if (allMatchesAreFromC) {
                return *matches.front();
            }

            for (auto* match : matches) {
                if (match->getModule() && match->getModule()->getName() == "std") {
                    return *match;
                }
            }

            error(expr.getCallee().getLocation(), "ambiguous reference to '", callee,
                  isInitCall ? ".init'" : "'");
    }
}

Type TypeChecker::typecheckCallExpr(CallExpr& expr) const {
    if (!expr.callsNamedFunction()) {
        fatalError("anonymous function calls not implemented yet");
    }

    if (Type::isBuiltinScalar(expr.getFunctionName())) {
        return typecheckBuiltinConversion(expr);
    }

    Decl* decl;

    if (expr.getCallee().isMemberExpr()) {
        Type receiverType = typecheckExpr(*expr.getReceiver());
        expr.setReceiverType(receiverType);

        if (receiverType.isOptionalType()) {
            error(expr.getReceiver()->getLocation(),
                  "cannot call member function through value of optional type '", receiverType,
                  "' which may be null");
        } else if (receiverType.removePointer().isArrayType()) {
            if (expr.getFunctionName() == "size") {
                validateArgs(expr, false, {}, false, expr.getFunctionName(), expr.getLocation());
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                return Type::getInt();
            }

            error(expr.getReceiver()->getLocation(), "type '", receiverType, "' has no method '",
                  expr.getFunctionName(), "'");
        }

        decl = &resolveOverload(expr, expr.getMangledFunctionName());
    } else {
        decl = &resolveOverload(expr, expr.getFunctionName());

        if (decl->isMethodDecl() && !decl->isInitDecl()) {
            auto& varDecl = llvm::cast<VarDecl>(findDecl("this", expr.getCallee().getLocation()));
            expr.setReceiverType(varDecl.getType());
        }
    }

    std::vector<ParamDecl> paramsStorage;
    llvm::ArrayRef<ParamDecl> params;

    switch (decl->getKind()) {
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl:
        case DeclKind::InitDecl:
            params = llvm::cast<FunctionDecl>(decl)->getParams();
            break;

        case DeclKind::VarDecl: {
            auto type = llvm::cast<VarDecl>(decl)->getType();
            paramsStorage = llvm::cast<FunctionType>(type.get())->getParamDecls();
            params = paramsStorage;
            break;
        }
        case DeclKind::ParamDecl: {
            auto type = llvm::cast<ParamDecl>(decl)->getType();
            paramsStorage = llvm::cast<FunctionType>(type.get())->getParamDecls();
            params = paramsStorage;
            break;
        }
        case DeclKind::FieldDecl: {
            auto type = llvm::cast<FieldDecl>(decl)->getType();
            paramsStorage = llvm::cast<FunctionType>(type.get())->getParamDecls();
            params = paramsStorage;
            break;
        }
        default:
            llvm_unreachable("invalid callee decl");
    }

    for (auto t : llvm::zip_first(params, expr.getArgs()) ) {
        if (!isImplicitlyCopyable(std::get<0>(t).getType())) {
            std::get<1>(t).getValue()->setMoved(true);
        }
    }

    if (decl->isMethodDecl() && !decl->isInitDecl()) {
        ASSERT(expr.getReceiverType());
        auto* methodDecl = llvm::cast<MethodDecl>(decl);

        if (!decl->isDeinitDecl() && !expr.getReceiverType().removePointer().isMutable()
            && methodDecl->isMutating()) {
            error(expr.getCallee().getLocation(), "cannot call mutating function '",
                  methodDecl->getTypeDecl()->getName(), ".", methodDecl->getName(),
                  "' on immutable receiver");
        }
    }

    expr.setCalleeDecl(decl);

    switch (decl->getKind()) {
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl:
            return llvm::cast<FunctionDecl>(decl)->getFunctionType()->getReturnType();

        case DeclKind::InitDecl:
            return llvm::cast<InitDecl>(decl)->getTypeDecl()->getType();

        case DeclKind::VarDecl:
            return llvm::cast<FunctionType>(llvm::cast<VarDecl>(decl)->getType().get())->getReturnType();

        case DeclKind::ParamDecl:
            return llvm::cast<FunctionType>(llvm::cast<ParamDecl>(decl)->getType().get())->getReturnType();

        case DeclKind::FieldDecl:
            return llvm::cast<FunctionType>(llvm::cast<FieldDecl>(decl)->getType().get())->getReturnType();

        default:
            llvm_unreachable("invalid callee decl");
    }
}

bool TypeChecker::isImplicitlyCopyable(Type type) const {
    switch (type.getKind()) {
        case TypeKind::BasicType:
            if (auto* typeDecl = getTypeDecl(llvm::cast<BasicType>(*type))) {
                return typeDecl->passByValue();
            }
            return true;
        case TypeKind::ArrayType:
            return false;
        case TypeKind::TupleType:
            return llvm::all_of(llvm::cast<TupleType>(*type).getSubtypes(), [&](Type type) {
                return isImplicitlyCopyable(type);
            });
        case TypeKind::FunctionType:
            return true;
        case TypeKind::PointerType:
            return true;
        case TypeKind::OptionalType:
            return isImplicitlyCopyable(type.getWrappedType());
    }
    llvm_unreachable("all cases handled");
}

bool TypeChecker::validateArgs(const CallExpr& expr, const FunctionDecl& functionDecl,
                               llvm::StringRef functionName, SourceLocation location) const {
    return validateArgs(expr, functionDecl.isMutating(), functionDecl.getParams(),
                        functionDecl.isVariadic(), functionName, location);
}

bool TypeChecker::validateArgs(const CallExpr& expr, bool isMutating, llvm::ArrayRef<ParamDecl> params,
                               bool isVariadic, llvm::StringRef functionName,
                               SourceLocation location) const {
    llvm::ArrayRef<Argument> args = expr.getArgs();
    bool returnOnError = functionName.empty();

    if (expr.getReceiver() && !expr.getReceiverType().removePointer().isMutable() && isMutating) {
        if (returnOnError) return false;
        error(location, "cannot call mutating method '", functionName,
              "' on immutable receiver of type '", expr.getReceiverType(), "'");
    }

    if (args.size() < params.size()) {
        if (returnOnError) return false;
        error(location, "too few arguments to '", functionName, "', expected ",
              isVariadic ? "at least " : "", params.size());
    }
    if (!isVariadic && args.size() > params.size()) {
        if (returnOnError) return false;
        error(location, "too many arguments to '", functionName, "', expected ", params.size());
    }

    for (size_t i = 0; i < args.size(); ++i) {
        const Argument& arg = args[i];
        const ParamDecl* param = i < params.size() ? &params[i] : nullptr;

        if (!arg.getName().empty() && (!param || arg.getName() != param->getName())) {
            if (returnOnError) return false;
            error(arg.getLocation(), "invalid argument name '", arg.getName(), "' for parameter '", param->getName(), "'");
        }

        auto argType = typecheckExpr(*arg.getValue());

        if (param) {
            Type convertedType;

            if (isImplicitlyConvertible(arg.getValue(), argType, param->getType(), &convertedType)) {
                arg.getValue()->setType(convertedType ? convertedType : argType);
            } else {
                if (returnOnError) return false;
                error(arg.getLocation(), "invalid argument #", i + 1, " type '", argType, "' to '", functionName,
                      "', expected '", param->getType(), "'");
            }
        }
    }

    return true;
}

static bool isValidCast(Type sourceType, Type targetType) {
    switch (sourceType.getKind()) {
        case TypeKind::BasicType:
        case TypeKind::ArrayType:
        case TypeKind::TupleType:
        case TypeKind::FunctionType:
            return false;

        case TypeKind::PointerType: {
            Type sourcePointee = sourceType.getPointee();

            if (targetType.isPointerType()) {
                Type targetPointee = targetType.getPointee();

                if (sourcePointee.isVoid() &&
                    (!targetPointee.isMutable() || sourcePointee.isMutable())) {
                    return true; // (mutable) void* -> T* / mutable void* -> mutable T*
                }

                if (targetPointee.isVoid() &&
                    (!targetPointee.isMutable() || sourcePointee.isMutable())) {
                    return true; // (mutable) T* -> void* / mutable T* -> mutable void*
                }
            }

            return false;
        }
        case TypeKind::OptionalType: {
            Type sourceWrappedType = sourceType.getWrappedType();

            if (sourceWrappedType.isPointerType() && targetType.isOptionalType()) {
                Type targetWrappedType = targetType.getWrappedType();

                if (targetWrappedType.isPointerType()
                    && isValidCast(sourceWrappedType, targetWrappedType)) {
                    return true;
                }
            }

            return false;
        }
    }
}

Type TypeChecker::typecheckCastExpr(CastExpr& expr) const {
    Type sourceType = typecheckExpr(expr.getExpr());
    Type targetType = expr.getTargetType();

    if (isValidCast(sourceType, targetType)) {
        return targetType;
    }

    error(expr.getLocation(), "illegal cast from '", sourceType, "' to '", targetType, "'");
}

Type TypeChecker::typecheckSizeofExpr(SizeofExpr&) const {
    return Type::getUInt64();
}

Type TypeChecker::typecheckMemberExpr(MemberExpr& expr) const {
    Type baseType = typecheckExpr(*expr.getBaseExpr());

    if (baseType.isPointerType()) {
        baseType = baseType.getPointee();
    }

    if (baseType.isOptionalType()) {
        error(expr.getBaseExpr()->getLocation(), "cannot access member through value of optional type '",
              baseType, "' which may be null");
    }

    if (baseType.isArrayType()) {
        auto sizeSynonyms = { "count", "length", "size" };

        if (llvm::is_contained(sizeSynonyms, expr.getMemberName())) {
            error(expr.getLocation(), "use the '.size()' method to get the number of elements in an array");
        }
    }

    if (baseType.isBasicType()) {
        Decl& typeDecl = findDecl(mangleTypeDecl(baseType.getName(), baseType.getGenericArgs()),
                                  expr.getBaseExpr()->getLocation());

        for (auto& field : llvm::cast<TypeDecl>(typeDecl).getFields()) {
            if (field.getName() == expr.getMemberName()) {
                if (baseType.isMutable()) {
                    auto* varExpr = llvm::dyn_cast<VarExpr>(expr.getBaseExpr());

                    if (varExpr && varExpr->getIdentifier() == "this"
                        && (currentFunction->isInitDecl() || currentFunction->isDeinitDecl())) {
                        return field.getType().asMutable(true);
                    } else {
                        return field.getType();
                    }
                } else {
                    return field.getType().asImmutable();
                }
            }
        }
    }

    error(expr.getLocation(), "no member named '", expr.getMemberName(), "' in '", baseType, "'");
}

Type TypeChecker::typecheckSubscriptExpr(SubscriptExpr& expr) const {
    Type lhsType = typecheckExpr(*expr.getBaseExpr());
    Type arrayType;

    if (lhsType.isArrayType()) {
        arrayType = lhsType;
    } else if (lhsType.isPointerType() && lhsType.getPointee().isArrayType()) {
        arrayType = lhsType.getPointee();
    } else if (lhsType.removePointer().isBuiltinType()) {
        error(expr.getLocation(), "'", lhsType, "' doesn't provide a subscript operator");
    } else {
        return typecheckCallExpr(expr);
    }

    Type indexType = typecheckExpr(*expr.getIndexExpr());
    Type convertedType;

    if (isImplicitlyConvertible(expr.getIndexExpr(), indexType, Type::getInt(), &convertedType)) {
        expr.getIndexExpr()->setType(convertedType ? convertedType : indexType);
    } else {
        error(expr.getIndexExpr()->getLocation(), "illegal subscript index type '", indexType,
              "', expected 'int'");
    }

    if (!arrayType.isUnsizedArrayType()) {
        if (auto* intLiteralExpr = llvm::dyn_cast<IntLiteralExpr>(expr.getIndexExpr())) {
            if (intLiteralExpr->getValue() >= arrayType.getArraySize()) {
                error(intLiteralExpr->getLocation(), "accessing array out-of-bounds with index ",
                      intLiteralExpr->getValue(), ", array size is ", arrayType.getArraySize());
            }
        }
    }

    return arrayType.getElementType();
}

Type TypeChecker::typecheckUnwrapExpr(UnwrapExpr& expr) const {
    Type type = typecheckExpr(expr.getOperand());
    if (!type.isOptionalType()) {
        error(expr.getLocation(), "cannot unwrap non-optional type '", type, "'");
    }
    return type.getWrappedType();
}

Type TypeChecker::typecheckExpr(Expr& expr, bool useIsWriteOnly) const {
    llvm::Optional<Type> type;

    switch (expr.getKind()) {
        case ExprKind::VarExpr: type = typecheckVarExpr(llvm::cast<VarExpr>(expr), useIsWriteOnly); break;
        case ExprKind::StringLiteralExpr: type = typecheckStringLiteralExpr(llvm::cast<StringLiteralExpr>(expr)); break;
        case ExprKind::CharacterLiteralExpr: type = typecheckCharacterLiteralExpr(llvm::cast<CharacterLiteralExpr>(expr)); break;
        case ExprKind::IntLiteralExpr: type = typecheckIntLiteralExpr(llvm::cast<IntLiteralExpr>(expr)); break;
        case ExprKind::FloatLiteralExpr: type = typecheckFloatLiteralExpr(llvm::cast<FloatLiteralExpr>(expr)); break;
        case ExprKind::BoolLiteralExpr: type = typecheckBoolLiteralExpr(llvm::cast<BoolLiteralExpr>(expr)); break;
        case ExprKind::NullLiteralExpr: type = typecheckNullLiteralExpr(llvm::cast<NullLiteralExpr>(expr)); break;
        case ExprKind::ArrayLiteralExpr: type = typecheckArrayLiteralExpr(llvm::cast<ArrayLiteralExpr>(expr)); break;
        case ExprKind::TupleExpr: type = typecheckTupleExpr(llvm::cast<TupleExpr>(expr)); break;
        case ExprKind::PrefixExpr: type = typecheckPrefixExpr(llvm::cast<PrefixExpr>(expr)); break;
        case ExprKind::BinaryExpr: type = typecheckBinaryExpr(llvm::cast<BinaryExpr>(expr)); break;
        case ExprKind::CallExpr: type = typecheckCallExpr(llvm::cast<CallExpr>(expr)); break;
        case ExprKind::CastExpr: type = typecheckCastExpr(llvm::cast<CastExpr>(expr)); break;
        case ExprKind::SizeofExpr: type = typecheckSizeofExpr(llvm::cast<SizeofExpr>(expr)); break;
        case ExprKind::MemberExpr: type = typecheckMemberExpr(llvm::cast<MemberExpr>(expr)); break;
        case ExprKind::SubscriptExpr: type = typecheckSubscriptExpr(llvm::cast<SubscriptExpr>(expr)); break;
        case ExprKind::UnwrapExpr: type = typecheckUnwrapExpr(llvm::cast<UnwrapExpr>(expr)); break;
    }

    expr.setType(*type);
    return expr.getType();
}
