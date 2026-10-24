#pragma once
#include <algorithm>

namespace rd {
// A directory checkpoint can predate an on-demand body download. Always retain
// the current cache reference for matching identities when resuming that scan.
template <typename Entries>
void merge_cached_entries(Entries &staged, const Entries &active) {
  for (auto &entry : staged) {
    auto current =
        std::find_if(active.begin(), active.end(), [&](const auto &e) {
          return e.source_id == entry.source_id &&
                 e.document_id == entry.document_id;
        });
    if (current != active.end() && !current->snapshot_id.empty())
      entry.snapshot_id = current->snapshot_id;
  }
}
} // namespace rd
