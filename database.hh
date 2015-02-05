/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef DATABASE_HH_
#define DATABASE_HH_

#include "core/sstring.hh"
#include "core/shared_ptr.hh"
#include "net/byteorder.hh"
#include "utils/UUID.hh"
#include "db_clock.hh"
#include "gc_clock.hh"
#include <functional>
#include <boost/any.hpp>
#include <cstdint>
#include <boost/variant.hpp>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>
#include <iostream>
#include <boost/functional/hash.hpp>
#include <experimental/optional>
#include <string.h>
#include "types.hh"
#include "tuple.hh"
#include "core/future.hh"
#include "cql3/column_specification.hh"
#include "schema.hh"

struct row;
struct paritition;
struct column_family;

struct row {
    std::vector<bytes> cells;
};

struct partition {
    explicit partition(column_family& cf);
    row static_columns;
    // row key within partition -> row
    std::map<bytes, row, key_compare> rows;
};

struct column_family {
    column_family(schema_ptr schema);
    partition& find_or_create_partition(const bytes& key);
    row& find_or_create_row(const bytes& partition_key, const bytes& clustering_key);
    partition* find_partition(const bytes& key);
    row* find_row(const bytes& partition_key, const bytes& clustering_key);
    schema_ptr _schema;
    // partition key -> partition
    std::map<bytes, partition, key_compare> partitions;
};

class keyspace {
public:
    std::unordered_map<sstring, column_family> column_families;
    static future<keyspace> populate(sstring datadir);
    schema_ptr find_schema(sstring cf_name);
};

class database {
public:
    std::unordered_map<sstring, keyspace> keyspaces;
    static future<database> populate(sstring datadir);
    keyspace* find_keyspace(sstring name);
};


#endif /* DATABASE_HH_ */
