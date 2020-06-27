//------------------------------------------------------------------------------
// SystemSubroutine.cpp
// System-defined subroutine handling
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/binding/SystemSubroutine.h"

#include "slang/binding/BindContext.h"
#include "slang/binding/Expression.h"
#include "slang/binding/LiteralExpressions.h"
#include "slang/compilation/Compilation.h"
#include "slang/compilation/SFormat.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/SysFuncsDiags.h"
#include "slang/mir/Procedure.h"
#include "slang/syntax/AllSyntax.h"

namespace slang {

bool SystemSubroutine::allowEmptyArgument(size_t) const {
    return false;
}

const Expression& SystemSubroutine::bindArgument(size_t, const BindContext& context,
                                                 const ExpressionSyntax& syntax) const {
    return Expression::bind(syntax, context);
}

string_view SystemSubroutine::kindStr() const {
    return kind == SubroutineKind::Task ? "task"sv : "function"sv;
}

bool SystemSubroutine::checkArgCount(const BindContext& context, bool isMethod, const Args& args,
                                     SourceRange callRange, size_t min, size_t max) {
    size_t provided = args.size();
    if (isMethod) {
        ASSERT(provided);
        provided--;
    }

    if (provided < min) {
        context.addDiag(diag::TooFewArguments, callRange) << min << provided;
        return false;
    }

    if (provided > max) {
        context.addDiag(diag::TooManyArguments, args[max]->sourceRange) << max << provided;
        return false;
    }

    for (auto arg : args) {
        if (arg->bad())
            return false;
    }

    return true;
}

bool SystemSubroutine::checkFormatArgs(const BindContext& context, const Args& args) {
    SmallVectorSized<SFormat::Arg, 8> specs;
    auto specIt = specs.begin();

    auto argIt = args.begin();
    while (argIt != args.end()) {
        auto arg = *argIt++;
        if (arg->kind == ExpressionKind::EmptyArgument) {
            // Empty arguments are ok as long as we aren't processing a format string.
            if (specIt == specs.end())
                continue;

            SFormat::Arg fmtArg = *specIt++;
            context.addDiag(diag::FormatEmptyArg, arg->sourceRange) << fmtArg.spec << fmtArg.range;
            return false;
        }

        if (arg->bad())
            return false;

        const Type& type = *arg->type;
        if (specIt == specs.end()) {
            if (arg->kind == ExpressionKind::StringLiteral) {
                specs.clear();
                auto& lit = arg->as<StringLiteral>();

                // We need to use the raw value here so that we can accurately
                // report errors for specific format specifiers within the string.
                string_view fmt = lit.getRawValue();
                if (fmt.length() >= 2)
                    fmt = fmt.substr(1, fmt.length() - 2);

                Diagnostics diags;
                if (!SFormat::parseArgs(fmt, arg->sourceRange.start() + 1, specs, diags)) {
                    context.scope.addDiags(diags);
                    return false;
                }

                specIt = specs.begin();
            }
            else if (type.isAggregate() && !type.isByteArray()) {
                context.addDiag(diag::FormatUnspecifiedType, arg->sourceRange) << type;
                return false;
            }
        }
        else {
            SFormat::Arg fmtArg = *specIt++;
            if (!SFormat::isArgTypeValid(fmtArg.type, type)) {
                if (SFormat::isRealToInt(fmtArg.type, type)) {
                    context.addDiag(diag::FormatRealInt, arg->sourceRange)
                        << fmtArg.spec << fmtArg.range;
                }
                else {
                    context.addDiag(diag::FormatMismatchedType, arg->sourceRange)
                        << type << fmtArg.spec << fmtArg.range;
                    return false;
                }
            }
        }
    }

    bool ok = true;
    while (specIt != specs.end()) {
        SFormat::Arg fmtArg = *specIt++;
        context.addDiag(diag::FormatNoArgument, fmtArg.range) << fmtArg.spec;
        ok = false;
    }

    return ok;
}

bool SystemSubroutine::checkFormatValues(const BindContext& context, const Args& args) {
    // If the format string is known at compile time, check it for correctness now.
    // Otherwise this will wait until runtime.
    if (args[0]->kind != ExpressionKind::StringLiteral)
        return true;

    // We need to use the raw value here so that we can accurately
    // report errors for specific format specifiers within the string.
    auto& lit = args[0]->as<StringLiteral>();
    string_view fmt = lit.getRawValue();
    if (fmt.length() >= 2)
        fmt = fmt.substr(1, fmt.length() - 2);

    Diagnostics diags;
    SmallVectorSized<SFormat::Arg, 8> specs;
    if (!SFormat::parseArgs(fmt, args[0]->sourceRange.start() + 1, specs, diags)) {
        context.scope.addDiags(diags);
        return false;
    }

    bool ok = true;
    size_t argIndex = 1;
    for (auto& fmtArg : specs) {
        if (argIndex >= args.size()) {
            context.addDiag(diag::FormatNoArgument, fmtArg.range) << fmtArg.spec;
            ok = false;
            continue;
        }

        const Type& type = *args[argIndex]->type;
        SourceRange range = args[argIndex]->sourceRange;
        argIndex++;

        if (!SFormat::isArgTypeValid(fmtArg.type, type)) {
            if (SFormat::isRealToInt(fmtArg.type, type)) {
                context.addDiag(diag::FormatRealInt, range) << fmtArg.spec << fmtArg.range;
            }
            else {
                context.addDiag(diag::FormatMismatchedType, range)
                    << type << fmtArg.spec << fmtArg.range;
                ok = false;
            }
        }
    }

    if (argIndex < args.size()) {
        context.addDiag(diag::FormatTooManyArgs, args[argIndex]->sourceRange);
        ok = false;
    }

    return ok;
}

static void lowerFormatArg(mir::Procedure& proc, const Expression& arg, char,
                           const SFormat::FormatOptions&) {
    // TODO: actually use the options
    mir::MIRValue argVal = proc.emitExpr(arg);
    const Type& type = arg.type->getCanonicalType();
    if (type.isIntegral()) {
        proc.emitCall(mir::SysCallKind::printInt, argVal);
        return;
    }

    switch (type.kind) {
        case SymbolKind::FloatingType:
        case SymbolKind::StringType:
        case SymbolKind::EventType:
        case SymbolKind::CHandleType:
        case SymbolKind::ClassType:
        case SymbolKind::NullType:
            THROW_UNREACHABLE;
        default:
            // Should only be reachable by invalid display calls,
            // in which case an error will already have been reported.
            break;
    }
}

void SystemSubroutine::lowerFormatArgs(mir::Procedure& proc, const Args& args, LiteralBase) {
    auto argIt = args.begin();
    while (argIt != args.end()) {
        auto arg = *argIt++;
        if (arg->bad())
            return;

        // Empty arguments always print a space.
        if (arg->kind == ExpressionKind::EmptyArgument) {
            proc.emitCall(mir::SysCallKind::printChar, proc.emitInt(8, ' ', false));
            continue;
        }

        // Handle string literals as format strings.
        if (arg->kind == ExpressionKind::StringLiteral) {
            // Strip quotes from the raw string.
            auto& lit = arg->as<StringLiteral>();
            string_view fmt = lit.getRawValue();
            if (fmt.length() >= 2)
                fmt = fmt.substr(1, fmt.length() - 2);

            bool result = SFormat::splitFormatString(
                fmt,
                [&](string_view text) {
                    proc.emitCall(mir::SysCallKind::printStringLit,
                                  proc.emitString(std::string(text)));
                },
                [&](char specifier, const SFormat::FormatOptions& options) {
                    if (argIt != args.end()) {
                        auto currentArg = *argIt++;
                        lowerFormatArg(proc, *currentArg, specifier, options);
                    }
                });

            if (!result)
                return;
        }
        else {
            // Otherwise, print the value with default options.
            // TODO: set correct specifier
            lowerFormatArg(proc, *arg, ' ', {});
        }
    }
}

const Type& SystemSubroutine::badArg(const BindContext& context, const Expression& arg) const {
    context.addDiag(diag::BadSystemSubroutineArg, arg.sourceRange) << *arg.type << kindStr();
    return context.getCompilation().getErrorType();
}

BindContext SystemSubroutine::makeNonConst(const BindContext& ctx) {
    BindContext nonConstCtx(ctx);
    if (nonConstCtx.flags & BindFlags::Constant) {
        nonConstCtx.flags &= ~BindFlags::Constant;
        nonConstCtx.flags |= BindFlags::NoHierarchicalNames;
    }
    return nonConstCtx;
}

const Expression& SimpleSystemSubroutine::bindArgument(size_t argIndex, const BindContext& context,
                                                       const ExpressionSyntax& syntax) const {
    optional<BindContext> nonConstCtx;
    const BindContext* ctx = &context;
    if (allowNonConst) {
        nonConstCtx.emplace(makeNonConst(context));
        ctx = &nonConstCtx.value();
    }

    if (argIndex >= argTypes.size())
        return SystemSubroutine::bindArgument(argIndex, *ctx, syntax);

    return Expression::bindRValue(*argTypes[argIndex], syntax, syntax.getFirstToken().location(),
                                  *ctx);
}

const Type& SimpleSystemSubroutine::checkArguments(const BindContext& context, const Args& args,
                                                   SourceRange range) const {
    auto& comp = context.getCompilation();
    if (!checkArgCount(context, isMethod, args, range, requiredArgs, argTypes.size()))
        return comp.getErrorType();

    return *returnType;
}

} // namespace slang