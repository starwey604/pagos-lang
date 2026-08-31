#include "pagos/source/source_manager.h"

#include <gtest/gtest.h>

namespace {

TEST(SourceManager, MapsOffsetsToOneBasedLocations) {
    const auto source = pagos::source::SourceFile::from_text(
        "test.pgs", "let first = 1;\nlet second = 2;\n");

    const auto position = source.position(19);

    EXPECT_EQ(position.line, 2U);
    EXPECT_EQ(position.column, 5U);
    EXPECT_EQ(source.line(2), "let second = 2;");
}

} // namespace
