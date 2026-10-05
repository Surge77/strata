#include <algorithm>

#include "db/compaction.hpp"

namespace strata {
namespace {

constexpr std::uint64_t kLevelSizeMultiplier = 10;

bool Overlaps(const Comparator* user, const FileMetaData& file, Slice smallest, Slice largest) {
  return user->Compare(ExtractUserKey(file.largest), smallest) >= 0 &&
         user->Compare(ExtractUserKey(file.smallest), largest) <= 0;
}

// Fills inputs[1] with the tables of the level below that overlap inputs[0].
void AddOverlapsBelow(const Version& version, const InternalKeyComparator& comparator,
                      Compaction* compaction) {
  const Comparator* user = comparator.user_comparator();
  const auto& upper = compaction->inputs[0];
  Slice smallest = ExtractUserKey(upper.front().smallest);
  Slice largest = ExtractUserKey(upper.front().largest);
  for (const FileMetaData& file : upper) {
    if (user->Compare(ExtractUserKey(file.smallest), smallest) < 0) {
      smallest = ExtractUserKey(file.smallest);
    }
    if (user->Compare(ExtractUserKey(file.largest), largest) > 0) {
      largest = ExtractUserKey(file.largest);
    }
  }
  for (const FileMetaData& file : version.files(compaction->output_level())) {
    if (Overlaps(user, file, smallest, largest)) {
      compaction->inputs[1].push_back(file);
    }
  }
}

// The oldest level-0 table plus every level-0 table that overlaps the growing
// set, transitively. A table left behind overlaps none of the chosen ones, so
// it shares no key with them and moving them below it cannot put an older
// version of a key above a newer one. Disjoint tables (a sequential load) are
// thus taken one at a time and moved instead of rewritten.
std::optional<Compaction> PickLevelZeroCompaction(const Version& version,
                                                  const InternalKeyComparator& comparator) {
  const Comparator* user = comparator.user_comparator();
  const auto& level0 = version.files(0);  // oldest first
  std::vector<bool> chosen(level0.size(), false);
  chosen[0] = true;
  std::string smallest = ToString(ExtractUserKey(level0[0].smallest));
  std::string largest = ToString(ExtractUserKey(level0[0].largest));
  for (bool grew = true; grew;) {
    grew = false;
    for (std::size_t index = 0; index < level0.size(); ++index) {
      if (chosen[index] || !Overlaps(user, level0[index], smallest, largest)) {
        continue;
      }
      chosen[index] = true;
      grew = true;
      if (user->Compare(ExtractUserKey(level0[index].smallest), smallest) < 0) {
        smallest = ToString(ExtractUserKey(level0[index].smallest));
      }
      if (user->Compare(ExtractUserKey(level0[index].largest), largest) > 0) {
        largest = ToString(ExtractUserKey(level0[index].largest));
      }
    }
  }
  Compaction compaction;
  compaction.level = 0;
  for (std::size_t index = 0; index < level0.size(); ++index) {
    if (chosen[index]) {
      compaction.inputs[0].push_back(level0[index]);
    }
  }
  AddOverlapsBelow(version, comparator, &compaction);
  return compaction;
}

}  // namespace

std::uint64_t MaxBytesForLevel(int level, std::uint64_t base) {
  std::uint64_t bytes = base;
  for (int deeper = 1; deeper < level; ++deeper) {
    bytes *= kLevelSizeMultiplier;
  }
  return bytes;
}

double CompactionScore(const Version& version, int level, std::uint64_t base) {
  if (level == 0) {
    // Counted in tables, not bytes: each level-0 table costs every lookup a
    // search, whatever its size.
    return static_cast<double>(version.files(0).size()) / static_cast<double>(kL0CompactionTrigger);
  }
  return static_cast<double>(version.LevelBytes(level)) /
         static_cast<double>(MaxBytesForLevel(level, base));
}

std::optional<Compaction> PickCompaction(const Version& version,
                                         const InternalKeyComparator& comparator,
                                         std::uint64_t base,
                                         std::array<std::string, kNumLevels>* next_start) {
  int best_level = -1;
  double best_score = 1.0;
  for (int level = 0; level + 1 < kNumLevels; ++level) {
    const double score = CompactionScore(version, level, base);
    if (score >= best_score) {
      best_score = score;
      best_level = level;
    }
  }
  if (best_level < 0) {
    return std::nullopt;
  }
  if (best_level == 0) {
    return PickLevelZeroCompaction(version, comparator);
  }

  // Deeper levels: one table at a time, cycling through the key range.
  const Comparator* user = comparator.user_comparator();
  const auto& files = version.files(best_level);
  std::string& start = (*next_start)[static_cast<std::size_t>(best_level)];
  auto chosen = std::find_if(files.begin(), files.end(), [&](const FileMetaData& file) {
    return start.empty() || user->Compare(ExtractUserKey(file.smallest), start) > 0;
  });
  if (chosen == files.end()) {
    chosen = files.begin();
  }
  start = ToString(ExtractUserKey(chosen->largest));
  Compaction compaction;
  compaction.level = best_level;
  compaction.inputs[0].push_back(*chosen);
  AddOverlapsBelow(version, comparator, &compaction);
  return compaction;
}

std::optional<Compaction> PickLevelCompaction(const Version& version,
                                              const InternalKeyComparator& comparator, int level) {
  if (level < 0 || level + 1 >= kNumLevels || version.files(level).empty()) {
    return std::nullopt;
  }
  Compaction compaction;
  compaction.level = level;
  compaction.inputs[0] = version.files(level);
  AddOverlapsBelow(version, comparator, &compaction);
  return compaction;
}

}  // namespace strata
