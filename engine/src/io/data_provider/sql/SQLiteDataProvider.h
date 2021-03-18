/*
 * Copyright 2021 BlazingDB, Inc.
 *     Copyright 2021 Percy Camilo Triveño Aucahuasi <percy@blazingdb.com>
 */

#ifndef SQLITEDATAPROVIDER_H_
#define SQLITEDATAPROVIDER_H_

#include "AbstractSQLDataProvider.h"

#include <sqlite3.h>

namespace ral {
namespace io {

/**
 * can generate a series of randomaccessfiles from uris that are provided
 * when it goes out of scope it will close any files it opened
 * this last point is debatable in terms of if this is the desired functionality
 */
class sqlite_data_provider : public abstractsql_data_provider {
public:
	sqlite_data_provider(const sql_connection &sql_conn,
                       const std::string &table,
                       size_t batch_size_hint = abstractsql_data_provider::DETAULT_BATCH_SIZE_HINT,
                       bool use_partitions = false);

  virtual ~sqlite_data_provider();

	std::shared_ptr<data_provider> clone() override; 

	/**
	 * if has partions will fetch each partition if not will use the limit/offset approach 
	 * with the batch size hint as range
	 */
	data_handle get_next(bool = true) override;

  // in case the table has not partitions will return row count / batch_size_hint else
  // will return the number of partitions
	size_t get_num_handles() override;

private:
  sqlite3* sqlite_connection;
  std::vector<std::string> partitions;
  std::vector<std::string> columns;
  std::vector<std::string> types;
};

} /* namespace io */
} /* namespace ral */

#endif /* SQLITEDATAPROVIDER_H_ */
