// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <map>
#include <string>

#include "containers/name_string.hpp"
#include "rdb_protocol/datum_string.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/pseudo_geometry.hpp"
#include "rdb_protocol/terms/writes.hpp"

namespace ql {

name_string_t get_name(const scoped_ptr_t<val_t> &val, const char *type_str) {
    r_sanity_check(val.has());
    const datum_string_t &raw_name = val->as_str();
    name_string_t name;
    bool assignment_successful = name.assign_value(raw_name);
    rcheck_target(val.get(),
                  assignment_successful,
                  base_exc_t::GENERIC,
                  strprintf("%s name `%s` invalid (%s).",
                            type_str,
                            raw_name.to_std().c_str(),
                            name_string_t::valid_char_msg));
    return name;
}

void get_replicas_and_director(const scoped_ptr_t<val_t> &replicas,
                               const scoped_ptr_t<val_t> &director_tag,
                               table_generate_config_params_t *params) {
    if (replicas.has()) {
        params->num_replicas.clear();
        datum_t datum = replicas->as_datum();
        if (datum.get_type() == datum_t::R_OBJECT) {
            rcheck_target(replicas.get(), director_tag.has(), base_exc_t::GENERIC,
                "`director_tag` must be specified when `replicas` is an OBJECT.");
            for (size_t i = 0; i < datum.obj_size(); ++i) {
                std::pair<datum_string_t, datum_t> pair = datum.get_pair(i);
                name_string_t name;
                bool assignment_successful = name.assign_value(pair.first);
                rcheck_target(replicas, assignment_successful, base_exc_t::GENERIC,
                    strprintf("Server tag name `%s` invalid (%s).",
                              pair.first.to_std().c_str(),
                              name_string_t::valid_char_msg));
                int64_t count = checked_convert_to_int(replicas.get(),
                                                       pair.second.as_num());
                rcheck_target(replicas.get(), count >= 0,
                    base_exc_t::GENERIC, "Can't have a negative number of replicas");
                size_t size_count = static_cast<size_t>(count);
                rcheck_target(replicas.get(), static_cast<int64_t>(size_count) == count,
                              base_exc_t::GENERIC,
                              strprintf("Integer too large: %" PRIi64, count));
                params->num_replicas.insert(std::make_pair(name, size_count));
            }
        } else if (datum.get_type() == datum_t::R_NUM) {
            rcheck_target(replicas.get(), !director_tag.has(), base_exc_t::GENERIC,
                "`replicas` must be an OBJECT if `director_tag` is specified.");
            size_t count = replicas->as_int<size_t>();
            params->num_replicas.insert(std::make_pair(params->director_tag, count));
        } else {
            rfail_target(replicas, base_exc_t::GENERIC,
                "Expected type OBJECT or NUMBER but found %s:\n%s",
                datum.get_type_name().c_str(), datum.print().c_str());
        }
    }

    if (director_tag.has()) {
        params->director_tag = get_name(director_tag, "Server tag");
    }
}

// Meta operations (BUT NOT TABLE TERMS) should inherit from this.
class meta_op_term_t : public op_term_t {
public:
    meta_op_term_t(compile_env_t *env, protob_t<const Term> term, argspec_t argspec,
              optargspec_t optargspec = optargspec_t({}))
        : op_term_t(env, std::move(term), std::move(argspec), std::move(optargspec)) { }

private:
    virtual bool is_deterministic() const { return false; }
};

class db_term_t : public meta_op_term_t {
public:
    db_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        name_string_t db_name = get_name(args->arg(env, 0), "Database");
        counted_t<const db_t> db;
        std::string error;
        if (!env->env->reql_cluster_interface()->db_find(db_name, env->env->interruptor,
                &db, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(db);
    }
    virtual const char *name() const { return "db"; }
};

class db_create_term_t : public meta_op_term_t {
public:
    db_create_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        name_string_t db_name = get_name(args->arg(env, 0), "Database");
        std::string error;
        ql::datum_t result;
        if (!env->env->reql_cluster_interface()->db_create(db_name,
                env->env->interruptor, &result, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "db_create"; }
};

class table_create_term_t : public meta_op_term_t {
public:
    table_create_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1, 2),
            optargspec_t({"primary_key", "shards", "replicas", "director_tag"})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        /* Parse arguments */
        table_generate_config_params_t config_params =
            table_generate_config_params_t::make_default();

        // Parse the 'shards' optarg
        if (scoped_ptr_t<val_t> shards_optarg = args->optarg(env, "shards")) {
            rcheck_target(shards_optarg, shards_optarg->as_int() > 0, base_exc_t::GENERIC,
                          "Every table must have at least one shard.");
            config_params.num_shards = shards_optarg->as_int();
        }

        // Parse the 'replicas' and 'director_tag' optargs
        get_replicas_and_director(args->optarg(env, "replicas"),
                                  args->optarg(env, "director_tag"),
                                  &config_params);

        std::string primary_key = "id";
        if (scoped_ptr_t<val_t> v = args->optarg(env, "primary_key")) {
            primary_key = v->as_str().to_std();
        }

        counted_t<const db_t> db;
        name_string_t tbl_name;
        if (args->num_args() == 1) {
            scoped_ptr_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db = dbv->as_db();
            tbl_name = get_name(args->arg(env, 0), "Table");
        } else {
            db = args->arg(env, 0)->as_db();
            tbl_name = get_name(args->arg(env, 1), "Table");
        }

        /* Create the table */
        std::string error;
        ql::datum_t result;
        if (!env->env->reql_cluster_interface()->table_create(tbl_name, db,
                config_params, primary_key, env->env->interruptor, &result, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "table_create"; }
};

class db_drop_term_t : public meta_op_term_t {
public:
    db_drop_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        name_string_t db_name = get_name(args->arg(env, 0), "Database");

        std::string error;
        ql::datum_t result;
        if (!env->env->reql_cluster_interface()->db_drop(db_name,
                env->env->interruptor, &result, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }

        return new_val(result);
    }
    virtual const char *name() const { return "db_drop"; }
};

class table_drop_term_t : public meta_op_term_t {
public:
    table_drop_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1, 2)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<const db_t> db;
        name_string_t tbl_name;
        if (args->num_args() == 1) {
            scoped_ptr_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db = dbv->as_db();
            tbl_name = get_name(args->arg(env, 0), "Table");
        } else {
            db = args->arg(env, 0)->as_db();
            tbl_name = get_name(args->arg(env, 1), "Table");
        }

        std::string error;
        ql::datum_t result;
        if (!env->env->reql_cluster_interface()->table_drop(tbl_name, db,
                env->env->interruptor, &result, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }

        return new_val(result);
    }
    virtual const char *name() const { return "table_drop"; }
};

class db_list_term_t : public meta_op_term_t {
public:
    db_list_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(0)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *, eval_flags_t) const {
        std::set<name_string_t> dbs;
        std::string error;
        if (!env->env->reql_cluster_interface()->db_list(
                env->env->interruptor, &dbs, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }

        std::vector<datum_t> arr;
        arr.reserve(dbs.size());
        for (auto it = dbs.begin(); it != dbs.end(); ++it) {
            arr.push_back(datum_t(datum_string_t(it->str())));
        }

        return new_val(datum_t(std::move(arr), env->env->limits()));
    }
    virtual const char *name() const { return "db_list"; }
};

class table_list_term_t : public meta_op_term_t {
public:
    table_list_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(0, 1)) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<const ql::db_t> db;
        if (args->num_args() == 0) {
            scoped_ptr_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv);
            db = dbv->as_db();
        } else {
            db = args->arg(env, 0)->as_db();
        }

        std::set<name_string_t> tables;
        std::string error;
        if (!env->env->reql_cluster_interface()->table_list(db,
                env->env->interruptor, &tables, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }

        std::vector<datum_t> arr;
        arr.reserve(tables.size());
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            arr.push_back(datum_t(datum_string_t(it->str())));
        }
        return new_val(datum_t(std::move(arr), env->env->limits()));
    }
    virtual const char *name() const { return "table_list"; }
};

class config_term_t : public meta_op_term_t {
public:
    config_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1, 1), optargspec_t({})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        scoped_ptr_t<val_t> target = args->arg(env, 0);
        scoped_ptr_t<val_t> selection;
        bool success;
        std::string error;
        /* Note that we always require an argument; we never take a default `db`
        argument. So `r.config()` is an error rather than the configuration for the
        current database. This is why we don't subclass from `table_or_db_meta_term_t`.
        */
        if (target->get_type().is_convertible(val_t::type_t::DB)) {
            success = env->env->reql_cluster_interface()->db_config(
                    target->as_db(), backtrace(), env->env, &selection, &error);
        } else {
            counted_t<table_t> table = target->as_table();
            name_string_t name = name_string_t::guarantee_valid(table->name.c_str());
            /* RSI(reql_admin): Make sure the user didn't call `.between()` or
            `.order_by()` on this table */
            success = env->env->reql_cluster_interface()->table_config(
                    table->db, name, backtrace(), env->env, &selection, &error);
        }
        if (!success) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return selection;
    }
    virtual const char *name() const { return "config"; }
};

class status_term_t : public meta_op_term_t {
public:
    status_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        meta_op_term_t(env, term, argspec_t(1, 1), optargspec_t({})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        name_string_t name = name_string_t::guarantee_valid(table->name.c_str());
        /* RSI(reql_admin): Make sure the user didn't call `.between()` or
        `.order_by()` on this table */
        std::string error;
        scoped_ptr_t<val_t> selection;
        if (!env->env->reql_cluster_interface()->table_config(
                table->db, name, backtrace(), env->env, &selection, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return selection;
    }
    virtual const char *name() const { return "status"; }
};

/* Common superclass for terms that can operate on either a table or a database: `wait`,
`reconfigure`, and `rebalance`. */
class table_or_db_meta_term_t : public meta_op_term_t {
public:
    table_or_db_meta_term_t(compile_env_t *env, const protob_t<const Term> &term,
            optargspec_t &&optargs) :
        /* None of the subclasses take positional arguments except for the table/db. */
        meta_op_term_t(env, term, argspec_t(0, 1), std::move(optargs))
        { }
protected:
    /* If the term is called on a table, then `db` and `name_if_table` indicate the
    table's database and name. If the term is called on a database, then `db `indicates
    the database and `name_if_table` will be empty. */
    virtual scoped_ptr_t<val_t> eval_impl_on_table_or_db(
            scope_env_t *env, args_t *args, eval_flags_t flags,
            const counted_t<const ql::db_t> &db,
            const boost::optional<name_string_t> &name_if_table) const = 0;
private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t flags) const {
        scoped_ptr_t<val_t> target;
        if (args->num_args() == 0) {
            target = args->optarg(env, "db");
            r_sanity_check(target.has());
        } else {
            target = args->arg(env, 0);
        }
        if (target->get_type().is_convertible(val_t::type_t::DB)) {
            return eval_impl_on_table_or_db(env, args, flags, target->as_db(), nullptr);
        } else {
            counted_t<table_t> table = target->as_table();
            name_string_t name = name_string_t::guarantee_valid(table->name.c_str());
            /* RSI(reql_admin): Make sure the user didn't call `.between()` or
            `.order_by()` on this table */
            return eval_impl_on_table_or_db(env, args, flags, table->db, name);
        }
    }
};

class wait_term_t : public table_or_db_meta_term_t {
public:
    wait_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        table_or_db_meta_term_t(env, term, optargspec_t({})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl_on_table_or_db(
            scope_env_t *env, args_t *args, eval_flags_t,
            const counted_t<const ql::db_t> &db,
            const boost::optional<name_string_t> &name_if_table) const {
        /* We've considered making `readiness` an optarg. See GitHub issue #2259. */
        table_readiness_t readiness = table_readiness_t::finished;
        ql::datum_t result;
        bool success;
        std::string error;
        if (static_cast<bool>(name_if_table)) {
            success = env->env->reql_cluster_interface()->table_wait(
                db, *name_if_table, readiness, env->env->interruptor, &result, &error);
        } else {
            success = env->env->reql_cluster_interface()->db_wait(
                db, readiness, env->env->interruptor, &result, &error);
        }
        if (!success) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "wait"; }
};

class reconfigure_term_t : public table_or_db_meta_term_t {
public:
    reconfigure_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        table_or_db_meta_term_t(env, term,
            optargspec_t({"director_tag", "dry_run", "replicas", "shards"})) { }
private:
    scoped_ptr_t<val_t> required_optarg(scope_env_t *env,
                                        args_t *args,
                                        const char *name) const {
        scoped_ptr_t<val_t> result = args->optarg(env, name);
        rcheck(result.has(), base_exc_t::GENERIC,
               strprintf("Missing required argument `%s`.", name));
        return result;
    }

    virtual scoped_ptr_t<val_t> eval_impl_on_table_or_db(
            scope_env_t *env, args_t *args, eval_flags_t
            const counted_t<const ql::db_t> &db,
            const boost::optional<name_string_t> &name_if_table) const {
        // Use the default director_tag, unless the optarg overwrites it
        table_generate_config_params_t config_params =
            table_generate_config_params_t::make_default();

        // Parse the 'shards' optarg
        scoped_ptr_t<val_t> shards_optarg = required_optarg(env, args, "shards");
        rcheck_target(shards_optarg, shards_optarg->as_int() > 0, base_exc_t::GENERIC,
                      "Every table must have at least one shard.");
        config_params.num_shards = shards_optarg->as_int();

        // Parse the 'replicas' and 'director_tag' optargs
        get_replicas_and_director(required_optarg(env, args, "replicas"),
                                  args->optarg(env, "director_tag"),
                                  &config_params);

        // Parse the 'dry_run' optarg
        bool dry_run = false;
        if (scoped_ptr_t<val_t> v = args->optarg(env, "dry_run")) {
            dry_run = v->as_bool();
        }

        bool success;
        datum_t result;
        std::string error;
        /* Perform the operation */
        if (static_cast<bool>(name_if_table)) {
            success = env->env->reql_cluster_interface()->table_reconfigure(
                    db, *name_if_table, config_params, dry_run,
                    env->env->interruptor, &result, &error);
        } else {
            success = env->env->reql_cluster_interface()->db_reconfigure(
                    db, config_params, dry_run, env->env->interruptor, &result, &error);
        }
        if (!success) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }

        return new_val(result);
    }
    virtual const char *name() const { return "reconfigure"; }
};

class rebalance_term_t : public table_or_db_meta_term_t {
public:
    rebalance_term_t(compile_env_t *env, const protob_t<const Term> &term) :
        table_or_db_meta_term_t(env, term, optargspec_t({})) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl_on_table_or_db(
            scope_env_t *env, args_t *args, eval_flags_t
            const counted_t<const ql::db_t> &db,
            const boost::optional<name_string_t> &name_if_table) const {
        /* We've considered making `readiness` an optarg. See GitHub issue #2259. */
        table_readiness_t readiness = table_readiness_t::finished;
        ql::datum_t result;
        bool success;
        std::string error;
        if (static_cast<bool>(name_if_table)) {
            success = env->env->reql_cluster_interface()->table_rebalance(
                db, *name_if_table, env->env->interruptor, &result, &error);
        } else {
            success = env->env->reql_cluster_interface()->db_rebalance(
                db, env->env->interruptor, &result, &error);
        }
        if (!success) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(result);
    }
    virtual const char *name() const { return "rebalance"; }
};

class sync_term_t : public meta_op_term_t {
public:
    sync_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : meta_op_term_t(env, term, argspec_t(1)) { }

private:
    virtual scoped_ptr_t<val_t> eval_impl(
            scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> t = args->arg(env, 0)->as_table();
        bool success = t->sync(env->env);
        r_sanity_check(success);
        ql::datum_object_builder_t result;
        result.overwrite("synced", ql::datum_t(1.0));
        return new_val(std::move(result).to_datum());
    }
    virtual const char *name() const { return "sync"; }
};

class table_term_t : public op_term_t {
public:
    table_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1, 2),
          optargspec_t({ "use_outdated", "identifier_format" })) { }
private:
    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        scoped_ptr_t<val_t> t = args->optarg(env, "use_outdated");
        bool use_outdated = t ? t->as_bool() : false;

        boost::optional<admin_identifier_format_t> identifier_format;
        if (scoped_ptr_t<val_t> v = args->optarg(env, "identifier_format")) {
            const datum_string_t &str = v->as_str();
            if (str == "name") {
                identifier_format = admin_identifier_format_t::name;
            } else if (str == "uuid") {
                identifier_format = admin_identifier_format_t::uuid;
            } else {
                rfail(base_exc_t::GENERIC, "Identifier format `%s` unrecognized "
                    "(options are \"name\" and \"uuid\").", str.to_std().c_str());
            }
        }

        counted_t<const db_t> db;
        name_string_t name;
        if (args->num_args() == 1) {
            scoped_ptr_t<val_t> dbv = args->optarg(env, "db");
            r_sanity_check(dbv.has());
            db = dbv->as_db();
            name = get_name(args->arg(env, 0), "Table");
        } else {
            r_sanity_check(args->num_args() == 2);
            db = args->arg(env, 0)->as_db();
            name = get_name(args->arg(env, 1), "Table");
        }

        std::string error;
        counted_t<base_table_t> table;
        if (!env->env->reql_cluster_interface()->table_find(name, db,
                identifier_format, env->env->interruptor, &table, &error)) {
            rfail(base_exc_t::GENERIC, "%s", error.c_str());
        }
        return new_val(make_counted<table_t>(
            std::move(table), db, name.str(), use_outdated, backtrace()));
    }
    virtual bool is_deterministic() const { return false; }
    virtual const char *name() const { return "table"; }
};

class get_term_t : public op_term_t {
public:
    get_term_t(compile_env_t *env, const protob_t<const Term> &term) : op_term_t(env, term, argspec_t(2)) { }
private:
    virtual scoped_ptr_t<val_t>
    eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        return new_val(single_selection_t::from_key(
                           env->env,
                           backtrace(),
                           args->arg(env, 0)->as_table(),
                           args->arg(env, 1)->as_datum()));
    }
    virtual const char *name() const { return "get"; }
};

class get_all_term_t : public op_term_t {
public:
    get_all_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(2, -1), optargspec_t({ "index" })) { }
private:
    datum_t get_key_arg(const scoped_ptr_t<val_t> &arg) const {
        datum_t datum_arg = arg->as_datum();

        rcheck_target(arg,
                      !datum_arg.is_ptype(pseudo::geometry_string),
                      base_exc_t::GENERIC,
                      "Cannot use a geospatial index with `get_all`. "
                      "Use `get_intersecting` instead.");
        return datum_arg;
    }

    virtual scoped_ptr_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        counted_t<table_t> table = args->arg(env, 0)->as_table();
        scoped_ptr_t<val_t> index = args->optarg(env, "index");
        std::string index_str = index ? index->as_str().to_std() : "";
        if (index && index_str != table->get_pkey()) {
            std::vector<counted_t<datum_stream_t> > streams;
            for (size_t i = 1; i < args->num_args(); ++i) {
                datum_t key = get_key_arg(args->arg(env, i));
                counted_t<datum_stream_t> seq =
                    table->get_all(env->env, key, index_str, backtrace());
                streams.push_back(seq);
            }
            counted_t<datum_stream_t> stream
                = make_counted<union_datum_stream_t>(std::move(streams), backtrace());
            return new_val(make_counted<selection_t>(table, stream));
        } else {
            datum_array_builder_t arr(env->env->limits());
            for (size_t i = 1; i < args->num_args(); ++i) {
                datum_t key = get_key_arg(args->arg(env, i));
                datum_t row = table->get_row(env->env, key);
                if (row.get_type() != datum_t::R_NULL) {
                    arr.add(row);
                }
            }
            counted_t<datum_stream_t> stream
                = make_counted<array_datum_stream_t>(std::move(arr).to_datum(),
                                                     backtrace());
            return new_val(make_counted<selection_t>(table, stream));
        }
    }
    virtual const char *name() const { return "get_all"; }
};

counted_t<term_t> make_db_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_term_t>(env, term);
}

counted_t<term_t> make_table_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_term_t>(env, term);
}

counted_t<term_t> make_get_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<get_term_t>(env, term);
}

counted_t<term_t> make_get_all_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<get_all_term_t>(env, term);
}

counted_t<term_t> make_db_create_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_create_term_t>(env, term);
}

counted_t<term_t> make_db_drop_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_drop_term_t>(env, term);
}

counted_t<term_t> make_db_list_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<db_list_term_t>(env, term);
}

counted_t<term_t> make_table_create_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_create_term_t>(env, term);
}

counted_t<term_t> make_table_drop_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_drop_term_t>(env, term);
}

counted_t<term_t> make_table_list_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<table_list_term_t>(env, term);
}

counted_t<term_t> make_config_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<config_term_t>(env, term);
}

counted_t<term_t> make_status_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<status_term_t>(env, term);
}

counted_t<term_t> make_wait_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<wait_term_t>(env, term);
}

counted_t<term_t> make_reconfigure_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<reconfigure_term_t>(env, term);
}

counted_t<term_t> make_rebalance_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<rebalance_term_t>(env, term);
}

counted_t<term_t> make_sync_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<sync_term_t>(env, term);
}



} // namespace ql
