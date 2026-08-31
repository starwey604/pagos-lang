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

} // namespace
