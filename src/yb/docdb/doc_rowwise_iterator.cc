// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/doc_rowwise_iterator.h"
#include <iterator>

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "yb/common/common.pb.h"
#include "yb/common/doc_hybrid_time.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/ql_expr.h"
#include "yb/common/ql_scanspec.h"
#include "yb/common/ql_type.h"
#include "yb/common/ql_value.h"
#include "yb/common/read_hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/docdb/docdb_compaction_context.h"
#include "yb/docdb/docdb_fwd.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_path.h"
#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/doc_read_context.h"
#include "yb/docdb/doc_reader.h"
#include "yb/docdb/doc_scanspec_util.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_types.h"
#include "yb/docdb/expiration.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/primitive_value.h"
#include "yb/docdb/scan_choices.h"
#include "yb/docdb/subdocument.h"
#include "yb/docdb/value.h"
#include "yb/docdb/value_type.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/rocksdb/db/compaction.h"
#include "yb/rocksutil/yb_rocksdb.h"

#include "yb/rocksdb/db.h"

#include "yb/tablet/tablet_metrics.h"

#include "yb/util/debug-util.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/result.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/strongly_typed_bool.h"

using std::string;

DEFINE_RUNTIME_bool(ysql_use_flat_doc_reader, true,
    "Use DocDBTableReader optimization that relies on having at most 1 subkey for YSQL.");

// Primary key update in table group creates copy of existing data
// in same tablet (which uses a single RocksDB instance). During this
// update, we are updating the source schema as well (which is not required).
// Until we figure out the correct approach to handle it, we are disabling
// offset based key decoding by default.
DEFINE_RUNTIME_bool(
    use_offset_based_key_decoding, false, "Use Offset based key decoding for reader.");

namespace yb {
namespace docdb {

DocRowwiseIterator::DocRowwiseIterator(
    const Schema &projection,
    std::reference_wrapper<const DocReadContext> doc_read_context,
    const TransactionOperationContext& txn_op_context,
    const DocDB& doc_db,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    RWOperationCounter* pending_op_counter,
    boost::optional<size_t> end_referenced_key_column_index)
    : projection_(projection),
      doc_read_context_(doc_read_context),
      txn_op_context_(txn_op_context),
      deadline_(deadline),
      read_time_(read_time),
      doc_db_(doc_db),
      pending_op_(pending_op_counter),
      doc_key_offsets_(doc_read_context_.schema.doc_key_offsets()),
      end_referenced_key_column_index_(end_referenced_key_column_index.get_value_or(
          doc_read_context_.schema.num_key_columns())) {
  SetupProjectionSubkeys();
}

DocRowwiseIterator::DocRowwiseIterator(
    std::unique_ptr<Schema> projection,
    std::reference_wrapper<const DocReadContext> doc_read_context,
    const TransactionOperationContext& txn_op_context,
    const DocDB& doc_db,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    RWOperationCounter* pending_op_counter,
    boost::optional<size_t> end_referenced_key_column_index)
    : projection_owner_(std::move(projection)),
      projection_(*projection_owner_),
      doc_read_context_(doc_read_context),
      txn_op_context_(txn_op_context),
      deadline_(deadline),
      read_time_(read_time),
      doc_db_(doc_db),
      pending_op_(pending_op_counter),
      doc_key_offsets_(doc_read_context_.schema.doc_key_offsets()),
      end_referenced_key_column_index_(end_referenced_key_column_index.get_value_or(
          doc_read_context_.schema.num_key_columns())) {
  SetupProjectionSubkeys();
}

DocRowwiseIterator::DocRowwiseIterator(
    std::unique_ptr<Schema> projection,
    std::shared_ptr<DocReadContext> doc_read_context,
    const TransactionOperationContext& txn_op_context,
    const DocDB& doc_db,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    RWOperationCounter* pending_op_counter,
    boost::optional<size_t> end_referenced_key_column_index)
    : projection_owner_(std::move(projection)),
      projection_(*projection_owner_),
      doc_read_context_holder_(std::move(doc_read_context)),
      doc_read_context_(*doc_read_context_holder_),
      txn_op_context_(txn_op_context),
      deadline_(deadline),
      read_time_(read_time),
      doc_db_(doc_db),
      pending_op_(pending_op_counter),
      doc_key_offsets_(doc_read_context_.schema.doc_key_offsets()),
      end_referenced_key_column_index_(end_referenced_key_column_index.get_value_or(
          doc_read_context_.schema.num_key_columns())) {
  SetupProjectionSubkeys();
}

void DocRowwiseIterator::SetupProjectionSubkeys() {
  reader_projection_.reserve(projection_.num_columns() + 1);
  reader_projection_.push_back({KeyEntryValue::kLivenessColumn, nullptr});
  for (size_t i = projection_.num_key_columns(); i < projection_.num_columns(); i++) {
    reader_projection_.push_back({
      .subkey = KeyEntryValue::MakeColumnId(projection_.column_id(i)),
      .type = projection_.column(i).type(),
    });
  }
  std::sort(reader_projection_.begin(), reader_projection_.end(),
            [](const auto& lhs, const auto& rhs) {
    return lhs.subkey < rhs.subkey;
  });
}

DocRowwiseIterator::~DocRowwiseIterator() = default;

void DocRowwiseIterator::CheckInitOnce() {
  if (is_initialized_) {
    YB_LOG_EVERY_N_SECS(DFATAL, 3600)
        << "DocRowwiseIterator(" << this << ") has been already initialized\n"
        << GetStackTrace();
  }
  is_initialized_ = true;
}

void DocRowwiseIterator::Init(TableType table_type, const Slice& sub_doc_key) {
  CheckInitOnce();
  db_iter_ = CreateIntentAwareIterator(
      doc_db_,
      BloomFilterMode::DONT_USE_BLOOM_FILTER,
      boost::none /* user_key_for_filter */,
      rocksdb::kDefaultQueryId,
      txn_op_context_,
      deadline_,
      read_time_);
  if (!sub_doc_key.empty()) {
    row_key_ = sub_doc_key;
  } else {
    DocKeyEncoder(&iter_key_).Schema(doc_read_context_.schema);
    row_key_ = iter_key_;
  }
  row_hash_key_ = row_key_;
  VLOG(3) << __PRETTY_FUNCTION__ << " Seeking to " << row_key_;
  db_iter_->Seek(row_key_);
  row_ready_ = false;
  has_bound_key_ = false;
  table_type_ = table_type;
  if (table_type == TableType::PGSQL_TABLE_TYPE) {
    ConfigureForYsql();
  }
  InitResult();
}

template <class T>
Status DocRowwiseIterator::DoInit(const T& doc_spec) {
  CheckInitOnce();
  InitResult();
  is_forward_scan_ = doc_spec.is_forward_scan();

  VLOG(4) << "Initializing iterator direction: " << (is_forward_scan_ ? "FORWARD" : "BACKWARD");

  auto lower_doc_key = VERIFY_RESULT(doc_spec.LowerBound());
  auto upper_doc_key = VERIFY_RESULT(doc_spec.UpperBound());
  VLOG(4) << "DocKey Bounds " << DocKey::DebugSliceToString(lower_doc_key.AsSlice())
          << ", " << DocKey::DebugSliceToString(upper_doc_key.AsSlice());

  // TODO(bogdan): decide if this is a good enough heuristic for using blooms for scans.
  const bool is_fixed_point_get =
      !lower_doc_key.empty() &&
      VERIFY_RESULT(HashedOrFirstRangeComponentsEqual(lower_doc_key, upper_doc_key));
  const auto mode = is_fixed_point_get ? BloomFilterMode::USE_BLOOM_FILTER
                                       : BloomFilterMode::DONT_USE_BLOOM_FILTER;

  db_iter_ = CreateIntentAwareIterator(
      doc_db_, mode, lower_doc_key.AsSlice(), doc_spec.QueryId(), txn_op_context_,
      deadline_, read_time_, doc_spec.CreateFileFilter());

  row_ready_ = false;

  if (is_forward_scan_) {
    has_bound_key_ = !upper_doc_key.empty();
    if (has_bound_key_) {
      bound_key_ = std::move(upper_doc_key);
      db_iter_->SetUpperbound(bound_key_);
    }
  } else {
    has_bound_key_ = !lower_doc_key.empty();
    if (has_bound_key_) {
      bound_key_ = std::move(lower_doc_key);
    }
  }

  scan_choices_ = ScanChoices::Create(
      doc_read_context_.schema, doc_spec,
      !is_forward_scan_ && has_bound_key_ ? bound_key_ : lower_doc_key,
      is_forward_scan_ && has_bound_key_ ? bound_key_ : upper_doc_key);
  if (is_forward_scan_) {
    VLOG(3) << __PRETTY_FUNCTION__ << " Seeking to " << DocKey::DebugSliceToString(lower_doc_key);
    db_iter_->Seek(lower_doc_key);
  } else {
    // TODO consider adding an operator bool to DocKey to use instead of empty() here.
    if (!upper_doc_key.empty()) {
      db_iter_->PrevDocKey(upper_doc_key);
    } else {
      db_iter_->SeekToLastDocKey();
    }
  }

  return Status::OK();
}

Status DocRowwiseIterator::Init(const YQLScanSpec& spec) {
  table_type_ = spec.client_type() == YQL_CLIENT_CQL ? TableType::YQL_TABLE_TYPE
                                                     : TableType::PGSQL_TABLE_TYPE;
  if (table_type_ == TableType::PGSQL_TABLE_TYPE) {
    ConfigureForYsql();
  }

  return DoInit(spec);
}

void DocRowwiseIterator::ConfigureForYsql() {
  ignore_ttl_ = true;
  if (FLAGS_ysql_use_flat_doc_reader) {
    is_flat_doc_ = IsFlatDoc::kTrue;
  }
}

void DocRowwiseIterator::InitResult() {
  if (is_flat_doc_) {
    result_ = std::vector<QLValuePB>();
    values_ = &std::get<std::vector<QLValuePB>>(result_);
    row_ = nullptr;
  } else {
    result_ = SubDocument();
    row_ = &std::get<SubDocument>(result_);
    values_ = nullptr;
  }
}

Status DocRowwiseIterator::AdvanceIteratorToNextDesiredRow() const {
  if (scan_choices_) {
    if (!IsNextStaticColumn()
        && !scan_choices_->CurrentTargetMatchesKey(row_key_)) {
      return scan_choices_->SeekToCurrentTarget(db_iter_.get());
    }
  }
  if (!is_forward_scan_) {
    VLOG(4) << __PRETTY_FUNCTION__ << " setting as PrevDocKey";
    db_iter_->PrevDocKey(row_key_);
  } else {
    db_iter_->SeekOutOfSubDoc(row_key_);
  }

  return Status::OK();
}

void DocRowwiseIterator::Done() {
  done_ = true;
  if (!doc_db_.metrics || !keys_found_) {
    return;
  }

  doc_db_.metrics->docdb_keys_found->IncrementBy(keys_found_);
  if (obsolete_keys_found_) {
    doc_db_.metrics->docdb_obsolete_keys_found->IncrementBy(obsolete_keys_found_);
    if (obsolete_keys_found_past_cutoff_) {
      doc_db_.metrics->docdb_obsolete_keys_found_past_cutoff->IncrementBy(
          obsolete_keys_found_past_cutoff_);
    }
  }
}

Result<bool> DocRowwiseIterator::HasNext() {
  VLOG(4) << __PRETTY_FUNCTION__ << ", has_next_status_: " << has_next_status_ << ", row_ready_: "
          << row_ready_ << ", done_: " << done_;

  // Repeated HasNext calls (without Skip/NextRow in between) should be idempotent:
  // 1. If a previous call failed we returned the same status.
  // 2. If a row is already available (row_ready_), return true directly.
  // 3. If we finished all target rows for the scan (done_), return false directly.
  RETURN_NOT_OK(has_next_status_);
  if (row_ready_) {
    // If row is ready, then HasNext returns true.
    return true;
  }
  if (done_) {
    return false;
  }

  bool doc_found = false;
  while (!doc_found) {
    if (db_iter_->IsOutOfRecords() || (scan_choices_ && scan_choices_->FinishedWithScanChoices())) {
      Done();
      return false;
    }

    const auto key_data_result = db_iter_->FetchKey();
    if (!key_data_result.ok()) {
      VLOG(4) << __func__ << ", key data: " << key_data_result.status();
      has_next_status_ = key_data_result.status();
      return has_next_status_;
    }
    const auto& key_data = *key_data_result;

    VLOG(4) << "*fetched_key is " << SubDocKey::DebugSliceToString(key_data.key);
    if (debug_dump_) {
      LOG(INFO) << __func__ << ", fetched key: " << SubDocKey::DebugSliceToString(key_data.key)
                << ", " << key_data.key.ToDebugHexString();
    }

    // The iterator is positioned by the previous GetSubDocument call (which places the iterator
    // outside the previous doc_key). Ensure the iterator is pushed forward/backward indeed. We
    // check it here instead of after GetSubDocument() below because we want to avoid the extra
    // expensive FetchKey() call just to fetch and validate the key.
    if (!iter_key_.data().empty() &&
        (is_forward_scan_ ? iter_key_.CompareTo(key_data.key) >= 0
                          : iter_key_.CompareTo(key_data.key) <= 0)) {
      // TODO -- could turn this check off in TPCC?
      has_next_status_ = STATUS_SUBSTITUTE(Corruption, "Infinite loop detected at $0",
                                           FormatSliceAsStr(key_data.key));
      return has_next_status_;
    }
    iter_key_.Reset(key_data.key);
    VLOG(4) << " Current iter_key_ is " << iter_key_;

    // e.g in cotable, row may point outside table bounds.
    if (!DocKeyBelongsTo(iter_key_, doc_read_context_.schema)) {
      Done();
      return false;
    }

    if (FLAGS_use_offset_based_key_decoding && doc_key_offsets_.has_value() &&
        iter_key_.size() >= doc_key_offsets_->doc_key_size) {
      row_hash_key_ = iter_key_.AsSlice().Prefix(doc_key_offsets_->hash_part_size);
      row_key_ = iter_key_.AsSlice().Prefix(doc_key_offsets_->doc_key_size);

      DCHECK(ValidateDocKeyOffsets(iter_key_));
    } else {
      const auto dockey_sizes = DocKey::EncodedHashPartAndDocKeySizes(iter_key_);
      if (!dockey_sizes.ok()) {
        has_next_status_ = dockey_sizes.status();
        return has_next_status_;
      }
      row_hash_key_ = iter_key_.AsSlice().Prefix(dockey_sizes->hash_part_size);
      row_key_ = iter_key_.AsSlice().Prefix(dockey_sizes->doc_key_size);
    }

    if (has_bound_key_ && is_forward_scan_ == (row_key_.compare(bound_key_) >= 0)) {
      Done();
      return false;
    }

    // Prepare the DocKey to get the SubDocument. Trim the DocKey to contain just the primary key.
    Slice doc_key = row_key_;
    VLOG(4) << " sub_doc_key part of iter_key_ is " << DocKey::DebugSliceToString(doc_key);

    bool is_static_column = IsNextStaticColumn();
    if (scan_choices_ && !is_static_column) {
      if (!scan_choices_->CurrentTargetMatchesKey(row_key_)) {
        // We must have seeked past the target key we are looking for (no result) so we can safely
        // skip all scan targets between the current target and row key (excluding row_key_ itself).
        // Update the target key and iterator and call HasNext again to try the next target.
        if (!VERIFY_RESULT(scan_choices_->SkipTargetsUpTo(row_key_))) {
          // SkipTargetsUpTo returns false when it fails to decode the key.
          if (!VERIFY_RESULT(IsColocatedTableTombstoneKey(row_key_))) {
            return STATUS_FORMAT(
                Corruption, "Key $0 is not table tombstone key.", row_key_.ToDebugHexString());
          }
          if (is_forward_scan_) {
            db_iter_->SeekOutOfSubDoc(&iter_key_);
          } else {
            db_iter_->PrevDocKey(row_key_);
          }
          continue;
        }

        // We updated scan target above, if it goes past the row_key_ we will seek again, and
        // process the found key in the next loop.
        if (!scan_choices_->CurrentTargetMatchesKey(row_key_)) {
          RETURN_NOT_OK(scan_choices_->SeekToCurrentTarget(db_iter_.get()));
          continue;
        }
      }
      // We found a match for the target key or a static column, so we move on to getting the
      // SubDocument.
    }

    if (doc_reader_ == nullptr) {
      doc_reader_ = std::make_unique<DocDBTableReader>(
          db_iter_.get(), deadline_, &reader_projection_, table_type_,
          doc_read_context_.schema_packing_storage);
      RETURN_NOT_OK(doc_reader_->UpdateTableTombstoneTime(
          VERIFY_RESULT(GetTableTombstoneTime(doc_key))));
      if (!ignore_ttl_) {
        doc_reader_->SetTableTtl(doc_read_context_.schema);
      }
    }

    if (!is_flat_doc_) {
      DCHECK(row_->type() == ValueEntryType::kObject);
      row_->object_container().clear();
    }

    auto doc_found_res =
        is_flat_doc_ ? doc_reader_->GetFlat(doc_key, values_) : doc_reader_->Get(doc_key, row_);
    if (!doc_found_res.ok()) {
      has_next_status_ = doc_found_res.status();
      return has_next_status_;
    }
    doc_found = *doc_found_res;
    // Use the write_time of the entire row.
    // May lose some precision by not examining write time of every column.
    IncrementKeyFoundStats(!doc_found, key_data.write_time);

    if (scan_choices_ && !is_static_column) {
      has_next_status_ = scan_choices_->DoneWithCurrentTarget();
      RETURN_NOT_OK(has_next_status_);
    }
    has_next_status_ = AdvanceIteratorToNextDesiredRow();
    RETURN_NOT_OK(has_next_status_);
    VLOG(4) << __func__ << ", iter: " << !db_iter_->IsOutOfRecords();
  }
  row_ready_ = true;
  return true;
}

void DocRowwiseIterator::IncrementKeyFoundStats(
    const bool obsolete, const EncodedDocHybridTime& write_time) {
  if (doc_db_.metrics) {
    ++keys_found_;
    if (obsolete) {
      ++obsolete_keys_found_;
      if (history_cutoff_.empty() && doc_db_.retention_policy) {
        // Lazy initialization to avoid extra steps in most cases.
        // It is expected that we will find obsolete keys quite rarely.
        history_cutoff_.Assign(DocHybridTime(doc_db_.retention_policy->ProposedHistoryCutoff()));
      }
      if (write_time < history_cutoff_) {
        // If the obsolete key found was written before the history cutoff, then count
        // record this in addition (since it can be removed via compaction).
        ++obsolete_keys_found_past_cutoff_;
      }
    }
  }
}

string DocRowwiseIterator::ToString() const {
  return "DocRowwiseIterator";
}

namespace {

// Set primary key column values (hashed or range columns) in a QL row value map.
Status SetQLPrimaryKeyColumnValues(const Schema& schema,
                                   const size_t begin_index,
                                   const size_t column_count,
                                   const char* column_type,
                                   const size_t end_referenced_key_column_index,
                                   DocKeyDecoder* decoder,
                                   QLTableRow* table_row) {
  const auto end_group_index = begin_index + column_count;
  SCHECK_LE(
      end_group_index, schema.num_columns(), InvalidArgument,
      Format(
          "$0 primary key columns between positions $1 and $2 go beyond table columns $3",
          column_type, begin_index, begin_index + column_count - 1, schema.num_columns()));
  SCHECK_LE(
      end_referenced_key_column_index, schema.num_key_columns(), InvalidArgument,
      Format(
          "End reference key column index $0 is higher than num of key columns in schema $1",
          end_referenced_key_column_index, schema.num_key_columns()));

  KeyEntryValue key_entry_value;
  size_t col_idx = begin_index;
  for (; col_idx < std::min(end_group_index, end_referenced_key_column_index); ++col_idx) {
    const auto ql_type = schema.column(col_idx).type();
    QLTableColumn& column = table_row->AllocColumn(schema.column_id(col_idx));
    RETURN_NOT_OK(decoder->DecodeKeyEntryValue(&key_entry_value));
    key_entry_value.ToQLValuePB(ql_type, &column.value);
  }

  return col_idx == end_group_index ? decoder->ConsumeGroupEnd() : Status::OK();
}

} // namespace

void DocRowwiseIterator::SkipRow() {
  row_ready_ = false;
}

Result<HybridTime> DocRowwiseIterator::RestartReadHt() {
  return db_iter_->RestartReadHt();
}

HybridTime DocRowwiseIterator::TEST_MaxSeenHt() {
  return db_iter_->TEST_MaxSeenHt();
}

bool DocRowwiseIterator::IsNextStaticColumn() const {
  return doc_read_context_.schema.has_statics() && row_hash_key_.end() + 1 == row_key_.end();
}

Status DocRowwiseIterator::DoNextRow(
    boost::optional<const Schema&> projection_opt, QLTableRow* table_row) {
  VLOG(4) << __PRETTY_FUNCTION__;

  if (PREDICT_FALSE(done_)) {
    return STATUS(NotFound, "end of iter");
  }

  // Ensure row is ready to be read. HasNext() must be called before reading the first row, or
  // again after the previous row has been read or skipped.
  if (!row_ready_) {
    return STATUS(InternalError, "next row has not be prepared for reading");
  }

  if (end_referenced_key_column_index_ > 0) {
    DocKeyDecoder decoder(row_key_);
    RETURN_NOT_OK(decoder.DecodeCotableId());
    RETURN_NOT_OK(decoder.DecodeColocationId());
    bool has_hash_components = VERIFY_RESULT(decoder.DecodeHashCode());

    // Populate the key column values from the doc key. The key column values in doc key were
    // written in the same order as in the table schema (see DocKeyFromQLKey). If the range columns
    // are present, read them also.
    if (has_hash_components) {
      RETURN_NOT_OK(SetQLPrimaryKeyColumnValues(
          doc_read_context_.schema, 0, doc_read_context_.schema.num_hash_key_columns(), "hash",
          end_referenced_key_column_index_, &decoder, table_row));
    }
    if (!decoder.GroupEnded()) {
      RETURN_NOT_OK(SetQLPrimaryKeyColumnValues(
          doc_read_context_.schema, doc_read_context_.schema.num_hash_key_columns(),
          doc_read_context_.schema.num_range_key_columns(), "range",
          end_referenced_key_column_index_, &decoder, table_row));
    }
  }

  const auto& projection = projection_opt.get_value_or(schema());

  DVLOG_WITH_FUNC(4) << "table_row: " << AsString(*table_row);
  if (is_flat_doc_) {
    DVLOG_WITH_FUNC(4) << "values: " << AsString(*values_);
    for (size_t column_reader_idx = 0; column_reader_idx < reader_projection_.size();
         ++column_reader_idx) {
      const auto& column_id = reader_projection_[column_reader_idx].subkey.GetColumnId();
      if (column_id.rep() == static_cast<ColumnIdRep>(SystemColumnIds::kLivenessColumn)) {
        // This has been already added by SetQLPrimaryKeyColumnValues, no need to overwrite.
        continue;
      }
      const auto column_projection_idx = projection.find_column_by_id(column_id);
      DVLOG_WITH_FUNC(4) << "column_reader_idx: " << column_reader_idx
                         << " column_id: " << column_id << " column: "
                         << (column_projection_idx == Schema::kColumnNotFound
                                 ? "not found"
                                 : AsString(projection.column(column_projection_idx)));
      if (column_projection_idx == Schema::kColumnNotFound) {
        // TODO: potentially could be optimized to not iterate over columns in reader that
        // we don't need here, but this is only possible in YCQL as of 2022-12-27.
        // And for YCQL we don't use IsFlatDoc::kTrue mode, because for YCQL we can have nested
        // SubDocuments.
        continue;
      }
      const auto ql_type = projection.column(column_projection_idx).type();
      table_row->AllocColumn(column_id, std::move((*values_)[column_reader_idx]));
    }
  } else {
    DVLOG_WITH_FUNC(4) << "subdocument: " << AsString(*row_);
    for (size_t i = projection.num_key_columns(); i < projection.num_columns(); i++) {
      const auto& column_id = projection.column_id(i);
      const auto ql_type = projection.column(i).type();
      const SubDocument* column_value = row_->GetChild(KeyEntryValue::MakeColumnId(column_id));
      if (column_value != nullptr) {
        QLTableColumn& column = table_row->AllocColumn(column_id);
        column_value->ToQLValuePB(ql_type, &column.value);
        column.ttl_seconds = column_value->GetTtl();
        if (column_value->IsWriteTimeSet()) {
          column.write_time = column_value->GetWriteTime();
        }
      }
    }
  }

  VLOG_WITH_FUNC(4) << "Returning row: " << table_row->ToString();

  row_ready_ = false;
  return Status::OK();
}

bool DocRowwiseIterator::LivenessColumnExists() const {
  if (is_flat_doc_) {
    return !IsNull((*values_)[0]);
  }
  const SubDocument* subdoc = row_->GetChild(KeyEntryValue::kLivenessColumn);
  return subdoc != nullptr && subdoc->value_type() != ValueEntryType::kInvalid;
}

Status DocRowwiseIterator::GetNextReadSubDocKey(SubDocKey* sub_doc_key) {
  if (db_iter_ == nullptr) {
    return STATUS(Corruption, "Iterator not initialized.");
  }

  // There are no more rows to fetch, so no next SubDocKey to read.
  if (!VERIFY_RESULT(HasNext())) {
    DVLOG(3) << "No Next SubDocKey";
    return Status::OK();
  }

  DocKey doc_key;
  RETURN_NOT_OK(doc_key.FullyDecodeFrom(row_key_));
  *sub_doc_key = SubDocKey(doc_key, read_time_.read);
  DVLOG(3) << "Next SubDocKey: " << sub_doc_key->ToString();
  return Status::OK();
}

Result<Slice> DocRowwiseIterator::GetTupleId() const {
  // Return tuple id without cotable id / colocation id if any.
  Slice tuple_id = row_key_;
  if (tuple_id.starts_with(KeyEntryTypeAsChar::kTableId)) {
    tuple_id.remove_prefix(1 + kUuidSize);
  } else if (tuple_id.starts_with(KeyEntryTypeAsChar::kColocationId)) {
    tuple_id.remove_prefix(1 + sizeof(ColocationId));
  }
  return tuple_id;
}

Result<bool> DocRowwiseIterator::SeekTuple(const Slice& tuple_id) {
  // If cotable id / colocation id is present in the table schema, then
  // we need to prepend it in the tuple key to seek.
  if (doc_read_context_.schema.has_cotable_id() || doc_read_context_.schema.has_colocation_id()) {
    uint32_t size = doc_read_context_.schema.has_colocation_id() ? sizeof(ColocationId) : kUuidSize;
    if (!tuple_key_) {
      tuple_key_.emplace();
      tuple_key_->Reserve(1 + size + tuple_id.size());

      if (doc_read_context_.schema.has_cotable_id()) {
        std::string bytes;
        doc_read_context_.schema.cotable_id().EncodeToComparable(&bytes);
        tuple_key_->AppendKeyEntryType(KeyEntryType::kTableId);
        tuple_key_->AppendRawBytes(bytes);
      } else {
        tuple_key_->AppendKeyEntryType(KeyEntryType::kColocationId);
        tuple_key_->AppendUInt32(doc_read_context_.schema.colocation_id());
      }
    } else {
      tuple_key_->Truncate(1 + size);
    }
    tuple_key_->AppendRawBytes(tuple_id);
    db_iter_->Seek(*tuple_key_);
  } else {
    db_iter_->Seek(tuple_id);
  }

  iter_key_.Clear();
  row_ready_ = false;

  return VERIFY_RESULT(HasNext()) && VERIFY_RESULT(GetTupleId()) == tuple_id;
}

bool DocRowwiseIterator::ValidateDocKeyOffsets(const Slice& iter_key) {
  const auto dockey_sizes = DocKey::EncodedHashPartAndDocKeySizes(iter_key_);
  if (!dockey_sizes.ok()) {
    LOG(INFO) << "Failed to decode the DocKey: " << dockey_sizes.status();
    return false;
  }

  DCHECK_EQ(dockey_sizes->hash_part_size, doc_key_offsets_->hash_part_size);
  DCHECK_EQ(dockey_sizes->doc_key_size, doc_key_offsets_->doc_key_size);

  return true;
}

}  // namespace docdb
}  // namespace yb
