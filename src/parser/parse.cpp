#include "parse.h"
#include <forward_list>
#include <sstream>
#include <vector>
#pragma warning(push, 0)
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SaveAndRestore.h>
#pragma warning(pop)
#include "lex.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../ast/token.h"
#include "../driver/driver.h"
#include "../support/utility.h"

using namespace delta;

static llvm::MemoryBuffer* getFileMemoryBuffer(llvm::StringRef filePath) {
    auto buffer = llvm::MemoryBuffer::getFile(filePath);
    if (!buffer) ABORT("couldn't open file '" << filePath << "'");
    return buffer->release();
}

Parser::Parser(llvm::StringRef filePath, Module& module, const CompileOptions& options)
: lexer(getFileMemoryBuffer(filePath)), currentModule(&module), currentTokenIndex(0), options(options) {
    tokenBuffer.emplace_back(lexer.nextToken());
}

Token Parser::currentToken() {
    ASSERT(currentTokenIndex < tokenBuffer.size());
    return tokenBuffer[currentTokenIndex];
}

SourceLocation Parser::getCurrentLocation() {
    return currentToken().getLocation();
}

Token Parser::lookAhead(int offset) {
    if (int(currentTokenIndex) + offset < 0) return Token(Token::None, SourceLocation());
    int count = int(currentTokenIndex) + offset - int(tokenBuffer.size()) + 1;
    while (count-- > 0) {
        tokenBuffer.emplace_back(lexer.nextToken());
    }
    return tokenBuffer[currentTokenIndex + offset];
}

Token Parser::consumeToken() {
    Token token = currentToken();
    if (currentTokenIndex + 1 == tokenBuffer.size()) {
        tokenBuffer.emplace_back(lexer.nextToken());
    }
    currentTokenIndex++;
    return token;
}

/// Adds quotes around the string representation of the given token unless
/// it's an identifier, numeric literal, string literal, or end-of-file.
static std::string quote(Token::Kind tokenKind) {
    std::ostringstream stream;
    if (tokenKind < Token::Break) {
        stream << tokenKind;
    } else {
        stream << '\'' << tokenKind << '\'';
    }
    return stream.str();
}

static std::string formatList(llvm::ArrayRef<Token::Kind> tokens) {
    std::string result;

    for (auto& token : tokens) {
        result += quote(token);

        if (tokens.size() > 2 && &token != &tokens.back()) {
            result += ", ";
        }

        if (&token == &tokens.back() - 1) {
            if (tokens.size() == 2) {
                result += " ";
            }
            result += "or ";
        }
    }

    return result;
}

[[noreturn]] static void unexpectedToken(Token token, llvm::ArrayRef<Token::Kind> expected = {}, const char* contextInfo = nullptr) {
    if (expected.size() == 0) {
        ERROR(token.getLocation(), "unexpected " << quote(token) << (contextInfo ? " " : "") << (contextInfo ? contextInfo : ""));
    } else {
        ERROR(token.getLocation(),
              "expected " << formatList(expected) << (contextInfo ? " " : "") << (contextInfo ? contextInfo : "") << ", got " << quote(token));
    }
}

Token Parser::parse(llvm::ArrayRef<Token::Kind> expected, const char* contextInfo) {
    if (!llvm::is_contained(expected, currentToken())) {
        unexpectedToken(currentToken(), expected, contextInfo);
    }
    return consumeToken();
}

void Parser::parseStmtTerminator(const char* contextInfo) {
    if (getCurrentLocation().line != lookAhead(-1).getLocation().line) return;

    switch (currentToken()) {
        case Token::RightBrace:
        case Token::RightParen:
        case Token::In:
            return;
        case Token::Semicolon:
            consumeToken();
            return;
        default:
            unexpectedToken(currentToken(), { Token::Newline, Token::Semicolon }, contextInfo);
    }
}

/// argument-list ::= '(' ')' | '(' nonempty-argument-list ')'
/// nonempty-argument-list ::= argument | nonempty-argument-list ',' argument
/// argument ::= (id ':')? expr
std::vector<NamedValue> Parser::parseArgumentList(bool allowEmpty) {
    parse(Token::LeftParen);
    std::vector<NamedValue> args;

    if (currentToken() == Token::RightParen && allowEmpty) {
        consumeToken();
        return {};
    }

    do {
        std::string name;
        SourceLocation location = SourceLocation();
        if (lookAhead(1) == Token::Colon) {
            auto result = parse(Token::Identifier);
            name = result.getString().str();
            location = result.getLocation();
            consumeToken();
        }
        auto value = parseExpr();
        if (!location.isValid()) location = value->getLocation();
        args.push_back({ std::move(name), value, location });
    } while (parse({ Token::Comma, Token::RightParen }) == Token::Comma);

    return args;
}

/// var-expr ::= id
VarExpr* Parser::parseVarExpr() {
    ASSERT(currentToken() == Token::Identifier);
    auto id = consumeToken();
    return new VarExpr(id.getString().str(), id.getLocation());
}

VarExpr* Parser::parseThis() {
    ASSERT(currentToken() == Token::This);
    auto expr = new VarExpr("this", getCurrentLocation());
    consumeToken();
    return expr;
}

static std::string replaceEscapeChars(llvm::StringRef literalContent, SourceLocation literalStartLocation) {
    std::string result;
    result.reserve(literalContent.size());

    for (auto it = literalContent.begin(), end = literalContent.end(); it != end; ++it) {
        if (*it == '\\') {
            ++it;
            ASSERT(it != end);
            switch (*it) {
                case '0':
                    result += '\0';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case '"':
                    result += '"';
                    break;
                case '\'':
                    result += '\'';
                    break;
                case '\\':
                    result += '\\';
                    break;
                default:
                    auto itColumn = literalStartLocation.column + 1 + (it - literalContent.begin());
                    SourceLocation itLocation(literalStartLocation.file, literalStartLocation.line, itColumn);
                    ERROR(itLocation, "unknown escape character '\\" << *it << "'");
            }
            continue;
        }
        result += *it;
    }
    return result;
}

StringLiteralExpr* Parser::parseStringLiteral() {
    ASSERT(currentToken() == Token::StringLiteral);
    auto content = replaceEscapeChars(currentToken().getString().drop_back().drop_front(), getCurrentLocation());
    auto expr = new StringLiteralExpr(std::move(content), getCurrentLocation());
    consumeToken();
    return expr;
}

CharacterLiteralExpr* Parser::parseCharacterLiteral() {
    ASSERT(currentToken() == Token::CharacterLiteral);
    auto content = replaceEscapeChars(currentToken().getString().drop_back().drop_front(), getCurrentLocation());
    if (content.size() != 1) ERROR(getCurrentLocation(), "character literal must consist of a single UTF-8 byte");
    auto expr = new CharacterLiteralExpr(content[0], getCurrentLocation());
    consumeToken();
    return expr;
}

IntLiteralExpr* Parser::parseIntLiteral() {
    ASSERT(currentToken() == Token::IntegerLiteral);
    auto expr = new IntLiteralExpr(currentToken().getIntegerValue(), getCurrentLocation());
    consumeToken();
    return expr;
}

FloatLiteralExpr* Parser::parseFloatLiteral() {
    ASSERT(currentToken() == Token::FloatLiteral);
    auto expr = new FloatLiteralExpr(currentToken().getFloatingPointValue(), getCurrentLocation());
    consumeToken();
    return expr;
}

BoolLiteralExpr* Parser::parseBoolLiteral() {
    BoolLiteralExpr* expr;
    switch (currentToken()) {
        case Token::True:
            expr = new BoolLiteralExpr(true, getCurrentLocation());
            break;
        case Token::False:
            expr = new BoolLiteralExpr(false, getCurrentLocation());
            break;
        default:
            llvm_unreachable("all cases handled");
    }
    consumeToken();
    return expr;
}

NullLiteralExpr* Parser::parseNullLiteral() {
    ASSERT(currentToken() == Token::Null);
    auto expr = new NullLiteralExpr(getCurrentLocation());
    consumeToken();
    return expr;
}

UndefinedLiteralExpr* Parser::parseUndefinedLiteral() {
    ASSERT(currentToken() == Token::Undefined);
    auto expr = new UndefinedLiteralExpr(getCurrentLocation());
    consumeToken();
    return expr;
}

/// array-literal ::= '[' expr-list ']'
ArrayLiteralExpr* Parser::parseArrayLiteral() {
    ASSERT(currentToken() == Token::LeftBracket);
    auto location = getCurrentLocation();
    consumeToken();
    auto elements = parseExprList();
    parse(Token::RightBracket);
    return new ArrayLiteralExpr(std::move(elements), location);
}

/// tuple-literal ::= '(' tuple-literal-elements ')'
/// tuple-literal-elements ::= tuple-literal-element | tuple-literal-elements ',' tuple-literal-element
/// tuple-literal-element ::= (id ':')? expr
/// paren-expr ::= '(' expr ')'
Expr* Parser::parseTupleLiteralOrParenExpr() {
    ASSERT(currentToken() == Token::LeftParen);
    auto location = getCurrentLocation();
    auto elements = parseArgumentList(false);

    if (elements.size() == 1 && elements[0].getName().empty()) {
        return elements[0].getValue();
    }

    for (auto& element : elements) {
        if (element.getName().empty()) {
            if (auto* varExpr = llvm::dyn_cast<VarExpr>(element.getValue())) {
                element.setName(varExpr->getIdentifier().str());
            }
        }
    }

    return new TupleExpr(std::move(elements), location);
}

/// non-empty-type-list ::= type | type ',' non-empty-type-list
std::vector<Type> Parser::parseNonEmptyTypeList() {
    std::vector<Type> types;

    while (true) {
        types.push_back(parseType());

        if (currentToken() == Token::Comma) {
            consumeToken();
        } else {
            if (currentToken() == Token::RightShift) {
                tokenBuffer[currentTokenIndex] = Token(Token::Greater, currentToken().getLocation());
                tokenBuffer.insert(tokenBuffer.begin() + currentTokenIndex + 1, Token(Token::Greater, currentToken().getLocation().nextColumn()));
            }
            return types;
        }
    }
}

/// generic-argument-list ::= '<' non-empty-type-list '>'
std::vector<Type> Parser::parseGenericArgumentList() {
    ASSERT(currentToken() == Token::Less);
    consumeToken();
    std::vector<Type> genericArgs = parseNonEmptyTypeList();
    parse(Token::Greater);
    return genericArgs;
}

int64_t Parser::parseArraySizeInBrackets() {
    ASSERT(currentToken() == Token::LeftBracket);
    consumeToken();
    int64_t arraySize;

    switch (currentToken()) {
        case Token::IntegerLiteral:
            arraySize = consumeToken().getIntegerValue().getExtValue();
            break;
        case Token::RightBracket:
            arraySize = ArrayType::runtimeSize;
            break;
        default:
            ERROR(getCurrentLocation(), "non-literal array bounds not implemented yet");
    }

    parse(Token::RightBracket);
    return arraySize;
}

/// simple-type ::= id | id generic-argument-list | id '[' (int-literal | '*')? ']'
Type Parser::parseSimpleType(Mutability mutability) {
    auto identifier = parse(Token::Identifier);
    std::vector<Type> genericArgs;

    switch (currentToken()) {
        case Token::Less:
            genericArgs = parseGenericArgumentList();
            LLVM_FALLTHROUGH;
        default:
            return BasicType::get(identifier.getString(), std::move(genericArgs), mutability, identifier.getLocation());
        case Token::LeftBracket:
            auto bracketLocation = getCurrentLocation();
            Type elementType = BasicType::get(identifier.getString(), {}, mutability, identifier.getLocation());
            return ArrayType::get(elementType, parseArraySizeInBrackets(), mutability, bracketLocation);
    }
}

/// tuple-type ::= '(' tuple-type-elements ')'
/// tuple-type-elements ::= tuple-type-element | tuple-type-elements ',' tuple-type-element
/// tuple-type-element ::= type id?
Type Parser::parseTupleType() {
    ASSERT(currentToken() == Token::LeftParen);
    auto location = getCurrentLocation();
    consumeToken();
    std::vector<TupleElement> elements;

    while (currentToken() != Token::RightParen) {
        auto type = parseType();
        std::string name = currentToken() == Token::Identifier ? consumeToken().getString().str() : "";
        elements.push_back({ std::move(name), type });
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }

    consumeToken();
    return TupleType::get(std::move(elements), Mutability::Mutable, location);
}

/// function-type ::= type '(' param-types ')'
/// param-types ::= '' | non-empty-param-types
/// non-empty-param-types ::= type | type ',' non-empty-param-types
Type Parser::parseFunctionType(Type returnType) {
    parse(Token::LeftParen);
    std::vector<Type> paramTypes;

    while (currentToken() != Token::RightParen) {
        paramTypes.emplace_back(parseType());
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }

    consumeToken();
    return FunctionType::get(returnType, std::move(paramTypes), Mutability::Mutable, returnType.getLocation());
}

/// type ::= simple-type | 'const' simple-type | type '&' | type '*' | type '?' | function-type | tuple-type
Type Parser::parseType() {
    Type type;
    auto location = getCurrentLocation();

    switch (currentToken()) {
        case Token::Identifier:
            type = parseSimpleType(Mutability::Mutable);
            break;
        case Token::Const:
            consumeToken();
            type = parseSimpleType(Mutability::Const);
            break;
        case Token::LeftParen:
            type = parseTupleType();
            break;
        default:
            unexpectedToken(currentToken());
    }

    while (true) {
        switch (currentToken()) {
            case Token::Ref:
                type = PointerType::get(type, Mutability::Mutable, getCurrentLocation());
                consumeToken();
                break;
            case Token::Star:
                type = ArrayType::get(type, ArrayType::unknownSize, type.getMutability(), getCurrentLocation());
                consumeToken();
                break;
            case Token::QuestionMark:
                type = OptionalType::get(type, Mutability::Mutable, getCurrentLocation());
                consumeToken();
                break;
            case Token::LeftParen:
                type = parseFunctionType(type);
                break;
            case Token::LeftBracket:
                type = ArrayType::get(type, parseArraySizeInBrackets(), type.getMutability(), getCurrentLocation());
                break;
            default:
                return type.withLocation(location);
        }
    }
}

/// sizeof-expr ::= 'sizeof' '(' type ')'
SizeofExpr* Parser::parseSizeofExpr() {
    ASSERT(currentToken() == Token::Sizeof);
    auto location = getCurrentLocation();
    consumeToken();
    parse(Token::LeftParen);
    auto type = parseType();
    parse(Token::RightParen);
    return new SizeofExpr(type, location);
}

/// addressof-expr ::= 'addressof' '(' expr ')'
AddressofExpr* Parser::parseAddressofExpr() {
    ASSERT(currentToken() == Token::Addressof);
    auto location = getCurrentLocation();
    consumeToken();
    parse(Token::LeftParen);
    auto operand = parseExpr();
    parse(Token::RightParen);
    return new AddressofExpr(operand, location);
}

/// member-expr ::= expr '.' id
MemberExpr* Parser::parseMemberExpr(Expr* lhs) {
    auto location = getCurrentLocation();
    auto member = parse(Token::Identifier);
    return new MemberExpr(lhs, member.getString().str(), location);
}

/// index-expr ::= expr '[' expr ']'
/// index-assignment-expr ::= index-expr '=' expr
Expr* Parser::parseIndexExprOrIndexAssignmentExpr(Expr* base) {
    ASSERT(currentToken() == Token::LeftBracket);
    auto location = getCurrentLocation();
    consumeToken();
    auto index = parseExpr();
    parse(Token::RightBracket);

    if (currentToken() == Token::Assignment) {
        consumeToken();
        return new IndexAssignmentExpr(base, index, parseExpr(), location);
    }

    return new IndexExpr(base, index, location);
}

/// unwrap-expr ::= expr '!'
UnwrapExpr* Parser::parseUnwrapExpr(Expr* operand) {
    ASSERT(currentToken() == Token::Not);
    auto location = getCurrentLocation();
    consumeToken();
    return new UnwrapExpr(operand, location);
}

/// call-expr ::= expr generic-argument-list? argument-list
CallExpr* Parser::parseCallExpr(Expr* callee) {
    std::vector<Type> genericArgs;
    if (currentToken() == Token::Less) {
        genericArgs = parseGenericArgumentList();
    }
    auto location = getCurrentLocation();
    auto args = parseArgumentList(true);
    return new CallExpr(callee, std::move(args), std::move(genericArgs), location);
}

LambdaExpr* Parser::parseLambdaExpr() {
    ASSERT(currentToken().is({ Token::LeftParen, Token::Identifier }));
    auto location = getCurrentLocation();
    std::vector<ParamDecl> params;

    if (currentToken() == Token::Identifier) {
        auto paramName = consumeToken();
        params.push_back(ParamDecl(Type(), paramName.getString().str(), false, paramName.getLocation()));
    } else {
        params = parseParamList(nullptr, false);
    }

    parse(Token::RightArrow);
    auto lambda = new LambdaExpr(std::move(params), currentModule, location);

    if (currentToken() == Token::LeftBrace) {
        lambda->functionDecl->setBody(parseBlock(lambda->functionDecl));
    } else {
        auto expr = parseExpr();
        lambda->functionDecl->setBody({ new ReturnStmt(expr, expr->getLocation()) });
    }

    return lambda;
}

/// if-expr ::= expr '?' expr ':' expr
IfExpr* Parser::parseIfExpr(Expr* condition) {
    ASSERT(currentToken() == Token::QuestionMark);
    auto location = getCurrentLocation();
    consumeToken();
    auto thenExpr = parseExpr();
    parse(Token::Colon);
    auto elseExpr = parseExpr();
    return new IfExpr(condition, thenExpr, elseExpr, location);
}

bool Parser::shouldParseVarStmt() {
    if (currentToken().is({ Token::Var, Token::Const })) return true;
    if (!currentToken().is({ Token::Identifier, Token::LeftParen })) return false;
    int offset = 2;

    while (true) {
        if (lookAhead(offset).is(Token::Assignment)) {
            if (lookAhead(offset - 1).is(Token::Identifier)) {
                if (lookAhead(offset - 2).is({ Token::Identifier, Token::RightBracket, Token::QuestionMark, Token::Greater })) {
                    return true;
                }
                if (lookAhead(offset - 2).is(Token::Star)) {
                    if (lookAhead(offset - 3).is(Token::Semicolon) || lookAhead(offset - 2).getLocation().line != lookAhead(offset - 3).getLocation().line) {
                        return false;
                    }
                    return true;
                }
            }
            return false;
        } else {
            if (lookAhead(offset).is(Token::Semicolon) || lookAhead(offset).getLocation().line != lookAhead(offset - 1).getLocation().line) {
                if (lookAhead(offset - 1).is(Token::Identifier)) {
                    if (lookAhead(offset - 2).is({ Token::Identifier, Token::RightBracket, Token::QuestionMark, Token::Greater, Token::Star })) {
                        return true;
                    }
                }
                return false;
            }
            offset++;
        }
    }
}

bool Parser::shouldParseGenericArgumentList() {
    // Temporary hack: use spacing to determine whether to parse a generic argument list
    // of a less-than binary expression. Zero spaces on either side of '<' will cause it
    // to be interpreted as a generic argument list, for now.
    return lookAhead(0).getLocation().column + int(lookAhead(0).getString().size()) == lookAhead(1).getLocation().column ||
           lookAhead(1).getLocation().column + 1 == lookAhead(2).getLocation().column;
}

/// Returns true if a right-arrow token immediately follows the current set of parentheses.
bool Parser::arrowAfterParentheses() {
    ASSERT(currentToken() == Token::LeftParen);
    int offset = 1;

    for (int parenDepth = 1; parenDepth > 0; ++offset) {
        switch (lookAhead(offset)) {
            case Token::LeftParen:
                ++parenDepth;
                break;
            case Token::RightParen:
                --parenDepth;
                break;
            default:
                break;
        }
    }

    return lookAhead(offset) == Token::RightArrow;
}

/// postfix-expr ::= postfix-expr postfix-op | call-expr | variable-expr | string-literal |
///                  int-literal | float-literal | bool-literal | null-literal |
///                  paren-expr | array-literal | tuple-literal | index-expr | index-assignment-expr
///                  member-expr | unwrap-expr | lambda-expr | sizeof-expr | addressof-expr
Expr* Parser::parsePostfixExpr() {
    Expr* expr;

    switch (currentToken()) {
        case Token::Identifier:
            switch (lookAhead(1)) {
                case Token::LeftParen:
                    expr = parseCallExpr(parseVarExpr());
                    break;
                case Token::RightArrow:
                    expr = parseLambdaExpr();
                    break;
                case Token::Less:
                    if (shouldParseGenericArgumentList()) {
                        expr = parseCallExpr(parseVarExpr());
                        break;
                    }
                    LLVM_FALLTHROUGH;
                default:
                    expr = parseVarExpr();
                    break;
            }
            break;
        case Token::StringLiteral:
            expr = parseStringLiteral();
            break;
        case Token::CharacterLiteral:
            expr = parseCharacterLiteral();
            break;
        case Token::IntegerLiteral:
            expr = parseIntLiteral();
            break;
        case Token::FloatLiteral:
            expr = parseFloatLiteral();
            break;
        case Token::True:
        case Token::False:
            expr = parseBoolLiteral();
            break;
        case Token::Null:
            expr = parseNullLiteral();
            break;
        case Token::This:
            expr = parseThis();
            break;
        case Token::LeftParen:
            if (arrowAfterParentheses()) {
                expr = parseLambdaExpr();
            } else {
                expr = parseTupleLiteralOrParenExpr();
            }
            break;
        case Token::LeftBracket:
            expr = parseArrayLiteral();
            break;
        case Token::Sizeof:
            expr = parseSizeofExpr();
            break;
        case Token::Addressof:
            expr = parseAddressofExpr();
            break;
        case Token::Undefined:
            expr = parseUndefinedLiteral();
            break;
        default:
            unexpectedToken(currentToken());
            break;
    }

    while (true) {
        switch (currentToken()) {
            case Token::LeftBracket:
                expr = parseIndexExprOrIndexAssignmentExpr(expr);
                break;
            case Token::LeftParen:
                expr = parseCallExpr(expr);
                break;
            case Token::Dot:
                consumeToken();
                expr = parseMemberExpr(expr);
                break;
            case Token::Increment:
            case Token::Decrement:
                expr = parseIncrementOrDecrementExpr(expr);
                break;
            case Token::Not:
                expr = parseUnwrapExpr(expr);
                break;
            default:
                return expr;
        }
    }
}

/// prefix-expr ::= prefix-operator (prefix-expr | postfix-expr)
UnaryExpr* Parser::parsePrefixExpr() {
    ASSERT(isUnaryOperator(currentToken()));
    auto op = consumeToken();
    return new UnaryExpr(op.getKind(), parsePreOrPostfixExpr(), op.getLocation());
}

Expr* Parser::parsePreOrPostfixExpr() {
    return isUnaryOperator(currentToken()) ? parsePrefixExpr() : parsePostfixExpr();
}

/// inc-expr ::= expr '++'
/// dec-expr ::= expr '--'
UnaryExpr* Parser::parseIncrementOrDecrementExpr(Expr* operand) {
    auto op = parse({ Token::Increment, Token::Decrement });
    return new UnaryExpr(op.getKind(), operand, op.getLocation());
}

/// binary-expr ::= expr op expr
Expr* Parser::parseBinaryExpr(int minPrecedence) {
    auto lhs = parsePreOrPostfixExpr();

    while (((isBinaryOperator(currentToken()) || currentToken() == Token::QuestionMark) && getPrecedence(currentToken()) >= minPrecedence)) {
        if (currentToken() == Token::QuestionMark) {
            lhs = parseIfExpr(lhs);
            continue;
        }

        auto backtrackLocation = currentTokenIndex;
        auto op = consumeToken();
        auto rhs = parseBinaryExpr(getPrecedence(op) + 1);

        if (isAssignmentOperator(currentToken())) {
            currentTokenIndex = backtrackLocation;
            break;
        }

        lhs = new BinaryExpr(op.getKind(), lhs, rhs, op.getLocation());
    }

    return lhs;
}

/// expr ::= prefix-expr | postfix-expr | binary-expr | if-expr
Expr* Parser::parseExpr() {
    return parseBinaryExpr(0);
}

Expr* Parser::parseExprOrVarDecl(Decl* parent) {
    if (!shouldParseVarStmt()) {
        return parseExpr();
    } else {
        return new VarDeclExpr(parseVarDecl(parent, AccessLevel::None));
    }
}

/// expr-list ::= '' | nonempty-expr-list
/// nonempty-expr-list ::= expr | expr ',' nonempty-expr-list
std::vector<Expr*> Parser::parseExprList() {
    std::vector<Expr*> exprs;

    switch (currentToken()) {
        case Token::Semicolon:
        case Token::RightBrace:
        case Token::RightBracket:
            return exprs;
        default:
            break;
    }

    while (true) {
        exprs.emplace_back(parseExpr());
        if (currentToken() != Token::Comma) return exprs;
        consumeToken();
    }
}

/// return-stmt ::= 'return' expr ('\n' | ';')
ReturnStmt* Parser::parseReturnStmt() {
    ASSERT(currentToken() == Token::Return);
    auto location = getCurrentLocation();
    consumeToken();
    auto returnValue = currentToken().is({ Token::Semicolon, Token::RightBrace }) ? nullptr : parseExpr();
    parseStmtTerminator();
    return new ReturnStmt(returnValue, location);
}

/// var-decl ::= type-specifier id '=' initializer ('\n' | ';')
/// type-specifier ::= 'const' | 'const' type | type | 'var'
/// initializer ::= expr | 'undefined'
VarDecl* Parser::parseVarDecl(Decl* parent, AccessLevel accessLevel) {
    Type type;
    auto mutability = Mutability::Mutable;

    if (currentToken() == Token::Const) {
        consumeToken();
        mutability = Mutability::Const;
    }

    if (currentToken() == Token::Var) {
        consumeToken();
    } else if (lookAhead(1) != Token::Assignment) {
        type = parseType();
    }

    auto name = parse(Token::Identifier);
    return parseVarDeclAfterName(parent, accessLevel, type.withMutability(mutability), name.getString(), name.getLocation());
}

VarDecl* Parser::parseVarDeclAfterName(Decl* parent, AccessLevel accessLevel, Type type, llvm::StringRef name, SourceLocation nameLocation) {
    Expr* initializer = nullptr;

    if (currentToken() == Token::Assignment) {
        consumeToken();
        initializer = parseExpr();
    } else if (currentToken() == Token::Semicolon || currentToken().getLocation().line != lookAhead(-1).getLocation().line) {
        WARN(nameLocation, "missing initializer");
    }

    parseStmtTerminator();
    return new VarDecl(type, name.str(), initializer, parent, accessLevel, *currentModule, nameLocation);
}

/// var-stmt ::= var-decl
VarStmt* Parser::parseVarStmt(Decl* parent) {
    return new VarStmt(parseVarDecl(parent, AccessLevel::None));
}

/// expr-stmt ::= expr ('\n' | ';')
ExprStmt* Parser::parseExprStmt() {
    auto stmt = new ExprStmt(parseExpr());
    parseStmtTerminator();
    return stmt;
}

/// block ::= '{' stmt* '}'
std::vector<Stmt*> Parser::parseBlock(Decl* parent) {
    parse(Token::LeftBrace);
    std::vector<Stmt*> stmts;
    while (currentToken() != Token::RightBrace) {
        stmts.push_back(parseStmt(parent));
    }
    consumeToken();
    return stmts;
}

/// block-or-stmt ::= block | stmt
std::vector<Stmt*> Parser::parseBlockOrStmt(Decl* parent) {
    if (currentToken() == Token::LeftBrace) {
        return parseBlock(parent);
    } else {
        return { parseStmt(parent) };
    }
}

/// defer-stmt ::= 'defer' expr ('\n' | ';')
DeferStmt* Parser::parseDeferStmt() {
    ASSERT(currentToken() == Token::Defer);
    consumeToken();
    auto stmt = new DeferStmt(parseExpr());
    parseStmtTerminator();
    return stmt;
}

/// if-stmt ::= 'if' (expr | var-decl) block-or-stmt ('else' block-or-stmt)?
IfStmt* Parser::parseIfStmt(Decl* parent) {
    ASSERT(currentToken() == Token::If);
    consumeToken();
    bool parens = currentToken() == Token::LeftParen;
    if (parens) consumeToken();
    auto condition = parseExprOrVarDecl(parent);
    if (parens) parse(Token::RightParen);
    auto thenStmts = parseBlockOrStmt(parent);
    std::vector<Stmt*> elseStmts;
    if (currentToken() == Token::Else) {
        consumeToken();
        elseStmts = parseBlockOrStmt(parent);
    }
    return new IfStmt(condition, std::move(thenStmts), std::move(elseStmts));
}

/// while-stmt ::= 'while' (expr | var-decl) block-or-stmt
WhileStmt* Parser::parseWhileStmt(Decl* parent) {
    ASSERT(currentToken() == Token::While);
    auto location = consumeToken().getLocation();
    bool parens = currentToken() == Token::LeftParen;
    if (parens) consumeToken();
    auto condition = parseExprOrVarDecl(parent);
    if (parens) parse(Token::RightParen);
    auto body = parseBlockOrStmt(parent);
    return new WhileStmt(condition, std::move(body), location);
}

/// for-stmt ::= 'for' for-header block-or-stmt
/// for-header ::= var-decl ';' expr ';' stmt |
///            '(' var-decl ';' expr ';' stmt ')'
/// foreach-stmt ::= 'for' foreach-header block-or-stmt
/// foreach-header ::= (type | 'var') id 'in' expr |
///                '(' (type | 'var') id 'in' expr ')'
Stmt* Parser::parseForOrForEachStmt(Decl* parent) {
    ASSERT(currentToken() == Token::For);
    auto location = consumeToken().getLocation();
    bool parens = currentToken() == Token::LeftParen;
    if (parens) consumeToken();
    auto varStmt = parseVarStmt(parent);

    if (varStmt->getDecl().getInitializer()) {
        // Semicolon is parsed inside parseVarDecl
        auto condition = parseExpr();
        parse(Token::Semicolon);
        auto increment = parseExpr();
        if (parens) parse(Token::RightParen);
        auto body = parseBlockOrStmt(parent);
        return new ForStmt(varStmt, condition, increment, std::move(body), location);
    } else {
        parse(Token::In);
        auto range = parseExpr();
        if (parens) parse(Token::RightParen);
        auto body = parseBlockOrStmt(parent);
        return new ForEachStmt(&varStmt->getDecl(), range, std::move(body), location);
    }
}

/// switch-stmt ::= 'switch' expr '{' cases default-case? '}'
/// cases ::= case | case cases
/// case ::= 'case' expr ':' stmt+
/// default-case ::= 'default' ':' stmt+
SwitchStmt* Parser::parseSwitchStmt(Decl* parent) {
    ASSERT(currentToken() == Token::Switch);
    consumeToken();
    auto condition = parseExpr();
    parse(Token::LeftBrace);
    std::vector<SwitchCase> cases;
    std::vector<Stmt*> defaultStmts;
    bool defaultSeen = false;

    while (true) {
        if (currentToken() == Token::Case) {
            consumeToken();
            auto value = parseExpr();

            VarDecl* associatedValue = nullptr;
            if (currentToken() == Token::As) {
                consumeToken();
                auto name = parse(Token::Identifier);
                // TODO: UndefinedLiteralExpr as initializer is a hack, should be nullptr.
                associatedValue = new VarDecl(Type(), name.getString().str(), new UndefinedLiteralExpr(name.getLocation()), parent, AccessLevel::None,
                                              *currentModule, name.getLocation());
            }

            parse(Token::Colon);
            auto stmts = parseStmtsUntilOneOf(Token::Case, Token::Default, Token::RightBrace, parent);
            cases.push_back(SwitchCase(value, associatedValue, std::move(stmts)));
        } else if (currentToken() == Token::Default) {
            if (defaultSeen) {
                ERROR(getCurrentLocation(), "switch-statement may only contain one 'default' case");
            }
            consumeToken();
            parse(Token::Colon);
            defaultStmts = parseStmtsUntilOneOf(Token::Case, Token::Default, Token::RightBrace, parent);
            defaultSeen = true;
        } else {
            ERROR(getCurrentLocation(), "expected 'case' or 'default'");
        }

        if (currentToken() == Token::RightBrace) break;
    }

    consumeToken();
    return new SwitchStmt(condition, std::move(cases), std::move(defaultStmts));
}

/// break-stmt ::= 'break' ('\n' | ';')
BreakStmt* Parser::parseBreakStmt() {
    auto location = getCurrentLocation();
    consumeToken();
    parseStmtTerminator();
    return new BreakStmt(location);
}

/// continue-stmt ::= 'continue' ('\n' | ';')
ContinueStmt* Parser::parseContinueStmt() {
    auto location = getCurrentLocation();
    consumeToken();
    parseStmtTerminator();
    return new ContinueStmt(location);
}

/// stmt ::= var-stmt | return-stmt | expr-stmt | defer-stmt | if-stmt | switch-stmt |
///          while-stmt | for-stmt | foreach-stmt | break-stmt | continue-stmt
Stmt* Parser::parseStmt(Decl* parent) {
    switch (currentToken()) {
        case Token::Return:
            return parseReturnStmt();
        case Token::Defer:
            return parseDeferStmt();
        case Token::If:
            return parseIfStmt(parent);
        case Token::While:
            return parseWhileStmt(parent);
        case Token::For:
            return parseForOrForEachStmt(parent);
        case Token::Switch:
            return parseSwitchStmt(parent);
        case Token::Break:
            return parseBreakStmt();
        case Token::Continue:
            return parseContinueStmt();
        case Token::Identifier:
            if (currentToken().getString() == "_") {
                consumeToken();
                parse(Token::Assignment);
                return parseExprStmt();
            } else if (lookAhead(1).is(Token::Assignment)) {
                return parseExprStmt();
            }
            LLVM_FALLTHROUGH;
        default:
            if (shouldParseVarStmt()) {
                return parseVarStmt(parent);
            }
            return parseExprStmt();
    }
}

std::vector<Stmt*> Parser::parseStmtsUntilOneOf(Token::Kind end1, Token::Kind end2, Token::Kind end3, Decl* parent) {
    std::vector<Stmt*> stmts;
    while (currentToken() != end1 && currentToken() != end2 && currentToken() != end3) {
        stmts.emplace_back(parseStmt(parent));
    }
    return stmts;
}

/// param-decl ::= 'public'? type? id
ParamDecl Parser::parseParam(bool requireType) {
    bool isPublic = currentToken() == Token::Public;
    if (isPublic) consumeToken();

    Type type;
    if (requireType || !lookAhead(1).is({ Token::Comma, Token::RightParen })) {
        type = parseType();
    }

    auto name = parse(Token::Identifier);
    return ParamDecl(type, name.getString().str(), isPublic, name.getLocation());
}

/// param-list ::= '(' params ')'
/// params ::= '' | non-empty-params
/// non-empty-params ::= param-decl | param-decl ',' non-empty-params
std::vector<ParamDecl> Parser::parseParamList(bool* isVariadic, bool requireTypes) {
    parse(Token::LeftParen);
    std::vector<ParamDecl> params;
    while (currentToken() != Token::RightParen) {
        if (isVariadic && currentToken() == Token::DotDotDot) {
            consumeToken();
            *isVariadic = true;
            break;
        }
        params.emplace_back(parseParam(requireTypes));
        if (currentToken() != Token::RightParen) parse(Token::Comma);
    }
    parse(Token::RightParen);
    return params;
}

void Parser::parseGenericParamList(std::vector<GenericParamDecl>& genericParams) {
    parse(Token::Less);
    while (true) {
        auto genericParamName = parse(Token::Identifier);
        genericParams.emplace_back(genericParamName.getString().str(), genericParamName.getLocation());

        if (currentToken() == Token::Colon) {
            consumeToken();
            genericParams.back().setConstraints(parseType());
        }

        if (currentToken() == Token::Greater) break;
        parse(Token::Comma);
    }
    parse(Token::Greater);
}

llvm::StringRef Parser::parseFunctionName(TypeDecl* receiverTypeDecl) {
    auto name = parse(Token::Identifier);

    if (name.getString() == "operator") {
        auto op = consumeToken();
        if (op == Token::LeftBracket) {
            parse(Token::RightBracket);
            if (currentToken() == Token::Assignment) {
                consumeToken();
                return "[]=";
            } else {
                return "[]";
            }
        } else {
            if (!isOverloadable(op)) {
                unexpectedToken(op, {}, "as function name");
            }
            if (receiverTypeDecl) {
                ERROR(name.getLocation(), "operator functions other than 'operator[]' must be non-member functions");
            }
            return toString(op);
        }
    } else {
        return name.getString();
    }
}

/// function-proto ::= type id param-list
FunctionDecl* Parser::parseFunctionProto(bool isExtern, TypeDecl* receiverTypeDecl, AccessLevel accessLevel, std::vector<GenericParamDecl>* genericParams,
                                         Type returnType, llvm::StringRef name, SourceLocation location) {
    if (currentToken() == Token::Less) {
        parseGenericParamList(*genericParams);
    }

    bool isVariadic = false;
    auto params = parseParamList(isExtern ? &isVariadic : nullptr);
    FunctionProto proto(name.str(), std::move(params), returnType, isVariadic, isExtern);

    if (receiverTypeDecl) {
        return new MethodDecl(std::move(proto), *receiverTypeDecl, std::vector<Type>(), accessLevel, location);
    } else {
        return new FunctionDecl(std::move(proto), std::vector<Type>(), accessLevel, *currentModule, location);
    }
}

/// function-template-proto ::= type id template-param-list param-list
/// template-param-list ::= '<' template-param-decls '>'
/// template-param-decls ::= id | id ',' template-param-decls
FunctionTemplate* Parser::parseFunctionTemplateProto(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name,
                                                     SourceLocation location) {
    std::vector<GenericParamDecl> genericParams;
    auto decl = parseFunctionProto(false, receiverTypeDecl, accessLevel, &genericParams, type, name, location);
    return new FunctionTemplate(std::move(genericParams), decl, accessLevel);
}

/// function-decl ::= function-proto '{' stmt* '}'
FunctionDecl* Parser::parseFunctionDecl(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, bool requireBody, Type type, llvm::StringRef name,
                                        SourceLocation location) {
    auto decl = parseFunctionProto(false, receiverTypeDecl, accessLevel, nullptr, type, name, location);

    if (requireBody || currentToken() == Token::LeftBrace) {
        decl->setBody(parseBlock(decl));
    }

    if (lookAhead(-1) != Token::RightBrace) {
        parseStmtTerminator();
    }

    return decl;
}

/// function-template-decl ::= function-template-proto '{' stmt* '}'
FunctionTemplate* Parser::parseFunctionTemplate(TypeDecl* receiverTypeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name, SourceLocation location) {
    auto decl = parseFunctionTemplateProto(receiverTypeDecl, accessLevel, type, name, location);
    decl->getFunctionDecl()->setBody(parseBlock(decl));
    return decl;
}

/// extern-function-decl ::= 'extern' function-proto ('\n' | ';')
FunctionDecl* Parser::parseExternFunctionDecl(Type type, llvm::StringRef name, SourceLocation location) {
    auto decl = parseFunctionProto(true, nullptr, AccessLevel::Default, nullptr, type, name, location);
    parseStmtTerminator();
    return decl;
}

/// constructor-decl ::= id param-list '{' stmt* '}'
ConstructorDecl* Parser::parseConstructorDecl(TypeDecl& receiverTypeDecl, AccessLevel accessLevel) {
    ASSERT(currentToken() == Token::Identifier);
    auto location = consumeToken().getLocation();
    auto params = parseParamList(nullptr);
    auto decl = new ConstructorDecl(receiverTypeDecl, std::move(params), accessLevel, location);
    decl->setBody(parseBlock(decl));
    return decl;
}

/// destructor-decl ::= '~' id param-list '{' stmt* '}'
DestructorDecl* Parser::parseDestructorDecl(TypeDecl& receiverTypeDecl) {
    ASSERT(currentToken() == Token::Tilde);
    auto location = consumeToken().getLocation();
    if (parse(Token::Identifier).getString() != receiverTypeDecl.getName()) {
        REPORT_ERROR(location, "expected '" << receiverTypeDecl.getName() << "' after '~'");
    }
    auto params = parseParamList(nullptr);
    if (!params.empty()) REPORT_ERROR(location, "destructors cannot have parameters");
    auto decl = new DestructorDecl(receiverTypeDecl, location);
    decl->setBody(parseBlock(decl));
    return decl;
}

/// field-decl ::= type id ('=' expr)? ('\n' | ';')
FieldDecl Parser::parseFieldDecl(TypeDecl& typeDecl, AccessLevel accessLevel, Type type, llvm::StringRef name, SourceLocation location) {
    Expr* defaultValue = nullptr;

    if (currentToken() == Token::Assignment) {
        consumeToken();
        defaultValue = parseExpr();
    }

    parseStmtTerminator();
    return FieldDecl(type, name.str(), defaultValue, typeDecl, accessLevel, location);
}

/// type-template-decl ::= ('struct' | 'interface') id generic-param-list? '{' member-decl* '}'
TypeTemplate* Parser::parseTypeTemplate(AccessLevel accessLevel) {
    std::vector<GenericParamDecl> genericParams;
    auto typeDecl = parseTypeDecl(&genericParams, accessLevel);
    return new TypeTemplate(std::move(genericParams), typeDecl, accessLevel);
}

Token Parser::parseTypeHeader(std::vector<Type>& interfaces, std::vector<GenericParamDecl>* genericParams) {
    auto name = parse(Token::Identifier);

    if (currentToken() == Token::Less) {
        parseGenericParamList(*genericParams);
    }

    if (currentToken() == Token::Colon) {
        consumeToken();
        interfaces = parseNonEmptyTypeList();
    }

    return name;
}

/// type-decl ::= ('struct' | 'interface') id generic-param-list? interface-list? '{' member-decl* '}'
/// interface-list ::= ':' non-empty-type-list
/// member-decl ::= field-decl | function-decl | constructor-decl | destructor-decl
TypeDecl* Parser::parseTypeDecl(std::vector<GenericParamDecl>* genericParams, AccessLevel typeAccessLevel) {
    TypeTag tag;
    switch (consumeToken()) {
        case Token::Struct:
            tag = TypeTag::Struct;
            break;
        case Token::Interface:
            tag = TypeTag::Interface;
            break;
        default:
            llvm_unreachable("invalid token");
    }

    std::vector<Type> interfaces;
    auto typeName = parseTypeHeader(interfaces, genericParams);
    auto typeDecl = new TypeDecl(tag, typeName.getString().str(), std::vector<Type>(), std::move(interfaces), typeAccessLevel, *currentModule, nullptr,
                                 typeName.getLocation());
    bool hasConstructor = false;
    parse(Token::LeftBrace);

    while (currentToken() != Token::RightBrace) {
        AccessLevel accessLevel = AccessLevel::Default;

    start:
        switch (currentToken()) {
            case Token::Private:
                if (tag == TypeTag::Interface) {
                    WARN(getCurrentLocation(), "interface members cannot be private");
                }
                if (accessLevel != AccessLevel::Default) {
                    WARN(getCurrentLocation(), "duplicate access specifier");
                }
                accessLevel = AccessLevel::Private;
                consumeToken();
                goto start;
            case Token::Tilde:
                if (accessLevel != AccessLevel::Default) {
                    WARN(lookAhead(-1).getLocation(), "destructors cannot be " << accessLevel);
                }
                typeDecl->addMethod(parseDestructorDecl(*typeDecl));
                break;
            case Token::Identifier:
                if (lookAhead(1) == Token::LeftParen && currentToken().getString() == typeName.getString()) {
                    typeDecl->addMethod(parseConstructorDecl(*typeDecl, accessLevel));
                    hasConstructor = true;
                    break;
                }
                LLVM_FALLTHROUGH;
            default: {
                auto type = parseType();
                auto location = getCurrentLocation();
                auto name = parseFunctionName(&*typeDecl);
                auto requireBody = tag != TypeTag::Interface;

                switch (currentToken()) {
                    case Token::LeftParen:
                        typeDecl->addMethod(parseFunctionDecl(typeDecl, accessLevel, requireBody, type, name, location));
                        break;
                    case Token::Less:
                        typeDecl->addMethod(parseFunctionTemplate(typeDecl, accessLevel, type, name, location));
                        break;
                    default:
                        typeDecl->addField(parseFieldDecl(*typeDecl, accessLevel, type, name, location));
                        break;
                }
                break;
            }
        }
    }

    if (tag == TypeTag::Struct && !hasConstructor) {
        typeDecl->addMethod(createAutogeneratedConstructor(typeDecl));
    }

    consumeToken();
    return typeDecl;
}

ConstructorDecl* Parser::createAutogeneratedConstructor(TypeDecl* typeDecl) const {
    std::vector<ParamDecl> params;
    std::vector<Stmt*> body;

    for (auto& field : typeDecl->getFields()) {
        auto* left = new MemberExpr(new VarExpr("this", field.getLocation()), field.getName().str(), field.getLocation());
        auto* right = field.getDefaultValue() ? field.getDefaultValue() : new VarExpr(field.getName().str(), field.getLocation());
        body.push_back(new ExprStmt(new BinaryExpr(Token::Assignment, left, right, field.getLocation())));

        if (!field.getDefaultValue()) {
            params.push_back(ParamDecl(field.getType(), field.getName().str(), false, field.getLocation()));
        }
    }

    auto* autogeneratedInit = new ConstructorDecl(*typeDecl, std::move(params), typeDecl->getAccessLevel(), typeDecl->getLocation());
    autogeneratedInit->setBody(std::move(body));
    return autogeneratedInit;
}

/// enum-decl ::= 'enum' id generic-param-list? interface-list? '{' enum-case-decl* '}'
/// enum-case-decl ::= id tuple-type? (',' | '\n' | ';')
EnumDecl* Parser::parseEnumDecl(AccessLevel typeAccessLevel) {
    ASSERT(currentToken() == Token::Enum);
    consumeToken();

    if (lookAhead(1) == Token::Less) {
        ERROR(getCurrentLocation(), "generic enums not implemented yet");
    }

    std::vector<GenericParamDecl> genericParams;
    std::vector<Type> interfaces;
    auto name = parseTypeHeader(interfaces, &genericParams);

    parse(Token::LeftBrace);
    std::vector<EnumCase> cases;
    auto valueCounter = llvm::APSInt::get(0);

    while (currentToken() != Token::RightBrace) {
        auto caseName = parse(Token::Identifier);
        Type associatedType;

        if (currentToken() == Token::LeftParen) {
            associatedType = parseTupleType();
        }

        auto value = new IntLiteralExpr(valueCounter, caseName.getLocation());
        cases.push_back(EnumCase(caseName.getString().str(), value, associatedType, typeAccessLevel, caseName.getLocation()));
        ++valueCounter;

        if (currentToken() == Token::Comma) {
            consumeToken();
        } else {
            parseStmtTerminator();
        }
    }

    consumeToken();
    return new EnumDecl(name.getString().str(), std::move(cases), typeAccessLevel, *currentModule, nullptr, name.getLocation());
}

/// import-decl ::= 'import' (id | string-literal) ('\n' | ';')
ImportDecl* Parser::parseImportDecl() {
    ASSERT(currentToken() == Token::Import);
    consumeToken();

    auto location = getCurrentLocation();
    std::string importTarget;

    if (currentToken() == Token::StringLiteral) {
        importTarget = parseStringLiteral()->getValue().str();
    } else {
        importTarget = parse({ Token::Identifier, Token::StringLiteral }, "after 'import'").getString().str();
    }

    parseStmtTerminator("after 'import' declaration");
    return new ImportDecl(std::move(importTarget), *currentModule, location);
}

void Parser::parseIfdefBody(std::vector<Decl*>* activeDecls) {
    if (currentToken() == Token::HashIf) {
        parseIfdef(activeDecls);
    } else {
        if (activeDecls) {
            activeDecls->emplace_back(parseTopLevelDecl(true));
        } else {
            parseTopLevelDecl(false);
        }
    }
}

void Parser::parseIfdef(std::vector<Decl*>* activeDecls) {
    ASSERT(currentToken() == Token::HashIf);
    consumeToken();
    bool negate = currentToken() == Token::Not;
    if (negate) consumeToken();
    auto identifier = parse(Token::Identifier);

    bool condition = false;
    if (identifier.getString() == "hasInclude") {
        parse(Token::LeftParen);
        auto header = parse(Token::StringLiteral);
        parse(Token::RightParen);

        for (llvm::StringRef path : llvm::concat<const std::string>(options.importSearchPaths, options.frameworkSearchPaths)) {
            auto headerPath = (path + "/" + header.getString().drop_back().drop_front()).str();
            if (llvm::sys::fs::exists(headerPath) && !llvm::sys::fs::is_directory(headerPath)) {
                condition = true;
                break;
            }
        }
    } else {
        condition = llvm::is_contained(options.defines, identifier.getString());
    }

    if (negate) condition = !condition;

    while (!currentToken().is({ Token::HashElse, Token::HashEndif })) {
        parseIfdefBody(condition ? activeDecls : nullptr);
    }

    if (currentToken() == Token::HashElse) {
        consumeToken();
        while (currentToken() != Token::HashEndif) {
            parseIfdefBody(condition ? nullptr : activeDecls);
        }
    }

    consumeToken();
}

/// top-level-decl ::= function-decl | extern-function-decl | type-decl | enum-decl | import-decl | var-decl
/// @throws CompileError
Decl* Parser::parseTopLevelDecl(bool addToSymbolTable) {
    AccessLevel accessLevel = AccessLevel::Default;
    Decl* decl = nullptr;

start:
    switch (currentToken()) {
        case Token::Private:
            if (accessLevel != AccessLevel::Default) WARN(getCurrentLocation(), "duplicate access specifier");
            accessLevel = AccessLevel::Private;
            consumeToken();
            goto start;
        case Token::Extern:
            if (accessLevel != AccessLevel::Default) {
                WARN(lookAhead(-1).getLocation(), "extern functions cannot have access specifiers");
            }
            consumeToken();
            return parseTopLevelFunctionOrVariable(true, addToSymbolTable, accessLevel);
        case Token::Struct:
        case Token::Interface:
            if (lookAhead(2) == Token::Less) {
                decl = parseTypeTemplate(accessLevel);
                if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<TypeTemplate>(*decl));
            } else {
                decl = parseTypeDecl(nullptr, accessLevel);
                if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<TypeDecl>(*decl));
            }
            break;
        case Token::Enum:
            decl = parseEnumDecl(accessLevel);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<EnumDecl>(*decl));
            break;
        case Token::Var:
        case Token::Const:
            // Determine if this is a constant declaration or if the const is part of a type.
            if (currentToken() == Token::Const && lookAhead(2) != Token::Assignment) {
                return parseTopLevelFunctionOrVariable(false, addToSymbolTable, accessLevel);
            }
            decl = parseVarDecl(nullptr, accessLevel);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<VarDecl>(*decl));
            break;
        case Token::Import:
            if (accessLevel != AccessLevel::Default) {
                WARN(lookAhead(-1).getLocation(), "imports cannot have access specifiers");
            }
            return parseImportDecl();
        default:
            return parseTopLevelFunctionOrVariable(false, addToSymbolTable, accessLevel);
    }

    return decl;
}

Decl* Parser::parseTopLevelFunctionOrVariable(bool isExtern, bool addToSymbolTable, AccessLevel accessLevel) {
    Decl* decl;
    auto type = parseType();
    auto location = getCurrentLocation();
    auto name = parseFunctionName(nullptr);

    switch (currentToken()) {
        case Token::LeftParen:
            if (isExtern) {
                decl = parseExternFunctionDecl(type, name, location);
            } else {
                decl = parseFunctionDecl(nullptr, accessLevel, false, type, name, location);
            }
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<FunctionDecl>(*decl));
            break;
        case Token::Less:
            decl = parseFunctionTemplate(nullptr, accessLevel, type, name, location);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<FunctionTemplate>(*decl));
            break;
        default:
            decl = parseVarDeclAfterName(nullptr, accessLevel, type, name, location);
            if (addToSymbolTable) currentModule->addToSymbolTable(llvm::cast<VarDecl>(*decl));
            break;
    }

    return decl;
}

void Parser::parse() {
    std::vector<Decl*> topLevelDecls;
    SourceFile sourceFile(lexer.getFilePath(), currentModule);

    try {
        while (currentToken() != Token::None) {
            if (currentToken() == Token::HashIf) {
                parseIfdef(&topLevelDecls);
            } else {
                auto previousTokenIndex = currentTokenIndex;
                topLevelDecls.push_back(parseTopLevelDecl(true));
                if (currentTokenIndex == previousTokenIndex) break;
            }
        }
    } catch (const CompileError& error) {
        error.print();
    }

    sourceFile.setDecls(std::move(topLevelDecls));
    currentModule->addSourceFile(std::move(sourceFile));
}
