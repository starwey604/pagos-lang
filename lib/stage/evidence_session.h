#pragma once

#include "pagos/stage/evidence_builder.h"

namespace pagos::stage::provenance {
class EvidenceSession {
  public:
    std::filesystem::path directory;
    ~EvidenceSession();
    [[nodiscard]] static std::expected<std::shared_ptr<EvidenceSession>,
                                       EvidenceError>
    create(const std::filesystem::path& parent);

  private:
    int descriptor_{-1};
};
} // namespace pagos::stage::provenance
