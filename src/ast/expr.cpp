#include "expr.h"
#pragma warning(push, 0)
#include <llvm/Support/ErrorHandling.h>
#pragma warning(pop)
#include "ast.h"
#include "decl.h"
#include "token.h"

using namespace delta;

bool Expr::isAssignment() const {
    auto* binaryExpr = llvm::dyn_cast<BinaryExpr>(this);
    return binaryExpr && isAssignmentOperator(binaryExpr->getOperator());
}

bool Expr::isIncrementOrDecrementExpr() const {
    auto* unaryExpr = llvm::dyn_cast<UnaryExpr>(this);
    return unaryExpr && (unaryExpr->getOperator() == Token::Increment || unaryExpr->getOperator() == Token::Decrement);
}

bool Expr::isReferenceExpr() const {
    auto* unaryExpr = llvm::dyn_cast<UnaryExpr>(this);
    return unaryExpr && unaryExpr->getOperator() == Token::Ref;
};

bool Expr::isConstant() const {
    switch (getKind()) {
        case ExprKind::VarExpr: {
            auto* decl = llvm::cast<VarExpr>(this)->getDecl();

            if (auto* varDecl = llvm::dyn_cast<VarDecl>(decl)) {
                if (!varDecl->getType().isMutable() && varDecl->getInitializer()) {
                    return varDecl->getInitializer()->isConstant();
                }
            }

            return false;
        }

        case ExprKind::StringLiteralExpr:
        case ExprKind::CharacterLiteralExpr:
        case ExprKind::IntLiteralExpr:
        case ExprKind::FloatLiteralExpr:
        case ExprKind::BoolLiteralExpr:
        case ExprKind::NullLiteralExpr:
            return true;

        case ExprKind::ArrayLiteralExpr:
            for (auto& element : llvm::cast<ArrayLiteralExpr>(this)->getElements()) {
                if (!element->isConstant()) {
                    return false;
                }
            }
            return true;

        case ExprKind::TupleExpr:
            for (auto& element : llvm::cast<TupleExpr>(this)->getElements()) {
                if (!element.getValue()->isConstant()) {
                    return false;
                }
            }
            return true;

        case ExprKind::UnaryExpr: {
            auto unaryExpr = llvm::cast<UnaryExpr>(this);
            switch (unaryExpr->getOperator()) {
                case Token::Plus:
                case Token::Minus:
                case Token::Tilde:
                    return unaryExpr->getOperand().isConstant();
                default:
                    return false;
            }
        }
        case ExprKind::BinaryExpr:
            return llvm::cast<BinaryExpr>(this)->getLHS().isConstant() && llvm::cast<BinaryExpr>(this)->getRHS().isConstant();

        case ExprKind::CallExpr:
        case ExprKind::SizeofExpr: // TODO: sizeof should be a constant expression.
        case ExprKind::AddressofExpr:
        case ExprKind::MemberExpr:
        case ExprKind::IndexExpr:
        case ExprKind::IndexAssignmentExpr:
        case ExprKind::UnwrapExpr:
        case ExprKind::LambdaExpr:
        case ExprKind::UndefinedLiteralExpr:
        case ExprKind::VarDeclExpr:
            return false;

        case ExprKind::ImplicitCastExpr:
            return llvm::cast<ImplicitCastExpr>(this)->getOperand()->isConstant();

        case ExprKind::IfExpr:
            return llvm::cast<IfExpr>(this)->getCondition()->isConstant() && llvm::cast<IfExpr>(this)->getThenExpr()->isConstant() &&
                   llvm::cast<IfExpr>(this)->getElseExpr()->isConstant();
    }

    llvm_unreachable("all cases handled");
}

llvm::APSInt Expr::getConstantIntegerValue() const {
    switch (getKind()) {
        case ExprKind::VarExpr:
            if (auto* varDecl = llvm::dyn_cast<VarDecl>(llvm::cast<VarExpr>(this)->getDecl())) {
                if (!varDecl->getType().isMutable() && varDecl->getInitializer()) {
                    return varDecl->getInitializer()->getConstantIntegerValue();
                }
            }
            llvm_unreachable("not a constant integer");
        case ExprKind::CharacterLiteralExpr:
            return llvm::APSInt::get(llvm::cast<CharacterLiteralExpr>(this)->getValue());
        case ExprKind::IntLiteralExpr:
            return llvm::cast<IntLiteralExpr>(this)->getValue();
        case ExprKind::UnaryExpr:
            return llvm::cast<UnaryExpr>(this)->getConstantIntegerValue();
        case ExprKind::BinaryExpr:
            return llvm::cast<BinaryExpr>(this)->getConstantIntegerValue();
        case ExprKind::SizeofExpr:
        case ExprKind::IfExpr:
            llvm_unreachable("unimplemented");
        default:
            llvm_unreachable("not a constant integer");
    }
}

bool Expr::isLvalue() const {
    switch (getKind()) {
        case ExprKind::VarExpr:
        case ExprKind::MemberExpr:
        case ExprKind::IndexExpr:
            return true;
        case ExprKind::UnaryExpr:
            return llvm::cast<UnaryExpr>(this)->getOperator() == Token::Star;
        default:
            return false;
    }
}

Expr* Expr::instantiate(const llvm::StringMap<Type>& genericArgs) const {
    switch (getKind()) {
        case ExprKind::VarExpr: {
            auto* varExpr = llvm::cast<VarExpr>(this);
            auto it = genericArgs.find(varExpr->getIdentifier());
            auto identifier = it != genericArgs.end() ? it->second.getName() : varExpr->getIdentifier();
            return new VarExpr(identifier.str(), varExpr->getLocation());
        }
        case ExprKind::StringLiteralExpr: {
            auto* stringLiteralExpr = llvm::cast<StringLiteralExpr>(this);
            return new StringLiteralExpr(stringLiteralExpr->getValue().str(), stringLiteralExpr->getLocation());
        }
        case ExprKind::CharacterLiteralExpr: {
            auto* characterLiteralExpr = llvm::cast<CharacterLiteralExpr>(this);
            return new CharacterLiteralExpr(characterLiteralExpr->getValue(), characterLiteralExpr->getLocation());
        }
        case ExprKind::IntLiteralExpr: {
            auto* intLiteralExpr = llvm::cast<IntLiteralExpr>(this);
            return new IntLiteralExpr(intLiteralExpr->getValue(), intLiteralExpr->getLocation());
        }
        case ExprKind::FloatLiteralExpr: {
            auto* floatLiteralExpr = llvm::cast<FloatLiteralExpr>(this);
            return new FloatLiteralExpr(floatLiteralExpr->getValue(), floatLiteralExpr->getLocation());
        }
        case ExprKind::BoolLiteralExpr: {
            auto* boolLiteralExpr = llvm::cast<BoolLiteralExpr>(this);
            return new BoolLiteralExpr(boolLiteralExpr->getValue(), boolLiteralExpr->getLocation());
        }
        case ExprKind::NullLiteralExpr: {
            auto* nullLiteralExpr = llvm::cast<NullLiteralExpr>(this);
            return new NullLiteralExpr(nullLiteralExpr->getLocation());
        }
        case ExprKind::UndefinedLiteralExpr: {
            auto* undefinedLiteralExpr = llvm::cast<UndefinedLiteralExpr>(this);
            return new UndefinedLiteralExpr(undefinedLiteralExpr->getLocation());
        }
        case ExprKind::ArrayLiteralExpr: {
            auto* arrayLiteralExpr = llvm::cast<ArrayLiteralExpr>(this);
            auto elements = ::instantiate(arrayLiteralExpr->getElements(), genericArgs);
            return new ArrayLiteralExpr(std::move(elements), arrayLiteralExpr->getLocation());
        }
        case ExprKind::TupleExpr: {
            auto* tupleExpr = llvm::cast<TupleExpr>(this);
            auto elements = map(tupleExpr->getElements(),
                                [&](const NamedValue& element) { return NamedValue(element.getName().str(), element.getValue()->instantiate(genericArgs)); });
            return new TupleExpr(std::move(elements), tupleExpr->getLocation());
        }
        case ExprKind::UnaryExpr: {
            auto* unaryExpr = llvm::cast<UnaryExpr>(this);
            auto operand = unaryExpr->getOperand().instantiate(genericArgs);
            return new UnaryExpr(unaryExpr->getOperator(), operand, unaryExpr->getLocation());
        }
        case ExprKind::BinaryExpr: {
            auto* binaryExpr = llvm::cast<BinaryExpr>(this);
            auto lhs = binaryExpr->getLHS().instantiate(genericArgs);
            auto rhs = binaryExpr->getRHS().instantiate(genericArgs);
            return new BinaryExpr(binaryExpr->getOperator(), lhs, rhs, binaryExpr->getLocation());
        }
        case ExprKind::CallExpr: {
            auto* callExpr = llvm::cast<CallExpr>(this);
            auto callee = callExpr->getCallee().instantiate(genericArgs);
            auto args = map(callExpr->getArgs(), [&](auto& arg) { return NamedValue(arg.getName().str(), arg.getValue()->instantiate(genericArgs)); });
            auto callGenericArgs = map(callExpr->getGenericArgs(), [&](Type type) { return type.resolve(genericArgs); });
            return new CallExpr(callee, std::move(args), std::move(callGenericArgs), callExpr->getLocation());
        }
        case ExprKind::SizeofExpr: {
            auto* sizeofExpr = llvm::cast<SizeofExpr>(this);
            auto type = sizeofExpr->getType().resolve(genericArgs);
            return new SizeofExpr(type, sizeofExpr->getLocation());
        }
        case ExprKind::AddressofExpr: {
            auto* addressofExpr = llvm::cast<AddressofExpr>(this);
            auto operand = addressofExpr->getOperand().instantiate(genericArgs);
            return new AddressofExpr(operand, addressofExpr->getLocation());
        }
        case ExprKind::MemberExpr: {
            auto* memberExpr = llvm::cast<MemberExpr>(this);
            auto base = memberExpr->getBaseExpr()->instantiate(genericArgs);
            return new MemberExpr(base, memberExpr->getMemberName().str(), memberExpr->getLocation());
        }
        case ExprKind::IndexExpr: {
            auto* indexExpr = llvm::cast<IndexExpr>(this);
            auto base = indexExpr->getBase()->instantiate(genericArgs);
            auto index = indexExpr->getIndex()->instantiate(genericArgs);
            return new IndexExpr(base, index, indexExpr->getLocation());
        }
        case ExprKind::IndexAssignmentExpr: {
            auto* indexAssignmentExpr = llvm::cast<IndexAssignmentExpr>(this);
            auto base = indexAssignmentExpr->getBase()->instantiate(genericArgs);
            auto index = indexAssignmentExpr->getIndex()->instantiate(genericArgs);
            auto value = indexAssignmentExpr->getValue()->instantiate(genericArgs);
            return new IndexAssignmentExpr(base, index, value, indexAssignmentExpr->getLocation());
        }
        case ExprKind::UnwrapExpr: {
            auto* unwrapExpr = llvm::cast<UnwrapExpr>(this);
            auto operand = unwrapExpr->getOperand().instantiate(genericArgs);
            return new UnwrapExpr(operand, unwrapExpr->getLocation());
        }
        case ExprKind::LambdaExpr: {
            auto* lambdaExpr = llvm::cast<LambdaExpr>(this);
            auto params = instantiateParams(lambdaExpr->getFunctionDecl()->getParams(), genericArgs);
            auto body = ::instantiate(lambdaExpr->getFunctionDecl()->getBody(), genericArgs);
            auto lambda = new LambdaExpr(std::move(params), lambdaExpr->getFunctionDecl()->getModule(), lambdaExpr->getLocation());
            lambda->functionDecl->setBody(std::move(body));
            return lambda;
        }
        case ExprKind::IfExpr: {
            auto* ifExpr = llvm::cast<IfExpr>(this);
            auto condition = ifExpr->getCondition()->instantiate(genericArgs);
            auto thenExpr = ifExpr->getThenExpr()->instantiate(genericArgs);
            auto elseExpr = ifExpr->getElseExpr()->instantiate(genericArgs);
            return new IfExpr(condition, thenExpr, elseExpr, ifExpr->getLocation());
        }
        case ExprKind::ImplicitCastExpr: {
            auto implicitCastExpr = llvm::cast<ImplicitCastExpr>(this);
            return new ImplicitCastExpr(implicitCastExpr->getOperand()->instantiate(genericArgs), implicitCastExpr->getType().resolve(genericArgs),
                                        implicitCastExpr->getImplicitCastKind());
        }
        case ExprKind::VarDeclExpr: {
            auto varDeclExpr = llvm::cast<VarDeclExpr>(this);
            return new VarDeclExpr(llvm::cast<VarDecl>(varDeclExpr->varDecl->instantiate(genericArgs, {})));
        }
    }

    llvm_unreachable("all cases handled");
}

FieldDecl* Expr::getFieldDecl() const {
    switch (getKind()) {
        case ExprKind::VarExpr:
            return llvm::dyn_cast<FieldDecl>(llvm::cast<VarExpr>(this)->getDecl());
        case ExprKind::MemberExpr:
            return llvm::dyn_cast_or_null<FieldDecl>(llvm::cast<MemberExpr>(this)->getDecl());
        default:
            return nullptr;
    }
}

const Expr* Expr::withoutImplicitCast() const {
    if (auto implicitCastExpr = llvm::dyn_cast<ImplicitCastExpr>(this)) {
        return implicitCastExpr->getOperand();
    }
    return this;
}

bool Expr::isThis() const {
    return isVarExpr() && llvm::cast<VarExpr>(this)->getIdentifier() == "this";
}

llvm::StringRef CallExpr::getFunctionName() const {
    switch (getCallee().getKind()) {
        case ExprKind::VarExpr:
            return llvm::cast<VarExpr>(getCallee()).getIdentifier();
        case ExprKind::MemberExpr:
            return llvm::cast<MemberExpr>(getCallee()).getMemberName();
        default:
            return "(anonymous function)";
    }
}

static Type getReceiverType(const CallExpr& call) {
    if (call.getCallee().isMemberExpr()) {
        return call.getReceiver()->getType().removeOptional().removePointer();
    }
    return Type();
}

std::string CallExpr::getQualifiedFunctionName() const {
    return ::getQualifiedFunctionName(::getReceiverType(*this), getFunctionName(), {});
}

bool CallExpr::isMoveInit() const {
    if (getFunctionName() != "init") return false;
    if (getArgs().size() != 1) return false;

    if (Type receiverType = ::getReceiverType(*this)) {
        return getArgs()[0].getValue()->getType().equalsIgnoreTopLevelMutable(receiverType);
    }

    return false;
}

const Expr* CallExpr::getReceiver() const {
    if (!isMethodCall()) return nullptr;
    return llvm::cast<MemberExpr>(getCallee()).getBaseExpr();
}

Expr* CallExpr::getReceiver() {
    if (!isMethodCall()) return nullptr;
    return llvm::cast<MemberExpr>(getCallee()).getBaseExpr();
}

llvm::APSInt UnaryExpr::getConstantIntegerValue() const {
    auto operand = getOperand().getConstantIntegerValue();

    switch (getOperator()) {
        case Token::Plus:
            return operand;
        case Token::Minus:
            return -operand;
        case Token::Tilde:
            return ~operand;
        default:
            llvm_unreachable("invalid constant integer prefix operator");
    }
}

bool delta::isBuiltinOp(Token::Kind op, Type left, Type right) {
    if (op == Token::Assignment) return true;
    if (op == Token::DotDot || op == Token::DotDotDot) return false;
    if (op == Token::PointerEqual || op == Token::PointerNotEqual) return true;
    if (left.isPointerType() && right.isPointerType()) return false;
    if (left.isEnumType() && left.equalsIgnoreTopLevelMutable(right)) return true;
    if (left.isEnumType() && right.isInteger()) return true;
    if (left.isInteger() && right.isEnumType()) return true;
    if (left.isImplementedAsPointer() && right.isImplementedAsPointer()) return true;
    return left.isBuiltinType() && right.isBuiltinType();
}

llvm::APSInt BinaryExpr::getConstantIntegerValue() const {
    // TODO: Add overflow checks.
    // TODO: Handle signedness for '>>' operator;

    auto lhs = getLHS().getConstantIntegerValue();
    auto rhs = getRHS().getConstantIntegerValue();

    switch (getOperator()) {
        case Token::Plus:
            return lhs + rhs;
        case Token::Minus:
            return lhs - rhs;
        case Token::Star:
            return lhs * rhs;
        case Token::Slash:
            return lhs / rhs;
        case Token::Modulo:
            return lhs % rhs;
        case Token::And:
            return lhs & rhs;
        case Token::Or:
            return lhs | rhs;
        case Token::Xor:
            return lhs ^ rhs;
        case Token::LeftShift:
            return lhs << static_cast<unsigned>(rhs.getZExtValue());
        case Token::RightShift:
            return lhs >> static_cast<unsigned>(rhs.getZExtValue());
        default:
            llvm_unreachable("invalid constant integer binary operator");
    }
}

LambdaExpr::LambdaExpr(std::vector<ParamDecl>&& params, Module* module, SourceLocation location) : Expr(ExprKind::LambdaExpr, location) {
    static uint64_t nameCounter = 0;
    FunctionProto proto("__lambda" + std::to_string(nameCounter++), std::move(params), Type(), false, false);
    this->functionDecl = new FunctionDecl(std::move(proto), std::vector<Type>(), AccessLevel::Private, *module, getLocation());
}

VarDeclExpr::VarDeclExpr(VarDecl* varDecl) : Expr(ExprKind::VarDeclExpr, varDecl->getLocation()), varDecl(varDecl) {}

const Expr* TupleExpr::getElementByName(llvm::StringRef name) const {
    for (auto& element : getElements()) {
        if (element.getName() == name) {
            return element.getValue();
        }
    }
    return nullptr;
}
