#pragma once

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning( \
        disable : 4459) // annoying warning about global "alloc" being shadowed by locals
#endif

#include <catch2/catch.hpp>
#include <sstream>

#include "slang/binding/Expressions.h"
#include "slang/binding/Statements.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/DiagnosticWriter.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"

namespace slang {

extern BumpAllocator alloc;
extern Diagnostics diagnostics;

} // namespace slang

using namespace slang;

#define CHECK_DIAGNOSTICS_EMPTY                                                   \
    do {                                                                          \
        diagnostics.sort(getSourceManager());                                     \
        if (!diagnostics.empty())                                                 \
            FAIL_CHECK(DiagnosticWriter(getSourceManager()).report(diagnostics)); \
    } while (0)

#define NO_COMPILATION_ERRORS                                \
    do {                                                     \
        Diagnostics diags = compilation.getAllDiagnostics(); \
        if (!diags.empty()) {                                \
            FAIL_CHECK(report(diags));                       \
        }                                                    \
    } while (0)

inline std::string report(Diagnostics& diags) {
    if (diags.empty())
        return "";

    return DiagnosticWriter{ SyntaxTree::getDefaultSourceManager() }.report(diags);
}

inline std::string findTestDir() {
    auto path = fs::current_path();
    while (!fs::exists(path / "tests")) {
        path = path.parent_path();
        ASSERT(!path.empty());
    }

    return (path / "tests/unittests/data/").string();
}

inline SourceManager& getSourceManager() {
    static SourceManager* sourceManager = nullptr;
    if (!sourceManager) {
        auto testDir = findTestDir();
        sourceManager = new SourceManager();
        sourceManager->addUserDirectory(string_view(testDir));
        sourceManager->addSystemDirectory(string_view(testDir));
        sourceManager->addSystemDirectory(string_view(testDir + "system/"));
    }
    return *sourceManager;
}

inline bool withinUlp(double a, double b) {
    static_assert(sizeof(double) == sizeof(int64_t));
    int64_t ia, ib;
    memcpy(&ia, &a, sizeof(double));
    memcpy(&ib, &b, sizeof(double));
    return std::abs(ia - ib) <= 1;
}

inline std::string to_string(const Diagnostic& diag) {
    return DiagnosticWriter(getSourceManager()).report(diag);
}

inline Token lexToken(string_view text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(text);

    Token token = preprocessor.next();
    REQUIRE(token);
    return token;
}

inline Token lexRawToken(string_view text) {
    diagnostics.clear();
    auto buffer = getSourceManager().assignText(text);
    Lexer lexer(buffer, alloc, diagnostics);

    Token token = lexer.lex();
    REQUIRE(token);
    return token;
}

inline const ModuleDeclarationSyntax& parseModule(const std::string& text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(string_view(text));

    Parser parser(preprocessor);
    return parser.parseModule();
}

inline const ClassDeclarationSyntax& parseClass(const std::string& text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(string_view(text));

    Parser parser(preprocessor);
    return parser.parseClass();
}

inline const MemberSyntax& parseMember(const std::string& text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(string_view(text));

    Parser parser(preprocessor);
    MemberSyntax* member = parser.parseMember();
    REQUIRE(member);
    return *member;
}

inline const StatementSyntax& parseStatement(const std::string& text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(string_view(text));

    Parser parser(preprocessor);
    return parser.parseStatement();
}

inline const ExpressionSyntax& parseExpression(const std::string& text) {
    diagnostics.clear();

    Preprocessor preprocessor(getSourceManager(), alloc, diagnostics);
    preprocessor.pushSource(string_view(text));

    Parser parser(preprocessor);
    return parser.parseExpression();
}

inline const ModuleInstanceSymbol& evalModule(std::shared_ptr<SyntaxTree> syntax,
                                              Compilation& compilation) {
    compilation.addSyntaxTree(syntax);
    const RootSymbol& root = compilation.getRoot();

    REQUIRE(root.topInstances.size() > 0);
    if (!syntax->diagnostics().empty())
        WARN(report(syntax->diagnostics()));

    return *root.topInstances[0];
}

class LogicExactlyEqualMatcher : public Catch::MatcherBase<logic_t> {
public:
    explicit LogicExactlyEqualMatcher(logic_t v) : value(v) {}

    bool match(const logic_t& t) const override final { return exactlyEqual(t, value); }

    std::string describe() const override final {
        std::ostringstream ss;
        ss << "equals " << value;
        return ss.str();
    }

private:
    logic_t value;
};

inline LogicExactlyEqualMatcher exactlyEquals(logic_t v) {
    return LogicExactlyEqualMatcher(v);
}

class SVIntExactlyEqualMatcher : public Catch::MatcherBase<SVInt> {
public:
    explicit SVIntExactlyEqualMatcher(SVInt v) : value(v) {}

    bool match(const SVInt& t) const override final { return exactlyEqual(t, value); }

    std::string describe() const override final {
        std::ostringstream ss;
        ss << "equals " << value;
        return ss.str();
    }

private:
    SVInt value;
};

inline SVIntExactlyEqualMatcher exactlyEquals(SVInt v) {
    return SVIntExactlyEqualMatcher(v);
}
