#include "duckdb/planner/operator/logical_get.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/field_writer.hpp"
#include "duckdb/function/table/table_scan.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/function/function_serialization.hpp"

namespace duckdb {

LogicalGet::LogicalGet(idx_t table_index, TableFunction function, unique_ptr<FunctionData> bind_data,
                       vector<LogicalType> returned_types, vector<string> returned_names)
    : LogicalOperator(LogicalOperatorType::LOGICAL_GET), table_index(table_index), function(move(function)),
      bind_data(move(bind_data)), returned_types(move(returned_types)), names(move(returned_names)) {
}

string LogicalGet::GetName() const {
	return StringUtil::Upper(function.name);
}

TableCatalogEntry *LogicalGet::GetTable() const {
	return TableScanFunction::GetTableEntry(function, bind_data.get());
}

string LogicalGet::ParamsToString() const {
	string result;
	for (auto &kv : table_filters.filters) {
		auto &column_index = kv.first;
		auto &filter = kv.second;
		if (column_index < names.size()) {
			result += filter->ToString(names[column_index]);
		}
		result += "\n";
	}
	if (!function.to_string) {
		return string();
	}
	return function.to_string(bind_data.get());
}

vector<ColumnBinding> LogicalGet::GetColumnBindings() {
	if (column_ids.empty()) {
		return {ColumnBinding(table_index, 0)};
	}
	vector<ColumnBinding> result;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		result.emplace_back(table_index, i);
	}
	return result;
}

void LogicalGet::ResolveTypes() {
	if (column_ids.empty()) {
		column_ids.push_back(COLUMN_IDENTIFIER_ROW_ID);
	}
	for (auto &index : column_ids) {
		if (index == COLUMN_IDENTIFIER_ROW_ID) {
			types.emplace_back(LogicalType::ROW_TYPE);
		} else {
			types.push_back(returned_types[index]);
		}
	}
}

idx_t LogicalGet::EstimateCardinality(ClientContext &context) {
	if (function.cardinality) {
		auto node_stats = function.cardinality(context, bind_data.get());
		if (node_stats && node_stats->has_estimated_cardinality) {
			return node_stats->estimated_cardinality;
		}
	}
	return 1;
}

void LogicalGet::Serialize(FieldWriter &writer) const {
	writer.WriteField(table_index);
	writer.WriteRegularSerializableList(returned_types);
	writer.WriteList<string>(names);
	writer.WriteList<column_t>(column_ids);
	writer.WriteSerializable(table_filters);

	FunctionSerializer::SerializeBase<TableFunction>(writer, function, bind_data.get());
	if (!function.serialize) {
		// no serialize method: serialize input values and named_parameters for rebinding purposes
		writer.WriteRegularSerializableList(parameters);
		if (!named_parameters.empty()) {
			throw SerializationException("LogicalGet - FIXME serialize named paramters");
		}
		writer.WriteRegularSerializableList(input_table_types);
		writer.WriteList<string>(input_table_names);
	}
}

unique_ptr<LogicalOperator> LogicalGet::Deserialize(LogicalDeserializationState &state, FieldReader &reader) {
	auto table_index = reader.ReadRequired<idx_t>();
	auto returned_types = reader.ReadRequiredSerializableList<LogicalType, LogicalType>();
	auto returned_names = reader.ReadRequiredList<string>();
	auto column_ids = reader.ReadRequiredList<column_t>();
	auto table_filters = reader.ReadRequiredSerializable<TableFilterSet>();

	unique_ptr<FunctionData> bind_data;
	bool has_deserialize;
	auto function = FunctionSerializer::DeserializeBaseInternal<TableFunction, TableFunctionCatalogEntry>(
	    reader, state.gstate, CatalogType::TABLE_FUNCTION_ENTRY, bind_data, has_deserialize);

	vector<Value> parameters;
	vector<LogicalType> input_table_types;
	vector<string> input_table_names;
	if (!has_deserialize) {
		D_ASSERT(!bind_data);
		parameters = reader.ReadRequiredSerializableList<Value, Value>();
		// FIXME: deserialize named_parameter_map
		named_parameter_map_t named_parameters;
		input_table_types = reader.ReadRequiredSerializableList<LogicalType, LogicalType>();
		input_table_names = reader.ReadRequiredList<string>();
		TableFunctionBindInput input(parameters, named_parameters, input_table_types, input_table_names,
		                             function.function_info.get());

		vector<LogicalType> bind_return_types;
		vector<string> bind_names;
		bind_data = function.bind(state.gstate.context, input, bind_return_types, bind_names);
		if (returned_types != bind_return_types) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different return types than were serialized");
		}
		if (returned_names != bind_names) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different returned names than were serialized");
		}
	}

	auto result = make_unique<LogicalGet>(table_index, function, move(bind_data), returned_types, returned_names);
	result->column_ids = column_ids;
	result->table_filters = move(*table_filters);
	result->parameters = move(parameters);
	result->input_table_types = input_table_types;
	result->input_table_names = input_table_names;
	return result;
}

} // namespace duckdb
