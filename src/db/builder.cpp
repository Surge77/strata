#include "db/builder.hpp"

#include "db/filename.hpp"
#include "table/table_builder.hpp"

namespace strata {

Status BuildTable(const std::string& dbname, Env* env, const InternalKeyComparator* comparator,
                  TableCache* tables, Iterator* source, FileMetaData* meta) {
  meta->file_size = 0;
  source->SeekToFirst();
  if (!source->Valid()) {
    return source->status();
  }

  const std::string file_name = TableFileName(dbname, meta->number);
  Status status = [&]() -> Status {
    STRATA_ASSIGN_OR_RETURN(auto file, env->NewWritableFile(file_name));
    table::TableBuilder builder({.comparator = comparator, .use_filter = true}, file.get());
    meta->smallest.assign(source->key());
    for (; source->Valid(); source->Next()) {
      meta->largest.assign(source->key());
      STRATA_RETURN_IF_ERROR(builder.Add(source->key(), source->value()));
    }
    STRATA_RETURN_IF_ERROR(source->status());
    STRATA_RETURN_IF_ERROR(builder.Finish());
    STRATA_RETURN_IF_ERROR(file->Sync());
    STRATA_RETURN_IF_ERROR(file->Close());
    meta->file_size = builder.FileSize();
    STRATA_RETURN_IF_ERROR(env->SyncDir(dbname));
    // Read it back before anything points at it.
    return tables->Find(meta->number, meta->file_size).status();
  }();

  if (!status.ok()) {
    meta->file_size = 0;
    tables->Evict(meta->number);
    static_cast<void>(env->RemoveFile(file_name));
  }
  return status;
}

}  // namespace strata
