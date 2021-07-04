#include "typecheck.h"
#include <algorithm>
#include <limits>
#include <string>
#include <vector>
#pragma warning(push, 0)
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/ErrorHandling.h>
#pragma warning(pop)
#include "../ast/decl.h"
#include "../ast/expr.h"
#include "../ast/module.h"
#include "../ast/type.h"

using namespace delta;

void Typechecker::checkHasAccess(const Decl& decl, SourceLocation location, AccessLevel userAccessLevel) {
    // FIXME: Compare SourceFile objects instead of file path strings.
    if (decl.getAccessLevel() == AccessLevel::Private && strcmp(decl.getLocation().file, location.file) != 0) {
        WARN(location, "'" << decl.getName() << "' is private");
    } else if (userAccessLevel != AccessLevel::None && decl.getAccessLevel() < userAccessLevel) {
        WARN(location, "using " << decl.getAccessLevel() << " type '" << decl.getName() << "' in " << userAccessLevel << " declaration");
    }
}

void Typechecker::checkLambdaCapture(const VariableDecl& variableDecl, const VarExpr& varExpr) const {
    auto* parent = getCurrentModule()->getSymbolTable().getCurrentScope().parent;
    if (parent && parent->isLambda() && variableDecl.getParentDecl() != parent) {
        ERROR(varExpr.getLocation(), "lambda capturing not implemented yet");
    }
}

Type Typechecker::typecheckVarExpr(VarExpr& expr, bool useIsWriteOnly) {
    auto* decl = findDecl(expr.getIdentifier(), expr.getLocation());
    checkHasAccess(*decl, expr.getLocation(), AccessLevel::None);
    decl->setReferenced(true);
    expr.setDecl(decl);

    if (auto variableDecl = llvm::dyn_cast<VariableDecl>(decl)) {
        checkLambdaCapture(*variableDecl, expr);
    }

    switch (decl->getKind()) {
        case DeclKind::VarDecl:
            if (!useIsWriteOnly) checkNotMoved(*decl, expr);
            return llvm::cast<VarDecl>(decl)->getType();
        case DeclKind::ParamDecl:
            if (!useIsWriteOnly) checkNotMoved(*decl, expr);
            return llvm::cast<ParamDecl>(decl)->getType();
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl:
            return Type(llvm::cast<FunctionDecl>(decl)->getFunctionType(), Mutability::Mutable, SourceLocation());
        case DeclKind::GenericParamDecl:
            llvm_unreachable("cannot refer to generic parameters yet");
        case DeclKind::ConstructorDecl:
            llvm_unreachable("cannot refer to constructors yet");
        case DeclKind::DestructorDecl:
            llvm_unreachable("cannot refer to destructors yet");
        case DeclKind::FunctionTemplate:
            llvm_unreachable("cannot refer to generic functions yet");
        case DeclKind::TypeDecl:
            return llvm::cast<TypeDecl>(decl)->getType();
        case DeclKind::TypeTemplate:
            llvm_unreachable("cannot refer to generic types yet");
        case DeclKind::EnumDecl:
            ERROR(expr.getLocation(), "'" << expr.getIdentifier() << "' is not a variable");
        case DeclKind::EnumCase:
            return llvm::cast<EnumCase>(decl)->getType();
        case DeclKind::FieldDecl:
            return llvm::cast<FieldDecl>(decl)->getType();
        case DeclKind::ImportDecl:
            llvm_unreachable("import statement validation not implemented yet");
    }
    llvm_unreachable("all cases handled");
}

Type typecheckStringLiteralExpr(StringLiteralExpr&) {
    return BasicType::get("string", {});
}

Type typecheckCharacterLiteralExpr(CharacterLiteralExpr&) {
    return Type::getChar();
}

Type typecheckIntLiteralExpr(IntLiteralExpr& expr) {
    if (expr.getValue().isSignedIntN(32)) {
        return Type::getInt();
    } else if (expr.getValue().isSignedIntN(64)) {
        return Type::getInt64();
    } else if (expr.getValue().isIntN(64)) {
        return Type::getUInt64();
    }
    ERROR(expr.getLocation(), "integer literal is too large");
}

Type typecheckFloatLiteralExpr(FloatLiteralExpr&) {
    return Type::getFloat();
}

Type typecheckBoolLiteralExpr(BoolLiteralExpr&) {
    return Type::getBool();
}

Type Typechecker::typecheckNullLiteralExpr(NullLiteralExpr&, Type expectedType) {
    if (expectedType && expectedType.isOptionalType() && !expectedType.containsUnresolvedPlaceholder()) {
        return expectedType;
    } else {
        return Type::getNull();
    }
}

Type typecheckUndefinedLiteralExpr(UndefinedLiteralExpr&, Type expectedType) {
    ASSERT(expectedType && !expectedType.containsUnresolvedPlaceholder());
    return expectedType;
}

Type Typechecker::typecheckArrayLiteralExpr(ArrayLiteralExpr& array, Type expectedType) {
    if (array.getElements().empty()) {
        if (expectedType) {
            return expectedType;
        } else {
            ERROR(array.getLocation(), "couldn't infer type of empty array literal");
        }
    }

    Type firstType = typecheckExpr(*array.getElements()[0]);

    for (auto& element : array.getElements().drop_front()) {
        Type type = typecheckExpr(*element);
        if (type != firstType) {
            ERROR(element->getLocation(), "mixed element types in array literal (expected '" << firstType << "', found '" << type << "')");
        }
    }

    return ArrayType::get(firstType, int64_t(array.getElements().size()));
}

Type Typechecker::typecheckTupleExpr(TupleExpr& expr) {
    auto elements = map(expr.getElements(), [&](NamedValue& namedValue) {
        return TupleElement { namedValue.getName().str(), typecheckExpr(*namedValue.getValue()) };
    });
    return TupleType::get(std::move(elements));
}

Type Typechecker::typecheckUnaryExpr(UnaryExpr& expr) {
    Type operandType = typecheckExpr(expr.getOperand());

    switch (expr.getOperator()) {
        case Token::Not:
            operandType = operandType.removePointer();

            if (!operandType.isBool() && !operandType.isOptionalType()) {
                ERROR(expr.getOperand().getLocation(), "invalid operand type '" << operandType << "' to logical not");
            }

            return Type::getBool();

        case Token::Star: // Dereference operation
            if (operandType.removeOptional().isPointerType()) {
                return operandType.removeOptional().getPointee();
            } else if (operandType.removeOptional().isArrayWithUnknownSize()) {
                return operandType.removeOptional().getElementType();
            }

            ERROR(expr.getLocation(), "cannot dereference non-pointer type '" << operandType << "'");

        case Token::Ref: // Address-of operation
            // Allow forming mutable pointers to constants. This is safe because constants will be inlined at the usage site.
            if (expr.isConstant()) {
                operandType = operandType.withMutability(Mutability::Mutable);
            }
            return PointerType::get(operandType);

        case Token::Increment:
            operandType = operandType.removePointer();

            if (!operandType.isMutable()) {
                ERROR(expr.getLocation(), "cannot increment immutable value of type '" << operandType << "'");
            } else if (!operandType.isIncrementable()) {
                ERROR(expr.getLocation(), "cannot increment '" << operandType << "'");
            }

            return Type::getVoid();

        case Token::Decrement:
            operandType = operandType.removePointer();

            if (!operandType.isMutable()) {
                ERROR(expr.getLocation(), "cannot decrement immutable value of type '" << operandType << "'");
            } else if (!operandType.isDecrementable()) {
                ERROR(expr.getLocation(), "cannot decrement '" << operandType << "'");
            }

            return Type::getVoid();

        default:
            return operandType;
    }
}

static void invalidOperandsToBinaryExpr(const BinaryExpr& expr, Token::Kind op) {
    std::string hint;

    if ((expr.getRHS().isNullLiteralExpr() || expr.getLHS().isNullLiteralExpr()) && (op == Token::Equal || op == Token::NotEqual)) {
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

    ERROR(expr.getLocation(), "invalid operands '" << expr.getLHS().getType() << "' and '" << expr.getRHS().getType() << "' to '" << toString(op) << "'" << hint);
}

static bool allowAssignmentOfUndefined(const Expr& lhs, const FunctionDecl* currentFunction) {
    if (auto* constructorDecl = llvm::dyn_cast<ConstructorDecl>(currentFunction)) {
        switch (lhs.getKind()) {
            case ExprKind::VarExpr: {
                auto* fieldDecl = llvm::dyn_cast<FieldDecl>(llvm::cast<VarExpr>(lhs).getDecl());
                return fieldDecl && fieldDecl->getParentDecl() == constructorDecl->getTypeDecl();
            }
            case ExprKind::MemberExpr: {
                auto* varExpr = llvm::dyn_cast<VarExpr>(llvm::cast<MemberExpr>(lhs).getBaseExpr());
                return varExpr && varExpr->getIdentifier() == "this";
            }
            default:
                return false;
        }
    }
    return false;
}

Type Typechecker::typecheckBinaryExpr(BinaryExpr& expr) {
    auto op = expr.getOperator();

    if (op == Token::Assignment) {
        typecheckAssignment(expr, expr.getLocation());
        return Type::getVoid();
    }

    if (isCompoundAssignmentOperator(op)) {
        auto rhs = new BinaryExpr(withoutCompoundEqSuffix(op), &expr.getLHS(), &expr.getRHS(), expr.getLocation());
        expr = BinaryExpr(Token::Assignment, &expr.getLHS(), rhs, expr.getLocation());
        return typecheckBinaryExpr(expr);
    }

    Type leftType = typecheckExpr(expr.getLHS());
    Type rightType = typecheckExpr(expr.getRHS(), false, leftType);

    if (!isBuiltinOp(op, leftType, rightType)) {
        return typecheckCallExpr(expr);
    }

    if (op == Token::AndAnd || op == Token::OrOr) {
        if (leftType.isBool() && rightType.isBool()) {
            return Type::getBool();
        }
        invalidOperandsToBinaryExpr(expr, op);
    }

    if (op == Token::PointerEqual || op == Token::PointerNotEqual) {
        if (!leftType.isImplementedAsPointer() || !rightType.isImplementedAsPointer()) {
            ERROR(expr.getLocation(), "both operands to pointer comparison operator must have pointer type");
        }

        auto leftPointeeType = leftType.removeOptional().removePointer().removeArrayWithUnknownSize();
        auto rightPointeeType = rightType.removeOptional().removePointer().removeArrayWithUnknownSize();

        if (!leftPointeeType.equalsIgnoreTopLevelMutable(rightPointeeType)) {
            ERROR(expr.getLocation(), "comparison of distinct pointer types ('" << leftType << "' and '" << rightType << "')");
        }
    }

    if (isBitwiseOperator(op) && (leftType.isFloatingPoint() || rightType.isFloatingPoint())) {
        invalidOperandsToBinaryExpr(expr, op);
    }

    if (auto convertedRHS = convert(&expr.getRHS(), leftType, true)) {
        expr.setRHS(convertedRHS);
    } else if (auto convertedLHS = convert(&expr.getLHS(), rightType, true)) {
        expr.setLHS(convertedLHS);
    } else if ((!leftType.removeOptional().isPointerType() || !rightType.removeOptional().isPointerType())) {
        invalidOperandsToBinaryExpr(expr, op);
    }

    return isComparisonOperator(op) ? Type::getBool() : expr.getLHS().getType();
}

void Typechecker::typecheckAssignment(BinaryExpr& expr, SourceLocation location) {
    auto* lhs = &expr.getLHS();
    auto* rhs = &expr.getRHS();

    typecheckExpr(*lhs, true);
    Type lhsType = lhs->getAssignableType();
    Type rhsType = typecheckExpr(*rhs, false, lhsType);

    if (rhs->isUndefinedLiteralExpr() && !allowAssignmentOfUndefined(*lhs, currentFunction)) {
        ERROR(rhs->getLocation(), "'undefined' is only allowed as an initial value");
    }

    if (auto converted = convert(rhs, lhsType)) {
        expr.setRHS(converted);
        rhs = converted;
    } else {
        ERROR(location, "cannot assign '" << rhsType << "' to '" << lhsType << "'");
    }

    if (!lhsType.isMutable()) {
        switch (lhs->getKind()) {
            case ExprKind::VarExpr: {
                auto identifier = llvm::cast<VarExpr>(lhs)->getIdentifier();
                ERROR(location, "cannot assign to immutable variable '" << identifier << "' of type '" << lhsType << "'");
            }
            case ExprKind::MemberExpr: {
                auto memberName = llvm::cast<MemberExpr>(lhs)->getMemberName();
                ERROR(location, "cannot assign to immutable variable '" << memberName << "' of type '" << lhsType << "'");
            }
            default:
                ERROR(location, "cannot assign to immutable expression of type '" << lhsType << "'");
        }
    }

    if (!rhsType.isImplicitlyCopyable() && !lhsType.removeOptional().isPointerType()) {
        setMoved(rhs, true);
        setMoved(lhs, false);
    }

    if (currentInitializedFields) {
        if (auto fieldDecl = lhs->getFieldDecl()) {
            currentInitializedFields->insert(fieldDecl);
        }
    }
}

static void checkRange(const Expr& expr, const llvm::APSInt& value, Type type) {
    if (llvm::APSInt::compareValues(value, llvm::APSInt::getMinValue(type.getIntegerBitWidth(), type.isUnsigned())) < 0 ||
        llvm::APSInt::compareValues(value, llvm::APSInt::getMaxValue(type.getIntegerBitWidth(), type.isUnsigned())) > 0) {
        ERROR(expr.getLocation(), value << " is out of range for type '" << type << "'");
    }
}

static bool hasField(TypeDecl& type, const FieldDecl& field) {
    return llvm::any_of(type.getFields(), [&](const FieldDecl& f) { return f.getName() == field.getName() && f.getType() == field.getType(); });
}

bool Typechecker::hasMethod(TypeDecl& type, FunctionDecl& functionDecl) const {
    auto decls = findDecls(getQualifiedFunctionName(type.getType(), functionDecl.getName(), {}));

    for (Decl* decl : decls) {
        if (!decl->isFunctionDecl()) continue;
        if (!llvm::cast<FunctionDecl>(decl)->getTypeDecl()) continue;
        if (llvm::cast<FunctionDecl>(decl)->getTypeDecl()->getName() != type.getName()) continue;
        if (!llvm::cast<FunctionDecl>(decl)->signatureMatches(functionDecl, /* matchReceiver: */ false)) continue;
        return true;
    }

    return false;
}

bool Typechecker::providesInterfaceRequirements(TypeDecl& type, TypeDecl& interface, std::string* errorReason) const {
    auto thisTypeResolvedInterface = llvm::cast<TypeDecl>(interface.instantiate({ { "This", type.getType() } }, {}));

    for (auto& fieldRequirement : thisTypeResolvedInterface->getFields()) {
        if (!hasField(type, fieldRequirement)) {
            if (errorReason) {
                *errorReason = ("doesn't have field '" + fieldRequirement.getName() + "'").str();
            }
            return false;
        }
    }

    for (auto& requiredMethod : thisTypeResolvedInterface->getMethods()) {
        if (auto* functionDecl = llvm::dyn_cast<FunctionDecl>(requiredMethod)) {
            if (functionDecl->hasBody()) continue;

            if (!hasMethod(type, *functionDecl)) {
                if (errorReason) {
                    *errorReason = ("doesn't have member function '" + functionDecl->getName() + "'").str();
                }
                return false;
            }
        } else {
            ERROR(requiredMethod->getLocation(), "non-function interface member requirements are not supported yet");
        }
    }

    return true;
}

Expr* Typechecker::convert(Expr* expr, Type type, bool allowPointerToTemporary) const {
    llvm::Optional<ImplicitCastExpr::Kind> implicitCastKind;
    if (Type convertedType = isImplicitlyConvertible(expr, expr->getType(), type, allowPointerToTemporary, &implicitCastKind)) {
        if (implicitCastKind) {
            return new ImplicitCastExpr(expr, convertedType, *implicitCastKind);
        } else if (convertedType != expr->getType()) {
            expr->setType(convertedType);

            if (auto* ifExpr = llvm::dyn_cast<IfExpr>(expr)) {
                ifExpr->getThenExpr()->setType(convertedType);
                ifExpr->getElseExpr()->setType(convertedType);
            }
        }
        return expr;
    }
    return nullptr;
}

Type Typechecker::isImplicitlyConvertible(const Expr* expr, Type source, Type target, bool allowPointerToTemporary,
                                          llvm::Optional<ImplicitCastExpr::Kind>* implicitCastKind) const {
    if (source.isBasicType() && target.isBasicType() && source.getName() == target.getName() && source.getGenericArgs() == target.getGenericArgs()) {
        return source;
    }

    if (source.isArrayType() && target.isArrayType() && source.getElementType() == target.getElementType()) {
        if (source.getArraySize() == target.getArraySize()) return source;
        if (source.isArrayWithConstantSize() && (target.isArrayWithUnknownSize() || target.isArrayWithRuntimeSize())) return source;
    }

    if (source.isTupleType() && target.isTupleType() && source.getTupleElements() == target.getTupleElements()) {
        return source;
    }

    if (source.isFunctionType() && target.isFunctionType() && source.getReturnType() == target.getReturnType() &&
        source.getParamTypes() == target.getParamTypes()) {
        return source;
    }

    if (source.isPointerType() && target.isPointerType() && (source.getPointee().isMutable() || !target.getPointee().isMutable()) &&
        (isImplicitlyConvertible(nullptr, source.getPointee(), target.getPointee()) || target.getPointee().isVoid())) {
        return source;
    }

    if (source.isArrayWithUnknownSize() && target.isArrayWithUnknownSize() && (source.getElementType().isMutable() || !target.getElementType().isMutable()) &&
        source.getElementType().equalsIgnoreTopLevelMutable(target.getElementType())) {
        return target;
    }

    if (source.isOptionalType() && target.isOptionalType() && (source.getWrappedType().isMutable() || !target.getWrappedType().isMutable()) &&
        isImplicitlyConvertible(nullptr, source.getWrappedType(), target.getWrappedType())) {
        return source;
    }

    if (expr) {
        if (expr->getType().isEnumType() && llvm::cast<EnumDecl>(expr->getType().getDecl())->getTagType() == target) {
            return source;
        }

        if (auto* ifExpr = llvm::dyn_cast<IfExpr>(expr)) {
            if (isImplicitlyConvertible(ifExpr->getThenExpr(), ifExpr->getThenExpr()->getType(), target) &&
                isImplicitlyConvertible(ifExpr->getElseExpr(), ifExpr->getElseExpr()->getType(), target)) {
                return target;
            }
        }

        // Auto-cast integer constants to target type if within range, error out if not within range.
        if ((expr->getType().isInteger() || expr->getType().isChar() || expr->getType().isEnumType()) && expr->isConstant()) {
            auto value = expr->getConstantIntegerValue();
            auto adjustedTarget = allowPointerToTemporary ? target.removePointer() : target; // Convert e.g. int literal to uint when comparing to uint*.

            if (adjustedTarget.isInteger()) {
                checkRange(*expr, value, adjustedTarget);
                return adjustedTarget;
            }

            if (adjustedTarget.isFloatingPoint()) {
                // TODO: Check that the integer value is losslessly convertible to the target type?
                return adjustedTarget;
            }
        }

        if (expr->getType().isFloatingPoint() && expr->isConstant() && target.isFloatingPoint()) {
            // TODO: Check that the floating-point value is losslessly convertible to the target type?
            return target;
        }

        if (expr->isNullLiteralExpr() && target.isOptionalType()) {
            return target;
        }

        if (expr->isStringLiteralExpr() && target.removeOptional().isArrayWithUnknownSize() && target.removeOptional().getElementType().isChar() &&
            !target.removeOptional().getElementType().isMutable()) {
            // Special case: allow passing string literals as C-strings (const char*).
            return target;
        }

        if (expr->isArrayLiteralExpr() && target.isArrayWithConstantSize()) {
            auto arrayLiteralExpr = llvm::cast<ArrayLiteralExpr>(expr);
            bool isConvertible = llvm::all_of(arrayLiteralExpr->getElements(),
                                              [&](Expr* element) { return isImplicitlyConvertible(element, source.getElementType(), target.getElementType()); });

            if (isConvertible) {
                for (auto& element : arrayLiteralExpr->getElements()) {
                    // FIXME: Don't set type here.
                    element->setType(target.getElementType());
                }
                return target;
            }
        }
    }

    if ((allowPointerToTemporary || (expr && expr->isLvalue())) && target.removeOptional().isPointerType() &&
        // Allow forming mutable pointers to constants. This is safe because constants will be inlined at the usage site.
        (source.isMutable() || (expr && expr->isConstant()) || !target.removeOptional().getPointee().isMutable()) &&
        isImplicitlyConvertible(expr, source, target.removeOptional().getPointee())) {
        if (implicitCastKind) *implicitCastKind = ImplicitCastExpr::AutoReference;
        return source;
    }

    if (source.isPointerType() && source.getPointee() == target && expr && !expr->isReferenceExpr()) {
        // Auto-dereference.
        return target;
    }

    if (target.isOptionalType() && (!expr || !expr->isNullLiteralExpr()) && isImplicitlyConvertible(expr, source, target.getWrappedType())) {
        if (implicitCastKind) *implicitCastKind = ImplicitCastExpr::OptionalWrap;
        return target;
    }

    if (source.isOptionalType() && source.getWrappedType() == target && expr && !expr->isCallExpr()) {
        // Implicit optional unwrap. Warnings for these are generated during null analysis.
        return source;
    }

    if (source.isPointerType() && source.getPointee().isArrayWithConstantSize() && (target.isArrayWithRuntimeSize() || target.isArrayWithUnknownSize()) &&
        source.getPointee().getElementType() == target.getElementType()) {
        return source;
    }

    if (source.isPointerType() && source.getPointee().isArrayType() && target.removeOptional().isPointerType() &&
        isImplicitlyConvertible(nullptr, source.getPointee().getElementType(), target.removeOptional().getPointee())) {
        return source;
    }

    if (source.isArrayWithUnknownSize() && target.isPointerType() && (source.getElementType() == target.getPointee() || target.getPointee().isVoid())) {
        return source;
    }

    if (target.isArrayWithUnknownSize() && source.isPointerType() && (target.getElementType() == source.getPointee() || target.getElementType().isVoid())) {
        return source;
    }

    if (source.isTupleType() && target.isTupleType()) {
        auto* tupleExpr = llvm::dyn_cast_or_null<TupleExpr>(expr);
        auto sourceElements = source.getTupleElements();
        auto targetElements = target.getTupleElements();

        for (size_t i = 0; i < sourceElements.size(); ++i) {
            if (sourceElements[i].name != targetElements[i].name) {
                return Type();
            }

            auto* elementValue = tupleExpr ? tupleExpr->getElements()[i].getValue() : nullptr;

            if (!isImplicitlyConvertible(elementValue, sourceElements[i].type, targetElements[i].type)) {
                return Type();
            }
        }

        return target;
    }

    return Type();
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
            for (Type paramType : type.getParamTypes()) {
                if (containsGenericParam(paramType, genericParam)) {
                    return true;
                }
            }
            return containsGenericParam(type.getReturnType(), genericParam);

        case TypeKind::PointerType:
            return containsGenericParam(type.getPointee(), genericParam);

        case TypeKind::UnresolvedType:
            llvm_unreachable("invalid unresolved type");
    }

    llvm_unreachable("all cases handled");
}

static Type findGenericArg(Type argType, Type paramType, llvm::StringRef genericParam) {
    if (paramType.isBasicType() && paramType.getName() == genericParam) {
        return argType;
    }

    switch (argType.getKind()) {
        case TypeKind::BasicType:
            if (!argType.getGenericArgs().empty() && paramType.isBasicType() && paramType.getName() == argType.getName()) {
                ASSERT(argType.getGenericArgs().size() == paramType.getGenericArgs().size());
                for (auto&& [argTypeGenericArg, paramTypeGenericArg] : llvm::zip_first(argType.getGenericArgs(), paramType.getGenericArgs())) {
                    if (Type type = findGenericArg(argTypeGenericArg, paramTypeGenericArg, genericParam)) {
                        return type;
                    }
                }
            }
            break;

        case TypeKind::ArrayType:
            if (paramType.isArrayType()) {
                return findGenericArg(argType.getElementType(), paramType.getElementType(), genericParam);
            }
            break;

        case TypeKind::TupleType:
            if (paramType.isTupleType()) {
                for (auto&& [argTypeElement, paramTypeElement] : llvm::zip_first(argType.getTupleElements(), paramType.getTupleElements())) {
                    if (Type type = findGenericArg(argTypeElement.type, paramTypeElement.type, genericParam)) {
                        return type;
                    }
                }
            }
            break;

        case TypeKind::FunctionType:
            if (paramType.isFunctionType()) {
                for (auto&& [argTypeParamType, paramTypeParamTypes] : llvm::zip_first(argType.getParamTypes(), paramType.getParamTypes())) {
                    if (Type type = findGenericArg(argTypeParamType, paramTypeParamTypes, genericParam)) {
                        return type;
                    }
                }
                return findGenericArg(argType.getReturnType(), paramType.getReturnType(), genericParam);
            }
            break;

        case TypeKind::PointerType:
            if (paramType.isPointerType()) {
                return findGenericArg(argType.getPointee(), paramType.getPointee(), genericParam);
            }
            break;

        case TypeKind::UnresolvedType:
            llvm_unreachable("invalid unresolved type");
    }

    if (paramType.removeOptional().isPointerType()) {
        return findGenericArg(argType, paramType.removeOptional().getPointee(), genericParam);
    }

    return Type();
}

static Type replaceUnresolvedGenericParamsWithPlaceholders(Type type, llvm::ArrayRef<GenericParamDecl> genericParams) {
    llvm::StringMap<Type> placeholders;

    for (auto& genericParam : genericParams) {
        placeholders.try_emplace(genericParam.getName(), UnresolvedType::get());
    }

    return type.resolve(placeholders);
}

std::vector<Type> Typechecker::inferGenericArgsFromCallArgs(llvm::ArrayRef<GenericParamDecl> genericParams, CallExpr& call, llvm::ArrayRef<ParamDecl> params,
                                                            bool returnOnError) {
    if (call.getArgs().size() != params.size()) return {};

    std::vector<Type> inferredGenericArgs;

    for (auto& genericParam : genericParams) {
        Type genericArg;
        Expr* genericArgValue = nullptr;

        for (auto&& [param, arg] : llvm::zip_first(params, call.getArgs())) {
            Type paramType = param.getType();

            if (containsGenericParam(paramType, genericParam.getName())) {
                // FIXME: The args will also be typechecked by validateAndConvertArguments() after this function. Get rid of this duplicated typechecking.
                auto* argValue = arg.getValue();
                auto expectedType = replaceUnresolvedGenericParamsWithPlaceholders(paramType, genericParams);
                // TODO: Should probably not typecheck here because it might change the expression's type?
                Type argType = argValue->hasType() ? argValue->getType() : typecheckExpr(*argValue, false, expectedType);
                Type maybeGenericArg = findGenericArg(argType, paramType, genericParam.getName());
                if (!maybeGenericArg) continue;

                if (!genericArg) {
                    genericArg = maybeGenericArg;
                    genericArgValue = argValue;
                } else {
                    Type paramTypeWithGenericArg = paramType.resolve({ { genericParam.getName(), genericArg } });
                    Type paramTypeWithMaybeGenericArg = paramType.resolve({ { genericParam.getName(), maybeGenericArg } });

                    if (isImplicitlyConvertible(argValue, argValue->getType(), paramTypeWithGenericArg, true)) {
                        continue;
                    } else if (isImplicitlyConvertible(genericArgValue, genericArgValue->getType(), paramTypeWithMaybeGenericArg, true)) {
                        genericArg = maybeGenericArg;
                        genericArgValue = argValue;
                    } else {
                        return {}; // TODO: Return "conflict argument types" as reason for inference failure.
                    }
                }
            }
        }

        if (genericArg) {
            inferredGenericArgs.push_back(genericArg);
        } else {
            return {};
        }
    }

    ASSERT(genericParams.size() == inferredGenericArgs.size());

    for (auto&& [genericParam, genericArg] : llvm::zip(genericParams, inferredGenericArgs)) {
        if (!genericParam.getConstraints().empty()) {
            ASSERT(genericParam.getConstraints().size() == 1, "cannot have multiple generic constraints yet");
            auto* interface = getTypeDecl(*llvm::cast<BasicType>(genericParam.getConstraints()[0].getBase()));

            if (auto basicType = llvm::dyn_cast<BasicType>(genericArg.getBase())) {
                auto* typeDecl = getTypeDecl(*basicType);
                if (typeDecl && typeDecl->hasInterface(*interface)) {
                    continue;
                }
            }

            if (returnOnError) {
                return {};
            } else {
                ERROR(call.getLocation(), "type '" << genericArg << "' doesn't implement interface '" << interface->getName() << "'");
            }
        }
    }

    return inferredGenericArgs;
}

void delta::validateGenericArgCount(size_t genericParamCount, llvm::ArrayRef<Type> genericArgs, llvm::StringRef name, SourceLocation location) {
    if (genericArgs.size() < genericParamCount) {
        REPORT_ERROR(location, "too few generic arguments to '" << name << "', expected " << genericParamCount);
    } else if (genericArgs.size() > genericParamCount) {
        REPORT_ERROR(location, "too many generic arguments to '" << name << "', expected " << genericParamCount);
    }
}

llvm::StringMap<Type> Typechecker::getGenericArgsForCall(llvm::ArrayRef<GenericParamDecl> genericParams, CallExpr& call, FunctionDecl* decl, bool returnOnError,
                                                         Type expectedType) {
    ASSERT(!genericParams.empty());
    std::vector<Type> inferredGenericArgs;
    llvm::ArrayRef<Type> genericArgTypes;

    if (call.getGenericArgs().empty()) {
        if (expectedType && expectedType.isBasicType() && !expectedType.getGenericArgs().empty() &&
            llvm::none_of(expectedType.getGenericArgs(), [](Type t) { return t.isUnresolvedType(); }) &&
            BasicType::get(expectedType.getName(), {}).getDecl() == (decl->isConstructorDecl() ? decl->getTypeDecl() : decl->getReturnType().getDecl())) {
            genericArgTypes = expectedType.getGenericArgs();
        } else if (call.getArgs().empty()) {
            if (returnOnError) return {};
            ERROR(call.getLocation(), "can't infer generic parameters, please specify them explicitly");
        } else {
            inferredGenericArgs = inferGenericArgsFromCallArgs(genericParams, call, decl->getParams(), returnOnError);
            if (inferredGenericArgs.empty()) return {};
            ASSERT(inferredGenericArgs.size() == genericParams.size());
            genericArgTypes = inferredGenericArgs;
        }
    } else {
        genericArgTypes = call.getGenericArgs();
    }

    llvm::StringMap<Type> genericArgs;
    auto genericArg = genericArgTypes.begin();

    for (const GenericParamDecl& genericParam : genericParams) {
        genericArgs.try_emplace(genericParam.getName(), *genericArg++);
    }

    return genericArgs;
}

Type Typechecker::typecheckBuiltinConversion(CallExpr& expr) {
    if (expr.getArgs().size() != 1) {
        ERROR(expr.getLocation(), "expected single argument to converting constructor");
    }
    if (!expr.getGenericArgs().empty()) {
        ERROR(expr.getLocation(), "expected no generic arguments to converting constructor");
    }
    if (!expr.getArgs().front().getName().empty()) {
        ERROR(expr.getLocation(), "expected unnamed argument to converting constructor");
    }

    auto sourceType = typecheckExpr(*expr.getArgs().front().getValue());
    auto targetType = BasicType::get(expr.getFunctionName(), {});

    if (sourceType == targetType) {
        WARN(expr.getCallee().getLocation(), "unnecessary conversion to same type");
    }

    expr.setType(targetType);
    return expr.getType();
}

static std::vector<Note> getCandidateNotes(llvm::ArrayRef<Decl*> candidates) {
    bool multipleModules = candidates.size() > 1 && llvm::any_of(candidates, [&](Decl* c) { return c->getModule() != candidates[0]->getModule(); });

    return map(candidates, [&](Decl* c) {
        auto message = ("candidate function" + (multipleModules && c->getModule() ? " in module '" + c->getModule()->getName() + "'" : "") + ":").str();
        return Note { c->getLocation(), std::move(message) };
    });
}

static const Match* findMatchByPredicate(llvm::ArrayRef<Match> matches, const CallExpr& call, llvm::function_ref<bool(Type param, Type arg)> predicate) {
    const Match* result = nullptr;

    for (auto& match : matches) {
        std::vector<ParamDecl> params;
        if (auto functionDecl = llvm::dyn_cast<FunctionDecl>(match.decl)) {
            params = functionDecl->getParams();
        } else if (auto variableDecl = llvm::dyn_cast<VariableDecl>(match.decl)) {
            params = llvm::cast<FunctionType>(variableDecl->getType().getBase())->getParamDecls();
        } else {
            llvm_unreachable("unhandled callee decl");
        }

        if (params.size() == call.getArgs().size()) {
            if (llvm::all_of(llvm::zip_first(params, call.getArgs()), [&](auto&& pair) {
                    auto&& [param, arg] = pair;
                    return predicate(param.getType(), arg.getValue()->getType());
                })) {
                if (result) return nullptr;
                result = &match;
            }
        }
    }

    return result;
}

static bool isStdlibDecl(const Match& match) {
    return match.decl->getModule() && match.decl->getModule()->getName() == "std";
}

static bool isCHeaderDecl(const Match& match) {
    return match.decl->getModule() && match.decl->getModule()->getName().endswith_lower(".h");
}

static const Match* resolveAmbiguousOverload(llvm::ArrayRef<Match> matches, const CallExpr& call) {
    if (llvm::count_if(matches, isStdlibDecl) == 1 && llvm::all_of(matches, [](auto& match) { return isStdlibDecl(match) || isCHeaderDecl(match); })) {
        return llvm::find_if(matches, isStdlibDecl);
    } else if (llvm::all_of(matches, isCHeaderDecl)) {
        // Redeclarations in multiple C headers are considered the same declaration, so just return one of them.
        return &matches[0];
    } else if (llvm::count_if(matches, [](auto& match) { return match.didConvertArguments == false; }) == 1) {
        return llvm::find_if(matches, [](auto& match) { return match.didConvertArguments == false; });
    } else if (auto match = findMatchByPredicate(matches, call, [](Type param, Type arg) { return param == arg; })) {
        return match;
    } else if (auto match = findMatchByPredicate(matches, call, [](Type param, Type arg) { return param == arg.getPointerTo(); })) {
        return match;
    } else {
        return nullptr;
    }
}

static bool equals(const llvm::StringMap<Type>& a, const llvm::StringMap<Type>& b) {
    if (a.size() != b.size()) return false;

    for (auto& aEntry : a) {
        auto bEntry = b.find(aEntry.getKey());
        if (bEntry == b.end() || aEntry.getValue() != bEntry->getValue()) return false;
    }

    return true;
}

Decl* Typechecker::resolveOverload(llvm::ArrayRef<Decl*> decls, CallExpr& expr, llvm::StringRef callee, Type expectedType) {
    std::vector<Match> matches;
    std::vector<Match> templateMatches;
    llvm::ArrayRef<Decl*> candidates = decls;
    std::vector<ConstructorDecl*> constructorDecls;
    bool isConstructorCall = false;

    for (Decl* decl : decls) {
        switch (decl->getKind()) {
            case DeclKind::FunctionTemplate: {
                auto* functionTemplate = llvm::cast<FunctionTemplate>(decl);
                auto genericParams = functionTemplate->getGenericParams();

                if (!expr.getGenericArgs().empty() && expr.getGenericArgs().size() != genericParams.size()) {
                    if (decls.size() == 1) {
                        validateGenericArgCount(genericParams.size(), expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                    }
                    continue;
                }

                auto genericArgs = getGenericArgsForCall(genericParams, expr, functionTemplate->getFunctionDecl(), decls.size() != 1, expectedType);
                if (genericArgs.empty()) continue; // Couldn't infer generic arguments.

                auto* functionDecl = functionTemplate->instantiate(genericArgs);

                if (decls.size() == 1) {
                    validateAndConvertArguments(expr, *functionDecl, callee, expr.getCallee().getLocation());
                    declsToTypecheck.emplace_back(functionDecl);
                    return functionDecl;
                }
                if (auto match = matchArguments(expr, functionDecl)) {
                    templateMatches.push_back(*match);
                }
                break;
            }
            case DeclKind::FunctionDecl:
            case DeclKind::MethodDecl:
            case DeclKind::ConstructorDecl: {
                auto functionDecl = llvm::cast<FunctionDecl>(decl);

                // TODO: Figure out where to perform this.
                if (functionDecl && functionDecl->getTypeDecl() && functionDecl->getTypeDecl()->isInterface()) {
                    functionDecl = functionDecl->instantiate({ { "This", functionDecl->getTypeDecl()->getType() } }, {});
                }

                if (decls.size() == 1) {
                    validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                    validateAndConvertArguments(expr, *functionDecl, callee, expr.getCallee().getLocation());
                    return functionDecl;
                }
                if (auto match = matchArguments(expr, functionDecl)) {
                    matches.push_back(*match);
                }
                break;
            }
            case DeclKind::TypeDecl: {
                auto* typeDecl = llvm::cast<TypeDecl>(decl);
                isConstructorCall = true;
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                constructorDecls = typeDecl->getConstructors();
                ASSERT(!constructorDecls.empty());
                ASSERT(decls.size() == 1);
                candidates = llvm::ArrayRef(reinterpret_cast<Decl**>(constructorDecls.data()), constructorDecls.size());

                for (auto* constructorDecl : constructorDecls) {
                    if (constructorDecls.size() == 1) {
                        validateAndConvertArguments(expr, *constructorDecl, callee, expr.getCallee().getLocation());
                        return constructorDecl;
                    }
                    if (auto match = matchArguments(expr, constructorDecl)) {
                        matches.push_back(*match);
                    }
                }
                break;
            }
            case DeclKind::TypeTemplate: {
                auto* typeTemplate = llvm::cast<TypeTemplate>(decl);
                isConstructorCall = true;
                constructorDecls = typeTemplate->getTypeDecl()->getConstructors();
                ASSERT(decls.size() == 1);
                candidates = llvm::ArrayRef(reinterpret_cast<Decl**>(constructorDecls.data()), constructorDecls.size());

                std::vector<llvm::StringMap<Type>> genericArgSets;

                for (auto* constructorDecl : constructorDecls) {
                    auto genericArgs = getGenericArgsForCall(typeTemplate->getGenericParams(), expr, constructorDecl, constructorDecls.size() != 1, expectedType);
                    if (genericArgs.empty()) continue; // Couldn't infer generic arguments.
                    if (llvm::find_if(genericArgSets, [&](auto& set) { return equals(set, genericArgs); }) == genericArgSets.end()) {
                        genericArgSets.push_back(genericArgs);
                    }
                }

                for (auto& genericArgs : genericArgSets) {
                    TypeDecl* typeDecl = nullptr;

                    auto genericArgTypes = map(genericArgs, [](auto& entry) { return entry.getValue(); });
                    auto typeDecls = findDecls(getQualifiedTypeName(typeTemplate->getTypeDecl()->getName(), genericArgTypes));

                    if (typeDecls.empty()) {
                        typeDecl = typeTemplate->instantiate(genericArgs);
                        getCurrentModule()->addToSymbolTable(*typeDecl);
                        declsToTypecheck.push_back(typeDecl);
                    } else {
                        typeDecl = llvm::cast<TypeDecl>(typeDecls[0]);
                    }

                    for (auto* constructorDecl : typeDecl->getConstructors()) {
                        if (constructorDecls.size() == 1) {
                            validateAndConvertArguments(expr, *constructorDecl, callee, expr.getCallee().getLocation());
                            return constructorDecl;
                        }
                        if (auto match = matchArguments(expr, constructorDecl)) {
                            templateMatches.push_back(*match);
                        }
                    }
                }
                break;
            }
            case DeclKind::VarDecl:
            case DeclKind::ParamDecl:
            case DeclKind::FieldDecl: {
                auto* variableDecl = llvm::cast<VariableDecl>(decl);

                if (auto* functionType = llvm::dyn_cast<FunctionType>(variableDecl->getType().getBase())) {
                    auto paramDecls = functionType->getParamDecls(variableDecl->getLocation());

                    if (decls.size() == 1) {
                        validateAndConvertArguments(expr, paramDecls, false, callee, expr.getCallee().getLocation());
                        return variableDecl;
                    }
                    if (auto match = matchArguments(expr, variableDecl, paramDecls)) {
                        matches.push_back(*match);
                    }
                }
                break;
            }
            case DeclKind::DestructorDecl:
                matches.push_back({ decl, false });
                break;

            default:
                continue;
        }
    }

    if (matches.empty()) {
        matches = std::move(templateMatches);
    }

    if (matches.size() > 1) {
        for (auto& arg : expr.getArgs()) {
            if (!arg.getValue()->hasType()) {
                typecheckExpr(*arg.getValue());
            }
        }

        if (auto match = resolveAmbiguousOverload(matches, expr)) {
            matches = { *match };
        } else {
            ERROR_WITH_NOTES(expr.getCallee().getLocation(), getCandidateNotes(map(matches, [](auto& match) { return match.decl; })),
                             "ambiguous reference to '" << callee << "'" << (isConstructorCall ? " constructor" : ""));
        }
    }

    if (matches.size() == 1) {
        validateAndConvertArguments(expr, *matches.front().decl);
        declsToTypecheck.push_back(matches.front().decl);
        return matches.front().decl;
    }

    if (decls.empty()) {
        ERROR(expr.getCallee().getLocation(), "unknown identifier '" << callee << "'");
    }

    bool atLeastOneFunction = llvm::any_of(decls, [](Decl* decl) {
        return decl->isFunctionDecl() || decl->isFunctionTemplate() || decl->isTypeDecl() || decl->isTypeTemplate();
    });

    if (atLeastOneFunction) {
        if (auto binaryExpr = llvm::dyn_cast<BinaryExpr>(&expr)) {
            ERROR_WITH_NOTES(expr.getCallee().getLocation(), getCandidateNotes(candidates),
                             "no matching operator '" << binaryExpr->getOperator() << "' with arguments '" << binaryExpr->getLHS().getType() << "' and '"
                                                      << binaryExpr->getRHS().getType() << "'");
        } else {
            auto argTypes = map(expr.getArgs(), [&](NamedValue& arg) { return typecheckExpr(*arg.getValue()).toString(); });
            ERROR_WITH_NOTES(expr.getCallee().getLocation(), getCandidateNotes(candidates),
                             (isConstructorCall ? "no matching constructor '" : "no matching function '") << callee << "(" << llvm::join(argTypes, ", ") << ")'");
        }
    } else {
        ERROR(expr.getCallee().getLocation(), "'" << callee << "' is not a function");
    }
}

std::vector<Decl*> Typechecker::findCalleeCandidates(const CallExpr& expr, llvm::StringRef callee) {
    TypeDecl* receiverTypeDecl;

    if (expr.getReceiverType() && expr.getReceiverType().removePointer().isBasicType()) {
        receiverTypeDecl = getTypeDecl(*llvm::cast<BasicType>(expr.getReceiverType().removePointer().getBase()));
    } else {
        receiverTypeDecl = nullptr;
    }

    return findDecls(callee, receiverTypeDecl, isPostProcessing);
}

Type Typechecker::typecheckCallExpr(CallExpr& expr, Type expectedType) {
    if (!expr.callsNamedFunction()) {
        ERROR(expr.getLocation(), "anonymous function calls not implemented yet");
    }

    if (Type::isBuiltinScalar(expr.getFunctionName())) {
        return typecheckBuiltinConversion(expr);
    }

    if (expr.isBuiltinCast()) {
        return typecheckBuiltinCast(expr);
    }

    if (expr.getFunctionName() == "assert") {
        ParamDecl assertParam(Type::getBool(), "", false, SourceLocation());
        validateAndConvertArguments(expr, assertParam, false, expr.getFunctionName(), expr.getLocation());
        validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
        return Type::getVoid();
    }

    Decl* decl;

    if (auto* enumCase = getEnumCase(expr.getCallee())) {
        decl = enumCase;
        llvm::cast<MemberExpr>(expr.getCallee()).setDecl(*decl);
    } else if (expr.getCallee().isMemberExpr()) {
        Type receiverType = typecheckExpr(*expr.getReceiver());
        expr.setReceiverType(receiverType);

        if (receiverType.removeOptional().removePointer().isArrayType()) {
            // TODO: Move these member functions to a 'struct Array' declaration in stdlib.
            if (expr.getFunctionName() == "data") {
                validateAndConvertArguments(expr, {}, false, expr.getFunctionName(), expr.getLocation());
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                return ArrayType::get(receiverType.removePointer().getElementType(), ArrayType::unknownSize);
            }
            if (expr.getFunctionName() == "size") {
                validateAndConvertArguments(expr, {}, false, expr.getFunctionName(), expr.getLocation());
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                return ArrayType::getIndexType();
            }
            if (expr.getFunctionName() == "iterator") {
                validateAndConvertArguments(expr, {}, false, expr.getFunctionName(), expr.getLocation());
                validateGenericArgCount(0, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
                return BasicType::get("ArrayIterator", receiverType.removePointer().getElementType());
            }

            ERROR(expr.getReceiver()->getLocation(), "type '" << receiverType.removePointer() << "' has no member function '" << expr.getFunctionName() << "'");
        } else if (receiverType.removeOptional().removePointer().isBuiltinType() && expr.getFunctionName() == "deinit") {
            return Type::getVoid();
        }

        if (expr.getArgs().size() == 1 && expr.getFunctionName() == "init") {
            typecheckExpr(*expr.getArgs()[0].getValue());

            if (expr.isMoveInit()) {
                if (!expr.getArgs()[0].getValue()->getType().isImplicitlyCopyable()) {
                    setMoved(expr.getArgs()[0].getValue(), true);
                }
                return Type::getVoid();
            }
        }

        auto callee = expr.getQualifiedFunctionName();
        auto decls = findCalleeCandidates(expr, callee);

        if (decls.empty() && expr.getFunctionName() == "deinit") {
            return Type::getVoid();
        }

        decl = resolveOverload(decls, expr, callee, expectedType);
    } else {
        auto callee = expr.getFunctionName();
        auto decls = findCalleeCandidates(expr, callee);
        decl = resolveOverload(decls, expr, callee, expectedType);

        if (auto* constructorDecl = llvm::dyn_cast<ConstructorDecl>(decl)) {
            expr.setReceiverType(constructorDecl->getTypeDecl()->getType());
        } else if (decl->isMethodDecl()) {
            auto* varDecl = llvm::cast<VarDecl>(findDecl("this", expr.getCallee().getLocation()));
            expr.setReceiverType(varDecl->getType());
        }
    }

    checkHasAccess(*decl, expr.getCallee().getLocation(), AccessLevel::None);
    if (auto* constructorDecl = llvm::dyn_cast<ConstructorDecl>(decl)) {
        checkHasAccess(*constructorDecl->getTypeDecl(), expr.getCallee().getLocation(), AccessLevel::None);
    }

    std::vector<ParamDecl> params;

    if (auto functionDecl = llvm::dyn_cast<FunctionDecl>(decl)) {
        params = functionDecl->getParams();
    } else if (auto variableDecl = llvm::dyn_cast<VariableDecl>(decl)) {
        params = llvm::cast<FunctionType>(variableDecl->getType().getBase())->getParamDecls();
    } else {
        auto type = llvm::cast<EnumCase>(decl)->getAssociatedType();
        params = map(type.getTupleElements(), [&](auto& e) { return ParamDecl(e.type, std::string(e.name), false, decl->getLocation()); });
        validateAndConvertArguments(expr, params, false, decl->getName(), expr.getLocation());
    }

    for (auto&& [param, arg] : llvm::zip_longest(params, expr.getArgs())) {
        if (arg && (!param || !param->getType().isImplicitlyCopyable())) {
            setMoved(arg->getValue(), true);
        }
    }

    expr.setCalleeDecl(decl);
    decl->setReferenced(true);

    if (auto constructorDecl = llvm::dyn_cast<ConstructorDecl>(decl)) {
        if (constructorDecl->getTypeDecl()->isInterface()) {
            typecheckFunctionDecl(*constructorDecl);
        }
        return llvm::cast<ConstructorDecl>(decl)->getTypeDecl()->getType();
    } else if (auto functionDecl = llvm::dyn_cast<FunctionDecl>(decl)) {
        return functionDecl->getFunctionType()->getReturnType();
    } else if (auto variableDecl = llvm::dyn_cast<VariableDecl>(decl)) {
        return llvm::cast<FunctionType>(variableDecl->getType().getBase())->getReturnType();
    } else {
        return llvm::cast<EnumCase>(decl)->getType();
    }
}

ArgumentValidation Typechecker::getArgumentValidationResult(CallExpr& expr, llvm::ArrayRef<ParamDecl> params, bool isVariadic) {
    if (expr.getArgs().size() < params.size()) {
        return ArgumentValidation::tooFew();
    } else if (!isVariadic && expr.getArgs().size() > params.size()) {
        return ArgumentValidation::tooMany();
    }

    bool didConvertArguments = false;

    for (size_t i = 0; i < expr.getArgs().size(); ++i) {
        auto& arg = expr.getArgs()[i];
        auto* param = i < params.size() ? &params[i] : nullptr;

        if (!arg.getName().empty() && (!param || arg.getName() != param->getName())) {
            return ArgumentValidation::invalidName(i);
        }

        if (param) {
            bool hadType = arg.getValue()->hasType();

            if (!arg.getValue()->hasType()) {
                typecheckExpr(*arg.getValue(), false, param ? param->getType() : Type());
            }

            bool invalidType = false;
            if (Type convertedType = isImplicitlyConvertible(arg.getValue(), arg.getValue()->getType(), param->getType(), true)) {
                didConvertArguments = didConvertArguments || convertedType != arg.getValue()->getType();
            } else {
                invalidType = true;
            }

            if (!hadType) arg.getValue()->removeTypes();
            if (invalidType) return ArgumentValidation::invalidType(i);
        }
    }

    return ArgumentValidation::success(didConvertArguments);
}

llvm::Optional<Match> Typechecker::matchArguments(CallExpr& expr, Decl* calleeDecl, llvm::ArrayRef<ParamDecl> params) {
    bool isVariadic = false;
    if (auto functionDecl = llvm::dyn_cast<FunctionDecl>(calleeDecl)) {
        params = functionDecl->getParams();
        isVariadic = functionDecl->isVariadic();
    }
    auto result = getArgumentValidationResult(expr, params, isVariadic);
    if (result.error) return llvm::None;
    return Match { calleeDecl, result.didConvertArguments };
}

void Typechecker::validateAndConvertArguments(CallExpr& expr, const Decl& calleeDecl, llvm::StringRef functionName, SourceLocation location) {
    if (auto functionDecl = llvm::dyn_cast<FunctionDecl>(&calleeDecl)) {
        validateAndConvertArguments(expr, functionDecl->getParams(), functionDecl->isVariadic(), functionName, location);
    } else {
        auto functionType = llvm::cast<FunctionType>(llvm::cast<VariableDecl>(calleeDecl).getType().getBase());
        auto paramDecls = functionType->getParamDecls(calleeDecl.getLocation());
        validateAndConvertArguments(expr, paramDecls, false, functionName, location);
    }
}

void Typechecker::validateAndConvertArguments(CallExpr& expr, llvm::ArrayRef<ParamDecl> params, bool isVariadic, llvm::StringRef callee, SourceLocation location) {
    auto result = getArgumentValidationResult(expr, params, isVariadic);

    for (auto&& [param, arg] : llvm::zip_longest(params, expr.getArgs())) {
        if (!arg) continue;
        if (!arg->getValue()->hasType()) {
            typecheckExpr(*arg->getValue(), false, param ? param->getType() : Type());
        }

        if (param) arg->setValue(convert(arg->getValue(), param->getType(), true));
    }

    switch (result.error) {
        case ArgumentValidation::None:
            break;
        case ArgumentValidation::TooFew:
            REPORT_ERROR(location, "too few arguments to '" << callee << "', expected " << (isVariadic ? "at least " : "") << params.size());
            break;
        case ArgumentValidation::TooMany:
            REPORT_ERROR(location, "too many arguments to '" << callee << "', expected " << params.size());
            break;
        case ArgumentValidation::InvalidName: {
            auto& arg = expr.getArgs()[result.index];
            auto* param = &params[result.index];
            ERROR(arg.getLocation(), "invalid argument name '" << arg.getName() << "' for parameter '" << param->getName() << "'");
            break;
        }
        case ArgumentValidation::InvalidType: {
            auto& arg = expr.getArgs()[result.index];
            auto* param = &params[result.index];
            ERROR(arg.getLocation(), "invalid argument #" << (result.index + 1) << " type '" << arg.getValue()->getType() << "' to '" << callee
                                                          << "', expected '" << param->getType() << "'");
            break;
        }
    }
}

static bool isValidCast(Type sourceType, Type targetType) {
    switch (sourceType.getKind()) {
        case TypeKind::BasicType:
            if (sourceType.isOptionalType()) {
                Type sourceWrappedType = sourceType.getWrappedType();

                if (sourceWrappedType.isPointerType() && targetType.isOptionalType()) {
                    Type targetWrappedType = targetType.getWrappedType();

                    if (targetWrappedType.isPointerType() && isValidCast(sourceWrappedType, targetWrappedType)) {
                        return true;
                    }
                }
            }
            return false;

        case TypeKind::TupleType:
        case TypeKind::FunctionType:
            return false;

        case TypeKind::PointerType: {
            Type sourcePointee = sourceType.getPointee();

            if (targetType.isPointerType()) {
                Type targetPointee = targetType.getPointee();

                if (sourcePointee.isVoid() && (!targetPointee.isMutable() || sourcePointee.isMutable())) {
                    return true;
                } else if (targetPointee.isVoid() && (!targetPointee.isMutable() || sourcePointee.isMutable())) {
                    return true;
                } else if (targetPointee.isArrayWithConstantSize() && sourcePointee == targetPointee.getElementType()) {
                    return true;
                }
            } else if (targetType.isArrayWithUnknownSize()) {
                if (sourcePointee.isVoid() && (!targetType.getElementType().isMutable() || sourcePointee.isMutable())) {
                    return true;
                } else if (sourcePointee == targetType.getElementType()) {
                    return true;
                }
            }

            return false;
        }
        case TypeKind::ArrayType: {
            if (targetType.isPointerType()) {
                Type targetPointee = targetType.getPointee();

                if (targetPointee.isVoid() && (!targetPointee.isMutable() || sourceType.getElementType().isMutable())) {
                    return true;
                }
            }

            return false;
        }
        case TypeKind::UnresolvedType:
            llvm_unreachable("invalid unresolved type");
    }

    llvm_unreachable("all cases handled");
}

Type Typechecker::typecheckBuiltinCast(CallExpr& expr) {
    Type sourceType = typecheckExpr(*expr.getArgs().front().getValue());
    Type targetType = expr.getGenericArgs().front();
    ParamDecl param(sourceType, "", false, expr.getLocation());

    validateGenericArgCount(1, expr.getGenericArgs(), expr.getFunctionName(), expr.getLocation());
    validateAndConvertArguments(expr, param, false, expr.getFunctionName(), expr.getLocation());

    if (sourceType.equalsIgnoreTopLevelMutable(targetType)) {
        WARN(expr.getCallee().getLocation(), "unnecessary cast from '" << sourceType << "' to '" << targetType << "'");
    } else if (!isValidCast(sourceType, targetType) && !isValidCast(sourceType.removeOptional(), targetType)) {
        ERROR(expr.getCallee().getLocation(), "illegal cast from '" << sourceType << "' to '" << targetType << "'");
    }

    return targetType;
}

Type Typechecker::typecheckSizeofExpr(SizeofExpr&) {
    return Type::getUInt64();
}

Type Typechecker::typecheckAddressofExpr(AddressofExpr& expr) {
    if (!typecheckExpr(expr.getOperand()).removeOptional().isPointerType()) {
        ERROR(expr.getLocation(), "operand to 'addressof' must have pointer type");
    }
    return Type::getUIntPtr();
}

Type Typechecker::typecheckMemberExpr(MemberExpr& expr) {
    if (auto* enumCase = getEnumCase(expr)) {
        checkHasAccess(*enumCase->getEnumDecl(), expr.getBaseExpr()->getLocation(), AccessLevel::None);
        expr.setDecl(*enumCase);
        return enumCase->getType();
    }

    Type baseType = typecheckExpr(*expr.getBaseExpr());
    if (!expr.getBaseExpr()->isThis()) baseType = baseType.removeOptional();
    baseType = baseType.removePointer();

    if (baseType.isArrayType()) {
        auto sizeSynonyms = { "count", "length", "size" };

        if (llvm::is_contained(sizeSynonyms, expr.getMemberName())) {
            ERROR(expr.getLocation(), "use the '.size()' member function to get the number of elements in an array");
        }
    }

    if (auto* typeDecl = baseType.getDecl()) {
        for (auto& field : typeDecl->getFields()) {
            if (field.getName() == expr.getMemberName()) {
                checkHasAccess(field, expr.getLocation(), AccessLevel::None);
                expr.setDecl(field);
                return field.getType().withMutability(baseType.getMutability());
            }
        }
    }

    if (baseType.isTupleType()) {
        for (auto& element : baseType.getTupleElements()) {
            if (element.name == expr.getMemberName()) {
                return element.type;
            }
        }
    }

    ERROR(expr.getLocation(), "no member named '" << expr.getMemberName() << "' in '" << baseType << "'");
}

Type Typechecker::typecheckIndexExpr(IndexExpr& expr) {
    Type lhsType = typecheckExpr(*expr.getBase());
    Type arrayType;

    if (lhsType.removeOptional().isArrayType()) {
        arrayType = lhsType.removeOptional();
    } else if (lhsType.isPointerType() && lhsType.getPointee().isArrayType()) {
        arrayType = lhsType.getPointee();
    } else if (lhsType.removeOptional().removePointer().isBuiltinType()) {
        ERROR(expr.getLocation(), "'" << lhsType << "' doesn't provide an index operator");
    } else {
        return typecheckCallExpr(expr);
    }

    Expr* indexExpr = expr.getIndex();
    Type indexType = typecheckExpr(*indexExpr);

    if (auto converted = convert(indexExpr, ArrayType::getIndexType())) {
        expr.setIndex(converted);
        indexExpr = converted;
    } else {
        ERROR(indexExpr->getLocation(), "illegal index type '" << indexType << "', expected '" << ArrayType::getIndexType() << "'");
    }

    if (arrayType.isArrayWithConstantSize()) {
        if (indexExpr->isConstant()) {
            auto index = indexExpr->getConstantIntegerValue();

            if (index < 0 || index >= arrayType.getArraySize()) {
                ERROR(indexExpr->getLocation(), "accessing array out-of-bounds with index " << index << ", array size is " << arrayType.getArraySize());
            }
        }
    }

    return arrayType.getElementType();
}

Type Typechecker::typecheckIndexAssignmentExpr(IndexAssignmentExpr& expr) {
    auto elementType = typecheckIndexExpr(expr);

    if (!expr.getBase()->getType().removeOptional().removePointer().isArrayType()) {
        return typecheckCallExpr(expr);
    }

    typecheckExpr(*expr.getValue());

    if (auto converted = convert(expr.getValue(), elementType)) {
        expr.setValue(converted);
    } else {
        ERROR(expr.getValue()->getLocation(), "cannot assign '" << expr.getValue()->getType() << "' to '" << elementType << "'");
    }

    return Type::getVoid();
}

Type Typechecker::typecheckUnwrapExpr(UnwrapExpr& expr) {
    Type type = typecheckExpr(expr.getOperand());
    if (!type.isOptionalType()) {
        ERROR(expr.getLocation(), "cannot unwrap non-optional type '" << type << "'");
    }
    return type.getWrappedType();
}

Type Typechecker::typecheckLambdaExpr(LambdaExpr& expr, Type expectedType) {
    for (size_t i = 0, e = expr.getFunctionDecl()->getParams().size(); i < e; ++i) {
        auto& param = expr.getFunctionDecl()->getParams()[i];
        if (!param.getType()) {
            auto inferredType = expectedType ? expectedType.getParamTypes()[i] : Type();
            if (!inferredType) {
                ERROR(param.getLocation(), "couldn't infer type for parameter '" << param.getName() << "'");
            }
            param.setType(inferredType);
        }
    }

    typecheckFunctionDecl(*expr.getFunctionDecl());
    return Type(expr.getFunctionDecl()->getFunctionType(), Mutability::Mutable, expr.getLocation());
}

Type Typechecker::typecheckIfExpr(IfExpr& expr) {
    auto conditionType = typecheckExpr(*expr.getCondition()).removePointer();

    if (!conditionType.isBool() && !conditionType.isOptionalType()) {
        ERROR(expr.getCondition()->getLocation(), "if-expression condition must have type 'bool' or optional type");
    }

    auto thenType = typecheckExpr(*expr.getThenExpr());
    auto elseType = typecheckExpr(*expr.getElseExpr());

    if (auto convertedElse = convert(expr.getElseExpr(), thenType)) {
        expr.setElseExpr(convertedElse);
        return thenType;
    } else if (auto convertedThen = convert(expr.getThenExpr(), elseType)) {
        expr.setThenExpr(convertedThen);
        return elseType;
    } else {
        ERROR(expr.getLocation(), "incompatible operand types ('" << thenType << "' and '" << elseType << "')");
    }
}

Type Typechecker::typecheckExpr(Expr& expr, bool useIsWriteOnly, Type expectedType) {
    Type type;

    switch (expr.getKind()) {
        case ExprKind::VarExpr:
            type = typecheckVarExpr(llvm::cast<VarExpr>(expr), useIsWriteOnly);
            if (!type) throw CompileError();
            break;
        case ExprKind::StringLiteralExpr:
            type = typecheckStringLiteralExpr(llvm::cast<StringLiteralExpr>(expr));
            break;
        case ExprKind::CharacterLiteralExpr:
            type = typecheckCharacterLiteralExpr(llvm::cast<CharacterLiteralExpr>(expr));
            break;
        case ExprKind::IntLiteralExpr:
            type = typecheckIntLiteralExpr(llvm::cast<IntLiteralExpr>(expr));
            break;
        case ExprKind::FloatLiteralExpr:
            type = typecheckFloatLiteralExpr(llvm::cast<FloatLiteralExpr>(expr));
            break;
        case ExprKind::BoolLiteralExpr:
            type = typecheckBoolLiteralExpr(llvm::cast<BoolLiteralExpr>(expr));
            break;
        case ExprKind::NullLiteralExpr:
            type = typecheckNullLiteralExpr(llvm::cast<NullLiteralExpr>(expr), expectedType);
            break;
        case ExprKind::UndefinedLiteralExpr:
            type = typecheckUndefinedLiteralExpr(llvm::cast<UndefinedLiteralExpr>(expr), expectedType);
            break;
        case ExprKind::ArrayLiteralExpr:
            type = typecheckArrayLiteralExpr(llvm::cast<ArrayLiteralExpr>(expr), expectedType);
            break;
        case ExprKind::TupleExpr:
            type = typecheckTupleExpr(llvm::cast<TupleExpr>(expr));
            break;
        case ExprKind::UnaryExpr:
            type = typecheckUnaryExpr(llvm::cast<UnaryExpr>(expr));
            break;
        case ExprKind::BinaryExpr:
            type = typecheckBinaryExpr(llvm::cast<BinaryExpr>(expr));
            break;
        case ExprKind::CallExpr:
            type = typecheckCallExpr(llvm::cast<CallExpr>(expr), expectedType);
            break;
        case ExprKind::SizeofExpr:
            type = typecheckSizeofExpr(llvm::cast<SizeofExpr>(expr));
            break;
        case ExprKind::AddressofExpr:
            type = typecheckAddressofExpr(llvm::cast<AddressofExpr>(expr));
            break;
        case ExprKind::MemberExpr:
            type = typecheckMemberExpr(llvm::cast<MemberExpr>(expr));
            break;
        case ExprKind::IndexExpr:
            type = typecheckIndexExpr(llvm::cast<IndexExpr>(expr));
            break;
        case ExprKind::IndexAssignmentExpr:
            type = typecheckIndexAssignmentExpr(llvm::cast<IndexAssignmentExpr>(expr));
            break;
        case ExprKind::UnwrapExpr:
            type = typecheckUnwrapExpr(llvm::cast<UnwrapExpr>(expr));
            break;
        case ExprKind::LambdaExpr:
            type = typecheckLambdaExpr(llvm::cast<LambdaExpr>(expr), expectedType);
            break;
        case ExprKind::IfExpr:
            type = typecheckIfExpr(llvm::cast<IfExpr>(expr));
            break;
        case ExprKind::ImplicitCastExpr:
            typecheckExpr(*llvm::cast<ImplicitCastExpr>(expr).getOperand());
            type = expr.getType();
            break;
        case ExprKind::VarDeclExpr:
            typecheckVarDecl(*llvm::cast<VarDeclExpr>(expr).varDecl);
            type = llvm::cast<VarDeclExpr>(expr).varDecl->getType();
            break;
    }

    expr.setType(type);
    expr.setAssignableType(type);

    if (!type.isUndefined()) { // TODO: Don't special-case the 'undefined' type.
        typecheckType(type, AccessLevel::None);
    }

    return expr.getType();
}

EnumCase* Typechecker::getEnumCase(const Expr& expr) {
    if (auto* memberExpr = llvm::dyn_cast<MemberExpr>(&expr)) {
        if (auto* varExpr = llvm::dyn_cast<VarExpr>(memberExpr->getBaseExpr())) {
            auto decls = findDecls(varExpr->getIdentifier());
            if (decls.size() == 1) {
                if (auto* enumDecl = llvm::dyn_cast<EnumDecl>(decls.front())) {
                    auto* enumCase = enumDecl->getCaseByName(memberExpr->getMemberName());
                    if (!enumCase) {
                        ERROR(expr.getLocation(), "enum '" << enumDecl->getName() << "' has no case named '" << memberExpr->getMemberName() << "'");
                    }
                    return enumCase;
                }
            }
        }
    }

    return nullptr;
}

void Typechecker::setMoved(Expr* expr, bool isMoved) {
    if (auto* varExpr = llvm::dyn_cast<VarExpr>(expr)) {
        ASSERT(varExpr->getDecl());

        if (isMoved) {
            movedDecls.insert(varExpr->getDecl());
        } else {
            movedDecls.erase(varExpr->getDecl());
        }
    }
}

void Typechecker::checkNotMoved(const Decl& decl, const VarExpr& expr) {
    if (movedDecls.count(&decl)) {
        ERROR(expr.getLocation(), "use of moved value '" << expr.getIdentifier() << "'");
    }
}
