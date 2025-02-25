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

#include "yb/docdb/doc_reader.h"

#include <string>
#include <vector>

#include "yb/common/doc_hybrid_time.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/ql_type.h"
#include "yb/common/transaction.h"

#include "yb/docdb/docdb_fwd.h"
#include "yb/docdb/shared_lock_manager_fwd.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/schema_packing.h"
#include "yb/docdb/subdocument.h"
#include "yb/docdb/value.h"
#include "yb/docdb/value_type.h"

#include "yb/util/fast_varint.h"
#include "yb/util/logging.h"
#include "yb/util/monotime.h"
#include "yb/util/result.h"
#include "yb/util/status.h"

using std::vector;

using yb::HybridTime;

namespace yb {
namespace docdb {

namespace {

constexpr int64_t kNothingFound = -1;

YB_STRONGLY_TYPED_BOOL(CheckExistOnly);

// The struct that stores encoded doc hybrid time and decode it on demand.
class LazyDocHybridTime {
 public:
  void Assign(const EncodedDocHybridTime& value) {
    encoded_ = value;
    decoded_ = DocHybridTime();
  }

  const EncodedDocHybridTime& encoded() const {
    return encoded_;
  }

  EncodedDocHybridTime* RawPtr() {
    decoded_ = DocHybridTime();
    return &encoded_;
  }

  Result<DocHybridTime> decoded() const {
    if (!decoded_.is_valid()) {
      decoded_ = VERIFY_RESULT(encoded_.Decode());
    }
    return decoded_;
  }

  std::string ToString() const {
    return encoded_.ToString();
  }

 private:
  EncodedDocHybridTime encoded_;
  mutable DocHybridTime decoded_;
};

// Shared information about packed row. I.e. common for all columns in this row.
struct PackedRowData {
  LazyDocHybridTime doc_ht;
  ValueControlFields control_fields;
};

struct PackedColumnData {
  const PackedRowData* row = nullptr;
  Slice encoded_value;
  bool liveness_column;

  explicit operator bool() const {
    return row != nullptr;
  }
};

Expiration GetNewExpiration(
    const Expiration& parent_exp, MonoDelta ttl, HybridTime new_write_ht) {
  Expiration new_exp = parent_exp;
  // We may need to update the TTL in individual columns.
  if (new_write_ht >= new_exp.write_ht) {
    // We want to keep the default TTL otherwise.
    if (ttl != ValueControlFields::kMaxTtl) {
      new_exp.write_ht = new_write_ht;
      new_exp.ttl = ttl;
    } else if (new_exp.ttl.IsNegative()) {
      new_exp.ttl = -new_exp.ttl;
    }
  }

  // If the hybrid time is kMin, then we must be using default TTL.
  if (new_exp.write_ht == HybridTime::kMin) {
    new_exp.write_ht = new_write_ht;
  }

  return new_exp;
}

int64_t GetTtlRemainingSeconds(
    HybridTime read_time, HybridTime ttl_write_time, const Expiration& expiration) {
  if (!expiration) {
    return -1;
  }

  int64_t expiration_time_us =
      ttl_write_time.GetPhysicalValueMicros() + expiration.ttl.ToMicroseconds();
  int64_t remaining_us = expiration_time_us - read_time.GetPhysicalValueMicros();
  if (remaining_us <= 0) {
    return 0;
  }
  return remaining_us / MonoTime::kMicrosecondsPerSecond;
}

Slice NullSlice() {
  static char null_column_type = ValueEntryTypeAsChar::kNullLow;
  return Slice(&null_column_type, sizeof(null_column_type));
}

} // namespace

Result<DocHybridTime> GetTableTombstoneTime(
    const Slice& root_doc_key, const DocDB& doc_db,
    const TransactionOperationContext& txn_op_context,
    CoarseTimePoint deadline, const ReadHybridTime& read_time) {
  if (root_doc_key[0] == KeyEntryTypeAsChar::kColocationId ||
      root_doc_key[0] == KeyEntryTypeAsChar::kTableId) {
    DocKey table_id;
    RETURN_NOT_OK(table_id.DecodeFrom(root_doc_key, DocKeyPart::kUpToId));

    auto table_id_encoded = table_id.Encode();
    auto iter = CreateIntentAwareIterator(
        doc_db, BloomFilterMode::USE_BLOOM_FILTER, table_id_encoded.AsSlice(),
        rocksdb::kDefaultQueryId, txn_op_context, deadline, read_time);
    iter->Seek(table_id_encoded);

    Slice value;
    EncodedDocHybridTime doc_ht(EncodedDocHybridTime::kMin);
    RETURN_NOT_OK(iter->FindLatestRecord(table_id_encoded, &doc_ht, &value));
    if (VERIFY_RESULT(Value::IsTombstoned(value))) {
      SCHECK(!doc_ht.empty(), Corruption, "Invalid hybrid time for table tombstone");
      return doc_ht.Decode();
    }
  }
  return DocHybridTime::kInvalid;
}

  // TODO(dtxn) scan through all involved transactions first to cache statuses in a batch,
  // so during building subdocument we don't need to request them one by one.
  // TODO(dtxn) we need to restart read with scan_ht = commit_ht if some transaction was committed
  // at time commit_ht within [scan_ht; read_request_time + max_clock_skew). Also we need
  // to wait until time scan_ht = commit_ht passed.
  // TODO(dtxn) for each scanned key (and its subkeys) we need to avoid *new* values committed at
  // ht <= scan_ht (or just ht < scan_ht?)
  // Question: what will break if we allow later commit at ht <= scan_ht ? Need to write down
  // detailed example.

Result<std::optional<SubDocument>> TEST_GetSubDocument(
    const Slice& sub_doc_key,
    const DocDB& doc_db,
    const rocksdb::QueryId query_id,
    const TransactionOperationContext& txn_op_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    const ReaderProjection* projection) {
  auto iter = CreateIntentAwareIterator(
      doc_db, BloomFilterMode::USE_BLOOM_FILTER, sub_doc_key, query_id,
      txn_op_context, deadline, read_time);
  DOCDB_DEBUG_LOG("GetSubDocument for key $0 @ $1", sub_doc_key.ToDebugHexString(),
                  iter->read_time().ToString());
  iter->SeekToLastDocKey();

  iter->Seek(sub_doc_key);
  if (iter->IsOutOfRecords()) {
    return std::nullopt;
  }
  auto fetched = VERIFY_RESULT(iter->FetchKey());
  if (!fetched.key.starts_with(sub_doc_key)) {
    return std::nullopt;
  }

  SchemaPackingStorage schema_packing_storage(TableType::YQL_TABLE_TYPE);
  DocDBTableReader doc_reader(
      iter.get(), deadline, projection, TableType::YQL_TABLE_TYPE, schema_packing_storage);
  RETURN_NOT_OK(doc_reader.UpdateTableTombstoneTime(VERIFY_RESULT(GetTableTombstoneTime(
      sub_doc_key, doc_db, txn_op_context, deadline, read_time))));
  SubDocument result;
  if (VERIFY_RESULT(doc_reader.Get(sub_doc_key, &result))) {
    return result;
  }
  return std::nullopt;
}

DocDBTableReader::DocDBTableReader(
    IntentAwareIterator* iter, CoarseTimePoint deadline,
    const ReaderProjection* projection,
    TableType table_type,
    std::reference_wrapper<const SchemaPackingStorage> schema_packing_storage)
    : iter_(iter),
      deadline_info_(deadline),
      projection_(projection),
      table_type_(table_type),
      schema_packing_storage_(schema_packing_storage) {
  if (projection_) {
    auto projection_size = projection_->size();
    encoded_projection_.resize(projection_size);
    for (size_t i = 0; i != projection_size; ++i) {
      (*projection_)[i].subkey.AppendToKey(&encoded_projection_[i]);
    }
  }
  VLOG_WITH_FUNC(4)
      << "Projection: " << AsString(projection_) << ", read time: " << iter_->read_time();
}

void DocDBTableReader::SetTableTtl(const Schema& table_schema) {
  table_expiration_ = Expiration(TableTTL(table_schema));
}

Status DocDBTableReader::UpdateTableTombstoneTime(DocHybridTime doc_ht) {
  if (doc_ht.is_valid()) {
    table_tombstone_time_.Assign(doc_ht);
  }
  return Status::OK();
}

// Scan state entry. See state_ description below for details.
struct StateEntry {
  KeyBytes key_entry; // Represents the part of the key that is related to this state entry.
  LazyDocHybridTime write_time;
  Expiration expiration;
  KeyEntryValue key_value; // Decoded key_entry.
  SubDocument* out;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(write_time, expiration, key_value);
  }
};

// Returns true if value is NOT tombstone, false if value is tombstone.
Result<bool> TryDecodeValueOnly(
    const Slice& value_slice, const QLTypePtr& ql_type, QLValuePB* out) {
  if (DecodeValueEntryType(value_slice) == ValueEntryType::kTombstone) {
    return false;
  }
  if (out) {
    if (ql_type) {
      RETURN_NOT_OK(PrimitiveValue::DecodeToQLValuePB(value_slice, ql_type, out));
    } else {
      out->Clear();
    }
  }
  return true;
}

Result<bool> TryDecodeValueOnly(
    const Slice& value_slice, const QLTypePtr& ql_type, PrimitiveValue* out) {
  if (DecodeValueEntryType(value_slice) == ValueEntryType::kTombstone) {
    if (out) {
      *out = PrimitiveValue::kTombstone;
    }
    return false;
  }
  if (out) {
    RETURN_NOT_OK(out->DecodeFromValue(value_slice));
  }
  return true;
}

// Implements main logic in the reader.
// Used keep scan state and avoid passing it between methods.
// It is less performant than FlatGetHelper, but handles the general case of nested documents.
// Not used for YSQL if FLAGS_ysql_use_flat_doc_reader is true.
template <bool is_flat_doc, bool ysql>
class DocDBTableReader::GetHelperBase {
 public:
  GetHelperBase(DocDBTableReader* reader, const Slice& root_doc_key)
      : reader_(*reader), root_doc_key_(root_doc_key) {}

  virtual ~GetHelperBase() {}

 protected:
  Result<bool> DoRun(Expiration* root_expiration, LazyDocHybridTime* root_write_time) {
    IntentAwareIteratorPrefixScope prefix_scope(root_doc_key_, reader_.iter_);

    RETURN_NOT_OK(Prepare(root_expiration, root_write_time));

    // projection could be null in tests only.
    if (reader_.projection_) {
      if (reader_.projection_->empty()) {
        packed_column_data_ = GetPackedColumn(KeyEntryValue::kLivenessColumn.GetColumnId());
        RETURN_NOT_OK(Scan(CheckExistOnly::kTrue));
        return Found();
      }
      UpdatePackedColumnData();
    } else {
      cannot_scan_columns_ = true;
    }
    RETURN_NOT_OK(Scan(CheckExistOnly::kFalse));

    if (last_found_ >= 0 ||
        CheckForRootValue()) { // Could only happen in tests.
      return true;
    }

    if (ysql || // YSQL always has liveness column, and it is always present in projection.
        !reader_.projection_) { // Could only happen in tests.
      return false;
    }

    reader_.iter_->Seek(root_doc_key_);
    RETURN_NOT_OK(Scan(CheckExistOnly::kTrue));
    if (Found()) {
      EmptyDocFound();
      return true;
    }

    return false;
  }

  virtual void EmptyDocFound() = 0;

  // Whether document was found or not.
  virtual bool Found() const = 0;

  // Scans DocDB for entries related to root_doc_key_.
  // Iterator should already point to the first such entry.
  // Changes nearly all internal state fields.
  Status Scan(CheckExistOnly check_exist_only) {
    while (!reader_.iter_->IsOutOfRecords()) {
      if (reader_.deadline_info_.CheckAndSetDeadlinePassed()) {
        return STATUS(Expired, "Deadline for query passed");
      }

      if (!VERIFY_RESULT(HandleRecord(check_exist_only))) {
        return Status::OK();
      }
    }
    if (!check_exist_only && !cannot_scan_columns_) {
      while (VERIFY_RESULT(NextColumn())) {}
    }
    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "(" << check_exist_only << "), found: " << last_found_ << ", column index: "
        << column_index_ << ", finished: " << reader_.iter_->IsOutOfRecords() << ", "
        << GetResultAsString();
    return Status::OK();
  }

  virtual std::string GetResultAsString() const = 0;

  Result<bool> HandleRecord(CheckExistOnly check_exist_only) {
    auto key_result = VERIFY_RESULT(reader_.iter_->FetchKey());
    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "check_exist_only: " << check_exist_only << ", key: "
        << SubDocKey::DebugSliceToString(key_result.key) << ", write time: "
        << key_result.write_time.ToString() << ", value: "
        << reader_.iter_->value().ToDebugHexString();
    DCHECK(key_result.key.starts_with(root_doc_key_));
    auto subkeys = key_result.key.WithoutPrefix(root_doc_key_.size());

    return DoHandleRecord(key_result, subkeys, check_exist_only);
  }

  Result<bool> DoHandleRecord(
      const FetchKeyResult& key_result, const Slice& subkeys, CheckExistOnly check_exist_only) {
    if (!check_exist_only && reader_.projection_) {
      auto projection_column_encoded_key_prefix =
          reader_.encoded_projection_[column_index_].AsSlice();
      int compare_result = subkeys.compare_prefix(projection_column_encoded_key_prefix);
      DVLOG_WITH_PREFIX_AND_FUNC(4) << "Subkeys: " << subkeys.ToDebugHexString()
                                    << ", column: " << (*reader_.projection_)[column_index_].subkey
                                    << ", compare_result: " << compare_result;
      if (compare_result < 0) {
        SeekProjectionColumn();
        return true;
      }

      if (compare_result > 0) {
        if (!VERIFY_RESULT(NextColumn())) {
          return false;
        }

        return DoHandleRecord(key_result, subkeys, check_exist_only);
      }

      if (is_flat_doc) {
        SCHECK_EQ(
            subkeys.size(), projection_column_encoded_key_prefix.size(), IllegalState,
            "DocDBTableReader::FlatGetHelper supports at most 1 subkey");
      }
    }

    if (VERIFY_RESULT(ProcessEntry(
            subkeys, reader_.iter_->value(), key_result.write_time, check_exist_only))) {
      packed_column_data_.row = nullptr;
    }
    if (check_exist_only && Found()) {
      return false;
    }
    reader_.iter_->SeekPastSubKey(key_result.key);
    return true;
  }

  // We are not yet reached next projection subkey, seek to it.
  void SeekProjectionColumn() {
    if (root_key_entry_->empty()) {
      // Lazily fill root doc key buffer.
      root_key_entry_->AppendRawBytes(root_doc_key_);
    }
    root_key_entry_->AppendRawBytes(
        reader_.encoded_projection_[column_index_].AsSlice());
    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "Seek next column: " << SubDocKey::DebugSliceToString(*root_key_entry_);
    reader_.iter_->SeekForward(root_key_entry_);
    root_key_entry_->Truncate(root_doc_key_.size());
  }

  // Process DB entry.
  // Return true if entry value was accepted.
  virtual Result<bool> ProcessEntry(
      Slice subkeys, Slice value_slice, const EncodedDocHybridTime& write_time,
      CheckExistOnly check_exist_only) = 0;

  Result<bool> NextColumn() {
    if (VERIFY_RESULT(DecodePackedColumn())) {
      last_found_ = column_index_;
    } else if (last_found_ < static_cast<int64_t>(column_index_)) {
      NoValueForColumnIndex();
    }
    ++column_index_;
    if (column_index_ == reader_.projection_->size()) {
      return false;
    }
    UpdatePackedColumnData();
    return true;
  }

  virtual Result<bool> DecodePackedColumn() = 0;

  virtual void NoValueForColumnIndex() = 0;

  template <class GetValueAddressFunc>
  Result<bool> DoDecodePackedColumn(
      const Expiration& parent_exp, GetValueAddressFunc get_value_address) {
    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "Packed data " << (packed_column_data_ ? "present" : "missing") << ", expiration: "
        << AsString(parent_exp);
    if (!packed_column_data_) {
      return false;
    }
    Slice value = packed_column_data_.encoded_value;
    if (ysql) {
      // Remove buggy intent_doc_ht from start of the column. See #16650 for details.
      if (value.TryConsumeByte(KeyEntryTypeAsChar::kHybridTime)) {
        RETURN_NOT_OK(DocHybridTime::EncodedFromStart(&value));
      }
      return TryDecodeValueOnly(
          value, (*reader_.projection_)[column_index_].type, get_value_address());
    }
    ValueControlFields control_fields;
    if (packed_column_data_.liveness_column) {
      control_fields = packed_column_data_.row->control_fields;
    } else {
      control_fields = VERIFY_RESULT(ValueControlFields::Decode(&value));
    }
    const auto& write_time = packed_column_data_.row->doc_ht;
    const auto expiration = GetNewExpiration(
          parent_exp, control_fields.ttl, VERIFY_RESULT(write_time.decoded()).hybrid_time());
    if (IsObsolete(expiration)) {
      return false;
    }
    return TryDecodeValue(
        control_fields.has_timestamp()
            ? control_fields.timestamp
            : packed_column_data_.row->control_fields.timestamp,
        write_time, expiration, value, get_value_address());
  }

  // Updates information about the current column packed data.
  // Before calling, all fields should have correct values, especially column_index_ that points
  // to the current column in projection.
  void UpdatePackedColumnData() {
    auto& column = (*reader_.projection_)[column_index_].subkey;
    if (column.IsColumnId()) {
      packed_column_data_ = GetPackedColumn(column.GetColumnId());
    } else {
      // Used in tests only.
      packed_column_data_.row = nullptr;
    }
  }

  virtual Status SetRootValue(ValueEntryType row_value_type, const Slice& row_value) = 0;

  virtual bool CheckForRootValue() = 0;

  Status Prepare(Expiration* root_expiration, LazyDocHybridTime* root_write_time) {
    DVLOG_WITH_PREFIX_AND_FUNC(4) << "Pos: " << reader_.iter_->DebugPosToString();

    root_key_entry_->AppendRawBytes(root_doc_key_);

    auto key_result = VERIFY_RESULT(reader_.iter_->FetchKey());
    DCHECK(key_result.key.starts_with(root_doc_key_));

    Slice value;
    LazyDocHybridTime doc_ht;
    doc_ht.Assign(reader_.table_tombstone_time_);
    if (root_doc_key_.size() == key_result.key.size() &&
        key_result.write_time >= doc_ht.encoded()) {
      doc_ht.Assign(key_result.write_time);
      value = reader_.iter_->value();
    }

    auto control_fields = VERIFY_RESULT(ValueControlFields::Decode(&value));

    auto value_type = DecodeValueEntryType(value);
    if (value_type == ValueEntryType::kPackedRow) {
      value.consume_byte();
      schema_packing_ = &VERIFY_RESULT(reader_.schema_packing_storage_.GetPacking(&value)).get();
      packed_row_.Assign(value);
      packed_row_data_.doc_ht = doc_ht;
      packed_row_data_.control_fields = control_fields;
      if (TtlCheckRequired()) {
        *root_expiration = GetNewExpiration(
            *root_expiration, ValueControlFields::kMaxTtl,
            VERIFY_RESULT(doc_ht.decoded()).hybrid_time());
      }
    } else if (value_type != ValueEntryType::kTombstone && value_type != ValueEntryType::kInvalid) {
      // Used in tests only
      RETURN_NOT_OK(SetRootValue(value_type, value));
    }

    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "Write time: " << doc_ht.ToString() << ", control fields: " << control_fields.ToString();
    *root_write_time = doc_ht;
    return Status::OK();
  }

  PackedColumnData GetPackedColumn(ColumnId column_id) {
    if (!schema_packing_) {
      // Actual for tests only.
      return PackedColumnData();
    }

    if (column_id == KeyEntryValue::kLivenessColumn.GetColumnId()) {
      DVLOG_WITH_PREFIX_AND_FUNC(4) << "Packed row for liveness column";
      return PackedColumnData {
        .row = &packed_row_data_,
        .encoded_value = NullSlice(),
        .liveness_column = true,
      };
    }

    auto slice = schema_packing_->GetValue(column_id, packed_row_.AsSlice());
    if (!slice) {
      DVLOG_WITH_PREFIX_AND_FUNC(4) << "No packed row data for " << column_id;
      return PackedColumnData();
    }

    DVLOG_WITH_PREFIX_AND_FUNC(4) << "Packed row " << column_id << ": "
                                  << slice->ToDebugHexString();
    return PackedColumnData {
      .row = &packed_row_data_,
      .encoded_value = slice->empty() ? NullSlice() : *slice,
      .liveness_column = false,
    };
  }

  Result<bool> TryDecodeValue(
      UserTimeMicros timestamp, const LazyDocHybridTime& write_time, const Expiration& expiration,
      const Slice& value_slice, PrimitiveValue* out) {
    auto has_value = VERIFY_RESULT(TryDecodeValueOnly(value_slice, /* ql_type= */ nullptr, out));
    if (has_value && out) {
      auto write_ht = VERIFY_RESULT(write_time.decoded()).hybrid_time();
      if (timestamp != ValueControlFields::kInvalidTimestamp) {
        out->SetWriteTime(timestamp);
      } else {
        out->SetWriteTime(write_ht.GetPhysicalValueMicros());
      }
      out->SetTtl(GetTtlRemainingSeconds(reader_.iter_->read_time().read, write_ht, expiration));
    }

    return has_value;
  }

  Result<bool> TryDecodeValue(
      UserTimeMicros timestamp, const LazyDocHybridTime& write_time, const Expiration& expiration,
      const Slice& value_slice, QLValuePB* out) {
    return TryDecodeValueOnly(
        value_slice, (*reader_.projection_)[column_index_].type, out);
  }

  bool IsObsolete(const Expiration& expiration) {
    if (expiration.ttl == ValueControlFields::kMaxTtl) {
      return false;
    }

    return HasExpiredTTL(expiration.write_ht, expiration.ttl, reader_.iter_->read_time().read);
  }

  std::string LogPrefix() const {
    return DocKey::DebugSliceToString(root_doc_key_) + ": ";
  }

  static constexpr bool TtlCheckRequired() {
    // TODO(scanperf) also avoid checking TTL for YCQL tables w/o TTL.
    return !ysql;
  }

  DocDBTableReader& reader_;
  const Slice root_doc_key_;
  // Pointer to root key entry that is owned by subclass. Can't be nullptr.
  KeyBytes* root_key_entry_;

  // Packed row related fields. Not changed after initialization.
  ValueBuffer packed_row_;
  PackedRowData packed_row_data_;
  const SchemaPacking* schema_packing_ = nullptr;

  // If packed row is found, this field contains data related to currently scanned column.
  PackedColumnData packed_column_data_;

  // Index of the current column in projection.
  size_t column_index_ = 0;

  // Index of the last found column in projection.
  int64_t last_found_ = kNothingFound;

  // Set to true when there is no projection or root is not an object (that only can happen when
  // called from the tests).
  bool cannot_scan_columns_ = false;
};

// Implements main logic in the reader.
// Used keep scan state and avoid passing it between methods.
class DocDBTableReader::GetHelper :
    public DocDBTableReader::GetHelperBase</* is_flat_doc = */ false, /* ysql = */ false> {
 public:
  using Base = DocDBTableReader::GetHelperBase</* is_flat_doc = */ false, /* ysql = */ false>;

  GetHelper(DocDBTableReader* reader, const Slice& root_doc_key, SubDocument* result)
      : Base(reader, root_doc_key), result_(*result) {
    state_.emplace_back(StateEntry {
      .key_entry = KeyBytes(),
      .write_time = LazyDocHybridTime(),
      .expiration = reader_.table_expiration_,
      .key_value = {},
      .out = &result_,
    });
    root_key_entry_ = &state_.front().key_entry;
  }

  Result<bool> Run() {
    auto& root = state_.front();
    return DoRun(&root.expiration, &root.write_time);
  }

  void EmptyDocFound() override {
    for (const auto& key : *reader_.projection_) {
      result_.AllocateChild(key.subkey);
    }
  }

  bool Found() const override {
    return last_found_ >= 0 || has_root_value_;
  }

  std::string GetResultAsString() const override { return AsString(result_); }

 private:
  Result<bool> ProcessEntry(
      Slice subkeys, Slice value_slice, const EncodedDocHybridTime& write_time,
      CheckExistOnly check_exist_only) override {
    subkeys = CleanupState(subkeys);
    if (state_.back().write_time.encoded() >= write_time) {
      DVLOG_WITH_PREFIX_AND_FUNC(4)
          << "State: " << AsString(state_) << ", write_time: " << write_time;
      return false;
    }
    auto control_fields = VERIFY_RESULT(ValueControlFields::Decode(&value_slice));
    RETURN_NOT_OK(AllocateNewStateEntries(
        subkeys, write_time, check_exist_only, control_fields.ttl));
    return ApplyEntryValue(value_slice, control_fields, check_exist_only);
  }

  // Removes state_ elements that are that are not related to the passed in subkeys.
  // Returns remaining part of subkeys, that not represented in state_.
  Slice CleanupState(Slice subkeys) {
    for (size_t i = 1; i != state_.size(); ++i) {
      if (!subkeys.starts_with(state_[i].key_entry)) {
        state_.resize(i);
        break;
      }
      subkeys.remove_prefix(state_[i].key_entry.size());
    }
    return subkeys;
  }

  Status AllocateNewStateEntries(
      Slice subkeys, const EncodedDocHybridTime& write_time, CheckExistOnly check_exist_only,
      MonoDelta ttl) {
    LazyDocHybridTime lazy_write_time;
    lazy_write_time.Assign(write_time);
    while (!subkeys.empty()) {
      auto start = subkeys.data();
      state_.emplace_back();
      auto& parent = state_[state_.size() - 2];
      auto& entry = state_.back();
      RETURN_NOT_OK(entry.key_value.DecodeFromKey(&subkeys));
      entry.key_entry.AppendRawBytes(Slice(start, subkeys.data()));
      entry.write_time = subkeys.empty() ? lazy_write_time : parent.write_time;
      entry.out = check_exist_only ? nullptr : &parent.out->AllocateChild(entry.key_value);
      if (TtlCheckRequired()) {
        entry.expiration = GetNewExpiration(
            parent.expiration, ttl, VERIFY_RESULT(entry.write_time.decoded()).hybrid_time());
      }
    }
    return Status::OK();
  }

  // Return true if entry value was accepted.
  Result<bool> ApplyEntryValue(
      const Slice& value_slice, const ValueControlFields& control_fields,
      CheckExistOnly check_exist_only) {
    auto& current = state_.back();
    DVLOG_WITH_PREFIX_AND_FUNC(4)
        << "State: " << AsString(state_) << ", value: " << value_slice.ToDebugHexString()
        << ", obsolete: " << IsObsolete(current.expiration);

    if (!IsObsolete(current.expiration)) {
      if (VERIFY_RESULT(TryDecodeValue(
              control_fields.timestamp, current.write_time, current.expiration, value_slice,
              current.out))) {
        last_found_ = column_index_;
        return true;
      }
    }

    // When projection is specified we should always report projection columns, even when they are
    // nulls.
    if (!check_exist_only && state_.size() > (reader_.projection_ ? 2 : 1)) {
      state_[state_.size() - 2].out->DeleteChild(current.key_value);
    }
    return true;
  }

  void NoValueForColumnIndex() override {
    DVLOG_WITH_PREFIX_AND_FUNC(4) << Format(
        "Did not have value for column_index $0 ($1), allocate invalid value for it, will be "
        "converted to null value by KeyEntryValue::ToQLValuePB.",
        column_index_, (*reader_.projection_)[column_index_].subkey);
    result_.AllocateChild((*reader_.projection_)[column_index_].subkey);
  }

  Result<bool> DecodePackedColumn() override {
    state_.resize(1);
    return DoDecodePackedColumn(state_.back().expiration, [&] {
      return &result_.AllocateChild((*reader_.projection_)[column_index_].subkey);
    });
  }

  Status SetRootValue(ValueEntryType root_value_type, const Slice& root_value) override {
    has_root_value_ = true;
    if (root_value_type != ValueEntryType::kObject) {
      SubDocument temp(root_value_type);
      RETURN_NOT_OK(temp.DecodeFromValue(root_value));
      result_ = temp;
      cannot_scan_columns_ = true;
    }
    return Status::OK();
  }

  bool CheckForRootValue() override {
    if (!has_root_value_) {
      return false;
    }
    if (IsCollectionType(result_.type())) {
      result_.object_container().clear();
    }
    return true;
  }


  SubDocument& result_;

  // Scanning stack.
  // I.e. the first entry is related to whole document (i.e. row).
  // The second entry corresponds to column.
  // And other entries are list/map entries in case of complex documents.
  boost::container::small_vector<StateEntry, 4> state_;

  // Used in tests only, when we have value for root_doc_key_ itself.
  // In actual DB we don't have values for pure doc key.
  // Only delete marker, that is handled in a different way.
  bool has_root_value_ = false;
};

// It is more performant than DocDBTableReader::GetHelper, but can't handle the general case of
// nested documents that is possible in YCQL.
// Used for YSQL if FLAGS_ysql_use_flat_doc_reader is true.
class DocDBTableReader::FlatGetHelper :
    public DocDBTableReader::GetHelperBase</* is_flat_doc= */ true, /* ysql= */ true> {
 public:
  using Base = DocDBTableReader::GetHelperBase</* is_flat_doc= */ true, /* ysql= */ true>;

  FlatGetHelper(
      DocDBTableReader* reader, const Slice& root_doc_key, std::vector<QLValuePB>* result)
      : Base(reader, root_doc_key), result_(*result) {
    row_expiration_ = reader_.table_expiration_;
    root_key_entry_ = &row_key_;
  }

  Result<bool> Run() {
    return DoRun(&row_expiration_, &row_write_time_);
  }

  void EmptyDocFound() override {}

  bool Found() const override {
    return last_found_ >= 0;
  }

  std::string GetResultAsString() const override { return AsString(result_); }

 private:
  // Return true if entry is more recent than packed row.
  Result<bool> ProcessEntry(
      Slice /* subkeys */, Slice value_slice, const EncodedDocHybridTime& write_time,
      CheckExistOnly check_exist_only) override {
    if (row_write_time_.encoded() >= write_time) {
      DVLOG_WITH_PREFIX_AND_FUNC(4) << "write_time: " << write_time.ToString();
      return false;
    }

    auto* column_value = check_exist_only ? nullptr : &result_[column_index_];

    if (TtlCheckRequired()) {
      auto control_fields = VERIFY_RESULT(ValueControlFields::Decode(&value_slice));

      LazyDocHybridTime lazy_write_time;
      lazy_write_time.Assign(write_time);

      Expiration column_expiration = GetNewExpiration(
            row_expiration_, control_fields.ttl,
            VERIFY_RESULT(lazy_write_time.decoded()).hybrid_time());

        DVLOG_WITH_PREFIX_AND_FUNC(4) << "column_index_: " << column_index_
                                      << ", value: " << value_slice.ToDebugHexString();
      if (IsObsolete(column_expiration)) {
        return true;
      }

      if (VERIFY_RESULT(TryDecodeValue(
              control_fields.timestamp, lazy_write_time, column_expiration, value_slice,
              column_value))) {
        last_found_ = column_index_;
      }
    } else {
      if (VERIFY_RESULT(TryDecodeValueOnly(
              value_slice, (*reader_.projection_)[column_index_].type, column_value))) {
        last_found_ = column_index_;
      }
    }

    return true;
  }

  void NoValueForColumnIndex() override {}

  Result<bool> DecodePackedColumn() override {
    return DoDecodePackedColumn(row_expiration_, [&] {
      return &result_[column_index_];
    });
  }

  Status SetRootValue(ValueEntryType row_value_type, const Slice& row_value) override {
    return Status::OK();
  };

  bool CheckForRootValue() override {
    return false;
  }

  // Owned by the DocDBTableReader::FlatGetHelper user.
  std::vector<QLValuePB>& result_;

  KeyBytes row_key_;
  LazyDocHybridTime row_write_time_;
  Expiration row_expiration_;
};

Result<bool> DocDBTableReader::Get(const Slice& root_doc_key, SubDocument* result) {
  GetHelper helper(this, root_doc_key, result);
  return helper.Run();
}

Result<bool> DocDBTableReader::GetFlat(
    const Slice& root_doc_key, std::vector<QLValuePB>* result) {
  // FlatGetHelper only works when projection_ is specified.
  SCHECK_NOTNULL(projection_);
  result->assign(projection_->size(), QLValuePB());

  FlatGetHelper helper(this, root_doc_key, result);
  return helper.Run();
}

std::string ProjectedColumn::ToString() const {
  return YB_STRUCT_TO_STRING(subkey, type);
}

}  // namespace docdb
}  // namespace yb
