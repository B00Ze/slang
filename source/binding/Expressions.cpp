//------------------------------------------------------------------------------
// Expressions.cpp
// Expression creation and analysis.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "slang/binding/Expressions.h"

#include <nlohmann/json.hpp>

#include "slang/compilation/Compilation.h"
#include "slang/symbols/ASTVisitor.h"
#include "slang/symbols/TypeSymbols.h"

namespace {

using namespace slang;

const Type* binaryOperatorType(Compilation& compilation, const Type* lt, const Type* rt,
                               bool forceFourState) {
    if (!lt->isNumeric() || !rt->isNumeric())
        return &compilation.getErrorType();

    // Figure out what the result type of an arithmetic binary operator should be. The rules are:
    // - If either operand is real, the result is real
    // - Otherwise, if either operand is shortreal, the result is shortreal
    // - Otherwise, result is integral with the following properties:
    //      - Bit width is max(lhs, rhs)
    //      - If either operand is four state (or if we force it), the result is four state
    //      - If either operand is unsigned, the result is unsigned
    const Type* result;
    if (lt->isFloating() || rt->isFloating()) {
        if ((lt->isFloating() && lt->getBitWidth() == 64) ||
            (rt->isFloating() && rt->getBitWidth() == 64)) {
            result = &compilation.getRealType();
        }
        else {
            result = &compilation.getShortRealType();
        }
    }
    else {
        bitwidth_t width = std::max(lt->getBitWidth(), rt->getBitWidth());

        bitmask<IntegralFlags> lf = lt->getIntegralFlags();
        bitmask<IntegralFlags> rf = rt->getIntegralFlags();

        bitmask<IntegralFlags> flags;
        if ((lf & IntegralFlags::Signed) && (rf & IntegralFlags::Signed))
            flags |= IntegralFlags::Signed;
        if (forceFourState || (lf & IntegralFlags::FourState) || (rf & IntegralFlags::FourState))
            flags |= IntegralFlags::FourState;
        if ((lf & IntegralFlags::Reg) && (rf & IntegralFlags::Reg))
            flags |= IntegralFlags::Reg;

        // If the width is 1, try to preserve whether this was a packed array with a width of 1
        // or a plain scalar type with no dimensions specified.
        if (width == 1 && (lt->isScalar() || rt->isScalar()))
            result = &compilation.getScalarType(flags);
        else
            result = &compilation.getType(width, flags);
    }

    // Attempt to preserve any type aliases passed in when selecting the result.
    if (lt->isMatching(*result))
        return lt;
    if (rt->isMatching(*result))
        return rt;
    return result;
}

const Type* forceFourState(Compilation& compilation, const Type* type) {
    if (type->isFloating() || type->isFourState())
        return type;

    // Use the logic in binaryOperatorType to create a type with the correct size and sign.
    return binaryOperatorType(compilation, type, type, true);
}

const Type* singleBitType(Compilation& compilation, const Type* lt, const Type* rt) {
    if (lt->isFourState() || rt->isFourState())
        return &compilation.getLogicType();
    return &compilation.getBitType();
}

const Type& getIndexedType(Compilation& compilation, const BindContext& context,
                           const Type& valueType, SourceRange exprRange, SourceRange valueRange,
                           bool isRangeSelect) {
    const Type& ct = valueType.getCanonicalType();
    if (ct.kind == SymbolKind::UnpackedArrayType) {
        return ct.as<UnpackedArrayType>().elementType;
    }
    else if (ct.kind == SymbolKind::PackedArrayType) {
        return ct.as<PackedArrayType>().elementType;
    }
    else if (ct.kind == SymbolKind::StringType && !isRangeSelect) {
        return compilation.getByteType();
    }
    else if (!ct.isIntegral()) {
        auto& diag = context.addDiag(DiagCode::BadIndexExpression, exprRange);
        diag << valueRange;
        diag << valueType;
        return compilation.getErrorType();
    }
    else if (ct.isScalar()) {
        auto& diag = context.addDiag(DiagCode::CannotIndexScalar, exprRange);
        diag << valueRange;
        return compilation.getErrorType();
    }
    else if (ct.isFourState()) {
        return compilation.getLogicType();
    }
    else {
        return compilation.getBitType();
    }
}

struct ToJsonVisitor {
    template<typename T>
    void visit(const T& expr, json& j) {
        j["kind"] = toString(expr.kind);
        j["type"] = *expr.type;

        if (expr.constant)
            j["constant"] = *expr.constant;

        if constexpr (!std::is_same_v<Expression, T>) {
            expr.toJson(j);
        }
    }

    void visitInvalid(const Expression& expr, json& j) { visit(expr.as<InvalidExpression>(), j); }
};

void checkBindFlags(const Expression& expr, const BindContext& context) {
    if (context.flags & BindFlags::Constant) {
        EvalContext evalContext;
        expr.eval(evalContext);

        const Diagnostics& diags = evalContext.getDiagnostics();
        if (!diags.empty()) {
            Diagnostic& diag = context.addDiag(DiagCode::ExpressionNotConstant, expr.sourceRange);
            for (const Diagnostic& note : diags)
                diag.addNote(note);
        }
    }
}

} // namespace

namespace slang {

// This visitor handles inserting implicit conversions into an expression tree where necessary.
// SystemVerilog has an additional weird feature where the type of one branch of an expression
// tree can bubble up and then propagate back down a different branch, which is also implemented
// here.
struct Expression::PropagationVisitor {
    HAS_METHOD_TRAIT(propagateType);

    Compilation& compilation;
    const Type& newType;

    PropagationVisitor(Compilation& compilation, const Type& newType) :
        compilation(compilation), newType(newType) {}

    template<typename T>
    Expression& visit(T& expr) {
        if (expr.bad())
            return expr;

        if (newType.isError()) {
            expr.type = &newType;
            return expr;
        }

        // If the new type is equivalent to the old type, there's no need for a conversion.
        // Otherwise if both types are integral or both are real, we have to check if the
        // conversion should be pushed further down the tree. Otherwise we should insert
        // the implicit conversion here.
        bool needConversion = !newType.isEquivalent(*expr.type);
        if constexpr (has_propagateType_v<T, bool, Compilation&, const Type&>) {
            if ((newType.isFloating() && expr.type->isFloating()) ||
                (newType.isIntegral() && expr.type->isIntegral()) || newType.isString()) {

                if (expr.propagateType(compilation, newType))
                    needConversion = false;
            }
        }

        Expression* result = &expr;
        if (needConversion)
            result = &Expression::implicitConversion(compilation, newType, expr);

        // Try to fold any constant values.
        ASSERT(!result->constant);
        EvalContext context;
        ConstantValue value = result->eval(context);
        if (value)
            result->constant = compilation.allocConstant(std::move(value));
        return *result;
    }

    Expression& visitInvalid(Expression& expr) { return expr; }
};

const InvalidExpression InvalidExpression::Instance(nullptr, ErrorType::Instance);

const Expression& Expression::bind(const ExpressionSyntax& syntax, const BindContext& context,
                                   bitmask<BindFlags> extraFlags) {
    const Expression& result =
        selfDetermined(context.scope.getCompilation(), syntax, context, extraFlags);
    checkBindFlags(result, context.resetFlags(extraFlags));
    return result;
}

const Expression& Expression::bind(const Type& lhs, const ExpressionSyntax& rhs,
                                   SourceLocation location, const BindContext& context) {
    Compilation& comp = context.scope.getCompilation();
    Expression& expr = create(comp, rhs, context);

    const Expression& result = convertAssignment(context.scope, lhs, expr, location);
    checkBindFlags(result, context);
    return result;
}

bool Expression::bad() const {
    return kind == ExpressionKind::Invalid || type->isError();
}

bool Expression::isLValue() const {
    switch (kind) {
        // TODO: concatenations
        case ExpressionKind::NamedValue:
        case ExpressionKind::ElementSelect:
        case ExpressionKind::RangeSelect:
        case ExpressionKind::MemberAccess:
            return true;
        default:
            return false;
    }
}

bool Expression::isImplicitString() const {
    if (type->isString())
        return true;

    switch (kind) {
        case ExpressionKind::StringLiteral:
            return true;
        case ExpressionKind::Concatenation: {
            auto& concat = as<ConcatenationExpression>();
            for (auto op : concat.operands()) {
                if (!op->isImplicitString())
                    return false;
            }
            return true;
        }
        case ExpressionKind::Replication: {
            auto& repl = as<ReplicationExpression>();
            return repl.concat().isImplicitString();
        }
        default:
            return false;
    }
}

void to_json(json& j, const Expression& expr) {
    ToJsonVisitor visitor;
    expr.visit(visitor, j);
}

Expression& Expression::create(Compilation& compilation, const ExpressionSyntax& syntax,
                               const BindContext& ctx, bitmask<BindFlags> extraFlags) {
    BindContext context = ctx.resetFlags(extraFlags);
    Expression* result;
    switch (syntax.kind) {
        case SyntaxKind::NullLiteralExpression:
            result = &NullLiteral::fromSyntax(compilation, syntax.as<LiteralExpressionSyntax>());
            break;
        case SyntaxKind::StringLiteralExpression:
            result = &StringLiteral::fromSyntax(compilation, syntax.as<LiteralExpressionSyntax>());
            break;
        case SyntaxKind::RealLiteralExpression:
            result = &RealLiteral::fromSyntax(compilation, syntax.as<LiteralExpressionSyntax>());
            break;
        case SyntaxKind::IntegerLiteralExpression:
            result = &IntegerLiteral::fromSyntax(compilation, syntax.as<LiteralExpressionSyntax>());
            break;
        case SyntaxKind::UnbasedUnsizedLiteralExpression:
            result = &UnbasedUnsizedIntegerLiteral::fromSyntax(
                compilation, syntax.as<LiteralExpressionSyntax>());
            break;
        case SyntaxKind::IntegerVectorExpression:
            result = &IntegerLiteral::fromSyntax(compilation,
                                                 syntax.as<IntegerVectorExpressionSyntax>());
            break;
        case SyntaxKind::ParenthesizedExpression:
            result = &create(compilation, *syntax.as<ParenthesizedExpressionSyntax>().expression,
                             context, extraFlags);
            break;
        case SyntaxKind::UnaryPlusExpression:
        case SyntaxKind::UnaryMinusExpression:
        case SyntaxKind::UnaryBitwiseNotExpression:
        case SyntaxKind::UnaryBitwiseAndExpression:
        case SyntaxKind::UnaryBitwiseOrExpression:
        case SyntaxKind::UnaryBitwiseXorExpression:
        case SyntaxKind::UnaryBitwiseNandExpression:
        case SyntaxKind::UnaryBitwiseNorExpression:
        case SyntaxKind::UnaryBitwiseXnorExpression:
        case SyntaxKind::UnaryLogicalNotExpression:
        case SyntaxKind::UnaryPreincrementExpression:
        case SyntaxKind::UnaryPredecrementExpression:
            result = &UnaryExpression::fromSyntax(
                compilation, syntax.as<PrefixUnaryExpressionSyntax>(), context);
            break;
        case SyntaxKind::PostincrementExpression:
        case SyntaxKind::PostdecrementExpression:
            result = &UnaryExpression::fromSyntax(
                compilation, syntax.as<PostfixUnaryExpressionSyntax>(), context);
            break;
        case SyntaxKind::AddExpression:
        case SyntaxKind::SubtractExpression:
        case SyntaxKind::MultiplyExpression:
        case SyntaxKind::DivideExpression:
        case SyntaxKind::ModExpression:
        case SyntaxKind::BinaryAndExpression:
        case SyntaxKind::BinaryOrExpression:
        case SyntaxKind::BinaryXorExpression:
        case SyntaxKind::BinaryXnorExpression:
        case SyntaxKind::EqualityExpression:
        case SyntaxKind::InequalityExpression:
        case SyntaxKind::CaseEqualityExpression:
        case SyntaxKind::CaseInequalityExpression:
        case SyntaxKind::GreaterThanEqualExpression:
        case SyntaxKind::GreaterThanExpression:
        case SyntaxKind::LessThanEqualExpression:
        case SyntaxKind::LessThanExpression:
        case SyntaxKind::WildcardEqualityExpression:
        case SyntaxKind::WildcardInequalityExpression:
        case SyntaxKind::LogicalAndExpression:
        case SyntaxKind::LogicalOrExpression:
        case SyntaxKind::LogicalImplicationExpression:
        case SyntaxKind::LogicalEquivalenceExpression:
        case SyntaxKind::LogicalShiftLeftExpression:
        case SyntaxKind::LogicalShiftRightExpression:
        case SyntaxKind::ArithmeticShiftLeftExpression:
        case SyntaxKind::ArithmeticShiftRightExpression:
        case SyntaxKind::PowerExpression:
            result = &BinaryExpression::fromSyntax(compilation, syntax.as<BinaryExpressionSyntax>(),
                                                   context);
            break;
        case SyntaxKind::AssignmentExpression:
        case SyntaxKind::AddAssignmentExpression:
        case SyntaxKind::SubtractAssignmentExpression:
        case SyntaxKind::MultiplyAssignmentExpression:
        case SyntaxKind::DivideAssignmentExpression:
        case SyntaxKind::ModAssignmentExpression:
        case SyntaxKind::AndAssignmentExpression:
        case SyntaxKind::OrAssignmentExpression:
        case SyntaxKind::XorAssignmentExpression:
        case SyntaxKind::LogicalLeftShiftAssignmentExpression:
        case SyntaxKind::LogicalRightShiftAssignmentExpression:
        case SyntaxKind::ArithmeticLeftShiftAssignmentExpression:
        case SyntaxKind::ArithmeticRightShiftAssignmentExpression:
            result = &AssignmentExpression::fromSyntax(
                compilation, syntax.as<BinaryExpressionSyntax>(), context);
            break;
        case SyntaxKind::InvocationExpression:
            result = &CallExpression::fromSyntax(compilation,
                                                 syntax.as<InvocationExpressionSyntax>(), context);
            break;
        case SyntaxKind::ConditionalExpression:
            result = &ConditionalExpression::fromSyntax(
                compilation, syntax.as<ConditionalExpressionSyntax>(), context);
            break;
        case SyntaxKind::ConcatenationExpression:
            result = &ConcatenationExpression::fromSyntax(
                compilation, syntax.as<ConcatenationExpressionSyntax>(), context);
            break;
        case SyntaxKind::MultipleConcatenationExpression:
            result = &ReplicationExpression::fromSyntax(
                compilation, syntax.as<MultipleConcatenationExpressionSyntax>(), context);
            break;
        case SyntaxKind::ElementSelectExpression:
            result = &bindSelectExpression(compilation, syntax.as<ElementSelectExpressionSyntax>(),
                                           context);
            break;
        case SyntaxKind::CastExpression:
            result = &ConversionExpression::fromSyntax(compilation,
                                                       syntax.as<CastExpressionSyntax>(), context);
            break;
        default:
            if (NameSyntax::isKind(syntax.kind)) {
                result = &bindName(compilation, syntax.as<NameSyntax>(), nullptr, context);
                break;
            }
            else if (DataTypeSyntax::isKind(syntax.kind)) {
                result = &DataTypeExpression::fromSyntax(compilation, syntax.as<DataTypeSyntax>(),
                                                         context);
                break;
            }
            THROW_UNREACHABLE;
    }

    result->syntax = &syntax;
    return *result;
}

Expression& Expression::bindName(Compilation& compilation, const NameSyntax& syntax,
                                 const InvocationExpressionSyntax* invocation,
                                 const BindContext& context) {

    bitmask<LookupFlags> flags = LookupFlags::AllowSystemSubroutine;
    if (invocation && invocation->arguments)
        flags |= LookupFlags::AllowDeclaredAfter;
    if (context.flags & BindFlags::Constant)
        flags |= LookupFlags::Constant;

    LookupResult result;
    context.scope.lookupName(syntax, context.lookupLocation, flags, result);

    if (result.hasError())
        compilation.addDiagnostics(result.getDiagnostics());

    SourceRange callRange = invocation ? invocation->sourceRange() : syntax.sourceRange();
    if (result.systemSubroutine) {
        // There won't be any selectors here; this gets checked in the lookup call.
        ASSERT(result.selectors.empty());
        return CallExpression::fromLookup(compilation, result.systemSubroutine, invocation,
                                          callRange, context);
    }

    const Symbol* symbol = result.found;
    if (!symbol)
        return badExpr(compilation, nullptr);

    if (symbol->isType() && (context.flags & BindFlags::AllowDataType) != 0) {
        // We looked up a named data type and we were allowed to do so, so return it.
        ASSERT(!invocation);
        const Type& resultType = Type::fromLookupResult(compilation, result, syntax,
                                                        context.lookupLocation, context.scope);
        return *compilation.emplace<DataTypeExpression>(resultType, syntax.sourceRange());
    }

    Expression* expr;
    if (symbol->kind == SymbolKind::Subroutine) {
        expr = &CallExpression::fromLookup(compilation, &symbol->as<SubroutineSymbol>(), invocation,
                                           callRange, context);
        invocation = nullptr;
    }
    else {
        expr = &NamedValueExpression::fromSymbol(context.scope, *symbol, result.isHierarchical,
                                                 syntax.sourceRange());
    }

    // Drill down into member accesses.
    for (const auto& selector : result.selectors) {
        if (expr->bad())
            return *expr;

        auto memberSelect = std::get_if<LookupResult::MemberSelector>(&selector);
        if (memberSelect) {
            string_view name = memberSelect->name;
            if (name.empty())
                return badExpr(compilation, expr);

            const Type& type = expr->type->getCanonicalType();
            switch (type.kind) {
                case SymbolKind::PackedStructType:
                case SymbolKind::UnpackedStructType:
                case SymbolKind::PackedUnionType:
                case SymbolKind::UnpackedUnionType:
                    expr = &MemberAccessExpression::fromSelector(compilation, *expr, *memberSelect,
                                                                 context);
                    break;

                case SymbolKind::EnumType:
                    expr = &CallExpression::fromSystemMethod(compilation, *expr, *memberSelect,
                                                             invocation, context);
                    invocation = nullptr;
                    break;

                default: {
                    auto& diag =
                        context.addDiag(DiagCode::InvalidMemberAccess, memberSelect->dotLocation);
                    diag << expr->sourceRange;
                    diag << memberSelect->nameRange;
                    diag << *expr->type;
                    return badExpr(compilation, expr);
                }
            }
        }
        else {
            // Element / range selectors.
            const ElementSelectSyntax* selectSyntax =
                std::get<const ElementSelectSyntax*>(selector);
            expr = &bindSelector(compilation, *expr, *selectSyntax, context);
        }
    }

    // If we require a subroutine, enforce that now. The invocation syntax will have been
    // nulled out if we used it above.
    if (invocation && !expr->bad()) {
        SourceLocation loc = invocation->arguments ? invocation->arguments->openParen.location()
                                                   : syntax.getFirstToken().location();
        auto& diag = context.addDiag(DiagCode::ExpressionNotCallable, loc);
        diag << syntax.sourceRange();
        return badExpr(compilation, nullptr);
    }

    return *expr;
}

Expression& Expression::bindSelectExpression(Compilation& compilation,
                                             const ElementSelectExpressionSyntax& syntax,
                                             const BindContext& context) {
    Expression& value = create(compilation, *syntax.left, context);
    return bindSelector(compilation, value, *syntax.select, context);
}

Expression& Expression::bindSelector(Compilation& compilation, Expression& value,
                                     const ElementSelectSyntax& syntax,
                                     const BindContext& context) {
    const SelectorSyntax* selector = syntax.selector;
    if (!selector) {
        context.addDiag(DiagCode::ExpectedExpression, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    // The full source range of the expression includes the value and the selector syntax.
    SourceRange fullRange = { value.sourceRange.start(), syntax.sourceRange().end() };

    switch (selector->kind) {
        case SyntaxKind::BitSelect:
            return ElementSelectExpression::fromSyntax(
                compilation, value, *selector->as<BitSelectSyntax>().expr, fullRange, context);
        case SyntaxKind::SimpleRangeSelect:
        case SyntaxKind::AscendingRangeSelect:
        case SyntaxKind::DescendingRangeSelect:
            return RangeSelectExpression::fromSyntax(
                compilation, value, selector->as<RangeSelectSyntax>(), fullRange, context);
        default:
            THROW_UNREACHABLE;
    }
}

void Expression::contextDetermined(Compilation& compilation, Expression*& expr,
                                   const Type& newType) {
    PropagationVisitor visitor(compilation, newType);
    expr = &expr->visit(visitor);
}

void Expression::selfDetermined(Compilation& compilation, Expression*& expr) {
    ASSERT(expr->type);
    PropagationVisitor visitor(compilation, *expr->type);
    expr = &expr->visit(visitor);
}

Expression& Expression::selfDetermined(Compilation& compilation, const ExpressionSyntax& syntax,
                                       const BindContext& context, bitmask<BindFlags> extraFlags) {
    Expression* expr = &create(compilation, syntax, context, extraFlags);
    selfDetermined(compilation, expr);
    return *expr;
}

Expression& Expression::implicitConversion(Compilation& compilation, const Type& targetType,
                                           Expression& expr) {
    ASSERT(targetType.isAssignmentCompatible(*expr.type) ||
           (targetType.isString() && expr.isImplicitString()));
    ASSERT(!targetType.isEquivalent(*expr.type));

    Expression* result = &expr;
    selfDetermined(compilation, result);
    return *compilation.emplace<ConversionExpression>(targetType, *result, result->sourceRange);
}

Expression& Expression::convertAssignment(const Scope& scope, const Type& type, Expression& expr,
                                          SourceLocation location, optional<SourceRange> lhsRange) {
    if (expr.bad())
        return expr;

    Compilation& compilation = scope.getCompilation();
    if (type.isError())
        return badExpr(compilation, &expr);

    const Type* rt = expr.type;
    if (!type.isAssignmentCompatible(*rt)) {
        // String literals have a type of integer, but are allowed to implicitly convert to the
        // string type.
        if (type.isString() && expr.isImplicitString()) {
            Expression* result = &expr;
            result = &implicitConversion(compilation, type, *result);
            selfDetermined(compilation, result);
            return *result;
        }

        DiagCode code =
            type.isCastCompatible(*rt) ? DiagCode::NoImplicitConversion : DiagCode::BadAssignment;
        auto& diag = scope.addDiag(code, location);
        diag << *rt << type;
        if (lhsRange)
            diag << *lhsRange;

        diag << expr.sourceRange;
        return badExpr(compilation, &expr);
    }

    Expression* result = &expr;
    if (type.isEquivalent(*rt)) {
        selfDetermined(compilation, result);
        return *result;
    }

    if (type.isNumeric() && rt->isNumeric()) {
        rt = binaryOperatorType(compilation, &type, rt, false);
        contextDetermined(compilation, result, *rt);

        if (type.isEquivalent(*rt))
            return *result;

        result = compilation.emplace<ConversionExpression>(type, *result, result->sourceRange);
    }
    else {
        result = &implicitConversion(compilation, type, *result);
    }

    selfDetermined(compilation, result);
    return *result;
}

Expression& Expression::badExpr(Compilation& compilation, const Expression* expr) {
    return *compilation.emplace<InvalidExpression>(expr, compilation.getErrorType());
}

void InvalidExpression::toJson(json& j) const {
    if (child)
        j["child"] = *child;
}

IntegerLiteral::IntegerLiteral(BumpAllocator& alloc, const Type& type, const SVInt& value,
                               SourceRange sourceRange) :
    Expression(ExpressionKind::IntegerLiteral, type, sourceRange),
    valueStorage(value.getBitWidth(), value.isSigned(), value.hasUnknown()) {

    if (value.isSingleWord())
        valueStorage.val = *value.getRawData();
    else {
        valueStorage.pVal =
            (uint64_t*)alloc.allocate(sizeof(uint64_t) * value.getNumWords(), alignof(uint64_t));
        memcpy(valueStorage.pVal, value.getRawData(), sizeof(uint64_t) * value.getNumWords());
    }
}

Expression& IntegerLiteral::fromSyntax(Compilation& compilation,
                                       const LiteralExpressionSyntax& syntax) {
    ASSERT(syntax.kind == SyntaxKind::IntegerLiteralExpression);

    return *compilation.emplace<IntegerLiteral>(compilation, compilation.getIntType(),
                                                syntax.literal.intValue(), syntax.sourceRange());
}

Expression& IntegerLiteral::fromSyntax(Compilation& compilation,
                                       const IntegerVectorExpressionSyntax& syntax) {
    const SVInt& value = syntax.value.intValue();

    bitmask<IntegralFlags> flags;
    if (value.isSigned())
        flags |= IntegralFlags::Signed;
    if (value.hasUnknown())
        flags |= IntegralFlags::FourState;

    const Type& type = compilation.getType(value.getBitWidth(), flags);
    return *compilation.emplace<IntegerLiteral>(compilation, type, value, syntax.sourceRange());
}

Expression& RealLiteral::fromSyntax(Compilation& compilation,
                                    const LiteralExpressionSyntax& syntax) {
    ASSERT(syntax.kind == SyntaxKind::RealLiteralExpression);

    return *compilation.emplace<RealLiteral>(compilation.getRealType(), syntax.literal.realValue(),
                                             syntax.sourceRange());
}

Expression& UnbasedUnsizedIntegerLiteral::fromSyntax(Compilation& compilation,
                                                     const LiteralExpressionSyntax& syntax) {
    ASSERT(syntax.kind == SyntaxKind::UnbasedUnsizedLiteralExpression);

    // UnsizedUnbasedLiteralExpressions default to a size of 1 in an undetermined
    // context, but can grow to be wider during propagation.
    logic_t val = syntax.literal.bitValue();
    return *compilation.emplace<UnbasedUnsizedIntegerLiteral>(
        compilation.getType(1,
                            val.isUnknown() ? IntegralFlags::FourState : IntegralFlags::TwoState),
        val, syntax.sourceRange());
}

bool UnbasedUnsizedIntegerLiteral::propagateType(Compilation&, const Type& newType) {
    bitwidth_t newWidth = newType.getBitWidth();
    ASSERT(newType.isIntegral());
    ASSERT(newWidth);

    type = &newType;
    return true;
}

Expression& NullLiteral::fromSyntax(Compilation& compilation,
                                    const LiteralExpressionSyntax& syntax) {
    ASSERT(syntax.kind == SyntaxKind::NullLiteralExpression);
    return *compilation.emplace<NullLiteral>(compilation.getNullType(), syntax.sourceRange());
}

StringLiteral::StringLiteral(const Type& type, string_view value, ConstantValue& intVal,
                             SourceRange sourceRange) :
    Expression(ExpressionKind::StringLiteral, type, sourceRange),
    value(value), intStorage(&intVal) {
}

Expression& StringLiteral::fromSyntax(Compilation& compilation,
                                      const LiteralExpressionSyntax& syntax) {
    ASSERT(syntax.kind == SyntaxKind::StringLiteralExpression);

    string_view value = syntax.literal.valueText();
    bitwidth_t width;
    ConstantValue* intVal;

    if (value.empty()) {
        // [11.10.3] says that empty string literals take on a value of "\0" (8 zero bits).
        width = 8;
        intVal = compilation.allocConstant(SVInt(8, 0, false));
    }
    else {
        width = bitwidth_t(value.size()) * 8;

        // String literals represented as integers put the first character on the
        // left, which translates to the msb, so we have to reverse the string.
        SmallVectorSized<byte, 64> bytes;
        for (char c : make_reverse_range(value))
            bytes.append((byte)c);

        intVal = compilation.allocConstant(SVInt(width, bytes, false));
    }

    auto& type = compilation.getType(width, IntegralFlags::Unsigned);
    return *compilation.emplace<StringLiteral>(type, value, *intVal, syntax.sourceRange());
}

void StringLiteral::toJson(json& j) const {
    j["literal"] = value;
}

Expression& NamedValueExpression::fromSymbol(const Scope& scope, const Symbol& symbol,
                                             bool isHierarchical, SourceRange sourceRange) {
    Compilation& compilation = scope.getCompilation();
    if (!symbol.isValue()) {
        scope.addDiag(DiagCode::NotAValue, sourceRange) << symbol.name;
        return badExpr(compilation, nullptr);
    }

    return *compilation.emplace<NamedValueExpression>(symbol.as<ValueSymbol>(), isHierarchical,
                                                      sourceRange);
}

void NamedValueExpression::toJson(json& j) const {
    j["symbol"] = Symbol::jsonLink(symbol);
    j["isHierarchical"] = isHierarchical;
}

Expression& UnaryExpression::fromSyntax(Compilation& compilation,
                                        const PrefixUnaryExpressionSyntax& syntax,
                                        const BindContext& context) {
    Expression& operand = create(compilation, *syntax.operand, context);
    const Type* type = operand.type;

    auto result = compilation.emplace<UnaryExpression>(getUnaryOperator(syntax.kind), *type,
                                                       operand, syntax.sourceRange());
    if (operand.bad())
        return badExpr(compilation, result);

    bool good;
    switch (syntax.kind) {
        case SyntaxKind::UnaryPlusExpression:
        case SyntaxKind::UnaryMinusExpression:
            // Supported for both integral and real types. Result is same as input type.
            good = type->isNumeric();
            result->type = type;
            break;
        case SyntaxKind::UnaryLogicalNotExpression:
            // Supported for both integral and real types. Result is a single bit.
            good = type->isNumeric();
            result->type =
                type->isFourState() ? &compilation.getLogicType() : &compilation.getBitType();
            selfDetermined(compilation, result->operand_);
            break;
        case SyntaxKind::UnaryBitwiseNotExpression:
            // Supported for integral only. Result type is always a single bit.
            good = type->isIntegral();
            result->type =
                type->isFourState() ? &compilation.getLogicType() : &compilation.getBitType();
            break;
        case SyntaxKind::UnaryBitwiseAndExpression:
        case SyntaxKind::UnaryBitwiseOrExpression:
        case SyntaxKind::UnaryBitwiseXorExpression:
        case SyntaxKind::UnaryBitwiseNandExpression:
        case SyntaxKind::UnaryBitwiseNorExpression:
        case SyntaxKind::UnaryBitwiseXnorExpression:
            // Supported for integral only. Result type is always a single bit.
            good = type->isIntegral();
            result->type =
                type->isFourState() ? &compilation.getLogicType() : &compilation.getBitType();
            selfDetermined(compilation, result->operand_);
            break;
        case SyntaxKind::UnaryPreincrementExpression:
        case SyntaxKind::UnaryPredecrementExpression:
            // Supported for both integral and real types. Result is same as input type.
            // The operand must also be an assignable lvalue.
            // TODO: detect and warn for multiple reads and writes of a single variable in an
            // expression
            // TODO: make sure modifications allowed in this expression
            good = type->isNumeric();
            result->type = type;
            if (!context.requireLValue(operand, syntax.operatorToken.location()))
                return badExpr(compilation, result);
            break;
        default:
            THROW_UNREACHABLE;
    }

    if (!good) {
        auto& diag = context.addDiag(DiagCode::BadUnaryExpression, syntax.operatorToken.location());
        diag << *type;
        diag << operand.sourceRange;
        return badExpr(compilation, result);
    }

    return *result;
}

Expression& UnaryExpression::fromSyntax(Compilation& compilation,
                                        const PostfixUnaryExpressionSyntax& syntax,
                                        const BindContext& context) {
    Expression& operand = create(compilation, *syntax.operand, context);
    const Type* type = operand.type;

    // This method is only ever called for postincrement and postdecrement operators, so
    // the operand must be an lvalue.
    Expression* result = compilation.emplace<UnaryExpression>(getUnaryOperator(syntax.kind), *type,
                                                              operand, syntax.sourceRange());
    if (operand.bad() || !context.requireLValue(operand, syntax.operatorToken.location()))
        return badExpr(compilation, result);

    if (!type->isNumeric()) {
        auto& diag = context.addDiag(DiagCode::BadUnaryExpression, syntax.operatorToken.location());
        diag << *type;
        diag << operand.sourceRange;
        return badExpr(compilation, result);
    }

    return *result;
}

bool UnaryExpression::propagateType(Compilation& compilation, const Type& newType) {
    switch (op) {
        case UnaryOperator::Plus:
        case UnaryOperator::Minus:
        case UnaryOperator::BitwiseNot:
            type = &newType;
            contextDetermined(compilation, operand_, newType);
            return true;
        case UnaryOperator::BitwiseAnd:
        case UnaryOperator::BitwiseOr:
        case UnaryOperator::BitwiseXor:
        case UnaryOperator::BitwiseNand:
        case UnaryOperator::BitwiseNor:
        case UnaryOperator::BitwiseXnor:
        case UnaryOperator::LogicalNot:
        case UnaryOperator::Preincrement:
        case UnaryOperator::Predecrement:
        case UnaryOperator::Postincrement:
        case UnaryOperator::Postdecrement:
            // Operand is self determined and already folded.
            return false;
    }
    THROW_UNREACHABLE;
}

void UnaryExpression::toJson(json& j) const {
    j["op"] = toString(op);
    j["operand"] = operand();
}

Expression& BinaryExpression::fromSyntax(Compilation& compilation,
                                         const BinaryExpressionSyntax& syntax,
                                         const BindContext& context) {
    Expression& lhs = create(compilation, *syntax.left, context);
    Expression& rhs = create(compilation, *syntax.right, context);
    const Type* lt = lhs.type;
    const Type* rt = rhs.type;

    auto result = compilation.emplace<BinaryExpression>(getBinaryOperator(syntax.kind), *lhs.type,
                                                        lhs, rhs, syntax.sourceRange());
    if (lhs.bad() || rhs.bad())
        return badExpr(compilation, result);

    bool bothIntegral = lt->isIntegral() && rt->isIntegral();
    bool bothNumeric = lt->isNumeric() && rt->isNumeric();
    bool bothStrings = lhs.isImplicitString() && rhs.isImplicitString();

    bool good;
    switch (syntax.kind) {
        case SyntaxKind::AddExpression:
        case SyntaxKind::SubtractExpression:
        case SyntaxKind::MultiplyExpression:
            good = bothNumeric;
            result->type = binaryOperatorType(compilation, lt, rt, false);
            break;
        case SyntaxKind::DivideExpression:
            // Result is forced to 4-state because result can be X.
            good = bothNumeric;
            result->type = binaryOperatorType(compilation, lt, rt, true);
            break;
        case SyntaxKind::ModExpression:
            // Result is forced to 4-state because result can be X.
            // Different from divide because only integers are allowed.
            good = bothIntegral;
            result->type = binaryOperatorType(compilation, lt, rt, true);
            break;
        case SyntaxKind::BinaryAndExpression:
        case SyntaxKind::BinaryOrExpression:
        case SyntaxKind::BinaryXorExpression:
        case SyntaxKind::BinaryXnorExpression:
            good = bothIntegral;
            result->type = binaryOperatorType(compilation, lt, rt, false);
            break;
        case SyntaxKind::LogicalShiftLeftExpression:
        case SyntaxKind::LogicalShiftRightExpression:
        case SyntaxKind::ArithmeticShiftLeftExpression:
        case SyntaxKind::ArithmeticShiftRightExpression:
            // The result is always the same type as the lhs, except that if the rhs is
            // four state then the lhs also becomes four state.
            good = bothIntegral;
            if (rt->isFourState())
                result->type = forceFourState(compilation, lt);
            else
                result->type = lt;
            selfDetermined(compilation, result->right_);
            break;
        case SyntaxKind::PowerExpression:
            good = bothNumeric;
            if (lt->isFloating() || rt->isFloating()) {
                result->type = binaryOperatorType(compilation, lt, rt, false);
                contextDetermined(compilation, result->right_, *result->type);
            }
            else {
                // Result is forced to 4-state because result can be X.
                result->type = forceFourState(compilation, lt);
                selfDetermined(compilation, result->right_);
            }
            break;
        case SyntaxKind::GreaterThanEqualExpression:
        case SyntaxKind::GreaterThanExpression:
        case SyntaxKind::LessThanEqualExpression:
        case SyntaxKind::LessThanExpression: {
            // Result is always a single bit.
            good = bothNumeric || bothStrings;
            result->type = singleBitType(compilation, lt, rt);

            // Result type is fixed but the two operands affect each other as they would in
            // other context-determined operators.
            auto nt = (good && !bothNumeric) ? &compilation.getStringType()
                                             : binaryOperatorType(compilation, lt, rt, false);
            contextDetermined(compilation, result->left_, *nt);
            contextDetermined(compilation, result->right_, *nt);
            break;
        }
        case SyntaxKind::LogicalAndExpression:
        case SyntaxKind::LogicalOrExpression:
        case SyntaxKind::LogicalImplicationExpression:
        case SyntaxKind::LogicalEquivalenceExpression:
            // Result is always a single bit.
            good = bothNumeric;
            result->type = singleBitType(compilation, lt, rt);
            selfDetermined(compilation, result->left_);
            selfDetermined(compilation, result->right_);
            break;
        case SyntaxKind::EqualityExpression:
        case SyntaxKind::InequalityExpression:
        case SyntaxKind::WildcardEqualityExpression:
        case SyntaxKind::WildcardInequalityExpression:
        case SyntaxKind::CaseEqualityExpression:
        case SyntaxKind::CaseInequalityExpression:
            // Two types are comparable if:
            // - they are both integral or floating
            // - both classes or null, and assignment compatible
            // - both chandles or null
            // - both aggregates and equivalent
            if (bothNumeric) {
                // For equality and inequality, result is four state if either operand is
                // four state. For case equality and case inequality, result is never four
                // state. For wildcard equality / inequality, result is four state only if
                // lhs is four state.
                if (syntax.kind == SyntaxKind::EqualityExpression ||
                    syntax.kind == SyntaxKind::InequalityExpression) {
                    good = true;
                    result->type = singleBitType(compilation, lt, rt);
                }
                else if (syntax.kind == SyntaxKind::CaseEqualityExpression ||
                         syntax.kind == SyntaxKind::CaseInequalityExpression) {
                    good = true;
                    result->type = &compilation.getBitType();
                }
                else {
                    good = bothIntegral;
                    result->type =
                        lt->isFourState() ? &compilation.getLogicType() : &compilation.getBitType();
                }

                // Result type is fixed but the two operands affect each other as they would
                // in other context-determined operators.
                auto nt = binaryOperatorType(compilation, lt, rt, false);
                contextDetermined(compilation, result->left_, *nt);
                contextDetermined(compilation, result->right_, *nt);
            }
            else {
                bool isContext = false;
                bool isWildcard = syntax.kind == SyntaxKind::WildcardEqualityExpression ||
                                  syntax.kind == SyntaxKind::WildcardInequalityExpression;

                if (bothStrings) {
                    good = !isWildcard;
                    result->type = &compilation.getBitType();

                    // If there is a literal involved, make sure it's converted to string.
                    isContext = true;
                    contextDetermined(compilation, result->left_, compilation.getStringType());
                    contextDetermined(compilation, result->right_, compilation.getStringType());
                }
                else if (lt->isAggregate() && lt->isEquivalent(*rt)) {
                    good = !isWildcard;
                    result->type = singleBitType(compilation, lt, rt);
                }
                else if ((lt->isClass() && lt->isAssignmentCompatible(*rt)) ||
                         (rt->isClass() && rt->isAssignmentCompatible(*lt))) {
                    good = true;
                    result->type = &compilation.getBitType();
                }
                else if ((lt->isCHandle() || lt->isNull()) && (rt->isCHandle() || rt->isNull())) {
                    good = true;
                    result->type = &compilation.getBitType();
                }
                else {
                    good = false;
                }

                if (!isContext) {
                    selfDetermined(compilation, result->left_);
                    selfDetermined(compilation, result->right_);
                }
            }
            break;
        default:
            THROW_UNREACHABLE;
    }

    auto location = syntax.operatorToken.location();
    if (!good) {
        auto& diag = context.addDiag(DiagCode::BadBinaryExpression, location);
        diag << *lt << *rt;
        diag << lhs.sourceRange;
        diag << rhs.sourceRange;
        return badExpr(compilation, result);
    }

    return *result;
}

bool BinaryExpression::propagateType(Compilation& compilation, const Type& newType) {
    switch (op) {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
        case BinaryOperator::Mod:
        case BinaryOperator::BinaryAnd:
        case BinaryOperator::BinaryOr:
        case BinaryOperator::BinaryXor:
        case BinaryOperator::BinaryXnor:
            type = &newType;
            contextDetermined(compilation, left_, newType);
            contextDetermined(compilation, right_, newType);
            return true;
        case BinaryOperator::Equality:
        case BinaryOperator::Inequality:
        case BinaryOperator::CaseEquality:
        case BinaryOperator::CaseInequality:
        case BinaryOperator::GreaterThanEqual:
        case BinaryOperator::GreaterThan:
        case BinaryOperator::LessThanEqual:
        case BinaryOperator::LessThan:
        case BinaryOperator::WildcardEquality:
        case BinaryOperator::WildcardInequality:
        case BinaryOperator::LogicalAnd:
        case BinaryOperator::LogicalOr:
        case BinaryOperator::LogicalImplication:
        case BinaryOperator::LogicalEquivalence:
            // Type is already set (always 1 bit) and operands are already folded.
            return false;
        case BinaryOperator::LogicalShiftLeft:
        case BinaryOperator::LogicalShiftRight:
        case BinaryOperator::ArithmeticShiftLeft:
        case BinaryOperator::ArithmeticShiftRight:
        case BinaryOperator::Power:
            // Only the left hand side gets propagated; the rhs is self determined.
            type = &newType;
            contextDetermined(compilation, left_, newType);
            return true;
    }
    THROW_UNREACHABLE;
}

void BinaryExpression::toJson(json& j) const {
    j["op"] = toString(op);
    j["left"] = left();
    j["right"] = right();
}

Expression& ConditionalExpression::fromSyntax(Compilation& compilation,
                                              const ConditionalExpressionSyntax& syntax,
                                              const BindContext& context) {
    // TODO: handle the pattern matching conditional predicate case, rather than just assuming
    // that it's a simple expression
    ASSERT(syntax.predicate->conditions.size() == 1);
    Expression& pred = selfDetermined(compilation, *syntax.predicate->conditions[0]->expr, context);
    Expression& left = create(compilation, *syntax.left, context);
    Expression& right = create(compilation, *syntax.right, context);

    // Force four-state return type for ambiguous condition case.
    const Type* resultType =
        binaryOperatorType(compilation, left.type, right.type, pred.type->isFourState());
    auto result = compilation.emplace<ConditionalExpression>(*resultType, pred, left, right,
                                                             syntax.sourceRange());
    if (pred.bad() || left.bad() || right.bad())
        return badExpr(compilation, result);

    if (!pred.type->isBooleanConvertible()) {
        context.addDiag(DiagCode::NotBooleanConvertible, pred.sourceRange) << *pred.type;
        return badExpr(compilation, result);
    }

    // If both sides of the expression are numeric, we've already determined the correct
    // result type. Otherwise, follow the rules in [11.14.11].
    bool good = true;
    const Type& lt = *left.type;
    const Type& rt = *right.type;
    if (!lt.isNumeric() || !rt.isNumeric()) {
        if (lt.isNull() && rt.isNull()) {
            result->type = &compilation.getNullType();
        }
        else if (lt.isClass() || rt.isClass()) {
            if (lt.isNull())
                result->type = &rt;
            else if (rt.isNull())
                result->type = &lt;
            else if (rt.isAssignmentCompatible(lt))
                result->type = &rt;
            else if (lt.isAssignmentCompatible(rt))
                result->type = &lt;
            // TODO: handle case for class types derived from common base
            else if (lt.isEquivalent(rt))
                result->type = &lt;
            else
                good = false;
        }
        else if (lt.isEquivalent(rt)) {
            result->type = &lt;
        }
        else if (left.isImplicitString() && right.isImplicitString()) {
            result->type = &compilation.getStringType();
        }
        else {
            good = false;
        }
    }

    if (!good) {
        auto& diag =
            context.addDiag(DiagCode::BadConditionalExpression, syntax.question.location());
        diag << lt << rt;
        diag << left.sourceRange;
        diag << right.sourceRange;
        return badExpr(compilation, result);
    }

    return *result;
}

bool ConditionalExpression::propagateType(Compilation& compilation, const Type& newType) {
    // The predicate is self determined so no need to handle it here.
    type = &newType;
    contextDetermined(compilation, left_, newType);
    contextDetermined(compilation, right_, newType);
    return true;
}

void ConditionalExpression::toJson(json& j) const {
    j["pred"] = pred();
    j["left"] = left();
    j["right"] = right();
}

Expression& AssignmentExpression::fromSyntax(Compilation& compilation,
                                             const BinaryExpressionSyntax& syntax,
                                             const BindContext& context) {
    Expression& lhs = selfDetermined(compilation, *syntax.left, context);
    Expression& rhs = create(compilation, *syntax.right, context);

    auto op = syntax.kind == SyntaxKind::AssignmentExpression
                  ? std::nullopt
                  : std::make_optional(getBinaryOperator(syntax.kind));

    // TODO: verify that assignment is allowed in this expression context
    auto result =
        compilation.emplace<AssignmentExpression>(op, *lhs.type, lhs, rhs, syntax.sourceRange());
    if (lhs.bad() || rhs.bad())
        return badExpr(compilation, result);

    // Make sure we can actually assign to the thing on the lhs.
    // TODO: check for const assignment
    auto location = syntax.operatorToken.location();
    if (!context.requireLValue(lhs, location))
        return badExpr(compilation, result);

    result->right_ =
        &convertAssignment(context.scope, *lhs.type, *result->right_, location, lhs.sourceRange);
    if (result->right_->bad())
        return badExpr(compilation, result);

    return *result;
}

void AssignmentExpression::toJson(json& j) const {
    j["left"] = left();
    j["right"] = right();

    if (op)
        j["op"] = toString(*op);
}

Expression& ElementSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                                const ExpressionSyntax& syntax,
                                                SourceRange fullRange, const BindContext& context) {
    Expression& selector = selfDetermined(compilation, syntax, context);
    const Type& resultType = getIndexedType(compilation, context, *value.type, syntax.sourceRange(),
                                            value.sourceRange, false);

    auto result =
        compilation.emplace<ElementSelectExpression>(resultType, value, selector, fullRange);
    if (value.bad() || selector.bad() || result->bad())
        return badExpr(compilation, result);

    if (!selector.type->isIntegral()) {
        context.addDiag(DiagCode::IndexMustBeIntegral, selector.sourceRange);
        return badExpr(compilation, result);
    }

    // If the selector is constant, we can do checking at compile time that it's within bounds.
    if (selector.constant) {
        optional<int32_t> index = selector.constant->integer().as<int32_t>();
        if (!index || !value.type->getArrayRange().containsPoint(*index)) {
            auto& diag = context.addDiag(DiagCode::IndexValueInvalid, selector.sourceRange);
            diag << *selector.constant;
            diag << *value.type;
            return badExpr(compilation, result);
        }
    }

    return *result;
}

void ElementSelectExpression::toJson(json& j) const {
    j["value"] = value();
    j["selector"] = selector();
}

Expression& RangeSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                              const RangeSelectSyntax& syntax,
                                              SourceRange fullRange, const BindContext& context) {
    // Left and right are either the extents of a part-select, in which case they must
    // both be constant, or the left hand side is the start and the right hand side is
    // the width of an indexed part select, in which case only the rhs need be constant.
    RangeSelectionKind selectionKind;
    switch (syntax.kind) {
        case SyntaxKind::SimpleRangeSelect:
            selectionKind = RangeSelectionKind::Simple;
            break;
        case SyntaxKind::AscendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedUp;
            break;
        case SyntaxKind::DescendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedDown;
            break;
        default:
            THROW_UNREACHABLE;
    }

    const Expression& left = selectionKind == RangeSelectionKind::Simple
                                 ? bind(*syntax.left, context, BindFlags::Constant)
                                 : selfDetermined(compilation, *syntax.left, context);

    const Expression& right = bind(*syntax.right, context, BindFlags::Constant);

    auto result = compilation.emplace<RangeSelectExpression>(
        selectionKind, compilation.getErrorType(), value, left, right, fullRange);

    if (value.bad() || left.bad() || right.bad())
        return badExpr(compilation, result);

    if (!left.type->isIntegral()) {
        context.addDiag(DiagCode::IndexMustBeIntegral, left.sourceRange);
        return badExpr(compilation, result);
    }
    if (!right.type->isIntegral()) {
        context.addDiag(DiagCode::IndexMustBeIntegral, right.sourceRange);
        return badExpr(compilation, result);
    }

    const Type& elementType = getIndexedType(compilation, context, *value.type,
                                             syntax.sourceRange(), value.sourceRange, true);
    if (elementType.isError())
        return badExpr(compilation, result);

    // As mentioned, rhs must always be a constant integer.
    optional<int32_t> rv = context.evalInteger(right);
    if (!rv)
        return badExpr(compilation, result);

    ConstantRange selectionRange;
    ConstantRange valueRange = value.type->getArrayRange();
    SourceRange errorRange{ left.sourceRange.start(), right.sourceRange.end() };

    // Helper function for validating the bounds of the selection.
    auto validateRange = [&](auto range) {
        if (!valueRange.containsPoint(range.left) || !valueRange.containsPoint(range.right)) {
            auto& diag = context.addDiag(DiagCode::BadRangeExpression, errorRange);
            diag << range.left << range.right;
            diag << *value.type;
            return false;
        }
        return true;
    };

    if (selectionKind == RangeSelectionKind::Simple) {
        optional<int32_t> lv = context.evalInteger(left);
        if (!lv)
            return badExpr(compilation, result);

        selectionRange = { *lv, *rv };
        if (selectionRange.isLittleEndian() != valueRange.isLittleEndian()) {
            auto& diag = context.addDiag(DiagCode::SelectEndianMismatch, errorRange);
            diag << *value.type;
            return badExpr(compilation, result);
        }

        if (!validateRange(selectionRange))
            return badExpr(compilation, result);
    }
    else {
        if (!context.requireGtZero(rv, right.sourceRange))
            return badExpr(compilation, result);

        if (bitwidth_t(*rv) > valueRange.width()) {
            auto& diag = context.addDiag(DiagCode::RangeWidthTooLarge, right.sourceRange);
            diag << *rv;
            diag << *value.type;
            return badExpr(compilation, result);
        }

        // If the lhs is a known constant, we can check that now too.
        if (left.constant) {
            optional<int32_t> index = left.constant->integer().as<int32_t>();
            if (!index) {
                auto& diag = context.addDiag(DiagCode::IndexValueInvalid, left.sourceRange);
                diag << *left.constant;
                diag << *value.type;
                return badExpr(compilation, result);
            }

            selectionRange =
                getIndexedRange(selectionKind, *index, *rv, valueRange.isLittleEndian());

            if (!validateRange(selectionRange))
                return badExpr(compilation, result);
        }
        else {
            // Otherwise, the resulting range will start with the fixed lower bound of the type.
            int32_t count = *rv - 1;
            if (selectionKind == RangeSelectionKind::IndexedUp) {
                selectionRange.left = valueRange.lower() + count;
                selectionRange.right = valueRange.lower();
            }
            else {
                selectionRange.left = valueRange.upper();
                selectionRange.right = valueRange.upper() - count;
            }

            if (!valueRange.isLittleEndian())
                selectionRange.reverse();
        }
    }

    // At this point, all expressions are good, ranges have been validated and
    // we know the final width of the selection, so pick the result type and we're done.
    if (value.type->isUnpackedArray())
        result->type = compilation.emplace<UnpackedArrayType>(elementType, selectionRange);
    else
        result->type = compilation.emplace<PackedArrayType>(elementType, selectionRange);

    return *result;
}

ConstantRange RangeSelectExpression::getIndexedRange(RangeSelectionKind kind, int32_t l, int32_t r,
                                                     bool littleEndian) {
    // TODO: avoid overflow
    ConstantRange result;
    int32_t count = r - 1;
    if (kind == RangeSelectionKind::IndexedUp) {
        result.left = l + count;
        result.right = l;
    }
    else {
        result.left = l;
        result.right = l - count;
    }

    if (!littleEndian)
        result.reverse();

    return result;
}

void RangeSelectExpression::toJson(json& j) const {
    j["selectionKind"] = toString(selectionKind);
    j["value"] = value();
    j["left"] = left();
    j["right"] = right();
}

Expression& MemberAccessExpression::fromSelector(Compilation& compilation, Expression& expr,
                                                 const LookupResult::MemberSelector& selector,
                                                 const BindContext& context) {

    const Symbol* member = expr.type->getCanonicalType().as<Scope>().find(selector.name);
    if (!member) {
        auto& diag = context.addDiag(DiagCode::UnknownMember, selector.nameRange.start());
        diag << expr.sourceRange;
        diag << selector.name;
        diag << *expr.type;
        return badExpr(compilation, &expr);
    }

    // The source range of the entire member access starts from the value being selected.
    SourceRange range{ expr.sourceRange.start(), selector.nameRange.end() };
    const auto& field = member->as<FieldSymbol>();
    return *compilation.emplace<MemberAccessExpression>(field.getType(), expr, field, range);
}

void MemberAccessExpression::toJson(json& j) const {
    j["field"] = Symbol::jsonLink(field);
    j["value"] = value();
}

Expression& ConcatenationExpression::fromSyntax(Compilation& compilation,
                                                const ConcatenationExpressionSyntax& syntax,
                                                const BindContext& context) {
    bool errored = false;
    bool anyStrings = false;
    bitmask<IntegralFlags> flags;
    bitwidth_t totalWidth = 0;
    SmallVectorSized<Expression*, 8> buffer;

    for (auto argSyntax : syntax.expressions) {
        // Replications inside of concatenations have a special feature that allows them to have
        // a width of zero. Check now if we're going to be binding a replication and if so set
        // an additional flag so that it knows it's ok to have that zero count.
        Expression* arg;
        if (argSyntax->kind == SyntaxKind::MultipleConcatenationExpression)
            arg = &create(compilation, *argSyntax, context, BindFlags::InsideConcatenation);
        else
            arg = &create(compilation, *argSyntax, context);
        buffer.append(arg);

        if (arg->bad()) {
            errored = true;
            break;
        }

        const Type& type = *arg->type;
        if (type.isVoid() && argSyntax->kind == SyntaxKind::MultipleConcatenationExpression)
            continue;

        if (type.isString()) {
            anyStrings = true;
            continue;
        }

        if (!type.isIntegral()) {
            errored = true;
            context.addDiag(DiagCode::BadConcatExpression, arg->sourceRange) << type;
            break;
        }

        // Can't overflow because 2*maxWidth is still less than 2^32-1.
        totalWidth += type.getBitWidth();

        if (!context.requireValidBitWidth(totalWidth, syntax.sourceRange())) {
            errored = true;
            break;
        }

        if (type.isFourState())
            flags |= IntegralFlags::FourState;
    }

    if (!errored) {
        for (uint32_t i = 0; i < buffer.size(); i++) {
            if (!anyStrings)
                selfDetermined(compilation, buffer[i]);
            else {
                Expression* expr = buffer[i];
                if (expr->type->isString())
                    selfDetermined(compilation, expr);
                else if (expr->isImplicitString())
                    contextDetermined(compilation, expr, compilation.getStringType());
                else {
                    errored = true;
                    context.addDiag(DiagCode::ConcatWithStringInt, expr->sourceRange);
                    break;
                }
                buffer[i] = expr;
            }
        }
    }

    if (errored) {
        return badExpr(compilation, compilation.emplace<ConcatenationExpression>(
                                        compilation.getErrorType(), span<const Expression*>(),
                                        syntax.sourceRange()));
    }

    const Type* type;
    if (anyStrings)
        type = &compilation.getStringType();
    else
        type = &compilation.getType(totalWidth, flags);

    auto copied = buffer.copy(compilation);
    span<const Expression* const> elements(copied.data(), copied.size());
    return *compilation.emplace<ConcatenationExpression>(*type, elements, syntax.sourceRange());
}

void ConcatenationExpression::toJson(json& j) const {
    for (auto op : operands())
        j["operands"].push_back(*op);
}

Expression& ReplicationExpression::fromSyntax(Compilation& compilation,
                                              const MultipleConcatenationExpressionSyntax& syntax,
                                              const BindContext& context) {
    Expression& left = selfDetermined(compilation, *syntax.expression, context);
    Expression* right = &create(compilation, *syntax.concatenation, context);

    auto result = compilation.emplace<ReplicationExpression>(compilation.getErrorType(), left,
                                                             *right, syntax.sourceRange());
    if (left.bad() || right->bad())
        return badExpr(compilation, result);

    // TODO: unpacked arrays
    if (!left.type->isIntegral() || (!right->type->isIntegral() && !right->type->isString())) {
        auto& diag = context.addDiag(DiagCode::BadReplicationExpression,
                                     syntax.concatenation->getFirstToken().location());
        diag << *left.type << *right->type;
        diag << left.sourceRange;
        diag << right->sourceRange;
        return badExpr(compilation, result);
    }

    // If the multiplier isn't constant this must be a string replication.
    if (!left.constant) {
        if (!right->isImplicitString()) {
            // They probably meant for this to be a constant (non-string) replication,
            // so do the normal error reporting for that case.
            checkBindFlags(left, context.resetFlags(BindFlags::Constant));
            return badExpr(compilation, result);
        }

        contextDetermined(compilation, right, compilation.getStringType());

        result->concat_ = right;
        result->type = &compilation.getStringType();
        return *result;
    }

    optional<int32_t> count = context.evalInteger(left);
    if (!count)
        return badExpr(compilation, result);

    if (*count < 0) {
        context.requireGtZero(count, left.sourceRange);
        return badExpr(compilation, result);
    }

    if (*count == 0) {
        if ((context.flags & BindFlags::InsideConcatenation) == 0) {
            context.addDiag(DiagCode::ReplicationZeroOutsideConcat, left.sourceRange);
            return badExpr(compilation, result);
        }

        // Use a placeholder type here to indicate to the parent concatenation that this had a
        // zero width.
        result->type = &compilation.getVoidType();
        return *result;
    }

    selfDetermined(compilation, right);
    result->concat_ = right;

    if (right->type->isString()) {
        result->type = &compilation.getStringType();
        return *result;
    }

    auto width = context.requireValidBitWidth(
        SVInt(32, uint64_t(*count), true) * right->type->getBitWidth(), syntax.sourceRange());
    if (!width)
        return badExpr(compilation, result);

    result->type = &compilation.getType(
        *width, right->type->isFourState() ? IntegralFlags::FourState : IntegralFlags::TwoState);
    return *result;
}

void ReplicationExpression::toJson(json& j) const {
    j["count"] = count();
    j["concat"] = concat();
}

Expression& CallExpression::fromSyntax(Compilation& compilation,
                                       const InvocationExpressionSyntax& syntax,
                                       const BindContext& context) {
    // TODO: once classes are supported, member access needs to work here
    if (!NameSyntax::isKind(syntax.left->kind)) {
        SourceLocation loc = syntax.arguments ? syntax.arguments->openParen.location()
                                              : syntax.left->getFirstToken().location();
        auto& diag = context.addDiag(DiagCode::ExpressionNotCallable, loc);
        diag << syntax.left->sourceRange();
        return badExpr(compilation, nullptr);
    }

    return bindName(compilation, syntax.left->as<NameSyntax>(), &syntax, context);
}

Expression& CallExpression::fromLookup(Compilation& compilation, const Subroutine& subroutine,
                                       const InvocationExpressionSyntax* syntax, SourceRange range,
                                       const BindContext& context) {

    if (subroutine.index() == 1) {
        const SystemSubroutine& systemSubroutine = *std::get<1>(subroutine);
        return createSystemCall(compilation, systemSubroutine, nullptr, syntax, range, context);
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);

    SmallVectorSized<const Expression*, 8> argBuffer;
    if (syntax && syntax->arguments) {
        auto actualArgs = syntax->arguments->parameters;

        // TODO: handle too few args as well, which requires looking at default values
        auto formalArgs = symbol.arguments;
        if (formalArgs.size() < actualArgs.size()) {
            auto& diag = context.addDiag(DiagCode::TooManyArguments, syntax->left->sourceRange());
            diag << formalArgs.size();
            diag << actualArgs.size();
            return badExpr(compilation, nullptr);
        }

        // TODO: handle named arguments in addition to ordered
        for (uint32_t i = 0; i < actualArgs.size(); i++) {
            const auto& arg = actualArgs[i]->as<OrderedArgumentSyntax>();
            argBuffer.append(&Expression::bind(formalArgs[i]->getType(), *arg.expr,
                                               arg.getFirstToken().location(), context));
        }
    }

    return *compilation.emplace<CallExpression>(&symbol, symbol.getReturnType(),
                                                argBuffer.copy(compilation), context.lookupLocation,
                                                range);
}

Expression& CallExpression::fromSystemMethod(Compilation& compilation, const Expression& expr,
                                             const LookupResult::MemberSelector& selector,
                                             const InvocationExpressionSyntax* syntax,
                                             const BindContext& context) {
    const Type& type = expr.type->getCanonicalType();
    auto subroutine = compilation.getSystemMethod(type.kind, selector.name);
    if (!subroutine) {
        if (syntax) {
            context.addDiag(DiagCode::UnknownSystemMethod, selector.nameRange) << selector.name;
        }
        else {
            auto& diag = context.addDiag(DiagCode::InvalidMemberAccess, selector.dotLocation);
            diag << expr.sourceRange;
            diag << selector.nameRange;
            diag << *expr.type;
        }
        return badExpr(compilation, &expr);
    }

    return createSystemCall(compilation, *subroutine, &expr, syntax,
                            syntax ? syntax->sourceRange() : expr.sourceRange, context);
}

Expression& CallExpression::createSystemCall(Compilation& compilation,
                                             const SystemSubroutine& subroutine,
                                             const Expression* firstArg,
                                             const InvocationExpressionSyntax* syntax,
                                             SourceRange range, const BindContext& context) {
    SmallVectorSized<const Expression*, 8> buffer;
    if (firstArg)
        buffer.append(firstArg);

    if (syntax && syntax->arguments) {
        auto actualArgs = syntax->arguments->parameters;
        for (uint32_t i = 0; i < actualArgs.size(); i++) {
            // TODO: error if not ordered arguments
            const auto& arg = actualArgs[i]->as<OrderedArgumentSyntax>();
            BindFlags extra = BindFlags::None;
            if (i == 0 && (subroutine.flags & SystemSubroutineFlags::AllowDataTypeArg) != 0)
                extra = BindFlags::AllowDataType;

            buffer.append(&selfDetermined(compilation, *arg.expr, context, extra));
        }
    }

    const Type& type = subroutine.checkArguments(compilation, buffer);
    auto expr = compilation.emplace<CallExpression>(&subroutine, type, buffer.copy(compilation),
                                                    context.lookupLocation, range);

    if (type.isError())
        return badExpr(compilation, expr);

    for (auto arg : expr->arguments()) {
        if (arg->bad())
            return badExpr(compilation, expr);
    }

    return *expr;
}

void CallExpression::toJson(json& j) const {
    if (subroutine.index() == 1) {
        const SystemSubroutine& systemSubroutine = *std::get<1>(subroutine);
        j["subroutine"] = systemSubroutine.name;
    }
    else {
        const SubroutineSymbol& symbol = *std::get<0>(subroutine);
        j["subroutine"] = Symbol::jsonLink(symbol);

        for (auto arg : arguments())
            j["arguments"].push_back(*arg);
    }
}

Expression& ConversionExpression::fromSyntax(Compilation& compilation,
                                             const CastExpressionSyntax& syntax,
                                             const BindContext& context) {
    auto& targetExpr = bind(*syntax.left, context, BindFlags::AllowDataType | BindFlags::Constant);
    auto& operand = selfDetermined(compilation, *syntax.right, context);

    auto result = compilation.emplace<ConversionExpression>(compilation.getErrorType(), operand,
                                                            syntax.sourceRange());
    if (targetExpr.bad() || operand.bad())
        return badExpr(compilation, result);

    if (targetExpr.kind == ExpressionKind::DataType) {
        result->type = targetExpr.type;
    }
    else if (!targetExpr.constant ||
             !context.requireIntegral(*targetExpr.constant, targetExpr.sourceRange)) {
        return badExpr(compilation, result);
    }
    else {
        // TODO: check for zero
        const SVInt& value = targetExpr.constant->integer();
        if (!context.requireNoUnknowns(value, targetExpr.sourceRange) ||
            !context.requirePositive(value, targetExpr.sourceRange)) {
            return badExpr(compilation, result);
        }

        auto width = context.requireValidBitWidth(value, targetExpr.sourceRange);
        if (!width)
            return badExpr(compilation, result);

        // TODO: require integer expression
        result->type = &compilation.getType(*width, operand.type->getIntegralFlags());
    }

    // TODO: make sure cast compatible
    return *result;
}

void ConversionExpression::toJson(json& j) const {
    j["operand"] = operand();
}

Expression& DataTypeExpression::fromSyntax(Compilation& compilation, const DataTypeSyntax& syntax,
                                           const BindContext& context) {
    if ((context.flags & BindFlags::AllowDataType) == 0) {
        context.addDiag(DiagCode::ExpectedExpression, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    const Type& type = compilation.getType(syntax, context.lookupLocation, context.scope);
    return *compilation.emplace<DataTypeExpression>(type, syntax.sourceRange());
}

UnaryOperator getUnaryOperator(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::UnaryPlusExpression:
            return UnaryOperator::Plus;
        case SyntaxKind::UnaryMinusExpression:
            return UnaryOperator::Minus;
        case SyntaxKind::UnaryBitwiseNotExpression:
            return UnaryOperator::BitwiseNot;
        case SyntaxKind::UnaryBitwiseAndExpression:
            return UnaryOperator::BitwiseAnd;
        case SyntaxKind::UnaryBitwiseOrExpression:
            return UnaryOperator::BitwiseOr;
        case SyntaxKind::UnaryBitwiseXorExpression:
            return UnaryOperator::BitwiseXor;
        case SyntaxKind::UnaryBitwiseNandExpression:
            return UnaryOperator::BitwiseNand;
        case SyntaxKind::UnaryBitwiseNorExpression:
            return UnaryOperator::BitwiseNor;
        case SyntaxKind::UnaryBitwiseXnorExpression:
            return UnaryOperator::BitwiseXnor;
        case SyntaxKind::UnaryLogicalNotExpression:
            return UnaryOperator::LogicalNot;
        case SyntaxKind::UnaryPreincrementExpression:
            return UnaryOperator::Preincrement;
        case SyntaxKind::UnaryPredecrementExpression:
            return UnaryOperator::Predecrement;
        case SyntaxKind::PostincrementExpression:
            return UnaryOperator::Postincrement;
        case SyntaxKind::PostdecrementExpression:
            return UnaryOperator::Postdecrement;
        default:
            THROW_UNREACHABLE;
    }
}

BinaryOperator getBinaryOperator(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::AddExpression:
            return BinaryOperator::Add;
        case SyntaxKind::SubtractExpression:
            return BinaryOperator::Subtract;
        case SyntaxKind::MultiplyExpression:
            return BinaryOperator::Multiply;
        case SyntaxKind::DivideExpression:
            return BinaryOperator::Divide;
        case SyntaxKind::ModExpression:
            return BinaryOperator::Mod;
        case SyntaxKind::BinaryAndExpression:
            return BinaryOperator::BinaryAnd;
        case SyntaxKind::BinaryOrExpression:
            return BinaryOperator::BinaryOr;
        case SyntaxKind::BinaryXorExpression:
            return BinaryOperator::BinaryXor;
        case SyntaxKind::BinaryXnorExpression:
            return BinaryOperator::BinaryXnor;
        case SyntaxKind::EqualityExpression:
            return BinaryOperator::Equality;
        case SyntaxKind::InequalityExpression:
            return BinaryOperator::Inequality;
        case SyntaxKind::CaseEqualityExpression:
            return BinaryOperator::CaseEquality;
        case SyntaxKind::CaseInequalityExpression:
            return BinaryOperator::CaseInequality;
        case SyntaxKind::GreaterThanEqualExpression:
            return BinaryOperator::GreaterThanEqual;
        case SyntaxKind::GreaterThanExpression:
            return BinaryOperator::GreaterThan;
        case SyntaxKind::LessThanEqualExpression:
            return BinaryOperator::LessThanEqual;
        case SyntaxKind::LessThanExpression:
            return BinaryOperator::LessThan;
        case SyntaxKind::WildcardEqualityExpression:
            return BinaryOperator::WildcardEquality;
        case SyntaxKind::WildcardInequalityExpression:
            return BinaryOperator::WildcardInequality;
        case SyntaxKind::LogicalAndExpression:
            return BinaryOperator::LogicalAnd;
        case SyntaxKind::LogicalOrExpression:
            return BinaryOperator::LogicalOr;
        case SyntaxKind::LogicalImplicationExpression:
            return BinaryOperator::LogicalImplication;
        case SyntaxKind::LogicalEquivalenceExpression:
            return BinaryOperator::LogicalEquivalence;
        case SyntaxKind::LogicalShiftLeftExpression:
            return BinaryOperator::LogicalShiftLeft;
        case SyntaxKind::LogicalShiftRightExpression:
            return BinaryOperator::LogicalShiftRight;
        case SyntaxKind::ArithmeticShiftLeftExpression:
            return BinaryOperator::ArithmeticShiftLeft;
        case SyntaxKind::ArithmeticShiftRightExpression:
            return BinaryOperator::ArithmeticShiftRight;
        case SyntaxKind::PowerExpression:
            return BinaryOperator::Power;
        case SyntaxKind::AddAssignmentExpression:
            return BinaryOperator::Add;
        case SyntaxKind::SubtractAssignmentExpression:
            return BinaryOperator::Subtract;
        case SyntaxKind::MultiplyAssignmentExpression:
            return BinaryOperator::Multiply;
        case SyntaxKind::DivideAssignmentExpression:
            return BinaryOperator::Divide;
        case SyntaxKind::ModAssignmentExpression:
            return BinaryOperator::Mod;
        case SyntaxKind::AndAssignmentExpression:
            return BinaryOperator::BinaryAnd;
        case SyntaxKind::OrAssignmentExpression:
            return BinaryOperator::BinaryOr;
        case SyntaxKind::XorAssignmentExpression:
            return BinaryOperator::BinaryXor;
        case SyntaxKind::LogicalLeftShiftAssignmentExpression:
            return BinaryOperator::LogicalShiftLeft;
        case SyntaxKind::LogicalRightShiftAssignmentExpression:
            return BinaryOperator::LogicalShiftRight;
        case SyntaxKind::ArithmeticLeftShiftAssignmentExpression:
            return BinaryOperator::ArithmeticShiftLeft;
        case SyntaxKind::ArithmeticRightShiftAssignmentExpression:
            return BinaryOperator::ArithmeticShiftRight;
        default:
            THROW_UNREACHABLE;
    }
}

} // namespace slang
