//------------------------------------------------------------------------------
// Parser.h
// SystemVerilog language parser.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#pragma once

#include "slang/numeric/VectorBuilder.h"
#include "slang/parsing/ParserBase.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/Bag.h"

namespace slang {

class BumpAllocator;
class Preprocessor;

/// Implements a full syntax parser for SystemVerilog.
class Parser : ParserBase {
public:
    // TODO: This should be configurable through the Options system
    static constexpr size_t MaxDepth = 100;

    explicit Parser(Preprocessor& preprocessor, const Bag& options = {});

    /// Parse a whole compilation unit.
    CompilationUnitSyntax& parseCompilationUnit();

    /// Parse an expression / statement / module / class / name.
    /// These are mostly for testing; only use if you know that the
    /// source stream is currently looking at one of these.
    ExpressionSyntax& parseExpression();
    StatementSyntax& parseStatement(bool allowEmpty = true);
    ModuleDeclarationSyntax& parseModule();
    ClassDeclarationSyntax& parseClass();
    MemberSyntax* parseMember();
    NameSyntax& parseName();

    /// Generalized node parse function that tries to figure out what we're
    /// looking at and parse that specifically. A normal batch compile won't call
    /// this, since in a well formed program every file is a compilation unit,
    /// but for snippets of code this can be convenient.
    SyntaxNode& parseGuess();

    /// Check whether the parser has consumed the entire input stream.
    bool isDone();

    /// Gets the EndOfFile token, if one has been consumed. Otherwise returns an empty token.
    Token getEOFToken();

private:
    ExpressionSyntax& parseMinTypMaxExpression();
    ExpressionSyntax& parsePrimaryExpression();
    ExpressionSyntax& parseIntegerExpression();
    ExpressionSyntax& parseInsideExpression(ExpressionSyntax& expr);
    ExpressionSyntax& parsePostfixExpression(ExpressionSyntax& expr);
    ExpressionSyntax& parseNewExpression(ExpressionSyntax* expr);
    ConcatenationExpressionSyntax& parseConcatenation(Token openBrace, ExpressionSyntax* first);
    StreamingConcatenationExpressionSyntax& parseStreamConcatenation(Token openBrace);
    StreamExpressionSyntax& parseStreamExpression();
    OpenRangeListSyntax& parseOpenRangeList();
    ExpressionSyntax& parseOpenRangeElement();
    ElementSelectSyntax& parseElementSelect();
    SelectorSyntax* parseElementSelector();
    NameSyntax& parseName(bool isForEach);
    NameSyntax& parseNamePart(bool isForEach);
    ParameterValueAssignmentSyntax* parseParameterValueAssignment();
    ArgumentListSyntax& parseArgumentList();
    ArgumentSyntax& parseArgument();
    PatternSyntax& parsePattern();
    AssignmentPatternExpressionSyntax& parseAssignmentPatternExpression(DataTypeSyntax* type);
    AssignmentPatternItemSyntax& parseAssignmentPatternItem(ExpressionSyntax* key);
    EventExpressionSyntax& parseEventExpression();
    NamedBlockClauseSyntax* parseNamedBlockClause();
    TimingControlSyntax* parseTimingControl();
    ConditionalPredicateSyntax& parseConditionalPredicate(ExpressionSyntax& first,
                                                          TokenKind endKind, Token& end);
    ConditionalPatternSyntax& parseConditionalPattern();
    ConditionalStatementSyntax& parseConditionalStatement(NamedLabelSyntax* label,
                                                          span<AttributeInstanceSyntax*> attributes,
                                                          Token uniqueOrPriority);
    ElseClauseSyntax* parseElseClause();
    CaseStatementSyntax& parseCaseStatement(NamedLabelSyntax* label,
                                            span<AttributeInstanceSyntax*> attributes,
                                            Token uniqueOrPriority, Token caseKeyword);
    DefaultCaseItemSyntax& parseDefaultCaseItem();
    LoopStatementSyntax& parseLoopStatement(NamedLabelSyntax* label,
                                            span<AttributeInstanceSyntax*> attributes);
    DoWhileStatementSyntax& parseDoWhileStatement(NamedLabelSyntax* label,
                                                  span<AttributeInstanceSyntax*> attributes);
    ForLoopStatementSyntax& parseForLoopStatement(NamedLabelSyntax* label,
                                                  span<AttributeInstanceSyntax*> attributes);
    SyntaxNode& parseForInitializer();
    ForeachLoopListSyntax& parseForeachLoopVariables();
    ForeachLoopStatementSyntax& parseForeachLoopStatement(
        NamedLabelSyntax* label, span<AttributeInstanceSyntax*> attributes);
    ReturnStatementSyntax& parseReturnStatement(NamedLabelSyntax* label,
                                                span<AttributeInstanceSyntax*> attributes);
    JumpStatementSyntax& parseJumpStatement(NamedLabelSyntax* label,
                                            span<AttributeInstanceSyntax*> attributes);
    ProceduralAssignStatementSyntax& parseProceduralAssignStatement(
        NamedLabelSyntax* label, span<AttributeInstanceSyntax*> attributes, SyntaxKind kind);
    ProceduralDeassignStatementSyntax& parseProceduralDeassignStatement(
        NamedLabelSyntax* label, span<AttributeInstanceSyntax*> attributes, SyntaxKind kind);
    StatementSyntax& parseDisableStatement(NamedLabelSyntax* label,
                                           span<AttributeInstanceSyntax*> attributes);
    StatementSyntax& parseAssertionStatement(NamedLabelSyntax* label,
                                             span<AttributeInstanceSyntax*> attributes);
    ConcurrentAssertionStatementSyntax& parseConcurrentAssertion(
        NamedLabelSyntax* label, span<AttributeInstanceSyntax*> attributes);
    PropertySpecSyntax& parsePropertySpec();
    ActionBlockSyntax& parseActionBlock();
    BlockStatementSyntax& parseBlock(SyntaxKind blockKind, TokenKind endKind,
                                     NamedLabelSyntax* label,
                                     span<AttributeInstanceSyntax*> attributes);
    StatementSyntax& parseWaitStatement(NamedLabelSyntax* label,
                                        span<AttributeInstanceSyntax*> attributes);
    WaitOrderStatementSyntax& parseWaitOrderStatement(NamedLabelSyntax* label,
                                                      span<AttributeInstanceSyntax*> attributes);
    RandCaseStatementSyntax& parseRandCaseStatement(NamedLabelSyntax* label,
                                                    span<AttributeInstanceSyntax*> attributes);
    EventTriggerStatementSyntax& parseEventTriggerStatement(
        NamedLabelSyntax* label, span<AttributeInstanceSyntax*> attributes);
    Token parseSigning();
    VariableDimensionSyntax* parseDimension();
    span<VariableDimensionSyntax*> parseDimensionList();
    StructUnionTypeSyntax& parseStructUnion(SyntaxKind syntaxKind);
    EnumTypeSyntax& parseEnum();
    DataTypeSyntax& parseDataType(bool allowImplicit);
    DotMemberClauseSyntax* parseDotMemberClause();
    span<AttributeInstanceSyntax*> parseAttributes();
    AttributeSpecSyntax& parseAttributeSpec();
    ModuleHeaderSyntax& parseModuleHeader();
    ParameterPortListSyntax* parseParameterPortList();
    ModuleDeclarationSyntax& parseModule(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax& parseModportSubroutinePortList(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax& parseModportPort();
    ModportItemSyntax& parseModportItem();
    ModportDeclarationSyntax& parseModportDeclaration(span<AttributeInstanceSyntax*> attributes);
    PortReferenceSyntax& parsePortReference();
    PortExpressionSyntax& parsePortExpression();
    NonAnsiPortSyntax& parseNonAnsiPort();
    MemberSyntax& parseAnsiPort();
    AnsiPortListSyntax& parseAnsiPortList(Token openParen);
    PortHeaderSyntax& parsePortHeader(Token direction);
    PortDeclarationSyntax& parsePortDeclaration(span<AttributeInstanceSyntax*> attributes);
    TimeUnitsDeclarationSyntax& parseTimeUnitsDeclaration(
        span<AttributeInstanceSyntax*> attributes);
    span<PackageImportDeclarationSyntax*> parsePackageImports();
    PackageImportDeclarationSyntax& parseImportDeclaration(
        span<AttributeInstanceSyntax*> attributes);
    PackageImportItemSyntax& parsePackageImportItem();
    DPIImportExportSyntax& parseDPIImportExport(span<AttributeInstanceSyntax*> attributes);
    AssertionItemPortListSyntax* parseAssertionItemPortList(TokenKind declarationKind);
    PropertyDeclarationSyntax& parsePropertyDeclaration(span<AttributeInstanceSyntax*> attributes);
    SequenceDeclarationSyntax& parseSequenceDeclaration(span<AttributeInstanceSyntax*> attributes);
    ParameterDeclarationSyntax& parseParameterPort();
    ClockingSkewSyntax* parseClockingSkew();
    ClockingDeclarationSyntax& parseClockingDeclaration(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax& parseVariableDeclaration(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax& parseNetDeclaration(span<AttributeInstanceSyntax*> attributes);
    HierarchyInstantiationSyntax& parseHierarchyInstantiation(
        span<AttributeInstanceSyntax*> attributes);
    HierarchicalInstanceSyntax& parseHierarchicalInstance();
    PortConnectionSyntax& parsePortConnection();
    FunctionPortSyntax& parseFunctionPort();
    FunctionPrototypeSyntax& parseFunctionPrototype(bool allowTasks = true);
    FunctionDeclarationSyntax& parseFunctionDeclaration(span<AttributeInstanceSyntax*> attributes,
                                                        SyntaxKind functionKind, TokenKind endKind);
    Token parseLifetime();
    span<SyntaxNode*> parseBlockItems(TokenKind endKind, Token& end);
    GenvarDeclarationSyntax& parseGenvarDeclaration(span<AttributeInstanceSyntax*> attributes);
    LoopGenerateSyntax& parseLoopGenerateConstruct(span<AttributeInstanceSyntax*> attributes);
    IfGenerateSyntax& parseIfGenerateConstruct(span<AttributeInstanceSyntax*> attributes);
    CaseGenerateSyntax& parseCaseGenerateConstruct(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax& parseGenerateBlock();
    ImplementsClauseSyntax* parseImplementsClause(TokenKind keywordKind, Token& semi);
    ClassDeclarationSyntax& parseClassDeclaration(span<AttributeInstanceSyntax*> attributes,
                                                  Token virtualOrInterface);
    MemberSyntax* parseClassMember();
    ContinuousAssignSyntax& parseContinuousAssign(span<AttributeInstanceSyntax*> attributes);
    VariableDeclaratorSyntax& parseVariableDeclarator(bool isFirst);
    span<TokenOrSyntax> parseOneVariableDeclarator();
    MemberSyntax* parseCoverageMember();
    BlockEventExpressionSyntax& parseBlockEventExpression();
    WithClauseSyntax* parseWithClause();
    CovergroupDeclarationSyntax& parseCovergroupDeclaration(
        span<AttributeInstanceSyntax*> attributes);
    CoverpointSyntax* parseCoverpoint(span<AttributeInstanceSyntax*> attributes,
                                      DataTypeSyntax* type, NamedLabelSyntax* label);
    CoverageOptionSyntax* parseCoverageOption(span<AttributeInstanceSyntax*> attributes);
    MemberSyntax* parseCoverpointMember();
    MemberSyntax& parseConstraint(span<AttributeInstanceSyntax*> attributes,
                                  span<Token> qualifiers);
    ConstraintBlockSyntax& parseConstraintBlock();
    ConstraintItemSyntax& parseConstraintItem(bool allowBlock);
    DistConstraintListSyntax& parseDistConstraintList();
    DistItemSyntax& parseDistItem();
    ExpressionSyntax& parseArrayOrRandomizeWithClause();
    DefParamAssignmentSyntax& parseDefParamAssignment();
    DefParamSyntax& parseDefParam(span<AttributeInstanceSyntax*> attributes);
    ExpressionSyntax& parseExpressionOrDist();
    TransRangeSyntax& parseTransRange();
    TransSetSyntax& parseTransSet();
    TransListCoverageBinInitializerSyntax& parseTransListInitializer();

    bool isPortDeclaration();
    bool isNetDeclaration();
    bool isVariableDeclaration();
    bool isHierarchyInstantiation();
    bool isNonAnsiPort();
    bool isPlainPortName();
    bool scanDimensionList(uint32_t& index);
    bool scanQualifiedName(uint32_t& index);

    void errorIfAttributes(span<AttributeInstanceSyntax*> attributes, DiagCode code);

    class DepthGuard {
    public:
        DepthGuard(Parser& _parser) : parser(_parser) {
            ++parser.depth;
            parser.throwIfTooDeep();
        }
        ~DepthGuard() { --parser.depth; }

    private:
        Parser& parser;
    };
    DepthGuard setDepthGuard() { return DepthGuard(*this); }
    void throwIfTooDeep();

    /// Various options for parsing expressions.
    // TODO: convert to bitmask
    struct ExpressionOptions {
        enum Enum {
            None = 0,

            // Allow pattern matching expressions; these are not allowed recursively so
            // they're turned off after finding the first one
            AllowPatternMatch = 1,

            // In a procedural assignment context, <= is a non-blocking assignment, not less than or
            // equals
            ProceduralAssignmentContext = 2,

            // In an event expression context, the "or" operator has special meaning
            EventExpressionContext = 4
        };
    };

    ExpressionSyntax& parseSubExpression(ExpressionOptions::Enum options, int precedence);
    ExpressionSyntax& parsePrefixExpression(ExpressionOptions::Enum options, SyntaxKind opKind);

    template<bool (*IsEnd)(TokenKind)>
    span<TokenOrSyntax> parseVariableDeclarators(TokenKind endKind, Token& end);
    span<TokenOrSyntax> parseVariableDeclarators(Token& semi);

    template<typename TMember, typename TParseFunc>
    span<TMember*> parseMemberList(TokenKind endKind, Token& endToken, TParseFunc&& parseFunc);

    template<bool (*IsEnd)(TokenKind)>
    bool scanTypePart(uint32_t& index, TokenKind start, TokenKind end);

    SyntaxFactory factory;

    // Scratch space for building up integer vector literals.
    VectorBuilder vectorBuilder;
    size_t depth = 0;
    Token eofToken;
};

template<bool (*IsEnd)(TokenKind)>
bool Parser::scanTypePart(uint32_t& index, TokenKind start, TokenKind end) {
    int nesting = 1;
    while (true) {
        auto kind = peek(index).kind;
        if (IsEnd(kind))
            return false;

        index++;
        if (kind == start)
            nesting++;
        else if (kind == end) {
            nesting--;
            if (nesting <= 0)
                return true;
        }
    }
}

} // namespace slang
