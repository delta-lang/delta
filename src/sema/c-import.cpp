#include "c-import.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#pragma warning(push, 0)
#include <clang/AST/Decl.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Builtins.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Parse/ParseAST.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#pragma warning(pop)
#include "typecheck.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../ast/type.h"
#include "../driver/driver.h"
#include "../support/utility.h"

using namespace delta;

static clang::TargetInfo* targetInfo;

static Type getIntTypeByWidth(int widthInBits, bool asSigned) {
    switch (widthInBits) {
        case 8:
            return asSigned ? Type::getInt8() : Type::getUInt8();
        case 16:
            return asSigned ? Type::getInt16() : Type::getUInt16();
        case 32:
            return asSigned ? Type::getInt32() : Type::getUInt32();
        case 64:
            return asSigned ? Type::getInt64() : Type::getUInt64();
    }
    llvm_unreachable("unsupported integer width");
}

static Type toDelta(const clang::BuiltinType& type) {
    switch (type.getKind()) {
        case clang::BuiltinType::Void:
            return Type::getVoid();
        case clang::BuiltinType::Bool:
            return Type::getBool();
        case clang::BuiltinType::Char_S:
        case clang::BuiltinType::Char_U:
            return Type::getChar();
        case clang::BuiltinType::SChar:
            return getIntTypeByWidth(targetInfo->getCharWidth(), true);
        case clang::BuiltinType::UChar:
            return getIntTypeByWidth(targetInfo->getCharWidth(), false);
        case clang::BuiltinType::Short:
            return getIntTypeByWidth(targetInfo->getShortWidth(), true);
        case clang::BuiltinType::UShort:
            return getIntTypeByWidth(targetInfo->getShortWidth(), false);
        case clang::BuiltinType::Int:
            return Type::getInt();
        case clang::BuiltinType::UInt:
            return Type::getUInt();
        case clang::BuiltinType::Long:
            return getIntTypeByWidth(targetInfo->getLongWidth(), true);
        case clang::BuiltinType::ULong:
            return getIntTypeByWidth(targetInfo->getLongWidth(), false);
        case clang::BuiltinType::LongLong:
            return getIntTypeByWidth(targetInfo->getLongLongWidth(), true);
        case clang::BuiltinType::ULongLong:
            return getIntTypeByWidth(targetInfo->getLongLongWidth(), false);
        case clang::BuiltinType::Float:
            return Type::getFloat();
        case clang::BuiltinType::Double:
            return Type::getFloat64();
        case clang::BuiltinType::LongDouble:
            return Type::getFloat80();
        default:
            break;
    }
    llvm_unreachable("unsupported builtin type");
}

static llvm::StringRef getName(const clang::TagDecl& decl) {
    if (!decl.getName().empty()) {
        return decl.getName();
    } else if (auto* typedefNameDecl = decl.getTypedefNameForAnonDecl()) {
        return typedefNameDecl->getName();
    }
    return "";
}

static Type toDelta(clang::QualType qualtype) {
    auto mutability = qualtype.isConstQualified() ? Mutability::Const : Mutability::Mutable;
    auto& type = *qualtype.getTypePtr();

    switch (type.getTypeClass()) {
        case clang::Type::Pointer: {
            auto pointeeType = llvm::cast<clang::PointerType>(type).getPointeeType();
            if (pointeeType->isFunctionType()) {
                return OptionalType::get(toDelta(pointeeType), mutability);
            }
            return OptionalType::get(PointerType::get(toDelta(pointeeType), Mutability::Mutable), mutability);
        }
        case clang::Type::Builtin:
            return toDelta(llvm::cast<clang::BuiltinType>(type)).withMutability(mutability);
        case clang::Type::Typedef: {
            auto desugared = llvm::cast<clang::TypedefType>(type).desugar();
            if (mutability == Mutability::Const) desugared.addConst();
            return toDelta(desugared);
        }
        case clang::Type::Elaborated:
            return toDelta(llvm::cast<clang::ElaboratedType>(type).getNamedType());
        case clang::Type::Record: {
            auto* recordDecl = llvm::cast<clang::RecordType>(type).getDecl();
            return BasicType::get(getName(*recordDecl), {}, mutability);
        }
        case clang::Type::Paren:
            return toDelta(llvm::cast<clang::ParenType>(type).getInnerType());
        case clang::Type::FunctionProto: {
            auto& functionProtoType = llvm::cast<clang::FunctionProtoType>(type);
            auto paramTypes = map(functionProtoType.getParamTypes(), [](clang::QualType qualType) { return toDelta(qualType); });
            return FunctionType::get(toDelta(functionProtoType.getReturnType()), std::move(paramTypes), mutability);
        }
        case clang::Type::FunctionNoProto: {
            auto& functionNoProtoType = llvm::cast<clang::FunctionNoProtoType>(type);
            // This treats it as a zero-argument function, but really it should accept any number of arguments of any types.
            return FunctionType::get(toDelta(functionNoProtoType.getReturnType()), {}, mutability);
        }
        case clang::Type::ConstantArray: {
            auto& constantArrayType = llvm::cast<clang::ConstantArrayType>(type);
            if (!constantArrayType.getSize().isIntN(64)) {
                ERROR(SourceLocation(), "array is too large");
            }
            return ArrayType::get(toDelta(constantArrayType.getElementType()), constantArrayType.getSize().getLimitedValue(), mutability);
        }
        case clang::Type::IncompleteArray:
            return ArrayType::get(toDelta(llvm::cast<clang::IncompleteArrayType>(type).getElementType()), ArrayType::unknownSize);
        case clang::Type::Attributed:
            return toDelta(llvm::cast<clang::AttributedType>(type).getEquivalentType());
        case clang::Type::Decayed:
            return toDelta(llvm::cast<clang::DecayedType>(type).getDecayedType());
        case clang::Type::Enum: {
            auto& enumType = llvm::cast<clang::EnumType>(type);
            auto name = getName(*enumType.getDecl());

            if (name.empty()) {
                return toDelta(enumType.getDecl()->getIntegerType());
            } else {
                return BasicType::get(name, {}, mutability);
            }
        }
        case clang::Type::Vector: {
            auto& vectorType = llvm::cast<clang::VectorType>(type);
            return ArrayType::get(toDelta(vectorType.getElementType()), vectorType.getNumElements());
        }
        default:
            WARN(SourceLocation(),
                 "unhandled type class '" << type.getTypeClassName() << "' (importing type '" << qualtype.getAsString() << "')");
            return Type::getInt();
    }
}

static FunctionDecl* toDelta(const clang::FunctionDecl& decl, Module* currentModule) {
    auto params = map(decl.parameters(), [](clang::ParmVarDecl* param) {
        return ParamDecl(toDelta(param->getType()), param->getNameAsString(), false, SourceLocation());
    });
    FunctionProto proto(decl.getNameAsString(), std::move(params), toDelta(decl.getReturnType()), decl.isVariadic(), true);
    return new FunctionDecl(std::move(proto), {}, AccessLevel::Default, *currentModule, SourceLocation());
}

static llvm::Optional<FieldDecl> toDelta(const clang::FieldDecl& decl, TypeDecl& typeDecl) {
    if (decl.getName().empty()) return llvm::None;
    return FieldDecl(toDelta(decl.getType()), decl.getNameAsString(), nullptr, typeDecl, AccessLevel::Default, SourceLocation());
}

static TypeDecl* toDelta(const clang::RecordDecl& decl, Module* currentModule) {
    auto tag = decl.isUnion() ? TypeTag::Union : TypeTag::Struct;
    auto* typeDecl = new TypeDecl(tag, getName(decl), {}, {}, AccessLevel::Default, *currentModule, SourceLocation());

    for (auto* field : decl.fields()) {
        if (auto fieldDecl = toDelta(*field, *typeDecl)) {
            typeDecl->getFields().emplace_back(std::move(*fieldDecl));
        } else {
            return nullptr;
        }
    }

    return typeDecl;
}

static VarDecl* toDelta(const clang::VarDecl& decl, Module* currentModule) {
    return new VarDecl(toDelta(decl.getType()), decl.getName(), nullptr, nullptr, AccessLevel::Default, *currentModule, SourceLocation());
}

static void addIntegerConstantToSymbolTable(llvm::StringRef name, llvm::APSInt value, clang::QualType qualType, Module& module) {
    auto initializer = new IntLiteralExpr(std::move(value), SourceLocation());
    auto type = toDelta(qualType).withMutability(Mutability::Const);
    initializer->setType(type);
    module.addToSymbolTable(new VarDecl(type, name, initializer, nullptr, AccessLevel::Default, module, SourceLocation()));
}

static void addFloatConstantToSymbolTable(llvm::StringRef name, llvm::APFloat value, Module& module) {
    auto initializer = new FloatLiteralExpr(std::move(value), SourceLocation());
    auto type = Type::getFloat64(Mutability::Const);
    initializer->setType(type);
    module.addToSymbolTable(new VarDecl(type, name, initializer, nullptr, AccessLevel::Default, module, SourceLocation()));
}

namespace {
class CToDeltaConverter : public clang::ASTConsumer {
public:
    CToDeltaConverter(Module& module) : module(module) {}

    bool HandleTopLevelDecl(clang::DeclGroupRef declGroup) final override {
        for (clang::Decl* decl : declGroup) {
            switch (decl->getKind()) {
                case clang::Decl::Function:
                    module.addToSymbolTable(toDelta(llvm::cast<clang::FunctionDecl>(*decl), &module));
                    break;
                case clang::Decl::Record: {
                    if (!decl->isFirstDecl()) break;
                    auto typeDecl = toDelta(llvm::cast<clang::RecordDecl>(*decl), &module);
                    if (typeDecl) {
                        ASSERT(module.getSymbolTable().find(typeDecl->getName()).empty());
                        module.addToSymbolTable(typeDecl);
                    }
                    break;
                }
                case clang::Decl::Enum: {
                    auto& enumDecl = llvm::cast<clang::EnumDecl>(*decl);
                    auto type = getName(enumDecl).empty() ? enumDecl.getIntegerType() : clang::QualType(enumDecl.getTypeForDecl(), 0);
                    std::vector<EnumCase> cases;

                    for (clang::EnumConstantDecl* enumerator : enumDecl.enumerators()) {
                        auto enumeratorName = enumerator->getName();
                        auto& value = enumerator->getInitVal();
                        auto valueExpr = new IntLiteralExpr(value, SourceLocation());
                        cases.push_back(EnumCase(enumeratorName, valueExpr, Type(), AccessLevel::Default, SourceLocation()));
                        addIntegerConstantToSymbolTable(enumeratorName, value, type, module);
                    }

                    module.addToSymbolTable(new EnumDecl(getName(enumDecl), std::move(cases), AccessLevel::Default, module, SourceLocation()));
                    break;
                }
                case clang::Decl::Var:
                    module.addToSymbolTable(toDelta(llvm::cast<clang::VarDecl>(*decl), &module));
                    break;
                case clang::Decl::Typedef: {
                    auto& typedefDecl = llvm::cast<clang::TypedefDecl>(*decl);
                    if (auto* baseTypeId = typedefDecl.getUnderlyingType().getBaseTypeIdentifier()) {
                        module.addIdentifierReplacement(typedefDecl.getName(), baseTypeId->getName());
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return true; // continue parsing
    }

private:
    Module& module;
};

class MacroImporter : public clang::PPCallbacks {
public:
    MacroImporter(Module& module, clang::Sema& clangSema) : module(module), clangSema(clangSema) {}

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* macro) final override {
        if (macro->getMacroInfo()->getNumTokens() != 1) return;
        auto& token = macro->getMacroInfo()->getReplacementToken(0);

        switch (token.getKind()) {
            case clang::tok::identifier:
                module.addIdentifierReplacement(name.getIdentifierInfo()->getName(), token.getIdentifierInfo()->getName());
                return;

            case clang::tok::numeric_constant:
                importNumericConstant(name.getIdentifierInfo()->getName(), token);
                return;

            default:
                return;
        }
    }

private:
    void importNumericConstant(llvm::StringRef name, const clang::Token& token) {
        auto result = clangSema.ActOnNumericConstant(token);
        if (!result.isUsable()) return;
        clang::Expr* parsed = result.get();

        if (auto* intLiteral = llvm::dyn_cast<clang::IntegerLiteral>(parsed)) {
            llvm::APSInt value(intLiteral->getValue(), parsed->getType()->isUnsignedIntegerType());
            addIntegerConstantToSymbolTable(name, std::move(value), parsed->getType(), module);
        } else if (auto* floatLiteral = llvm::dyn_cast<clang::FloatingLiteral>(parsed)) {
            addFloatConstantToSymbolTable(name, floatLiteral->getValue(), module);
        }
    }

private:
    Module& module;
    clang::Sema& clangSema;
};
} // namespace

bool delta::importCHeader(SourceFile& importer, llvm::StringRef headerName, const CompileOptions& options, SourceLocation importLocation) {
    auto it = Module::getAllImportedModulesMap().find(headerName);
    if (it != Module::getAllImportedModulesMap().end()) {
        importer.addImportedModule(it->second);
        return true;
    }

    auto module = new Module(headerName);

    clang::CompilerInstance ci;
    ci.createDiagnostics();
    auto args = map(options.cflags, [](auto& cflag) { return cflag.c_str(); });
    clang::CompilerInvocation::CreateFromArgs(ci.getInvocation(), args, ci.getDiagnostics());

    std::shared_ptr<clang::TargetOptions> pto = std::make_shared<clang::TargetOptions>();
    pto->Triple = llvm::sys::getDefaultTargetTriple();
    targetInfo = clang::TargetInfo::CreateTargetInfo(ci.getDiagnostics(), pto);
    ci.setTarget(targetInfo);

    ci.createFileManager();
    ci.createSourceManager(ci.getFileManager());

    for (llvm::StringRef includePath : options.importSearchPaths) {
        ci.getHeaderSearchOpts().AddPath(includePath, clang::frontend::System, false, false);
    }
    for (llvm::StringRef frameworkPath : options.frameworkSearchPaths) {
        ci.getHeaderSearchOpts().AddPath(frameworkPath, clang::frontend::System, true, false);
    }

    ci.createPreprocessor(clang::TU_Complete);
    auto& pp = ci.getPreprocessor();
    pp.getBuiltinInfo().initializeBuiltins(pp.getIdentifierTable(), pp.getLangOpts());

    ci.setASTConsumer(std::make_unique<CToDeltaConverter>(*module));
    ci.createASTContext();
    ci.createSema(clang::TU_Complete, nullptr);
    pp.addPPCallbacks(std::make_unique<MacroImporter>(*module, ci.getSema()));

    const clang::DirectoryLookup* curDir = nullptr;
    auto fileEntry = ci.getPreprocessor().getHeaderSearchInfo().LookupFile(headerName, {}, false, nullptr, curDir, {}, nullptr, nullptr,
                                                                           nullptr, nullptr, nullptr, nullptr);
    if (!fileEntry) {
        REPORT_ERROR(importLocation, "couldn't find C header file '" << headerName << "'");
        return false;
    }

    auto fileID = ci.getSourceManager().createFileID(*fileEntry, clang::SourceLocation(), clang::SrcMgr::C_System);
    ci.getSourceManager().setMainFileID(fileID);
    ci.getDiagnosticClient().BeginSourceFile(ci.getLangOpts(), &ci.getPreprocessor());
    clang::ParseAST(ci.getPreprocessor(), &ci.getASTConsumer(), ci.getASTContext());
    ci.getDiagnosticClient().EndSourceFile();
    ci.getDiagnosticClient().finish();

    if (ci.getDiagnosticClient().getNumErrors() > 0) {
        return false;
    }

    importer.addImportedModule(module);
    Module::getAllImportedModulesMap()[module->getName()] = module;
    return true;
}
