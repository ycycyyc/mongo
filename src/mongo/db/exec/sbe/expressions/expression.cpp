/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/expressions/expression.h"

#include <iomanip>
#include <sstream>

#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
/**
 * This function generates bytecode for testing whether the top of the stack is Nothing. If it is
 * not Nothing then code generated by the 'generator' parameter is executed otherwise it is skipped.
 * The test is appended to the 'code' parameter.
 */
template <typename F>
std::unique_ptr<vm::CodeFragment> wrapNothingTest(std::unique_ptr<vm::CodeFragment> code,
                                                  F&& generator) {
    auto inner = std::make_unique<vm::CodeFragment>();
    inner = generator(std::move(inner));

    invariant(inner->stackSize() == 0);

    // Append the jump that skips around the inner block.
    code->appendJumpNothing(inner->instrs().size());

    code->append(std::move(inner));

    return code;
}

std::unique_ptr<EExpression> EConstant::clone() const {
    auto [tag, val] = value::copyValue(_tag, _val);
    return std::make_unique<EConstant>(tag, val);
}

std::unique_ptr<vm::CodeFragment> EConstant::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    code->appendConstVal(_tag, _val);

    return code;
}

std::vector<DebugPrinter::Block> EConstant::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    std::stringstream ss;
    ss << std::make_pair(_tag, _val);

    ret.emplace_back(ss.str());

    return ret;
}

std::unique_ptr<EExpression> EVariable::clone() const {
    return _frameId ? std::make_unique<EVariable>(*_frameId, _var)
                    : std::make_unique<EVariable>(_var);
}

std::unique_ptr<vm::CodeFragment> EVariable::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    if (_frameId) {
        int offset = -_var - 1;
        code->appendLocalVal(*_frameId, offset, _moveFrom);
    } else {
        auto accessor = ctx.root->getAccessor(ctx, _var);
        if (_moveFrom) {
            code->appendMoveVal(accessor);
        } else {
            code->appendAccessVal(accessor);
        }
    }

    return code;
}

std::vector<DebugPrinter::Block> EVariable::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    if (_frameId) {
        DebugPrinter::addIdentifier(ret, *_frameId, _var);
    } else {
        DebugPrinter::addIdentifier(ret, _var);
    }

    return ret;
}

std::unique_ptr<EExpression> EPrimBinary::clone() const {
    if (_nodes.size() == 2) {
        return std::make_unique<EPrimBinary>(_op, _nodes[0]->clone(), _nodes[1]->clone());
    } else {
        invariant(_nodes.size() == 3);
        return std::make_unique<EPrimBinary>(
            _op, _nodes[0]->clone(), _nodes[1]->clone(), _nodes[2]->clone());
    }
}

std::unique_ptr<vm::CodeFragment> EPrimBinary::compile(CompileCtx& ctx) const {
    const bool hasCollatorArg = (_nodes.size() == 3);
    auto code = std::make_unique<vm::CodeFragment>();

    invariant(!hasCollatorArg || isComparisonOp(_op));

    auto lhs = _nodes[0]->compile(ctx);
    auto rhs = _nodes[1]->compile(ctx);

    if (_op == EPrimBinary::logicAnd) {
        auto codeFalseBranch = std::make_unique<vm::CodeFragment>();
        codeFalseBranch->appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

        // Jump to the merge point that will be right after the thenBranch (rhs).
        codeFalseBranch->appendJump(rhs->instrs().size());

        code->append(std::move(lhs));
        code = wrapNothingTest(std::move(code), [&](std::unique_ptr<vm::CodeFragment> code) {
            code->appendJumpTrue(codeFalseBranch->instrs().size());
            code->append(std::move(codeFalseBranch), std::move(rhs));

            return code;
        });

        return code;
    } else if (_op == EPrimBinary::logicOr) {
        auto codeTrueBranch = std::make_unique<vm::CodeFragment>();
        codeTrueBranch->appendConstVal(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));

        // Jump to the merge point that will be right after the thenBranch (true branch).
        rhs->appendJump(codeTrueBranch->instrs().size());

        code->append(std::move(lhs));
        code = wrapNothingTest(std::move(code), [&](std::unique_ptr<vm::CodeFragment> code) {
            code->appendJumpTrue(rhs->instrs().size());
            code->append(std::move(rhs), std::move(codeTrueBranch));

            return code;
        });

        return code;
    }

    if (hasCollatorArg) {
        auto collator = _nodes[2]->compile(ctx);
        code->append(std::move(collator));
    }

    code->append(std::move(lhs));
    code->append(std::move(rhs));

    switch (_op) {
        case EPrimBinary::add:
            code->appendAdd();
            break;
        case EPrimBinary::sub:
            code->appendSub();
            break;
        case EPrimBinary::mul:
            code->appendMul();
            break;
        case EPrimBinary::div:
            code->appendDiv();
            break;
        case EPrimBinary::less:
            hasCollatorArg ? code->appendCollLess() : code->appendLess();
            break;
        case EPrimBinary::lessEq:
            hasCollatorArg ? code->appendCollLessEq() : code->appendLessEq();
            break;
        case EPrimBinary::greater:
            hasCollatorArg ? code->appendCollGreater() : code->appendGreater();
            break;
        case EPrimBinary::greaterEq:
            hasCollatorArg ? code->appendCollGreaterEq() : code->appendGreaterEq();
            break;
        case EPrimBinary::eq:
            hasCollatorArg ? code->appendCollEq() : code->appendEq();
            break;
        case EPrimBinary::neq:
            hasCollatorArg ? code->appendCollNeq() : code->appendNeq();
            break;
        case EPrimBinary::cmp3w:
            hasCollatorArg ? code->appendCollCmp3w() : code->appendCmp3w();
            break;
        default:
            MONGO_UNREACHABLE;
    }
    return code;
}

std::vector<DebugPrinter::Block> EPrimBinary::debugPrint() const {
    bool hasCollatorArg = (_nodes.size() == 3);
    std::vector<DebugPrinter::Block> ret;

    invariant(!hasCollatorArg || isComparisonOp(_op));

    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());

    switch (_op) {
        case EPrimBinary::logicAnd:
            ret.emplace_back("&&");
            break;
        case EPrimBinary::logicOr:
            ret.emplace_back("||");
            break;
        case EPrimBinary::add:
            ret.emplace_back("+");
            break;
        case EPrimBinary::sub:
            ret.emplace_back("-");
            break;
        case EPrimBinary::mul:
            ret.emplace_back("*");
            break;
        case EPrimBinary::div:
            ret.emplace_back("/");
            break;
        case EPrimBinary::less:
            ret.emplace_back("<");
            break;
        case EPrimBinary::lessEq:
            ret.emplace_back("<=");
            break;
        case EPrimBinary::greater:
            ret.emplace_back(">");
            break;
        case EPrimBinary::greaterEq:
            ret.emplace_back(">=");
            break;
        case EPrimBinary::eq:
            ret.emplace_back("==");
            break;
        case EPrimBinary::neq:
            ret.emplace_back("!=");
            break;
        case EPrimBinary::cmp3w:
            ret.emplace_back("<=>");
            break;
        default:
            MONGO_UNREACHABLE;
    }

    if (hasCollatorArg) {
        ret.emplace_back("`[`");
        DebugPrinter::addBlocks(ret, _nodes[2]->debugPrint());
        ret.emplace_back("`]");
    }

    DebugPrinter::addBlocks(ret, _nodes[1]->debugPrint());

    return ret;
}

std::unique_ptr<EExpression> EPrimUnary::clone() const {
    return std::make_unique<EPrimUnary>(_op, _nodes[0]->clone());
}

std::unique_ptr<vm::CodeFragment> EPrimUnary::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    auto operand = _nodes[0]->compile(ctx);

    switch (_op) {
        case negate:
            code->append(std::move(operand));
            code->appendNegate();
            break;
        case EPrimUnary::logicNot:
            code->append(std::move(operand));
            code->appendNot();
            break;
        default:
            MONGO_UNREACHABLE;
    }
    return code;
}

std::vector<DebugPrinter::Block> EPrimUnary::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    switch (_op) {
        case EPrimUnary::negate:
            ret.emplace_back("-");
            break;
        case EPrimUnary::logicNot:
            ret.emplace_back("!");
            break;
        default:
            MONGO_UNREACHABLE;
    }

    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());

    return ret;
}

std::unique_ptr<EExpression> EFunction::clone() const {
    std::vector<std::unique_ptr<EExpression>> args;
    args.reserve(_nodes.size());
    for (auto& a : _nodes) {
        args.emplace_back(a->clone());
    }
    return std::make_unique<EFunction>(_name, std::move(args));
}

namespace {
/**
 * The arity test function. It returns true if the number of arguments is correct.
 */
using ArityFn = bool (*)(size_t);

/**
 * The arity test function that trivially accepts any number of arguments.
 */
static constexpr ArityFn kAnyNumberOfArgs = [](size_t) { return true; };

/**
 * The builtin function description.
 */
struct BuiltinFn {
    ArityFn arityTest;
    vm::Builtin builtin;
    bool aggregate;
};

/**
 * The map of recognized builtin functions.
 */
static stdx::unordered_map<std::string, BuiltinFn> kBuiltinFunctions = {
    {"dateDiff",
     BuiltinFn{[](size_t n) { return n == 5 || n == 6; }, vm::Builtin::dateDiff, false}},
    {"dateParts", BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::dateParts, false}},
    {"dateToParts",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateToParts, false}},
    {"isoDateToParts",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::isoDateToParts, false}},
    {"dayOfYear", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::dayOfYear, false}},
    {"dayOfMonth", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::dayOfMonth, false}},
    {"dayOfWeek", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::dayOfWeek, false}},
    {"datePartsWeekYear",
     BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::datePartsWeekYear, false}},
    {"split", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::split, false}},
    {"regexMatch", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexMatch, false}},
    {"replaceOne", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::replaceOne, false}},
    {"dropFields", BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::dropFields, false}},
    {"newArray", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::newArray, false}},
    {"keepFields", BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::keepFields, false}},
    {"newArrayFromRange",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::newArrayFromRange, false}},
    {"newObj", BuiltinFn{[](size_t n) { return n % 2 == 0; }, vm::Builtin::newObj, false}},
    {"ksToString", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ksToString, false}},
    {"ks", BuiltinFn{[](size_t n) { return n > 2; }, vm::Builtin::newKs, false}},
    {"abs", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::abs, false}},
    {"ceil", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ceil, false}},
    {"floor", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::floor, false}},
    {"trunc", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::trunc, false}},
    {"exp", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::exp, false}},
    {"ln", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ln, false}},
    {"log10", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::log10, false}},
    {"sqrt", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sqrt, false}},
    {"addToArray", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToArray, true}},
    {"addToSet", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToSet, true}},
    {"collAddToSet", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::collAddToSet, true}},
    {"doubleDoubleSum",
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoubleSum, false}},
    {"bitTestZero", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestZero, false}},
    {"bitTestMask", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestMask, false}},
    {"bitTestPosition",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::bitTestPosition, false}},
    {"bsonSize", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::bsonSize, false}},
    {"toLower", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toLower, false}},
    {"toUpper", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toUpper, false}},
    {"coerceToString",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::coerceToString, false}},
    {"acos", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acos, false}},
    {"acosh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acosh, false}},
    {"asin", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asin, false}},
    {"asinh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asinh, false}},
    {"atan", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atan, false}},
    {"atanh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atanh, false}},
    {"atan2", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::atan2, false}},
    {"cos", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cos, false}},
    {"cosh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cosh, false}},
    {"degreesToRadians",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::degreesToRadians, false}},
    {"radiansToDegrees",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::radiansToDegrees, false}},
    {"sin", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sin, false}},
    {"sinh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sinh, false}},
    {"tan", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tan, false}},
    {"tanh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tanh, false}},
    {"round", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::round, false}},
    {"concat", BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::concat, false}},
    {"isMember", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isMember, false}},
    {"collIsMember", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collIsMember, false}},
    {"indexOfBytes",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfBytes, false}},
    {"indexOfCP",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfCP, false}},
    {"isDayOfWeek", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isDayOfWeek, false}},
    {"isTimeUnit", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isTimeUnit, false}},
    {"isTimezone", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isTimezone, false}},
    {"setUnion", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setUnion, false}},
    {"setIntersection", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setIntersection, false}},
    {"setDifference",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setDifference, false}},
    {"collSetUnion", BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetUnion, false}},
    {"collSetIntersection",
     BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetIntersection, false}},
    {"collSetDifference",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetDifference, false}},
    {"runJsPredicate",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::runJsPredicate, false}},
    {"regexCompile", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexCompile, false}},
    {"regexFind", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFind, false}},
    {"regexFindAll", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFindAll, false}},
    {"getRegexPattern",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexPattern, false}},
    {"getRegexFlags",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexFlags, false}},
    {"shardFilter", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::shardFilter, false}},
    {"shardHash", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::shardHash, false}},
    {"extractSubArray",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::extractSubArray, false}},
    {"isArrayEmpty", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isArrayEmpty, false}},
    {"reverseArray", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::reverseArray, false}},
    {"dateAdd", BuiltinFn{[](size_t n) { return n == 5; }, vm::Builtin::dateAdd, false}},
    {"hasNullBytes", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::hasNullBytes, false}},
    {"hash", BuiltinFn{[](size_t n) { return n >= 0; }, vm::Builtin::hash, false}},
    {"ftsMatch", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::ftsMatch, false}},
    {"generateSortKey",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::generateSortKey, false}},
    {"tsSecond", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsSecond, false}},
    {"tsIncrement", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsIncrement, false}},
};

/**
 * The code generation function.
 */
using CodeFn = void (vm::CodeFragment::*)();

/**
 * The function description.
 */
struct InstrFn {
    ArityFn arityTest;
    CodeFn generate;
    bool aggregate;
};

/**
 * The map of functions that resolve directly to instructions.
 */
static stdx::unordered_map<std::string, InstrFn> kInstrFunctions = {
    {"getField",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendGetField, false}},
    {"getElement",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendGetElement, false}},
    {"getArraySize",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendGetArraySize, false}},
    {"collComparisonKey",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendCollComparisonKey, false}},
    {"getFieldOrElement",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendGetFieldOrElement, false}},
    {"fillEmpty",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendFillEmpty, false}},
    {"traverseP",
     InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendTraverseP, false}},
    {"traverseF",
     InstrFn{[](size_t n) { return n == 3; }, &vm::CodeFragment::appendTraverseF, false}},
    {"setField",
     InstrFn{[](size_t n) { return n == 3; }, &vm::CodeFragment::appendSetField, false}},
    {"exists", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendExists, false}},
    {"isNull", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsNull, false}},
    {"isObject",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsObject, false}},
    {"isArray", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsArray, false}},
    {"isString",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsString, false}},
    {"isNumber",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsNumber, false}},
    {"isBinData",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsBinData, false}},
    {"isDate", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsDate, false}},
    {"isNaN", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsNaN, false}},
    {"isInfinity",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsInfinity, false}},
    {"isRecordId",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsRecordId, false}},
    {"isMinKey",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsMinKey, false}},
    {"isMaxKey",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsMaxKey, false}},
    {"isTimestamp",
     InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendIsTimestamp, false}},
    {"sum", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendSum, true}},
    {"min", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendMin, true}},
    {"max", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendMax, true}},
    {"first", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendFirst, true}},
    {"last", InstrFn{[](size_t n) { return n == 1; }, &vm::CodeFragment::appendLast, true}},
    {"collMin", InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendCollMin, true}},
    {"collMax", InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendCollMax, true}},
    {"mod", InstrFn{[](size_t n) { return n == 2; }, &vm::CodeFragment::appendMod, false}},
};
}  // namespace

std::unique_ptr<vm::CodeFragment> EFunction::compile(CompileCtx& ctx) const {
    if (auto it = kBuiltinFunctions.find(_name); it != kBuiltinFunctions.end()) {
        auto arity = _nodes.size();
        if (!it->second.arityTest(arity)) {
            uasserted(4822843,
                      str::stream() << "function call: " << _name << " has wrong arity: " << arity);
        }
        auto code = std::make_unique<vm::CodeFragment>();

        for (size_t idx = arity; idx-- > 0;) {
            code->append(_nodes[idx]->compile(ctx));
        }

        if (it->second.aggregate) {
            uassert(4822844,
                    str::stream() << "aggregate function call: " << _name
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression);

            code->appendMoveVal(ctx.accumulator);
            ++arity;
        }

        code->appendFunction(it->second.builtin, arity);

        return code;
    }

    if (auto it = kInstrFunctions.find(_name); it != kInstrFunctions.end()) {
        if (!it->second.arityTest(_nodes.size())) {
            uasserted(4822845,
                      str::stream()
                          << "function call: " << _name << " has wrong arity: " << _nodes.size());
        }
        auto code = std::make_unique<vm::CodeFragment>();

        if (it->second.aggregate) {
            uassert(4822846,
                    str::stream() << "aggregate function call: " << _name
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression);

            code->appendAccessVal(ctx.accumulator);
        }

        // The order of evaluation is flipped for instruction functions. We may want to change the
        // evaluation code for those functions so we have the same behavior for all functions.
        for (size_t idx = 0; idx < _nodes.size(); ++idx) {
            code->append(_nodes[idx]->compile(ctx));
        }
        (*code.*(it->second.generate))();

        return code;
    }

    uasserted(4822847, str::stream() << "unknown function call: " << _name);
}

std::vector<DebugPrinter::Block> EFunction::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, _name);

    ret.emplace_back("(`");
    for (size_t idx = 0; idx < _nodes.size(); ++idx) {
        if (idx) {
            ret.emplace_back("`,");
        }

        DebugPrinter::addBlocks(ret, _nodes[idx]->debugPrint());
    }
    ret.emplace_back("`)");

    return ret;
}

std::unique_ptr<EExpression> EIf::clone() const {
    return std::make_unique<EIf>(_nodes[0]->clone(), _nodes[1]->clone(), _nodes[2]->clone());
}

std::unique_ptr<vm::CodeFragment> EIf::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    auto thenBranch = _nodes[1]->compile(ctx);

    auto elseBranch = _nodes[2]->compile(ctx);

    // The then and else branches must be balanced.
    invariant(thenBranch->stackSize() == elseBranch->stackSize());

    // Jump to the merge point that will be right after the thenBranch.
    elseBranch->appendJump(thenBranch->instrs().size());

    // Compile the condition.
    code->append(_nodes[0]->compile(ctx));
    code = wrapNothingTest(std::move(code), [&](std::unique_ptr<vm::CodeFragment> code) {
        // Jump around the elseBranch.
        code->appendJumpTrue(elseBranch->instrs().size());
        // Append else and then branches.
        code->append(std::move(elseBranch), std::move(thenBranch));

        return code;
    });
    return code;
}

std::vector<DebugPrinter::Block> EIf::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "if");

    ret.emplace_back("(`");

    // Print the condition.
    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());
    ret.emplace_back("`,");
    // Print thenBranch.
    DebugPrinter::addBlocks(ret, _nodes[1]->debugPrint());
    ret.emplace_back("`,");
    // Print elseBranch.
    DebugPrinter::addBlocks(ret, _nodes[2]->debugPrint());

    ret.emplace_back("`)");

    return ret;
}

std::unique_ptr<EExpression> ELocalBind::clone() const {
    std::vector<std::unique_ptr<EExpression>> binds;
    binds.reserve(_nodes.size() - 1);
    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        binds.emplace_back(_nodes[idx]->clone());
    }
    return std::make_unique<ELocalBind>(_frameId, std::move(binds), _nodes.back()->clone());
}

std::unique_ptr<vm::CodeFragment> ELocalBind::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    // Generate bytecode for local variables and the 'in' expression. The 'in' expression is in the
    // last position of _nodes.
    for (size_t idx = 0; idx < _nodes.size(); ++idx) {
        auto c = _nodes[idx]->compile(ctx);
        code->append(std::move(c));
    }

    // After the execution we have to cleanup the stack; i.e. local variables go out of scope.
    // However, note that the top of the stack holds the overall result (i.e. the 'in' expression)
    // and it cannot be destroyed. So we 'bubble' it down with a series of swap/pop instructions.
    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        code->appendSwap();
        code->appendPop();
    }

    // Local variables are no longer accessible after this point so remove any fixup information.
    code->removeFixup(_frameId);
    return code;
}

std::vector<DebugPrinter::Block> ELocalBind::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "let");

    ret.emplace_back("[`");
    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        if (idx != 0) {
            ret.emplace_back("`,");
        }

        DebugPrinter::addIdentifier(ret, _frameId, idx);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, _nodes[idx]->debugPrint());
    }
    ret.emplace_back("`]");

    DebugPrinter::addBlocks(ret, _nodes.back()->debugPrint());

    return ret;
}

std::unique_ptr<EExpression> ELocalLambda::clone() const {
    return std::make_unique<ELocalLambda>(_frameId, _nodes.back()->clone());
}

std::unique_ptr<vm::CodeFragment> ELocalLambda::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    // Compile the body first so we know its size.
    auto body = _nodes.back()->compile(ctx);
    body->appendRet();
    invariant(body->stackSize() == 1);
    body->fixup(1);
    // Lambda parameter is no longer accessible after this point so remove any fixup information.
    body->removeFixup(_frameId);

    // Jump around the body.
    code->appendJump(body->instrs().size());

    // Remember the position and append the body.
    auto bodyPosition = code->instrs().size();
    code->appendNoStack(std::move(body));

    // Push the lambda value on the stack
    code->appendLocalLambda(bodyPosition);

    return code;
}

std::vector<DebugPrinter::Block> ELocalLambda::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "\\");
    DebugPrinter::addIdentifier(ret, _frameId, 0);
    ret.emplace_back(".");
    DebugPrinter::addBlocks(ret, _nodes.back()->debugPrint());

    return ret;
}


std::unique_ptr<EExpression> EFail::clone() const {
    return std::make_unique<EFail>(_code, getStringView(_messageTag, _messageVal));
}

std::unique_ptr<vm::CodeFragment> EFail::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    code->appendConstVal(value::TypeTags::NumberInt64,
                         value::bitcastFrom<int64_t>(static_cast<int64_t>(_code)));

    code->appendConstVal(_messageTag, _messageVal);

    code->appendFail();

    return code;
}

std::vector<DebugPrinter::Block> EFail::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "fail");

    ret.emplace_back("(");

    ret.emplace_back(std::to_string(_code));
    ret.emplace_back(",`");
    ret.emplace_back(getStringView(_messageTag, _messageVal));

    ret.emplace_back("`)");

    return ret;
}

std::unique_ptr<EExpression> ENumericConvert::clone() const {
    return std::make_unique<ENumericConvert>(_nodes[0]->clone(), _target);
}

std::unique_ptr<vm::CodeFragment> ENumericConvert::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    auto operand = _nodes[0]->compile(ctx);
    code->append(std::move(operand));
    code->appendNumericConvert(_target);

    return code;
}

std::vector<DebugPrinter::Block> ENumericConvert::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "convert");

    ret.emplace_back("(");

    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());

    ret.emplace_back("`,");

    switch (_target) {
        case value::TypeTags::NumberInt32:
            ret.emplace_back("int32");
            break;
        case value::TypeTags::NumberInt64:
            ret.emplace_back("int64");
            break;
        case value::TypeTags::NumberDouble:
            ret.emplace_back("double");
            break;
        case value::TypeTags::NumberDecimal:
            ret.emplace_back("decimal");
            break;
        default:
            MONGO_UNREACHABLE;
    }

    ret.emplace_back("`)");
    return ret;
}

std::unique_ptr<EExpression> ETypeMatch::clone() const {
    return std::make_unique<ETypeMatch>(_nodes[0]->clone(), _typeMask);
}

std::unique_ptr<vm::CodeFragment> ETypeMatch::compile(CompileCtx& ctx) const {
    auto code = std::make_unique<vm::CodeFragment>();

    auto variable = _nodes[0]->compile(ctx);
    code->append(std::move(variable));
    code->appendTypeMatch(_typeMask);

    return code;
}

std::vector<DebugPrinter::Block> ETypeMatch::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "typeMatch");

    ret.emplace_back("(`");

    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());
    ret.emplace_back("`,");
    std::stringstream ss;
    ss << "0x" << std::setfill('0') << std::uppercase << std::setw(8) << std::hex << _typeMask;
    ret.emplace_back(ss.str());

    ret.emplace_back("`)");

    return ret;
}

RuntimeEnvironment::RuntimeEnvironment(const RuntimeEnvironment& other)
    : _state{other._state}, _isSmp{other._isSmp} {
    for (auto&& [slotId, index] : _state->slots) {
        emplaceAccessor(slotId, index);
    }
}

RuntimeEnvironment::~RuntimeEnvironment() {
    if (_state.use_count() == 1) {
        for (size_t idx = 0; idx < _state->vals.size(); ++idx) {
            if (_state->owned[idx]) {
                releaseValue(_state->typeTags[idx], _state->vals[idx]);
            }
        }
    }
}

value::SlotId RuntimeEnvironment::registerSlot(StringData name,
                                               value::TypeTags tag,
                                               value::Value val,
                                               bool owned,
                                               value::SlotIdGenerator* slotIdGenerator) {
    auto slot = registerSlot(tag, val, owned, slotIdGenerator);
    _state->nameSlot(name, slot);
    return slot;
}

value::SlotId RuntimeEnvironment::registerSlot(value::TypeTags tag,
                                               value::Value val,
                                               bool owned,
                                               value::SlotIdGenerator* slotIdGenerator) {
    tassert(5645903, "Slot Id generator is null", slotIdGenerator);
    auto slot = slotIdGenerator->generate();
    emplaceAccessor(slot, _state->pushSlot(slot));
    _accessors.at(slot).reset(owned, tag, val);
    return slot;
}

value::SlotId RuntimeEnvironment::getSlot(StringData name) {
    auto slot = getSlotIfExists(name);
    uassert(4946305, str::stream() << "environment slot is not registered: " << name, slot);
    return *slot;
}

boost::optional<value::SlotId> RuntimeEnvironment::getSlotIfExists(StringData name) {
    if (auto it = _state->namedSlots.find(name); it != _state->namedSlots.end()) {
        return it->second;
    }

    return boost::none;
}

void RuntimeEnvironment::resetSlot(value::SlotId slot,
                                   value::TypeTags tag,
                                   value::Value val,
                                   bool owned) {
    // With intra-query parallelism enabled the global environment can hold only read-only values.
    invariant(!_isSmp);

    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        it->second.reset(owned, tag, val);
        return;
    }

    uasserted(4946300, str::stream() << "undefined slot accessor:" << slot);
}

RuntimeEnvironment::Accessor* RuntimeEnvironment::getAccessor(value::SlotId slot) {
    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        return &it->second;
    }

    uasserted(4946301, str::stream() << "undefined slot accessor:" << slot);
}

std::unique_ptr<RuntimeEnvironment> RuntimeEnvironment::makeCopy(bool isSmp) {
    // Once this environment is used to create a copy for a parallel plan execution, it becomes
    // a parallel environment itself.
    if (isSmp) {
        _isSmp = isSmp;
    }

    return std::unique_ptr<RuntimeEnvironment>(new RuntimeEnvironment(*this));
}

void RuntimeEnvironment::debugString(StringBuilder* builder) {
    using namespace std::literals;

    value::SlotMap<StringData> slotName;
    for (const auto& [name, slot] : _state->namedSlots) {
        slotName[slot] = name;
    }

    *builder << "env: { ";
    bool first = true;
    for (auto&& [slot, _] : _state->slots) {
        if (first) {
            first = false;
        } else {
            *builder << ", ";
        }

        std::stringstream ss;
        ss << _accessors.at(slot).getViewOfValue();

        *builder << "s" << slot << " = " << ss.str();

        if (auto it = slotName.find(slot); it != slotName.end()) {
            *builder << " (" << it->second << ")";
        }
    }
    *builder << " }";
}

value::SlotAccessor* CompileCtx::getAccessor(value::SlotId slot) {
    for (auto it = correlated.rbegin(); it != correlated.rend(); ++it) {
        if (it->first == slot) {
            return it->second;
        }
    }

    return env->getAccessor(slot);
}

std::shared_ptr<SpoolBuffer> CompileCtx::getSpoolBuffer(SpoolId spool) {
    if (spoolBuffers.find(spool) == spoolBuffers.end()) {
        spoolBuffers.emplace(spool, std::make_shared<SpoolBuffer>());
    }
    return spoolBuffers[spool];
}

void CompileCtx::pushCorrelated(value::SlotId slot, value::SlotAccessor* accessor) {
    correlated.emplace_back(slot, accessor);
}

void CompileCtx::popCorrelated() {
    correlated.pop_back();
}

CompileCtx CompileCtx::makeCopy(bool isSmp) {
    return {env->makeCopy(isSmp)};
}
}  // namespace sbe
}  // namespace mongo
