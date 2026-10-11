#include "cache_test_input.h"
#include "pagos/cache/store.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
using namespace pagos;
using namespace pagos::cache;
namespace fs = std::filesystem;

Digest named(std::string_view name) { return fingerprint(name, {}); }
ArtifactInput value(std::uint32_t number = 49, std::string name = "result",
                    bool required = true) {
    return {std::move(name), ObjectKind::Value,
            *encode_value({sema::Type::integer(32), IntegerValue(number)}),
            required};
}
Bytes read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const fs::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}
void checksum(Bytes& bytes) {
    ASSERT_GE(bytes.size(), 32U);
    const auto hash = fingerprint("pagos.query-manifest.v1",
                                  std::span(bytes).first(bytes.size() - 32));
    std::ranges::copy(hash.bytes, bytes.end() - 32);
}
class CacheStore : public ::testing::Test {
  protected:
    CacheStore() {
        static std::atomic<unsigned> sequence{};
        const auto time =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory = fs::temp_directory_path() /
                    ("pagos-store-test-" + std::to_string(time) + "-" +
                     std::to_string(sequence++));
        fs::create_directory(directory);
        root = directory / "cache";
    }
    ~CacheStore() override {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
    auto publish(FileStore& store, const Digest& input = named("inputs"),
                 const Digest& result = named("result")) {
        const std::array artifacts{value()};
        return store.publish(key, input, result, artifacts);
    }
    fs::path manifest(const Digest& input = named("inputs")) const {
        const auto hash = query_fingerprint(key)->hex();
        return root / "queries" / hash.substr(0, 2) / hash /
               (input.hex() + ".manifest");
    }
    fs::path object(const ObjectRef& ref) const {
        const auto hash = ref.digest.hex();
        return root / "objects" / hash.substr(0, 2) / hash;
    }
    std::size_t files(std::string_view subdirectory) const {
        if (!fs::exists(root / subdirectory))
            return 0;
        std::size_t count = 0;
        for (const auto& entry :
             fs::recursive_directory_iterator(root / subdirectory))
            count += entry.is_regular_file() ? 1U : 0U;
        return count;
    }
    QueryKey key{cache_test::key()};
    fs::path directory;
    fs::path root;
};

TEST_F(CacheStore, ReadOnlyMissDoesNotCreateFilesystemEntries) {
    FileStore store(root);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::NotFound);
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(CacheStore, RoundTripReopensAndSharesContentAcrossQueries) {
    FileStore store(root);
    EXPECT_EQ(publish(store), PublishResult::Published);
    FileStore reopened(root);
    const auto candidate = reopened.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate->key, key);
    EXPECT_EQ(candidate->inputs, named("inputs"));
    EXPECT_EQ(candidate->semantic_result, named("result"));
    ASSERT_EQ(candidate->artifacts.size(), 1U);
    EXPECT_EQ(candidate->artifacts[0].bytes, value().bytes);
    EXPECT_EQ(candidate->artifacts[0].object.size, value().bytes.size());
    EXPECT_EQ(publish(reopened), PublishResult::AlreadyPresent);
    key.arguments[0].value = IntegerValue(8);
    EXPECT_EQ(publish(reopened), PublishResult::Published);
    EXPECT_EQ(files("objects"), 1U);
    EXPECT_EQ(files("tmp"), 0U);
}

TEST_F(CacheStore, DifferentVerifiedInputsDoNotCreateDeterminismConflicts) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const std::array changed{value(64)};
    EXPECT_EQ(store.publish(key, named("edited-module"), named("edited-result"),
                            changed),
              PublishResult::Published);
    EXPECT_TRUE(store.lookup(key, named("inputs")));
    const auto newer = store.lookup(key, named("edited-module"));
    ASSERT_TRUE(newer);
    EXPECT_EQ(newer->artifacts[0].bytes, changed[0].bytes);
    EXPECT_EQ(store.lookup(key, named("missing-inputs")).error().code,
              StoreErrorCode::NotFound);
}

TEST_F(CacheStore, ConflictingSemanticResultsPersistAReuseTombstone) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const std::array changed{value(50)};
    EXPECT_EQ(
        store.publish(key, named("inputs"), named("wrong-result"), changed)
            .error()
            .code,
        StoreErrorCode::DeterminismConflict);
    FileStore reopened(root);
    EXPECT_EQ(reopened.lookup(key, named("inputs")).error().code,
              StoreErrorCode::DeterminismConflict);
    EXPECT_EQ(publish(reopened).error().code,
              StoreErrorCode::DeterminismConflict);
    EXPECT_EQ(files("objects"), 1U);
    EXPECT_TRUE(publish(reopened, named("other-inputs")));
}

TEST_F(CacheStore, ConflictMarkerIoFailureStillDisablesReuseInThisProcess) {
    FileStore healthy(root);
    ASSERT_TRUE(publish(healthy));
    StoreOptions options;
    options.fault = [](IoOperation operation, const fs::path&) {
        return operation == IoOperation::PublishManifest
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    FileStore failing(root, options);
    const auto conflict = publish(failing, named("inputs"), named("different"));
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, StoreErrorCode::DeterminismConflict);
    EXPECT_EQ(conflict.error().io, std::errc::no_space_on_device);
    EXPECT_EQ(healthy.lookup(key, named("inputs")).error().code,
              StoreErrorCode::DeterminismConflict);
    EXPECT_EQ(publish(healthy).error().code,
              StoreErrorCode::DeterminismConflict);
}

TEST_F(CacheStore,
       MultipleRequiredKindsAndOwnedCandidatesRemainIndependentOfFiles) {
    FileStore store(root);
    const auto anchor = *stage::encode_source_anchor(
        {{key.context.module, DefinitionKind::Function, "square"},
         key.definition_fingerprint,
         4});
    const std::array artifacts{
        value(),
        ArtifactInput{"query", ObjectKind::QueryKey, *encode_query_key(key)},
        ArtifactInput{"site", ObjectKind::SourceAnchor, anchor}};
    ASSERT_TRUE(
        store.publish(key, named("inputs"), named("result"), artifacts));
    const auto candidate = store.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    ASSERT_EQ(candidate->artifacts.size(), 3U);
    EXPECT_EQ(candidate->artifacts[0].object.kind, ObjectKind::QueryKey);
    EXPECT_EQ(candidate->artifacts[2].object.kind, ObjectKind::SourceAnchor);
    fs::remove(object(candidate->artifacts[2].object));
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::MissingObject);
    fs::remove_all(root);
    EXPECT_EQ(decode_query_key(*candidate->artifacts[0].bytes), key);
    EXPECT_TRUE(decode_value(*candidate->artifacts[1].bytes));
    EXPECT_EQ(*candidate->artifacts[2].bytes, anchor);
}

TEST_F(CacheStore, OptionalDetailsCanBeMissingAndDoNotChangeSemanticIdentity) {
    FileStore store(root);
    const std::array artifacts{value(7, "detail", false), value()};
    ASSERT_TRUE(
        store.publish(key, named("inputs"), named("result"), artifacts));
    const auto old = store.lookup(key, named("inputs"));
    ASSERT_TRUE(old);
    EXPECT_EQ(old->artifacts[0].name, "detail");
    fs::remove(object(old->artifacts[0].object));
    const auto candidate = store.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    EXPECT_FALSE(candidate->artifacts[0].bytes);
    EXPECT_TRUE(candidate->artifacts[1].bytes);
    EXPECT_EQ(publish(store), PublishResult::AlreadyPresent);
}

TEST_F(CacheStore, MissingRequiredObjectsPreventCandidatesAndCanBeRepublished) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    auto candidate = store.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    fs::remove(object(candidate->artifacts[0].object));
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::MissingObject);
    EXPECT_EQ(publish(store), PublishResult::Published);
    EXPECT_TRUE(store.lookup(key, named("inputs")));
}

TEST_F(CacheStore, EveryManifestTruncationAndChecksumFailureIsRejected) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const auto path = manifest();
    const auto bytes = read(path);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        write(path, Bytes(bytes.begin(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        EXPECT_FALSE(store.lookup(key, named("inputs"))) << length;
    }
    auto modified = bytes;
    modified[modified.size() / 2] ^= 1;
    write(path, modified);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::CorruptManifest);
    EXPECT_EQ(publish(store), PublishResult::Published);
    EXPECT_TRUE(store.lookup(key, named("inputs")));
}

TEST_F(CacheStore, ValidChecksumsStillRequireMatchingKeysInputsKindsAndLimits) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const auto path = manifest();
    const auto bytes = read(path);
    auto changed = bytes;
    changed[4] = 2; // Envelope version.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::IncompatibleVersion);
    const auto key_size = encode_query_key(key)->size();
    changed = bytes;
    changed[17 + key_size] ^= 1; // Input fingerprint.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::InputMismatch);
    changed = bytes;
    changed[17 + key_size - 8] ^= 1; // Valid u32 argument constant.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::KeyMismatch);
    const auto reference_offset = 17 + key_size + 64 + 4 + 4 + 6;
    changed = bytes;
    changed[reference_offset] = 255; // Unknown object kind.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::CorruptManifest);
    changed = bytes;
    changed[reference_offset + 2 + 7] = 255; // Overflowing payload length.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::LimitExceeded);
    changed = bytes;
    changed[reference_offset + 10] ^= 1; // A hash-derived missing reference.
    checksum(changed);
    write(path, changed);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::MissingObject);
    write(path, bytes);
    StoreOptions small;
    small.manifest_bytes = bytes.size() - 1;
    EXPECT_EQ(FileStore(root, small).lookup(key, named("inputs")).error().code,
              StoreErrorCode::LimitExceeded);
    for (const auto index : std::array<std::size_t, 4>{16, 17 + key_size + 64,
                                                       reference_offset - 7,
                                                       reference_offset + 1}) {
        changed = bytes;
        changed[index] = 255;
        checksum(changed);
        write(path, changed);
        EXPECT_FALSE(store.lookup(key, named("inputs"))) << index;
    }
}

TEST_F(CacheStore, EveryObjectTruncationAndHeaderOrDigestDamageIsRejected) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const auto candidate = store.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    const auto path = object(candidate->artifacts[0].object);
    const auto bytes = read(path);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        write(path, Bytes(bytes.begin(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        EXPECT_FALSE(store.lookup(key, named("inputs"))) << length;
    }
    for (std::size_t index : {0U, 8U, 9U, 17U, 49U, 60U}) {
        auto changed = bytes;
        changed[index] ^= 1;
        write(path, changed);
        EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
                  StoreErrorCode::CorruptObject)
            << index;
        EXPECT_FALSE(publish(store)); // Never overwrite a corrupt CAS inode.
    }
    auto version = bytes;
    version[4] = 2;
    write(path, version);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::IncompatibleVersion);
    auto trailing = bytes;
    trailing.push_back(0);
    write(path, trailing);
    EXPECT_FALSE(store.lookup(key, named("inputs")));
}

TEST_F(CacheStore,
       PublicationFailuresCleanTemporaryFilesAndNeverExposeHalfResults) {
    for (const auto operation :
         {IoOperation::CreateDirectories, IoOperation::LockFile,
          IoOperation::TemporaryCreated, IoOperation::WriteTemporary,
          IoOperation::CloseTemporary, IoOperation::PublishObject,
          IoOperation::ObjectPublished, IoOperation::PublishManifest}) {
        std::error_code ec;
        fs::remove_all(root, ec);
        StoreOptions options;
        options.fault = [operation](IoOperation point, const fs::path&) {
            return point == operation
                       ? std::make_error_code(std::errc::no_space_on_device)
                       : std::error_code{};
        };
        FileStore failing(root, options);
        const auto result = publish(failing);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, StoreErrorCode::IoFailure);
        EXPECT_EQ(result.error().io, std::errc::no_space_on_device);
        EXPECT_FALSE(FileStore(root).lookup(key, named("inputs")));
        EXPECT_EQ(files("tmp"), 0U);
        FileStore healthy(root);
        EXPECT_TRUE(publish(healthy));
        EXPECT_TRUE(healthy.lookup(key, named("inputs")));
    }
}

TEST_F(CacheStore, FailureAfterManifestCommitLeavesOnlyACompleteCandidate) {
    StoreOptions options;
    options.fault = [](IoOperation point, const fs::path&) {
        return point == IoOperation::ManifestPublished
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    FileStore failing(root, options);
    EXPECT_FALSE(publish(failing));
    EXPECT_TRUE(FileStore(root).lookup(key, named("inputs")));
    EXPECT_EQ(files("tmp"), 0U);
}

TEST_F(CacheStore, InvalidRequestsAndCapacityLimitsHaveNoPublicationEffects) {
    FileStore store(root);
    std::array artifacts{value()};
    auto check = [&](StoreErrorCode code) {
        EXPECT_EQ(
            store.publish(key, named("inputs"), named("result"), artifacts)
                .error()
                .code,
            code);
        EXPECT_FALSE(fs::exists(root));
    };
    artifacts[0].name = "../../escape";
    check(StoreErrorCode::InvalidRequest);
    artifacts[0] = value();
    artifacts[0].required = false;
    check(StoreErrorCode::InvalidRequest);
    artifacts[0] = value();
    artifacts[0].kind = ObjectKind::SourceAnchor;
    check(StoreErrorCode::CorruptObject);
    artifacts[0] = value();
    artifacts[0].bytes.clear();
    check(StoreErrorCode::CorruptObject);
    artifacts[0] = value();
    StoreOptions options;
    options.object_bytes = artifacts[0].bytes.size() - 1;
    EXPECT_EQ(FileStore(root, options)
                  .publish(key, named("inputs"), named("result"), artifacts)
                  .error()
                  .code,
              StoreErrorCode::LimitExceeded);
    options = {};
    options.manifest_bytes = 1;
    EXPECT_EQ(FileStore(root, options)
                  .publish(key, named("inputs"), named("result"), artifacts)
                  .error()
                  .code,
              StoreErrorCode::LimitExceeded);
    const std::array duplicate{value(), value()};
    EXPECT_EQ(store.publish(key, named("inputs"), named("result"), duplicate)
                  .error()
                  .code,
              StoreErrorCode::InvalidRequest);
    ++key.context.budget_version;
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::IncompatibleVersion);
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(CacheStore,
       DeletedOrUnwritableDirectoriesAndReadFailuresAreRecoverable) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    fs::remove_all(root);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::NotFound);
    ASSERT_TRUE(publish(store));
    StoreOptions options;
    options.fault = [](IoOperation point, const fs::path&) {
        return point == IoOperation::ReadFile
                   ? std::make_error_code(std::errc::permission_denied)
                   : std::error_code{};
    };
    EXPECT_EQ(
        FileStore(root, options).lookup(key, named("inputs")).error().code,
        StoreErrorCode::IoFailure);
    fs::remove_all(root);
    write(root, {1}); // Root is a file, including when tests run as root.
    EXPECT_EQ(publish(store).error().code, StoreErrorCode::IoFailure);
}

TEST_F(CacheStore, ConcurrentThreadsShareObjectsAndPublishOneManifest) {
    constexpr unsigned count = 8;
    std::array<std::thread, count> threads;
    std::array<bool, count> success{};
    for (unsigned index = 0; index < count; ++index)
        threads[index] = std::thread([&, index] {
            StoreOptions options;
            options.lock_timeout = std::chrono::seconds(5);
            FileStore store(root, options);
            success[index] = publish(store).has_value();
        });
    for (auto& thread : threads)
        thread.join();
    EXPECT_TRUE(std::ranges::all_of(success, [](bool item) { return item; }));
    EXPECT_TRUE(FileStore(root).lookup(key, named("inputs")));
    EXPECT_EQ(files("objects"), 1U);
    EXPECT_EQ(files("tmp"), 0U);
}

TEST_F(CacheStore, NonRegularEntriesCannotBecomeCacheCandidates) {
    FileStore store(root);
    ASSERT_TRUE(publish(store));
    const auto candidate = store.lookup(key, named("inputs"));
    ASSERT_TRUE(candidate);
    const auto path = object(candidate->artifacts[0].object);
    const auto outside = directory / "outside";
    write(outside, read(path));
    fs::remove(path);
    fs::create_symlink(outside, path);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::CorruptObject);
    EXPECT_FALSE(publish(store));
    fs::remove(path);
    fs::create_directory(path);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::CorruptObject);
    const auto hash = query_fingerprint(key)->hex();
    const auto lock = root / "queries" / hash.substr(0, 2) / (hash + ".lock");
    fs::remove(lock);
    fs::create_directory(lock);
    EXPECT_EQ(store.lookup(key, named("inputs")).error().code,
              StoreErrorCode::IoFailure);
}
} // namespace
