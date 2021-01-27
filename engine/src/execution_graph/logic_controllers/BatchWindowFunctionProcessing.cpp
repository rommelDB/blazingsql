#include "BatchWindowFunctionProcessing.h"
#include "execution_graph/logic_controllers/BlazingColumn.h"
#include "taskflow/executor.h"
#include "CodeTimer.h"

#include <src/utilities/CommonOperations.h>
#include "parser/expression_utils.hpp"
#include <blazingdb/io/Util/StringUtil.h>

#include <cudf/concatenate.hpp>
#include <cudf/stream_compaction.hpp>
#include "cudf/column/column_view.hpp"
#include <cudf/rolling.hpp>
#include <cudf/filling.hpp>
#include <cudf/partitioning.hpp>
#include <cudf/types.hpp>
#include <cudf/copying.hpp>

namespace ral {
namespace batch {

// BEGIN SortKernel

SortKernel::SortKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::SortKernel} {
    this->query_graph = query_graph;
}

void SortKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& /*args*/) {

    CodeTimer eventTimer(false);

    std::unique_ptr<ral::frame::BlazingTable> & input = inputs[0];

    output->addToCache(std::move(input));
}

kstatus SortKernel::run() {

    CodeTimer timer;

    std::unique_ptr <ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();
    while (cache_data != nullptr) {
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                "query_id"_a=context->getContextToken(),
                                "step"_a=context->getQueryStep(),
                                "substep"_a=context->getQuerySubstep(),
                                "info"_a="Sort Kernel tasks created",
                                "duration"_a=timer.elapsed_time(),
                                "kernel_id"_a=this->get_id());
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="Sort Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }

    return kstatus::proceed;
}

// END SortKernel


// BEGIN SplitByKeysKernel

SplitByKeysKernel::SplitByKeysKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::SplitByKeysKernel} {
    this->query_graph = query_graph;
}

void SplitByKeysKernel::do_process(std::vector<std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& /*args*/) {

    CodeTimer eventTimer(false);

    std::unique_ptr<ral::frame::BlazingTable> & input = inputs[0];

    std::vector<cudf::order> sortOrderTypes;
    std::tie(this->column_indices_partitioned, sortOrderTypes) = ral::operators::get_vars_to_partition(this->expression);
    
    // we want get only diff keys from each batch
    std::unique_ptr<cudf::table> unique_keys_table = cudf::drop_duplicates(input->view(), this->column_indices_partitioned, cudf::duplicate_keep_option::KEEP_FIRST);

    // Using the cudf::has_partition and cudf::split
    cudf::table_view batch_view = input->view();
    std::size_t num_partitions = unique_keys_table->num_rows();
    std::vector<cudf::table_view> partitioned_cudf_view;
    std::unique_ptr<CudfTable> hashed_data; // Keep table alive in this scope
    if (batch_view.num_rows() > 0) {
        std::vector<cudf::size_type> hased_data_offsets;
        std::tie(hashed_data, hased_data_offsets) = cudf::hash_partition(input->view(), this->column_indices_partitioned, num_partitions, cudf::hash_id::HASH_IDENTITY);
        // the offsets returned by hash_partition will always start at 0, which is a value we want to ignore for cudf::split
        std::vector<cudf::size_type> split_indexes(hased_data_offsets.begin() + 1, hased_data_offsets.end());
        partitioned_cudf_view = cudf::split(hashed_data->view(), split_indexes);
    } else {
        //  copy empty view
        for (std::size_t i = 0; i < num_partitions; i++) {
            partitioned_cudf_view.push_back(batch_view);
        }
    }

    for (std::size_t i = 0; i < partitioned_cudf_view.size(); i++) {
        output->addToCache(std::make_unique<ral::frame::BlazingTable>(std::make_unique<cudf::table>(partitioned_cudf_view[i]), input->names()));
    }
}

kstatus SplitByKeysKernel::run() {

    CodeTimer timer;

    std::unique_ptr<ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();

    while (cache_data != nullptr ){
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                "query_id"_a=context->getContextToken(),
                                "step"_a=context->getQueryStep(),
                                "substep"_a=context->getQuerySubstep(),
                                "info"_a="SplitByKeys Kernel tasks created",
                                "duration"_a=timer.elapsed_time(),
                                "kernel_id"_a=this->get_id());
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="SplitByKeys Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }

    return kstatus::proceed;
}

// END SplitByKeysKernel


// BEGIN ComputeWindowKernel

ComputeWindowKernel::ComputeWindowKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::ComputeWindowKernel} {
    this->query_graph = query_graph;
}

// TODO: support for LAG(), LEAD(), currently looks like Calcite has an issue when obtain the optimized plan
// TODO: Support for RANK() and DENSE_RANK()
// TODO: Support for first_value() and last_value() should improve, for now fails
std::unique_ptr<CudfColumn> ComputeWindowKernel::compute_column_from_window_function(cudf::table_view input_cudf_view, cudf::column_view input_col_view, std::size_t pos) {
    if (this->aggs_wind_func[pos] == "FIRST_VALUE") {
        std::vector<cudf::size_type> splits(1, 1);
        std::vector<cudf::column_view> partitioned = cudf::split(input_col_view, splits);
        partitioned.pop_back();  // want the first value (as column)
        cudf::table_view table_view_with_single_col(partitioned);
        std::vector< std::unique_ptr<CudfColumn> > windowed_cols = cudf::repeat(table_view_with_single_col, input_col_view.size())->release();
        return std::move(windowed_cols[0]);
    } else if (this->aggs_wind_func[pos] == "LAST_VALUE") {
        std::vector<cudf::size_type> splits(1, input_col_view.size() - 1);
        std::vector<cudf::column_view> partitioned = cudf::split(input_col_view, splits);
        partitioned.erase(partitioned.begin());  // want the last value (as column)
        cudf::table_view table_view_with_single_col(partitioned);
        std::vector< std::unique_ptr<CudfColumn> > windowed_cols = cudf::repeat(table_view_with_single_col, input_col_view.size())->release();
        return std::move(windowed_cols[0]);
    } else {
        std::unique_ptr<cudf::aggregation> window_function = get_window_aggregate(this->aggs_wind_func[pos]);
        std::unique_ptr<CudfColumn> windowed_col;
        std::vector<cudf::column_view> table_to_rolling;
        table_to_rolling.push_back(input_cudf_view.column(this->column_indices_partitioned[0]));
        cudf::table_view table_view_with_single_col(table_to_rolling);

        // when contains `order by` -> RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW is the default behavior
        if (this->expression.find("order by") != std::string::npos  &&  this->expression.find("between") == std::string::npos ) {
            windowed_col = cudf::grouped_rolling_window(table_view_with_single_col , input_col_view, input_col_view.size(), 0, 1, window_function);
        } else {
            windowed_col = cudf::grouped_rolling_window(table_view_with_single_col , input_col_view, input_col_view.size(), input_col_view.size(), 1, window_function);
        }

        return std::move(windowed_col);
    }
}

void ComputeWindowKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& /*args*/) {

    if (inputs.size() == 0) {
        return;
    }

    CodeTimer eventTimer(false);

    std::unique_ptr<ral::frame::BlazingTable> & input = inputs[0];

    cudf::table_view input_cudf_view = input->view();

    // saving the names of the columns and after we will add one by each new col
    std::vector<std::string> input_names = input->names();
    this->column_indices_wind_funct = get_columns_to_apply_window_function(this->expression);
    std::tie(this->column_indices_partitioned, std::ignore) = ral::operators::get_vars_to_partition(this->expression);
    this->aggs_wind_func = get_window_function_agg(this->expression);

    std::vector< std::unique_ptr<CudfColumn> > new_wind_funct_cols;
    for (std::size_t col_i = 0; col_i < this->aggs_wind_func.size(); ++col_i) {
        cudf::column_view input_col_view = input_cudf_view.column(this->column_indices_wind_funct[col_i]);

        // calling main window function
        std::unique_ptr<CudfColumn> windowed_col = compute_column_from_window_function(input_cudf_view, input_col_view, col_i);
        new_wind_funct_cols.push_back(std::move(windowed_col));
        input_names.push_back("");
    }

    // Adding these new columns
    std::unique_ptr<cudf::table> cudf_input = input->releaseCudfTable();
    std::vector< std::unique_ptr<CudfColumn> > output_columns = cudf_input->release();
    for (std::size_t col_i = 0; col_i < new_wind_funct_cols.size(); ++col_i) {
        output_columns.push_back(std::move(new_wind_funct_cols[col_i]));
    }

    std::unique_ptr<cudf::table> cudf_table_window = std::make_unique<cudf::table>(std::move(output_columns));
    std::unique_ptr<ral::frame::BlazingTable> windowed_table = std::make_unique<ral::frame::BlazingTable>(std::move(cudf_table_window), input_names);

    if (windowed_table) {
        cudf::size_type num_rows = windowed_table->num_rows();
        std::size_t num_bytes = windowed_table->sizeInBytes();

        events_logger->info("{ral_id}|{query_id}|{kernel_id}|{input_num_rows}|{input_num_bytes}|{output_num_rows}|{output_num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
                        "ral_id"_a=context->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
                        "query_id"_a=context->getContextToken(),
                        "kernel_id"_a=this->get_id(),
                        "input_num_rows"_a=num_rows,
                        "input_num_bytes"_a=num_bytes,
                        "output_num_rows"_a=num_rows,
                        "output_num_bytes"_a=num_bytes,
                        "event_type"_a="ComputeWindowKernel compute",
                        "timestamp_begin"_a=eventTimer.start_time(),
                        "timestamp_end"_a=eventTimer.end_time());
    }

    output->addToCache(std::move(windowed_table));
}

kstatus ComputeWindowKernel::run() {
    CodeTimer timer;

    std::unique_ptr<ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();

    while (cache_data != nullptr ){
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="ComputeWindow Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }
    return kstatus::proceed;
}

// END ComputeWindowKernel


// BEGIN ConcatPartitionsByKeysKernel

ConcatPartitionsByKeysKernel::ConcatPartitionsByKeysKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id,queryString, context, kernel_type::ConcatPartitionsByKeysKernel} {
    this->query_graph = query_graph;
}

void ConcatPartitionsByKeysKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& /*args*/) {
    if (inputs.empty()) {
        // no op
    } else if(inputs.size() == 1) {
        output->addToCache(std::move(inputs[0]));
    } else {
        std::vector< ral::frame::BlazingTableView > tableViewsToConcat;
        for (std::size_t i = 0; i < inputs.size(); i++){
            tableViewsToConcat.emplace_back(inputs[i]->toBlazingTableView());
        }
        std::unique_ptr<ral::frame::BlazingTable> concatenated = ral::utilities::concatTables(tableViewsToConcat);
        output->addToCache(std::move(concatenated));
    }
}

kstatus ConcatPartitionsByKeysKernel::run() {
    CodeTimer timer;
    std::unique_ptr <ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();

    while (cache_data != nullptr) {
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                "query_id"_a=context->getContextToken(),
                                "step"_a=context->getQueryStep(),
                                "substep"_a=context->getQuerySubstep(),
                                "info"_a="ConcatPartitionsByKey Kernel tasks created",
                                "duration"_a=timer.elapsed_time(),
                                "kernel_id"_a=this->get_id());
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="ConcatPartitionsByKey Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }

    return kstatus::proceed;
}

// END ConcatPartitionsByKeysKernel

} // namespace batch
} // namespace ral
