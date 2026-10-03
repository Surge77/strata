#pragma once

#include "strata/env.hpp"
#include "strata/iterator.hpp"
#include "strata/status.hpp"

#include <string>

#include "db/dbformat.hpp"
#include "db/table_cache.hpp"
#include "db/version_edit.hpp"

namespace strata {

// Writes every entry of `source` into SSTable number meta->number, filling in
// meta's size and key range, and makes it durable: the file is fdatasynced
// and its directory synced, so the table can be committed to the MANIFEST
// (docs/format/manifest.md, publish step 1). The new table is then opened
// through `tables` as a final check that it reads back.
//
// An empty source produces no file (meta->file_size stays 0). On any failure
// the partial file is deleted.
Status BuildTable(const std::string& dbname, Env* env, const InternalKeyComparator* comparator,
                  TableCache* tables, Iterator* source, FileMetaData* meta);

}  // namespace strata
