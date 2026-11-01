#include "pagos/cache/task_store.h"
#include "pagos/stage/function_evidence.h"
#include "pagos/stage/generator_replay_execution.h"
#include "pagos/stage/provenance_recursive.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/provenance_topology.h"
#include "pagos/stage/scalar_replay_execution.h"

#include <gtest/gtest.h>

namespace {
TEST(ProtocolBaseline, PublicVersionsRequireCompatibilityReview) {
    using namespace pagos;
    using namespace pagos::stage;
    using namespace pagos::stage::provenance;
    // Independent of encoder defaults. A deliberate bump needs the baseline
    // inventory, old-reader policy and fixed-fixture compatibility reviewed.
    for (const auto version : {cache::format_version,
                               cache::query_protocol_version,
                               cache::budget_protocol_version,
                               cache::store_format_version,
                               cache::task_store_version,
                               provenance_format_version,
                               provenance_record_model,
                               evidence_block_version,
                               function_evidence_version,
                               query_checkpoint_version,
                               query_engine_version,
                               topology_engine_version,
                               recursive_engine_version,
                               replay_descriptor_version,
                               scalar_recipe_semantics_version,
                               scalar_replay_evidence_version,
                               scalar_replay_execution_version,
                               generator_replay_descriptor_version,
                               closed_generator_semantics_version,
                               generator_replay_evidence_version,
                               generator_replay_execution_version}) {
        EXPECT_EQ(version, 1U);
    }
}
} // namespace
