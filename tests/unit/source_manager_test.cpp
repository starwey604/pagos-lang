#include "pagos/source/path.h"
#include "pagos/source/source_manager.h"
#include "temporary_directory.h"

#include <fstream>
#include <gtest/gtest.h>

namespace {

TEST(SourceManager, LoadsNativeUnicodePathsAndReportsMissingPathsAsUtf8) {
    using namespace pagos::source;
    pagos::test::TemporaryDirectory temporary("pagos-source-path");
    const auto directory = temporary.path() / path_from_utf8("中文 空格");
    std::filesystem::create_directory(directory);
    const auto path = directory / path_from_utf8("输入 文件.pgs");
    const std::string text = "let result = 42;\n";
    std::ofstream(path, std::ios::binary) << text;
    const auto source = SourceFile::load(path);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->path(), path_to_utf8(path));
    EXPECT_EQ(source->text(), text);
    EXPECT_EQ(path_from_utf8(path_to_utf8(path)), path);
    const auto missing = directory / path_from_utf8("不存在.pgs");
    const auto failed = SourceFile::load(missing);
    ASSERT_FALSE(failed);
    EXPECT_NE(failed.error().find(path_to_utf8(missing)), std::string::npos);
}

TEST(SourceManager, MapsOffsetsToOneBasedLocations) {
    const auto source = pagos::source::SourceFile::from_text(
        "test.pgs", "let first = 1;\nlet second = 2;\n");

    const auto position = source.position(19);

    EXPECT_EQ(position.line, 2U);
    EXPECT_EQ(position.column, 5U);
    EXPECT_EQ(source.line(2), "let second = 2;");
}

} // namespace
