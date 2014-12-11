// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_VAL_HPP_
#define RDB_PROTOCOL_VAL_HPP_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "containers/counted.hpp"
#include "rdb_protocol/datum_stream.hpp"
#include "rdb_protocol/datum_string.hpp"
#include "rdb_protocol/geo/distances.hpp"
#include "rdb_protocol/geo/lon_lat_types.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/ql2.pb.h"

class ellipsoid_spec_t;

namespace ql {

class datum_t;
class env_t;
template <class> class protob_t;
class scope_env_t;
class stream_cache_t;
class term_t;
class val_t;

/* A `table_t` is an `r.table` term, possibly with some other things chained onto
onto it. */
class table_t : public single_threaded_countable_t<table_t>, public pb_rcheckable_t {
public:
    table_t(counted_t<base_table_t> &&,
            counted_t<const db_t> db, const std::string &name,
            bool use_outdated, const protob_t<const Backtrace> &src);
    const std::string &get_pkey();
    datum_t get_row(env_t *env, datum_t pval);
    counted_t<datum_stream_t> get_all(
            env_t *env,
            datum_t value,
            const std::string &sindex_id,
            const protob_t<const Backtrace> &bt);
    counted_t<datum_stream_t> get_intersecting(
            env_t *env,
            const datum_t &query_geometry,
            const std::string &new_sindex_id,
            const pb_rcheckable_t *parent);
    datum_t get_nearest(
            env_t *env,
            lon_lat_point_t center,
            double max_dist,
            uint64_t max_results,
            const ellipsoid_spec_t &geo_system,
            dist_unit_t dist_unit,
            const std::string &new_sindex_id,
            const configured_limits_t &limits);

    datum_t make_error_datum(const base_exc_t &exception);

    datum_t batched_replace(
        env_t *env,
        const std::vector<datum_t> &vals,
        const std::vector<datum_t> &keys,
        counted_t<const func_t> replacement_generator,
        bool nondeterministic_replacements_ok,
        durability_requirement_t durability_requirement,
        return_changes_t return_changes);

    datum_t batched_insert(
        env_t *env,
        std::vector<datum_t> &&insert_datums,
        std::vector<bool> &&pkey_was_autogenerated,
        conflict_behavior_t conflict_behavior,
        durability_requirement_t durability_requirement,
        return_changes_t return_changes);

    MUST_USE bool sindex_create(
        env_t *env, const std::string &name,
        counted_t<const func_t> index_func, sindex_multi_bool_t multi,
        sindex_geo_bool_t geo);
    MUST_USE bool sindex_drop(env_t *env, const std::string &name);
    MUST_USE sindex_rename_result_t sindex_rename(
        env_t *env, const std::string &old_name,
        const std::string &new_name, bool overwrite);
    datum_t sindex_list(env_t *env);
    datum_t sindex_status(env_t *env,
        std::set<std::string> sindex);
    MUST_USE bool sync(env_t *env);

    /* `db` and `name` are mostly for display purposes, but some things like the
    `reconfigure()` logic use them. */
    counted_t<const db_t> db;
    const std::string name;   /* TODO: Make this a `name_string_t` */
    std::string display_name() {
        return db->name.str() + "." + name;
    }

    counted_t<datum_stream_t> as_seq(
        env_t *env,
        const std::string &idx,
        const protob_t<const Backtrace> &bt,
        const datum_range_t &bounds,
        sorting_t sorting);

    counted_t<base_table_t> tbl;

private:
    datum_t batched_insert_with_keys(
        env_t *env,
        const std::vector<store_key_t> &keys,
        const std::vector<datum_t> &insert_datums,
        conflict_behavior_t conflict_behavior,
        durability_requirement_t durability_requirement);

    MUST_USE bool sync_depending_on_durability(
        env_t *env, durability_requirement_t durability_requirement);

    bool use_outdated;
};

class table_slice_t
    : public single_threaded_countable_t<table_slice_t>, public pb_rcheckable_t {
public:
    table_slice_t(counted_t<table_t> _tbl,
                  boost::optional<std::string> _idx = boost::none,
                  sorting_t _sorting = sorting_t::UNORDERED,
                  datum_range_t _bounds = datum_range_t::universe());
    counted_t<datum_stream_t> as_seq(env_t *env, const protob_t<const Backtrace> &bt);
    counted_t<table_slice_t> with_sorting(std::string idx, sorting_t sorting);
    counted_t<table_slice_t> with_bounds(std::string idx, datum_range_t bounds);
    const counted_t<table_t> &get_tbl() const { return tbl; }
    const boost::optional<std::string> &get_idx() const { return idx; }
    ql::changefeed::keyspec_t::range_t get_change_spec();
private:
    friend class distinct_term_t;
    const counted_t<table_t> tbl;
    const boost::optional<std::string> idx;
    const sorting_t sorting;
    const datum_range_t bounds;
};

enum function_shortcut_t {
    NO_SHORTCUT = 0,
    CONSTANT_SHORTCUT = 1,
    GET_FIELD_SHORTCUT = 2,
    PLUCK_SHORTCUT = 3,
    PAGE_SHORTCUT = 4
};

class single_selection_t : public single_threaded_countable_t<single_selection_t> {
public:
    static counted_t<single_selection_t> from_key(
        env_t *env, protob_t<const Backtrace> bt,
        counted_t<table_t> table, datum_t key);
    static counted_t<single_selection_t> from_row(
        env_t *env, protob_t<const Backtrace> bt,
        counted_t<table_t> table, datum_t row);
    static counted_t<single_selection_t> from_slice(
        env_t *env, protob_t<const Backtrace> bt,
        counted_t<table_slice_t> table, std::string err);
    virtual ~single_selection_t() { }

    virtual datum_t get() = 0;
    virtual counted_t<datum_stream_t> read_changes() = 0;
    virtual datum_t replace(
        counted_t<const func_t> f, bool nondet_ok,
        durability_requirement_t dur_req, return_changes_t return_changes) = 0;
    virtual const counted_t<table_t> &get_tbl() = 0;
protected:
    single_selection_t() = default;
};

class selection_t : public single_threaded_countable_t<selection_t> {
public:
    selection_t(counted_t<table_t> _table, counted_t<datum_stream_t> _seq)
        : table(std::move(_table)), seq(std::move(_seq)) { }
    counted_t<table_t> table;
    counted_t<datum_stream_t> seq;
};

// A value is anything RQL can pass around -- a datum, a sequence, a function, a
// selection, whatever.
class val_t : public pb_rcheckable_t {
public:
    // This type is intentionally opaque.  It is almost always an error to
    // compare two `val_t` types rather than testing whether one is convertible
    // to another.
    class type_t {
        friend class val_t;
        friend void run(Query *q, scoped_ptr_t<env_t> *env_ptr,
                        Response *res, stream_cache_t *stream_cache,
                        bool *response_needed_out);
    public:
        enum raw_type_t {
            DB               = 1, // db
            TABLE            = 2, // table
            TABLE_SLICE      = 9, // table_slice
            SELECTION        = 3, // table, sequence
            SEQUENCE         = 4, // sequence
            SINGLE_SELECTION = 5, // table, datum (object)
            DATUM            = 6, // datum
            FUNC             = 7, // func
            GROUPED_DATA     = 8  // grouped_data
        };
        type_t(raw_type_t _raw_type);  // NOLINT(runtime/explicit)
        bool is_convertible(type_t rhs) const;

        raw_type_t get_raw_type() const { return raw_type; }
        const char *name() const;

    private:
        friend class coerce_term_t;
        friend class typeof_term_t;
        friend int val_type(const scoped_ptr_t<val_t> &v);
        raw_type_t raw_type;
    };
    type_t get_type() const;
    const char *get_type_name() const;

    val_t(datum_t _datum, protob_t<const Backtrace> bt);
    val_t(const counted_t<grouped_data_t> &groups,
          protob_t<const Backtrace> bt);
    val_t(counted_t<single_selection_t> _selection, protob_t<const Backtrace> bt);
    val_t(env_t *env, counted_t<datum_stream_t> _seq, protob_t<const Backtrace> bt);
    val_t(counted_t<table_t> _table, protob_t<const Backtrace> bt);
    val_t(counted_t<table_slice_t> _table_slice, protob_t<const Backtrace> bt);
    val_t(counted_t<selection_t> _selection, protob_t<const Backtrace> bt);
    val_t(counted_t<const db_t> _db, protob_t<const Backtrace> bt);
    val_t(counted_t<const func_t> _func, protob_t<const Backtrace> bt);
    ~val_t();

    counted_t<const db_t> as_db() const;
    counted_t<table_t> as_table();
    counted_t<table_t> get_underlying_table() const;
    counted_t<table_slice_t> as_table_slice();
    counted_t<selection_t> as_selection(env_t *env);
    counted_t<datum_stream_t> as_seq(env_t *env);
    counted_t<single_selection_t> as_single_selection();
    // See func.hpp for an explanation of shortcut functions.
    counted_t<const func_t> as_func(function_shortcut_t shortcut = NO_SHORTCUT);

    // This set of interfaces is atrocious.  Basically there are some places
    // where we want grouped_data, some places where we maybe want grouped_data,
    // and some places where we maybe want grouped data even if we have to
    // coerce to grouped data from a grouped stream.  (We can't use the usual
    // `is_convertible` interface because the type information is actually a
    // property of the stream, because I'm a terrible programmer.)
    counted_t<grouped_data_t> as_grouped_data();
    counted_t<grouped_data_t> as_promiscuous_grouped_data(env_t *env);
    counted_t<grouped_data_t> maybe_as_grouped_data();
    counted_t<grouped_data_t> maybe_as_promiscuous_grouped_data(env_t *env);

    datum_t as_datum() const; // prefer the forms below
    datum_t as_ptype(const std::string s = "") const;
    bool as_bool() const;
    double as_num() const;
    template<class T>
    T as_int() const {
        int64_t i = as_int();
        T t = static_cast<T>(i);
        rcheck(static_cast<int64_t>(t) == i,
               base_exc_t::GENERIC,
               strprintf("Integer too large: %" PRIi64, i));
        return t;
    }
    int64_t as_int() const;
    datum_string_t as_str() const;

    std::string print() const;
    std::string trunc_print() const;

private:
    friend int val_type(const scoped_ptr_t<val_t> &v); // type_manip version
    void rcheck_literal_type(type_t::raw_type_t expected_raw_type) const;

    type_t type;
    // We pretend that this variant is a union -- as if it doesn't have type
    // information.  The sequence, datum, func, and db_ptr functions get the
    // fields of the variant.
    boost::variant<counted_t<const db_t>,
                   counted_t<datum_stream_t>,
                   datum_t,
                   counted_t<const func_t>,
                   counted_t<grouped_data_t>,
                   counted_t<table_t>,
                   counted_t<table_slice_t>,
                   counted_t<single_selection_t>,
                   counted_t<selection_t> > u;

    const counted_t<const db_t> &db() const {
        return boost::get<counted_t<const db_t> >(u);
    }
    counted_t<datum_stream_t> &sequence() {
        return boost::get<counted_t<datum_stream_t> >(u);
    }
    datum_t &datum() {
        return boost::get<datum_t>(u);
    }
    const datum_t &datum() const {
        return boost::get<datum_t>(u);
    }
    const counted_t<single_selection_t> &single_selection() const {
        return boost::get<counted_t<single_selection_t> >(u);
    }
    const counted_t<selection_t> &selection() const {
        return boost::get<counted_t<selection_t> >(u);
    }
    const counted_t<table_t> &table() const {
        return boost::get<counted_t<table_t> >(u);
    }
    const counted_t<table_slice_t> &table_slice() const {
        return boost::get<counted_t<table_slice_t> >(u);
    }
    counted_t<const func_t> &func() { return boost::get<counted_t<const func_t> >(u); }

    DISABLE_COPYING(val_t);
};


}  // namespace ql

#endif // RDB_PROTOCOL_VAL_HPP_
