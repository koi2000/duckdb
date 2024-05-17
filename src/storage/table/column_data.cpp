#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/distinct_statistics.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/list_column_data.hpp"
#include "duckdb/storage/table/standard_column_data.hpp"
#include "duckdb/storage/table/array_column_data.hpp"
#include "duckdb/storage/table/struct_column_data.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/serializer/read_stream.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"

namespace duckdb {

ColumnData::ColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, idx_t start_row,
                       LogicalType type_p, optional_ptr<ColumnData> parent)
    : start(start_row), count(0), block_manager(block_manager), info(info), column_index(column_index),
      type(std::move(type_p)), parent(parent), allocation_size(0) {
	if (!parent) {
		stats = make_uniq<SegmentStatistics>(type);
	}
}

ColumnData::~ColumnData() {
}

void ColumnData::SetStart(idx_t new_start) {
	this->start = new_start;
	idx_t offset = 0;
	for (auto &segment : data.Segments()) {
		segment.start = start + offset;
		offset += segment.count;
	}
	data.Reinitialize();
}

DatabaseInstance &ColumnData::GetDatabase() const {
	return info.GetDB().GetDatabase();
}

DataTableInfo &ColumnData::GetTableInfo() const {
	return info;
}

const LogicalType &ColumnData::RootType() const {
	if (parent) {
		return parent->RootType();
	}
	return type;
}

bool ColumnData::HasUpdates() const {
	lock_guard<mutex> update_guard(update_lock);
	return updates.get();
}

void ColumnData::ClearUpdates() {
	lock_guard<mutex> update_guard(update_lock);
	updates.reset();
}

idx_t ColumnData::GetMaxEntry() {
	return count;
}

void ColumnData::InitializeScan(ColumnScanState &state) {
	state.current = data.GetRootSegment();
	state.segment_tree = &data;
	state.row_index = state.current ? state.current->start : 0;
	state.internal_index = state.row_index;
	state.initialized = false;
	state.scan_state.reset();
	state.last_offset = 0;
}

void ColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	state.current = data.GetSegment(row_idx);
	state.segment_tree = &data;
	state.row_index = row_idx;
	state.internal_index = state.current->start;
	state.initialized = false;
	state.scan_state.reset();
	state.last_offset = 0;
}

idx_t ColumnData::ScanVector(ColumnScanState &state, Vector &result, idx_t remaining, bool has_updates) {
	state.previous_states.clear();
	if (!state.initialized) {
		D_ASSERT(state.current);
		state.current->InitializeScan(state);
		state.internal_index = state.current->start;
		state.initialized = true;
	}
	D_ASSERT(data.HasSegment(state.current));
	D_ASSERT(state.internal_index <= state.row_index);
	if (state.internal_index < state.row_index) {
		state.current->Skip(state);
	}
	D_ASSERT(state.current->type == type);
	idx_t initial_remaining = remaining;
	while (remaining > 0) {
		D_ASSERT(state.row_index >= state.current->start &&
		         state.row_index <= state.current->start + state.current->count);
		idx_t scan_count = MinValue<idx_t>(remaining, state.current->start + state.current->count - state.row_index);
		idx_t result_offset = initial_remaining - remaining;
		if (scan_count > 0) {
			if (state.scan_options && state.scan_options->force_fetch_row) {
				for (idx_t i = 0; i < scan_count; i++) {
					ColumnFetchState fetch_state;
					state.current->FetchRow(fetch_state, UnsafeNumericCast<row_t>(state.row_index + i), result,
					                        result_offset + i);
				}
			} else {
				bool entire_vector = !has_updates && scan_count == initial_remaining;
				state.current->Scan(state, scan_count, result, result_offset, entire_vector);
			}

			state.row_index += scan_count;
			remaining -= scan_count;
		}

		if (remaining > 0) {
			auto next = data.GetNextSegment(state.current);
			if (!next) {
				break;
			}
			state.previous_states.emplace_back(std::move(state.scan_state));
			state.current = next;
			state.current->InitializeScan(state);
			state.segment_checked = false;
			D_ASSERT(state.row_index >= state.current->start &&
			         state.row_index <= state.current->start + state.current->count);
		}
	}
	state.internal_index = state.row_index;
	return initial_remaining - remaining;
}

unique_ptr<BaseStatistics> ColumnData::GetUpdateStatistics() {
	lock_guard<mutex> update_guard(update_lock);
	return updates ? updates->GetStatistics() : nullptr;
}

void ColumnData::FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result, idx_t scan_count,
                              bool allow_updates, bool scan_committed) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		return;
	}
	if (!allow_updates && updates->HasUncommittedUpdates(vector_index)) {
		throw TransactionException("Cannot create index with outstanding updates");
	}
	result.Flatten(scan_count);
	if (scan_committed) {
		updates->FetchCommitted(vector_index, result);
	} else {
		updates->FetchUpdates(transaction, vector_index, result);
	}
}

void ColumnData::FetchUpdateRow(TransactionData transaction, row_t row_id, Vector &result, idx_t result_idx) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		return;
	}
	updates->FetchRow(transaction, NumericCast<idx_t>(row_id), result, result_idx);
}

void ColumnData::UpdateInternal(TransactionData transaction, idx_t column_index, Vector &update_vector, row_t *row_ids,
                                idx_t update_count, Vector &base_vector) {
	lock_guard<mutex> update_guard(update_lock);
	if (!updates) {
		updates = make_uniq<UpdateSegment>(*this);
	}
	updates->Update(transaction, column_index, update_vector, row_ids, update_count, base_vector);
}

template <bool SCAN_COMMITTED, bool ALLOW_UPDATES>
idx_t ColumnData::ScanVector(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result) {
	idx_t current_row = vector_index * STANDARD_VECTOR_SIZE;
	auto vector_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - current_row);

	auto scan_count = ScanVector(state, result, vector_count, HasUpdates());
	FetchUpdates(transaction, vector_index, result, scan_count, ALLOW_UPDATES, SCAN_COMMITTED);
	return scan_count;
}

template idx_t ColumnData::ScanVector<false, false>(TransactionData transaction, idx_t vector_index,
                                                    ColumnScanState &state, Vector &result);
template idx_t ColumnData::ScanVector<true, false>(TransactionData transaction, idx_t vector_index,
                                                   ColumnScanState &state, Vector &result);
template idx_t ColumnData::ScanVector<false, true>(TransactionData transaction, idx_t vector_index,
                                                   ColumnScanState &state, Vector &result);
template idx_t ColumnData::ScanVector<true, true>(TransactionData transaction, idx_t vector_index,
                                                  ColumnScanState &state, Vector &result);

idx_t ColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result) {
	return ScanVector<false, true>(transaction, vector_index, state, result);
}

idx_t ColumnData::ScanCommitted(idx_t vector_index, ColumnScanState &state, Vector &result, bool allow_updates) {
	if (allow_updates) {
		return ScanVector<true, true>(TransactionData(0, 0), vector_index, state, result);
	} else {
		return ScanVector<true, false>(TransactionData(0, 0), vector_index, state, result);
	}
}

void ColumnData::ScanCommittedRange(idx_t row_group_start, idx_t offset_in_row_group, idx_t s_count, Vector &result) {
	ColumnScanState child_state;
	InitializeScanWithOffset(child_state, row_group_start + offset_in_row_group);
	bool has_updates = HasUpdates();
	auto scan_count = ScanVector(child_state, result, s_count, has_updates);
	if (has_updates) {
		result.Flatten(scan_count);
		updates->FetchCommittedRange(offset_in_row_group, s_count, result);
	}
}

idx_t ColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t scan_count) {
	if (scan_count == 0) {
		return 0;
	}
	// ScanCount can only be used if there are no updates
	D_ASSERT(!HasUpdates());
	return ScanVector(state, result, scan_count, false);
}

void ColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                        SelectionVector &sel, idx_t &s_count, const TableFilter &filter) {
	idx_t scan_count = Scan(transaction, vector_index, state, result);

	UnifiedVectorFormat vdata;
	result.ToUnifiedFormat(scan_count, vdata);
	ColumnSegment::FilterSelection(sel, result, vdata, filter, scan_count, s_count);
}

void ColumnData::FilterScan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                            SelectionVector &sel, idx_t s_count) {
	Scan(transaction, vector_index, state, result);
	result.Slice(sel, s_count);
}

void ColumnData::FilterScanCommitted(idx_t vector_index, ColumnScanState &state, Vector &result, SelectionVector &sel,
                                     idx_t s_count, bool allow_updates) {
	ScanCommitted(vector_index, state, result, allow_updates);
	result.Slice(sel, s_count);
}

void ColumnData::Skip(ColumnScanState &state, idx_t s_count) {
	state.Next(s_count);
}

void ColumnData::Append(BaseStatistics &append_stats, ColumnAppendState &state, Vector &vector, idx_t append_count) {
	UnifiedVectorFormat vdata;
	vector.ToUnifiedFormat(append_count, vdata);
	AppendData(append_stats, state, vdata, append_count);
}

void ColumnData::Append(ColumnAppendState &state, Vector &vector, idx_t append_count) {
	if (parent || !stats) {
		throw InternalException("ColumnData::Append called on a column with a parent or without stats");
	}
	lock_guard<mutex> l(stats_lock);
	Append(stats->statistics, state, vector, append_count);
}

bool ColumnData::CheckZonemap(TableFilter &filter) {
	if (!stats) {
		throw InternalException("ColumnData::CheckZonemap called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	auto propagate_result = filter.CheckStatistics(stats->statistics);
	if (propagate_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
	    propagate_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
		return false;
	}
	return true;
}

unique_ptr<BaseStatistics> ColumnData::GetStatistics() {
	if (!stats) {
		throw InternalException("ColumnData::GetStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return stats->statistics.ToUnique();
}

void ColumnData::MergeStatistics(const BaseStatistics &other) {
	if (!stats) {
		throw InternalException("ColumnData::MergeStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return stats->statistics.Merge(other);
}

void ColumnData::MergeIntoStatistics(BaseStatistics &other) {
	if (!stats) {
		throw InternalException("ColumnData::MergeIntoStatistics called on a column without stats");
	}
	lock_guard<mutex> l(stats_lock);
	return other.Merge(stats->statistics);
}

void ColumnData::InitializeAppend(ColumnAppendState &state) {
	auto l = data.Lock();
	if (data.IsEmpty(l)) {
		// no segments yet, append an empty segment
		AppendTransientSegment(l, start);
	}
	auto segment = data.GetLastSegment(l);
	if (segment->segment_type == ColumnSegmentType::PERSISTENT || !segment->function.get().init_append) {
		// we cannot append to this segment - append a new segment
		auto total_rows = segment->start + segment->count;
		AppendTransientSegment(l, total_rows);
		state.current = data.GetLastSegment(l);
	} else {
		state.current = segment;
	}

	D_ASSERT(state.current->segment_type == ColumnSegmentType::TRANSIENT);
	state.current->InitializeAppend(state);
	D_ASSERT(state.current->function.get().append);
}

void ColumnData::AppendData(BaseStatistics &append_stats, ColumnAppendState &state, UnifiedVectorFormat &vdata,
                            idx_t append_count) {
	idx_t offset = 0;
	this->count += append_count;
	while (true) {
		// append the data from the vector
		idx_t copied_elements = state.current->Append(state, vdata, offset, append_count);
		append_stats.Merge(state.current->stats.statistics);
		if (copied_elements == append_count) {
			// finished copying everything
			break;
		}

		// we couldn't fit everything we wanted in the current column segment, create a new one
		{
			auto l = data.Lock();
			AppendTransientSegment(l, state.current->start + state.current->count);
			state.current = data.GetLastSegment(l);
			state.current->InitializeAppend(state);
		}
		offset += copied_elements;
		append_count -= copied_elements;
	}
}

void ColumnData::RevertAppend(row_t start_row) {
	auto l = data.Lock();
	// check if this row is in the segment tree at all
	auto last_segment = data.GetLastSegment(l);
	if (idx_t(start_row) >= last_segment->start + last_segment->count) {
		// the start row is equal to the final portion of the column data: nothing was ever appended here
		D_ASSERT(idx_t(start_row) == last_segment->start + last_segment->count);
		return;
	}
	// find the segment index that the current row belongs to
	idx_t segment_index = data.GetSegmentIndex(l, UnsafeNumericCast<idx_t>(start_row));
	auto segment = data.GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(segment_index));
	auto &transient = *segment;
	D_ASSERT(transient.segment_type == ColumnSegmentType::TRANSIENT);

	// remove any segments AFTER this segment: they should be deleted entirely
	data.EraseSegments(l, segment_index);

	this->count = UnsafeNumericCast<idx_t>(start_row) - this->start;
	segment->next = nullptr;
	transient.RevertAppend(UnsafeNumericCast<idx_t>(start_row));
}

idx_t ColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	D_ASSERT(row_id >= 0);
	D_ASSERT(idx_t(row_id) >= start);
	// perform the fetch within the segment
	state.row_index =
	    start + ((UnsafeNumericCast<idx_t>(row_id) - start) / STANDARD_VECTOR_SIZE * STANDARD_VECTOR_SIZE);
	state.current = data.GetSegment(state.row_index);
	state.internal_index = state.current->start;
	return ScanVector(state, result, STANDARD_VECTOR_SIZE, false);
}

void ColumnData::FetchRow(TransactionData transaction, ColumnFetchState &state, row_t row_id, Vector &result,
                          idx_t result_idx) {
	auto segment = data.GetSegment(UnsafeNumericCast<idx_t>(row_id));

	// now perform the fetch within the segment
	segment->FetchRow(state, row_id, result, result_idx);
	// merge any updates made to this row

	FetchUpdateRow(transaction, row_id, result, result_idx);
}

void ColumnData::Update(TransactionData transaction, idx_t column_index, Vector &update_vector, row_t *row_ids,
                        idx_t update_count) {
	Vector base_vector(type);
	ColumnScanState state;
	auto fetch_count = Fetch(state, row_ids[0], base_vector);

	base_vector.Flatten(fetch_count);
	UpdateInternal(transaction, column_index, update_vector, row_ids, update_count, base_vector);
}

void ColumnData::UpdateColumn(TransactionData transaction, const vector<column_t> &column_path, Vector &update_vector,
                              row_t *row_ids, idx_t update_count, idx_t depth) {
	// this method should only be called at the end of the path in the base column case
	D_ASSERT(depth >= column_path.size());
	ColumnData::Update(transaction, column_path[0], update_vector, row_ids, update_count);
}

void ColumnData::AppendTransientSegment(SegmentLock &l, idx_t start_row) {

	idx_t vector_segment_size = Storage::BLOCK_SIZE;
	if (start_row == idx_t(MAX_ROW_ID)) {
#if STANDARD_VECTOR_SIZE < 1024
		vector_segment_size = 1024 * GetTypeIdSize(type.InternalType());
#else
		vector_segment_size = STANDARD_VECTOR_SIZE * GetTypeIdSize(type.InternalType());
#endif
	}

	// the segment size is bound by the block size, but can be smaller
	idx_t segment_size = Storage::BLOCK_SIZE < vector_segment_size ? Storage::BLOCK_SIZE : vector_segment_size;
	allocation_size += segment_size;
	auto new_segment = ColumnSegment::CreateTransientSegment(GetDatabase(), type, start_row, segment_size);
	data.AppendSegment(l, std::move(new_segment));
}

void ColumnData::CommitDropColumn() {
	for (auto &segment_p : data.Segments()) {
		auto &segment = segment_p;
		segment.CommitDropSegment();
	}
}

unique_ptr<ColumnCheckpointState> ColumnData::CreateCheckpointState(RowGroup &row_group,
                                                                    PartialBlockManager &partial_block_manager) {
	return make_uniq<ColumnCheckpointState>(row_group, *this, partial_block_manager);
}

void ColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t row_group_start, idx_t count,
                                Vector &scan_vector) {
	if (state.scan_options && state.scan_options->force_fetch_row) {
		for (idx_t i = 0; i < count; i++) {
			ColumnFetchState fetch_state;
			segment.FetchRow(fetch_state, UnsafeNumericCast<row_t>(state.row_index + i), scan_vector, i);
		}
	} else {
		segment.Scan(state, count, scan_vector, 0, !HasUpdates());
	}

	if (updates) {
		D_ASSERT(scan_vector.GetVectorType() == VectorType::FLAT_VECTOR);
		updates->FetchCommittedRange(state.row_index - row_group_start, count, scan_vector);
	}
}

unique_ptr<ColumnCheckpointState> ColumnData::Checkpoint(RowGroup &row_group, ColumnCheckpointInfo &checkpoint_info) {
	// scan the segments of the column data
	// set up the checkpoint state
	auto checkpoint_state = CreateCheckpointState(row_group, checkpoint_info.info.manager);
	checkpoint_state->global_stats = BaseStatistics::CreateEmpty(type).ToUnique();

	auto l = data.Lock();
	auto nodes = data.MoveSegments(l);
	if (nodes.empty()) {
		// empty table: flush the empty list
		return checkpoint_state;
	}

	ColumnDataCheckpointer checkpointer(*this, row_group, *checkpoint_state, checkpoint_info);
	checkpointer.Checkpoint(std::move(nodes));

	// replace the old tree with the new one
	data.Replace(l, checkpoint_state->new_tree);
	ClearUpdates();

	return checkpoint_state;
}

void ColumnData::DeserializeColumn(Deserializer &deserializer, BaseStatistics &target_stats) {
	// load the data pointers for the column
	deserializer.Set<DatabaseInstance &>(info.GetDB().GetDatabase());
	deserializer.Set<LogicalType &>(type);

	vector<DataPointer> data_pointers;
	deserializer.ReadProperty(100, "data_pointers", data_pointers);

	deserializer.Unset<DatabaseInstance>();
	deserializer.Unset<LogicalType>();

	// construct the segments based on the data pointers
	this->count = 0;
	for (auto &data_pointer : data_pointers) {
		// Update the count and statistics
		this->count += data_pointer.tuple_count;

		// Merge the statistics. If this is a child column, the target_stats reference will point into the parents stats
		// otherwise if this is a top level column, `stats->statistics` == `target_stats`

		target_stats.Merge(data_pointer.statistics);

		// create a persistent segment
		auto segment = ColumnSegment::CreatePersistentSegment(
		    GetDatabase(), block_manager, data_pointer.block_pointer.block_id, data_pointer.block_pointer.offset, type,
		    data_pointer.row_start, data_pointer.tuple_count, data_pointer.compression_type,
		    std::move(data_pointer.statistics), std::move(data_pointer.segment_state));

		data.AppendSegment(std::move(segment));
	}
}

shared_ptr<ColumnData> ColumnData::Deserialize(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                               idx_t start_row, ReadStream &source, const LogicalType &type) {
	auto entry = ColumnData::CreateColumn(block_manager, info, column_index, start_row, type, nullptr);
	BinaryDeserializer deserializer(source);
	deserializer.Begin();
	entry->DeserializeColumn(deserializer, entry->stats->statistics);
	deserializer.End();
	return entry;
}

void ColumnData::GetColumnSegmentInfo(idx_t row_group_index, vector<idx_t> col_path,
                                      vector<ColumnSegmentInfo> &result) {
	D_ASSERT(!col_path.empty());

	// convert the column path to a string
	string col_path_str = "[";
	for (idx_t i = 0; i < col_path.size(); i++) {
		if (i > 0) {
			col_path_str += ", ";
		}
		col_path_str += to_string(col_path[i]);
	}
	col_path_str += "]";

	// iterate over the segments
	idx_t segment_idx = 0;
	auto segment = data.GetRootSegment();
	while (segment) {
		ColumnSegmentInfo column_info;
		column_info.row_group_index = row_group_index;
		column_info.column_id = col_path[0];
		column_info.column_path = col_path_str;
		column_info.segment_idx = segment_idx;
		column_info.segment_type = type.ToString();
		column_info.segment_start = segment->start;
		column_info.segment_count = segment->count;
		column_info.compression_type = CompressionTypeToString(segment->function.get().type);
		{
			lock_guard<mutex> l(stats_lock);
			column_info.segment_stats = segment->stats.statistics.ToString();
		}
		column_info.has_updates = ColumnData::HasUpdates();
		// persistent
		// block_id
		// block_offset
		if (segment->segment_type == ColumnSegmentType::PERSISTENT) {
			column_info.persistent = true;
			column_info.block_id = segment->GetBlockId();
			column_info.block_offset = segment->GetBlockOffset();
		} else {
			column_info.persistent = false;
		}
		auto segment_state = segment->GetSegmentState();
		if (segment_state) {
			column_info.segment_info = segment_state->GetSegmentInfo();
		}
		result.emplace_back(column_info);

		segment_idx++;
		segment = data.GetNextSegment(segment);
	}
}

void ColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	D_ASSERT(this->start == parent.start);
	data.Verify();
	if (type.InternalType() == PhysicalType::STRUCT || type.InternalType() == PhysicalType::ARRAY) {
		// structs and fixed size lists don't have segments
		D_ASSERT(!data.GetRootSegment());
		return;
	}
	idx_t current_index = 0;
	idx_t current_start = this->start;
	idx_t total_count = 0;
	for (auto &segment : data.Segments()) {
		D_ASSERT(segment.index == current_index);
		D_ASSERT(segment.start == current_start);
		current_start += segment.count;
		total_count += segment.count;
		current_index++;
	}
	D_ASSERT(this->count == total_count);
#endif
}

template <class RET, class OP>
static RET CreateColumnInternal(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, idx_t start_row,
                                const LogicalType &type, optional_ptr<ColumnData> parent) {
	if (type.InternalType() == PhysicalType::STRUCT) {
		return OP::template Create<StructColumnData>(block_manager, info, column_index, start_row, type, parent);
	} else if (type.InternalType() == PhysicalType::LIST) {
		return OP::template Create<ListColumnData>(block_manager, info, column_index, start_row, type, parent);
	} else if (type.InternalType() == PhysicalType::ARRAY) {
		return OP::template Create<ArrayColumnData>(block_manager, info, column_index, start_row, type, parent);
	} else if (type.id() == LogicalTypeId::VALIDITY) {
		return OP::template Create<ValidityColumnData>(block_manager, info, column_index, start_row, *parent);
	}
	return OP::template Create<StandardColumnData>(block_manager, info, column_index, start_row, type, parent);
}

shared_ptr<ColumnData> ColumnData::CreateColumn(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                                idx_t start_row, const LogicalType &type,
                                                optional_ptr<ColumnData> parent) {
	return CreateColumnInternal<shared_ptr<ColumnData>, SharedConstructor>(block_manager, info, column_index, start_row,
	                                                                       type, parent);
}

unique_ptr<ColumnData> ColumnData::CreateColumnUnique(BlockManager &block_manager, DataTableInfo &info,
                                                      idx_t column_index, idx_t start_row, const LogicalType &type,
                                                      optional_ptr<ColumnData> parent) {
	return CreateColumnInternal<unique_ptr<ColumnData>, UniqueConstructor>(block_manager, info, column_index, start_row,
	                                                                       type, parent);
}

} // namespace duckdb
