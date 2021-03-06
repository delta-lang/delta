#include "c-import.h"
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
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Parse/ParseAST.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Path.h>
#pragma warning(pop)
#include "typecheck.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../ast/type.h"
#include "../driver/driver.h"
#include "../support/utility.h"

using namespace cx;

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

static Type toCx(const clang::BuiltinType& type) {
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
        case clang::BuiltinType::Int128:
            return Type::getInt128();
        case clang::BuiltinType::UInt128:
            return Type::getUInt128();
        default:
            type.dump();
            llvm_unreachable("unsupported clang::BuiltinType");
    }
}

static llvm::StringRef getName(const clang::TagDecl& decl) {
    if (!decl.getName().empty()) {
        return decl.getName();
    } else if (auto* typedefNameDecl = decl.getTypedefNameForAnonDecl()) {
        return typedefNameDecl->getName();
    }
    return "";
}

static Type toCx(clang::QualType qualtype) {
    auto mutability = qualtype.isConstQualified() ? Mutability::Const : Mutability::Mutable;
    auto& type = *qualtype.getTypePtr();

    switch (type.getTypeClass()) {
        case clang::Type::Pointer: {
            auto pointeeType = llvm::cast<clang::PointerType>(type).getPointeeType();
            if (pointeeType->isFunctionType()) {
                return OptionalType::get(toCx(pointeeType), mutability);
            }
            return OptionalType::get(PointerType::get(toCx(pointeeType), Mutability::Mutable), mutability);
        }
        case clang::Type::Builtin:
            return toCx(llvm::cast<clang::BuiltinType>(type)).withMutability(mutability);
        case clang::Type::Typedef: {
            auto desugared = llvm::cast<clang::TypedefType>(type).desugar();
            if (mutability == Mutability::Const) desugared.addConst();
            return toCx(desugared);
        }
        case clang::Type::Elaborated:
            return toCx(llvm::cast<clang::ElaboratedType>(type).getNamedType());
        case clang::Type::Record: {
            auto* recordDecl = llvm::cast<clang::RecordType>(type).getDecl();
            return BasicType::get(getName(*recordDecl), {}, mutability);
        }
        case clang::Type::Paren:
            return toCx(llvm::cast<clang::ParenType>(type).getInnerType());
        case clang::Type::FunctionProto: {
            auto& functionProtoType = llvm::cast<clang::FunctionProtoType>(type);
            auto paramTypes = map(functionProtoType.getParamTypes(), [](clang::QualType qualType) { return toCx(qualType); });
            return FunctionType::get(toCx(functionProtoType.getReturnType()), std::move(paramTypes), mutability);
        }
        case clang::Type::FunctionNoProto: {
            auto& functionNoProtoType = llvm::cast<clang::FunctionNoProtoType>(type);
            // This treats it as a zero-argument function, but really it should accept any number of arguments of any types.
            return FunctionType::get(toCx(functionNoProtoType.getReturnType()), {}, mutability);
        }
        case clang::Type::ConstantArray: {
            auto& constantArrayType = llvm::cast<clang::ConstantArrayType>(type);
            if (!constantArrayType.getSize().isIntN(64)) {
                ERROR(SourceLocation(), "array is too large");
            }
            return ArrayType::get(toCx(constantArrayType.getElementType()), constantArrayType.getSize().getLimitedValue());
        }
        case clang::Type::IncompleteArray:
            return ArrayType::get(toCx(llvm::cast<clang::IncompleteArrayType>(type).getElementType()), ArrayType::UnknownSize);
        case clang::Type::Attributed:
            return toCx(llvm::cast<clang::AttributedType>(type).getEquivalentType());
        case clang::Type::Decayed:
            return toCx(llvm::cast<clang::DecayedType>(type).getDecayedType());
        case clang::Type::Enum: {
            auto& enumType = llvm::cast<clang::EnumType>(type);
            auto name = getName(*enumType.getDecl());

            if (name.empty()) {
                return toCx(enumType.getDecl()->getIntegerType());
            } else {
                return BasicType::get(name, {}, mutability);
            }
        }
        case clang::Type::Vector: {
            auto& vectorType = llvm::cast<clang::VectorType>(type);
            return ArrayType::get(toCx(vectorType.getElementType()), vectorType.getNumElements());
        }
        default:
            WARN(SourceLocation(), "unhandled type class '" << type.getTypeClassName() << "' (importing type '" << qualtype.getAsString() << "')");
            return Type::getInt();
    }
}

static llvm::Optional<FieldDecl> toCx(const clang::FieldDecl& decl, TypeDecl& typeDecl) {
    if (decl.getName().empty()) return llvm::None;
    return FieldDecl(toCx(decl.getType()), decl.getNameAsString(), nullptr, typeDecl, AccessLevel::Default, SourceLocation());
}

static TypeDecl* toCx(const clang::RecordDecl& decl, Module* currentModule) {
    auto tag = decl.isUnion() ? TypeTag::Union : TypeTag::Struct;
    auto* typeDecl = new TypeDecl(tag, getName(decl).str(), {}, {}, AccessLevel::Default, *currentModule, nullptr, SourceLocation());
    typeDecl->packed = decl.hasAttr<clang::PackedAttr>();

    for (auto* field : decl.fields()) {
        if (auto fieldDecl = toCx(*field, *typeDecl)) {
            typeDecl->getFields().emplace_back(std::move(*fieldDecl));
        } else {
            return nullptr;
        }
    }

    return typeDecl;
}

static VarDecl* toCx(const clang::VarDecl& decl, Module* currentModule) {
    return new VarDecl(toCx(decl.getType()), decl.getName().str(), nullptr, nullptr, AccessLevel::Default, *currentModule, SourceLocation());
}

static void addIntegerConstantToSymbolTable(llvm::StringRef name, llvm::APSInt value, clang::QualType qualType, Module& module) {
    auto initializer = new IntLiteralExpr(std::move(value), SourceLocation());
    auto type = toCx(qualType).withMutability(Mutability::Const);
    initializer->setType(type);
    module.addToSymbolTable(new VarDecl(type, name.str(), initializer, nullptr, AccessLevel::Default, module, SourceLocation()));
}

static void addFloatConstantToSymbolTable(llvm::StringRef name, llvm::APFloat value, Module& module) {
    auto initializer = new FloatLiteralExpr(std::move(value), SourceLocation());
    auto type = Type::getFloat64(Mutability::Const);
    initializer->setType(type);
    module.addToSymbolTable(new VarDecl(type, name.str(), initializer, nullptr, AccessLevel::Default, module, SourceLocation()));
}

namespace {
struct CToCxConverter : clang::ASTConsumer {
    CToCxConverter(Module& module, clang::SourceManager& sourceManager) : module(module), sourceManager(sourceManager) {}

    bool HandleTopLevelDecl(clang::DeclGroupRef declGroup) final override {
        for (clang::Decl* decl : declGroup) {
            switch (decl->getKind()) {
                case clang::Decl::Function: {
                    auto functionDecl = toCx(*llvm::cast<clang::FunctionDecl>(decl), &module);
                    if (module.getSymbolTable().find(functionDecl->getName()).empty()) {
                        module.addToSymbolTable(functionDecl);
                    }
                    break;
                }
                case clang::Decl::Record: {
                    if (!decl->isFirstDecl()) break;
                    auto typeDecl = ::toCx(llvm::cast<clang::RecordDecl>(*decl), &module);
                    if (typeDecl && module.getSymbolTable().find(typeDecl->getName()).empty()) {
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
                        cases.push_back(EnumCase(enumeratorName.str(), valueExpr, Type(), AccessLevel::Default, SourceLocation()));
                        addIntegerConstantToSymbolTable(enumeratorName, value, type, module);
                    }

                    module.addToSymbolTable(new EnumDecl(getName(enumDecl).str(), std::move(cases), AccessLevel::Default, module, nullptr, SourceLocation()));
                    break;
                }
                case clang::Decl::Var:
                    module.addToSymbolTable(::toCx(llvm::cast<clang::VarDecl>(*decl), &module));
                    break;
                case clang::Decl::Typedef: {
                    auto& typedefDecl = llvm::cast<clang::TypedefDecl>(*decl);
                    auto type = ::toCx(typedefDecl.getUnderlyingType());
                    if (type.isBasicType()) {
                        llvm::cast<BasicType>(BasicType::get(typedefDecl.getName(), {}).getBase())->setName(type.getName().str());
                    } else {
                        // TODO: Import non-BasicType typedefs from C headers.
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return true; // continue parsing
    }

    FunctionDecl* toCx(const clang::FunctionDecl& decl, Module* currentModule) {
        auto params = map(decl.parameters(),
                          [](clang::ParmVarDecl* param) { return ParamDecl(::toCx(param->getType()), param->getNameAsString(), false, SourceLocation()); });
        FunctionProto proto(decl.getNameAsString(), std::move(params), ::toCx(decl.getReturnType()), decl.isVariadic(), true);
        if (auto asmLabelAttr = decl.getAttr<clang::AsmLabelAttr>()) {
            proto.asmLabel = asmLabelAttr->getLabel().str();
        }
        return new FunctionDecl(std::move(proto), {}, AccessLevel::Default, *currentModule, toCx(decl.getLocation()));
    }

    SourceLocation toCx(clang::SourceLocation location) {
        auto presumedLocation = sourceManager.getPresumedLoc(location);
        return SourceLocation(strdup(presumedLocation.getFilename()), presumedLocation.getLine(), presumedLocation.getColumn());
    }

private:
    Module& module;
    clang::SourceManager& sourceManager;
};

struct MacroImporter : clang::PPCallbacks {
    MacroImporter(Module& module, clang::CompilerInstance& compilerInstance) : module(module), compilerInstance(compilerInstance) {}

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* macro) final override {
        if (macro->getMacroInfo()->getNumTokens() != 1) return;
        auto& token = macro->getMacroInfo()->getReplacementToken(0);

        switch (token.getKind()) {
            case clang::tok::identifier:
                module.addIdentifierReplacement(name.getIdentifierInfo()->getName(), token.getIdentifierInfo()->getName());
                break;
            case clang::tok::numeric_constant:
                importMacroConstant(name.getIdentifierInfo()->getName(), token);
                break;
            default:
                break;
        }
    }

private:
    void importMacroConstant(llvm::StringRef name, const clang::Token& token) {
        auto result = compilerInstance.getSema().ActOnNumericConstant(token);
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
    clang::CompilerInstance& compilerInstance;
};
} // namespace

bool cx::importCHeader(SourceFile& importer, llvm::StringRef headerName, const CompileOptions& options, SourceLocation importLocation) {
    auto it = Module::getAllImportedModulesMap().find(headerName);
    if (it != Module::getAllImportedModulesMap().end()) {
        importer.addImportedModule(it->second);
        return true;
    }

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
    ci.getHeaderSearchOpts().AddPath(llvm::sys::path::parent_path(importer.getFilePath()), clang::frontend::Quoted, false, true);

    for (llvm::StringRef includePath : options.importSearchPaths) {
        ci.getHeaderSearchOpts().AddPath(includePath, clang::frontend::System, false, true);
        ci.getHeaderSearchOpts().AddPath(includePath, clang::frontend::System, false, false);
    }
    for (llvm::StringRef frameworkPath : options.frameworkSearchPaths) {
        ci.getHeaderSearchOpts().AddPath(frameworkPath, clang::frontend::System, true, true);
        ci.getHeaderSearchOpts().AddPath(frameworkPath, clang::frontend::System, true, false);
    }
    for (llvm::StringRef define : options.defines) {
        ci.getPreprocessorOpts().addMacroDef(define);
    }

    ci.createPreprocessor(clang::TU_Complete);
    auto& pp = ci.getPreprocessor();
    pp.getBuiltinInfo().initializeBuiltins(pp.getIdentifierTable(), pp.getLangOpts());

    const clang::DirectoryLookup* curDir = nullptr;
    clang::HeaderSearch& headerSearch = ci.getPreprocessor().getHeaderSearchInfo();
    auto fileEntry = headerSearch.LookupFile(headerName, {}, false, nullptr, curDir, {}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!fileEntry) {
        std::string searchDirs;
        for (auto searchDir = headerSearch.search_dir_begin(), end = headerSearch.search_dir_end(); searchDir != end; ++searchDir) {
            searchDirs += '\n';
            searchDirs += searchDir->getName();
        }
        REPORT_ERROR(importLocation, "couldn't find C header file '" << headerName << "' in the following locations:" << searchDirs);
        return false;
    }

    auto module = new Module(headerName);
    ci.setASTConsumer(std::make_unique<CToCxConverter>(*module, ci.getSourceManager()));
    ci.createASTContext();
    ci.createSema(clang::TU_Complete, nullptr);
    pp.addPPCallbacks(std::make_unique<MacroImporter>(*module, ci));

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
