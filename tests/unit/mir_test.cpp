#include "pagos/mir/mir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

pagos::mir::Module constant_module() {
    using namespace pagos::mir;
    return {
        .functions = {
            {.name = "pagos_main",
             .result_type = Type::U32,
             .entry = 0,
             .blocks = {{
                 .id = 0,
                 .name = "entry",
                 .instructions = {{
                     .result = 0,
                     .type = Type::U32,
                     .operation = ConstantOperation{.value = std::uint32_t{42}},
                     .span = {},
                 }},
                 .terminator = Return{.value = 0},
             }}}}};
}

TEST(MirVerifier, AcceptsTypedConstantFunction) {
    const auto result = pagos::mir::verify(constant_module());
    EXPECT_TRUE(result.has_value());
}

pagos::mir::Module call_module() {
    using namespace pagos::mir;
    auto module = constant_module();
    auto callee = module.functions.front();
    callee.name = "identity";
    callee.internal = true;
    callee.parameters = {Type::U32};
    callee.blocks[0].instructions[0].operation = ParameterOperation{0};
    auto& entry = module.functions[0].blocks[0];
    entry.instructions.push_back({.result = 1,
                                  .type = Type::U32,
                                  .operation = CallOperation{"identity", {0}},
                                  .span = {}});
    entry.terminator = Return{1};
    module.functions.push_back(std::move(callee));
    return module;
}

TEST(MirVerifier, CallsResolveForwardDeclarationsAndCheckSignatures) {
    using namespace pagos::mir;
    auto module = call_module();
    ASSERT_TRUE(verify(module));
    auto& instruction = module.functions[0].blocks[0].instructions[1];
    auto& call = std::get<CallOperation>(instruction.operation);
    call.callee = "missing";
    EXPECT_FALSE(verify(module));
    call.callee = "identity";
    call.arguments.clear();
    EXPECT_FALSE(verify(module));
    call.arguments = {0, 0};
    EXPECT_FALSE(verify(module));
    call.arguments = {0};
    module.functions[1].result_type = Type::Bool;
    EXPECT_FALSE(verify(module));
    module.functions[1].result_type = Type::U32;
    module.functions[1].parameters[0] = Type::Bool;
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, CallArgumentsMustExistAndDominateTheCall) {
    using namespace pagos::mir;
    auto module = call_module();
    auto& call = std::get<CallOperation>(
        module.functions[0].blocks[0].instructions[1].operation);
    call.arguments = {99};
    EXPECT_FALSE(verify(module));
    call.arguments = {1};
    EXPECT_FALSE(verify(module));
    call.arguments = {0};
    EXPECT_TRUE(verify(module));
}

TEST(MirVerifier, ParametersRequireValidIndexTypeAndTarget) {
    using namespace pagos::mir;
    auto module = call_module();
    auto& parameter = module.functions[1].blocks[0].instructions[0];
    parameter.operation = ParameterOperation{1};
    EXPECT_FALSE(verify(module));
    parameter.operation = ParameterOperation{0};
    parameter.type = Type::Bool;
    EXPECT_FALSE(verify(module));
    parameter.type = Type::U32;
    module.functions[1].parameters[0] = Type::Invalid;
    EXPECT_FALSE(verify(module));
    module.functions[1].parameters[0] = Type::integer(64, false, true);
    module.pointer_bits = 32;
    EXPECT_FALSE(verify(module));
    module.functions[1].parameters[0] = Type::U32;
    EXPECT_TRUE(verify(module));
}

TEST(MirVerifier, CallSignaturesKeepUsizeDistinctFromEqualWidthIntegers) {
    using namespace pagos::mir;
    auto module = call_module();
    module.pointer_bits = 32;
    const auto size = Type::integer(32, false, true);
    auto& callee = module.functions[1];
    callee.result_type = callee.parameters[0] =
        callee.blocks[0].instructions[0].type = size;
    module.functions[0].result_type =
        module.functions[0].blocks[0].instructions[1].type = size;
    const auto result = verify(module);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "MIR call argument type mismatch");
}

TEST(MirVerifier, IntegerConstantsMustMatchEveryPayloadWidth) {
    using namespace pagos;
    using namespace pagos::mir;
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        auto module = constant_module();
        auto& function = module.functions[0];
        auto& instruction = function.blocks[0].instructions[0];
        function.result_type = instruction.type = Type::integer(width);
        const auto value = *IntegerValue::create({width, false}, 42);
        instruction.operation = ConstantOperation{.value = value};
        EXPECT_TRUE(verify(module));
        instruction.operation = ConstantOperation{
            .value = *IntegerValue::create({width, true}, 42)};
        EXPECT_FALSE(verify(module));
        function.result_type = instruction.type =
            Type::array(Type::integer(width), 2);
        instruction.operation =
            ConstantOperation{.value = std::vector<IntegerValue>{value, value}};
        EXPECT_TRUE(verify(module));
        instruction.operation = ConstantOperation{
            .value = std::vector<IntegerValue>{
                value,
                *IntegerValue::create({width == 64 ? 8U : 64U, false}, 42)}};
        EXPECT_FALSE(verify(module));
        Type record{Type::Record};
        record.record_name = "R";
        record.fields = {Type::integer(width)};
        function.result_type = instruction.type = record;
        instruction.operation =
            ConstantOperation{.value = RecordConstant{"R", {value}}};
        EXPECT_TRUE(verify(module));
        instruction.operation =
            ConstantOperation{.value = RecordConstant{"R", {true}}};
        EXPECT_FALSE(verify(module));
    }
}

TEST(MirVerifier, IntegerCastsRequireIntegerTypesAndDominatingValues) {
    using namespace pagos::mir;
    auto module = constant_module();
    auto& function = module.functions[0];
    auto& block = function.blocks[0];
    block.instructions.push_back(
        {.result = 1,
         .type = Type::integer(8),
         .operation = IntegerCastOperation{.operand = 0},
         .span = {}});
    block.terminator = Return{.value = 1};
    function.result_type = Type::integer(8);
    EXPECT_TRUE(verify(module));
    function.result_type = block.instructions[1].type = Type::integer(8, true);
    EXPECT_TRUE(verify(module));
    for (const auto& type : {Type(Type::Bool), Type::array(Type::U32, 1)}) {
        function.result_type = block.instructions[1].type = type;
        EXPECT_FALSE(verify(module));
    }
    function.result_type = block.instructions[1].type = Type::integer(8);
    block.instructions[1].operation = IntegerCastOperation{.operand = 2};
    EXPECT_FALSE(verify(module));
    block.instructions[1].operation = IntegerCastOperation{.operand = 1};
    EXPECT_FALSE(verify(module));
    block.instructions[1].operation = IntegerCastOperation{.operand = 0};
    block.instructions[0].type = Type::Bool;
    block.instructions[0].operation = ConstantOperation{.value = true};
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, NegationRequiresMatchingSignedIntegerTypes) {
    using namespace pagos::mir;
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        auto module = constant_module();
        auto& function = module.functions[0];
        auto& block = function.blocks[0];
        function.result_type = block.instructions[0].type =
            Type::integer(width, true);
        block.instructions[0].operation = ConstantOperation{
            .value = *pagos::IntegerValue::create({width, true}, 1)};
        block.instructions.push_back(
            {.result = 1,
             .type = function.result_type,
             .operation = UnaryOperation{.operation = UnaryOperator::Negate,
                                         .operand = 0},
             .span = {}});
        block.terminator = Return{.value = 1};
        EXPECT_TRUE(verify(module));
        function.result_type = block.instructions[1].type =
            Type::integer(width);
        EXPECT_FALSE(verify(module));
        block.instructions[0].type = Type::integer(width);
        block.instructions[0].operation = ConstantOperation{
            .value = *pagos::IntegerValue::create({width, false}, 1)};
        EXPECT_FALSE(verify(module));
    }
}

pagos::mir::Module record_module() {
    using namespace pagos::mir;
    auto module = constant_module();
    Type record{Type::Record};
    record.record_name = "Config";
    record.fields = {Type::U32, Type::Bool};
    auto& block = module.functions[0].blocks[0];
    block.instructions.push_back({.result = 1,
                                  .type = Type::Bool,
                                  .operation = ConstantOperation{.value = true},
                                  .span = {}});
    block.instructions.push_back(
        {.result = 2,
         .type = record,
         .operation = RecordOperation{.fields = {0, 1}},
         .span = {}});
    block.instructions.push_back(
        {.result = 3,
         .type = Type::U32,
         .operation = FieldOperation{.record = 2, .index = 0},
         .span = {}});
    block.terminator = Return{.value = 3};
    return module;
}

TEST(MirVerifier, AcceptsRecordConstructionProjectionAndConstants) {
    using namespace pagos::mir;
    auto module = record_module();
    EXPECT_TRUE(verify(module).has_value());
    module.functions[0].blocks[0].instructions[2].operation = ConstantOperation{
        .value = pagos::RecordConstant{.name = "Config",
                                       .fields = {std::uint32_t{42}, true}}};
    EXPECT_TRUE(verify(module).has_value());
}

TEST(MirVerifier, RejectsMalformedRecordConstruction) {
    using namespace pagos::mir;
    for (const auto& fields :
         {std::vector<ValueId>{0}, std::vector<ValueId>{1, 0},
          std::vector<ValueId>{0, 99}, std::vector<ValueId>{3, 1}}) {
        auto module = record_module();
        module.functions[0].blocks[0].instructions[2].operation =
            RecordOperation{.fields = fields};
        EXPECT_FALSE(verify(module).has_value());
    }
}

TEST(MirVerifier, RejectsMalformedRecordConstants) {
    using namespace pagos::mir;
    for (const auto& value :
         {pagos::RecordConstant{.name = "Other",
                                .fields = {std::uint32_t{42}, true}},
          pagos::RecordConstant{.name = "Config",
                                .fields = {std::uint32_t{42}}},
          pagos::RecordConstant{.name = "Config",
                                .fields = {true, std::uint32_t{42}}}}) {
        auto module = record_module();
        module.functions[0].blocks[0].instructions[2].operation =
            ConstantOperation{.value = value};
        EXPECT_FALSE(verify(module).has_value());
    }
}

TEST(MirVerifier, RejectsMalformedFieldAccess) {
    using namespace pagos::mir;
    for (const auto field : {FieldOperation{.record = 2, .index = 2},
                             FieldOperation{.record = 2, .index = 1},
                             FieldOperation{.record = 0, .index = 0},
                             FieldOperation{.record = 99, .index = 0}}) {
        auto module = record_module();
        module.functions[0].blocks[0].instructions[3].operation = field;
        EXPECT_FALSE(verify(module).has_value());
    }
}

TEST(MirVerifier, RejectsInconsistentRecordDefinitionsAndEquality) {
    using namespace pagos::mir;
    auto module = record_module();
    auto& block = module.functions[0].blocks[0];
    auto duplicate = block.instructions[2];
    duplicate.result = 4;
    duplicate.type.fields = {Type::Bool, Type::U32};
    block.instructions.push_back(duplicate);
    const auto result = verify(module);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "inconsistent MIR record definition");
    block.instructions.back() = {
        .result = 4,
        .type = Type::Bool,
        .operation = BinaryOperation{.operation = BinaryOperator::Equal,
                                     .left = 2,
                                     .right = 2},
        .span = {}};
    EXPECT_FALSE(verify(module).has_value());
}

TEST(MirVerifier, RecordTypesRejectEmptyAndNonscalarFields) {
    using namespace pagos::mir;
    Type type{Type::Record};
    EXPECT_FALSE(type.valid());
    type.record_name = "Config";
    EXPECT_FALSE(type.valid());
    type.fields = {Type::array(Type::U32, 1)};
    EXPECT_FALSE(type.valid());
    type.fields = {Type::U32};
    EXPECT_TRUE(type.valid());
    type.length = 1;
    EXPECT_FALSE(type.valid());
}

TEST(MirVerifier, BitwiseUnaryRequiresU32) {
    using namespace pagos::mir;
    auto module = constant_module();
    auto& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        {.result = 1,
         .type = Type::U32,
         .operation =
             UnaryOperation{.operation = UnaryOperator::BitNot, .operand = 0},
         .span = {}});
    block.terminator = Return{.value = 1};
    EXPECT_TRUE(verify(module).has_value());
    block.instructions[1].type = Type::Bool;
    EXPECT_FALSE(verify(module).has_value());
    block.instructions[1].type = Type::U32;
    block.instructions[0].type = Type::Bool;
    block.instructions[0].operation = ConstantOperation{.value = true};
    EXPECT_FALSE(verify(module).has_value());
}

TEST(MirVerifier, BitwiseBinaryRequiresU32OperandsAndResult) {
    using namespace pagos::mir;
    for (const auto operation :
         {BinaryOperator::BitAnd, BinaryOperator::BitOr, BinaryOperator::BitXor,
          BinaryOperator::ShiftLeftChecked,
          BinaryOperator::ShiftRightChecked}) {
        auto module = constant_module();
        auto& block = module.functions.front().blocks.front();
        block.instructions.push_back(
            {.result = 1,
             .type = Type::U32,
             .operation =
                 BinaryOperation{.operation = operation, .left = 0, .right = 0},
             .span = {}});
        block.terminator = Return{.value = 1};
        EXPECT_TRUE(verify(module).has_value());
        block.instructions[1].type = Type::Bool;
        EXPECT_FALSE(verify(module).has_value());
        block.instructions[1].type = Type::U32;
        block.instructions[0].type = Type::Bool;
        block.instructions[0].operation = ConstantOperation{.value = true};
        EXPECT_FALSE(verify(module).has_value());
    }
}

TEST(MirVerifier, RejectsUndefinedOperands) {
    auto module = constant_module();
    auto& instruction = module.functions.front().blocks.front().instructions[0];
    instruction.operation = pagos::mir::BinaryOperation{
        .operation = pagos::mir::BinaryOperator::Add,
        .left = 7,
        .right = 8,
    };

    const auto result = pagos::mir::verify(module);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "undefined MIR value `%7`");
}

TEST(MirVerifier, RejectsValuesThatDoNotDominateTheirUse) {
    using namespace pagos::mir;
    const Module
        module{.functions =
                   {
                       {
                           .name = "pagos_main",
                           .result_type = Type::U32,
                           .entry = 0,
                           .blocks =
                               {
                                   {.id = 0,
                                    .name = "entry",
                                    .instructions = {{
                                        .result = 0,
                                        .type = Type::Bool,
                                        .operation =
                                            ConstantOperation{.value = true},
                                        .span = {},
                                    }},
                                    .terminator =
                                        ConditionalBranch{
                                            .condition = 0,
                                            .then_target = 1,
                                            .else_target = 2,
                                        }},
                                   {.id = 1,
                                    .name = "then",
                                    .instructions = {{
                                        .result = 1,
                                        .type = Type::U32,
                                        .operation =
                                            ConstantOperation{
                                                .value = std::uint32_t{1}},
                                        .span = {},
                                    }},
                                    .terminator = Branch{.target = 3}},
                                   {.id = 2,
                                    .name = "else",
                                    .instructions = {},
                                    .terminator = Branch{.target = 3}},
                                   {.id = 3,
                                    .name = "merge",
                                    .instructions =
                                        {
                                            {
                                                .result = 2,
                                                .type = Type::U32,
                                                .operation =
                                                    BinaryOperation{
                                                        .operation =
                                                            BinaryOperator::Add,
                                                        .left = 1,
                                                        .right = 1,
                                                    },
                                                .span = {},
                                            }},
                                    .terminator = Return{.value = 2}},
                               },
                       }}};

    const auto result = verify(module);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "MIR value `%1` does not dominate its use");
}

TEST(MirVerifier, AcceptsLoopBackedgeAndIndexPhi) {
    using namespace pagos::mir;
    const Module
        module{.functions =
                   {
                       {
                           .name = "pagos_main",
                           .result_type = Type::U32,
                           .entry = 0,
                           .blocks =
                               {
                                   {.id = 0,
                                    .name = "entry",
                                    .instructions =
                                        {{.result = 0,
                                          .type = Type::U32,
                                          .operation =
                                              ConstantOperation{
                                                  .value = std::uint32_t{0}},
                                          .span = {}},
                                         {.result = 1,
                                          .type = Type::U32,
                                          .operation =
                                              ConstantOperation{
                                                  .value = std::uint32_t{4}},
                                          .span = {}}},
                                    .terminator = Branch{.target = 1}},
                                   {.id = 1,
                                    .name = "loop.header",
                                    .instructions = {{.result = 2,
                                                      .type = Type::U32,
                                                      .operation =
                                                          PhiOperation{
                                                              .incoming = {{.block =
                                                                                0,
                                                                            .value = 0},
                                                                           {.block =
                                                                                2,
                                                                            .value =
                                                                                5}}},
                                                      .span = {}},
                                                     {.result = 3,
                                                      .type = Type::Bool,
                                                      .operation =
                                                          BinaryOperation{
                                                              .operation = BinaryOperator::Less, .left = 2, .right = 1},
                                                      .span = {}}},
                                    .terminator =
                                        ConditionalBranch{.condition = 3,
                                                          .then_target = 2,
                                                          .else_target = 3}},
                                   {.id = 2,
                                    .name = "loop.body",
                                    .instructions = {{.result = 4,
                                                      .type = Type::U32,
                                                      .operation =
                                                          ConstantOperation{
                                                              .value =
                                                                  std::uint32_t{1}},
                                                      .span = {}},
                                                     {.result = 5,
                                                      .type = Type::U32,
                                                      .operation =
                                                          BinaryOperation{.operation =
                                                                              BinaryOperator::Add,
                                                                          .left =
                                                                              2,
                                                                          .right =
                                                                              4},
                                                      .span = {}}},
                                    .terminator = Branch{.target = 1}},
                                   {.id = 3,
                                    .name = "loop.exit",
                                    .instructions = {},
                                    .terminator = Return{.value = 1}},
                               },
                       }}};

    EXPECT_TRUE(verify(module).has_value());
}

pagos::mir::Module array_module() {
    using namespace pagos::mir;
    auto module = constant_module();
    auto& block = module.functions[0].blocks[0];
    block.instructions[0].operation =
        ConstantOperation{.value = std::uint32_t{0}};
    block.instructions.push_back(
        {.result = 1,
         .type = Type::U32,
         .operation = ConstantOperation{.value = std::uint32_t{42}},
         .span = {}});
    block.instructions.push_back(
        {.result = 2,
         .type = Type::array(Type::U32, 2),
         .operation = ArrayOperation{.elements = {0, 1}},
         .span = {}});
    block.instructions.push_back(
        {.result = 3,
         .type = Type::U32,
         .operation = IndexOperation{.array = 2, .index = 0},
         .span = {}});
    block.terminator = Return{.value = 3};
    return module;
}

TEST(MirVerifier, AcceptsArraysAndCheckedIndexing) {
    using namespace pagos::mir;
    auto module = array_module();
    EXPECT_TRUE(verify(module));
    auto& array = module.functions[0].blocks[0].instructions[2];
    array.operation =
        ConstantOperation{.value = std::vector<pagos::IntegerValue>{0, 42}};
    EXPECT_TRUE(verify(module));
}

TEST(MirVerifier, RejectsMalformedArrayConstants) {
    using namespace pagos::mir;
    auto module = array_module();
    auto& array = module.functions[0].blocks[0].instructions[2];
    array.operation =
        ConstantOperation{.value = std::vector<pagos::IntegerValue>{42}};
    EXPECT_FALSE(verify(module));
    array.operation =
        ConstantOperation{.value = std::vector<bool>{true, false}};
    EXPECT_FALSE(verify(module));
    array.type = Type::array(Type::U32, 0);
    array.operation =
        ConstantOperation{.value = std::vector<pagos::IntegerValue>{}};
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, RejectsMalformedArrayConstruction) {
    using namespace pagos::mir;
    auto module = array_module();
    auto& instructions = module.functions[0].blocks[0].instructions;
    instructions[2].operation = ArrayOperation{.elements = {0}};
    EXPECT_FALSE(verify(module));
    instructions[2].operation = ArrayOperation{.elements = {0, 7}};
    EXPECT_FALSE(verify(module));
    instructions[2].operation = ArrayOperation{.elements = {0, 3}};
    auto result = verify(module);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "MIR value `%3` does not dominate its use");
    instructions[2].operation = ArrayOperation{.elements = {0, 1}};
    instructions[2].type = Type::array(Type::Bool, 2);
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, RejectsMalformedCheckedIndexing) {
    using namespace pagos::mir;
    auto module = array_module();
    auto& access = module.functions[0].blocks[0].instructions[3];
    access.operation = IndexOperation{.array = 0, .index = 1};
    EXPECT_FALSE(verify(module));
    access.operation = IndexOperation{.array = 2, .index = 2};
    EXPECT_FALSE(verify(module));
    access.operation = IndexOperation{.array = 2, .index = 9};
    EXPECT_FALSE(verify(module));
    access.operation = IndexOperation{.array = 9, .index = 0};
    EXPECT_FALSE(verify(module));
    access.operation = IndexOperation{.array = 2, .index = 3};
    auto result = verify(module);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "MIR value `%3` does not dominate its use");
    access.operation = IndexOperation{.array = 2, .index = 0};
    access.type = Type::Bool;
    module.functions[0].result_type = Type::Bool;
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, RejectsArrayEquality) {
    using namespace pagos::mir;
    auto module = array_module();
    module.functions[0].result_type = Type::Bool;
    auto& access = module.functions[0].blocks[0].instructions[3];
    access.type = Type::Bool;
    access.operation = BinaryOperation{
        .operation = BinaryOperator::Equal, .left = 2, .right = 2};
    EXPECT_FALSE(verify(module));
}

TEST(MirVerifier, AcceptsBooleanArrayConstants) {
    using namespace pagos::mir;
    auto module = array_module();
    module.functions[0].result_type = Type::Bool;
    auto& instructions = module.functions[0].blocks[0].instructions;
    instructions[2].type = Type::array(Type::Bool, 2);
    instructions[2].operation =
        ConstantOperation{.value = std::vector<bool>{true, false}};
    instructions[3].type = Type::Bool;
    EXPECT_TRUE(verify(module));
}

} // namespace
