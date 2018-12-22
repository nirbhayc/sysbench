#include <assert.h>
#include <cdb2api.h>
#include "db_driver.h"
#include "sb_options.h"

/* Comdb2 driver arguments */
static sb_arg_t comdb2_drv_args[] = {
    SB_OPT("comdb2-db", "Comdb2 database name", "sbtest", STRING),
    SB_OPT("comdb2-host", "Comdb2 server host", "localhost", STRING),
    SB_OPT("comdb2-ignore-errors", "List of error codes to ignore", "", LIST),
    SB_OPT(
        "comdb2-verbose",
        "Print more information. (1: query, 2: effects, 3: result, 4: debug)",
        0, INT),
    /* TODO: Add options for user, password & ssl */
    SB_OPT_END};

typedef struct {
    char *db;
    char *host;
    sb_list_t *ignored_errors;
    int verbose;
} comdb2_drv_args_t;

static comdb2_drv_args_t args;

/* Whether to use or emulate server-side prepared statement? */
static char use_ps;

/* Comdb2 driver capabilities */
static drv_caps_t comdb2_drv_caps = {
    1, /* supports multi-row insert */
    1, /* supports prepared statement */
    0, /* supports auto increment */
    0, /* needs explicit commit after INSERTs */
    0, /* supports SERIAL clause */
    0, /* supports UNSIGNED INT types */
};

static int comdb2_drv_init(void)
{
    args.db = sb_get_value_string("comdb2-db");
    args.host = sb_get_value_string("comdb2-host");
    args.ignored_errors = sb_get_value_list("comdb2-ignore-errors");
    args.verbose = sb_get_value_int("comdb2-verbose");

    /* Bump the verbosity appropriately */
    if (args.verbose > 0) {
        sb_globals.verbosity = LOG_INFO;
    }

    if (args.verbose > 3) {
        sb_globals.verbosity = LOG_DEBUG;
    }

    if (db_globals.ps_mode != DB_PS_MODE_DISABLE) {
        use_ps = 1;
    } else {
        use_ps = 0;
    }

    return DB_ERROR_NONE;
}

static int comdb2_drv_describe(drv_caps_t *caps)
{
    *caps = comdb2_drv_caps;
    return DB_ERROR_NONE;
}

static db_error_t check_error(cdb2_hndl_tp *conn_hndl, const char *func,
                              const char *query, sb_counter_type_t *counter,
                              int rc)
{
    sb_list_item_t *pos;
    int tmp;

    *counter = SB_CNT_ERROR;

    SB_LIST_FOR_EACH(pos, args.ignored_errors)
    {
        const char *val = SB_LIST_ENTRY(pos, value_t, listitem)->data;
        tmp = atoi(val);
        if (rc == tmp) {
            log_text(LOG_DEBUG, "Ignoring error %d", rc);
        }

        /* TODO: Check if we need to reconnect */

        log_text(LOG_WARNING, "%s failed (query: %s, reason: %s, rc: %d)", func,
                 (query) ? query : "(null)", cdb2_errstr(conn_hndl), rc);
        return DB_ERROR_IGNORABLE;
    }

    log_text(LOG_FATAL, "%s failed (query: %s, reason: %s, rc: %d)", func,
             (query) ? query : "(null)", cdb2_errstr(conn_hndl), rc);

    return DB_ERROR_FATAL;
}

/* Connect to Comdb2 */
static int comdb2_drv_connect(db_conn_t *sb_conn)
{
    cdb2_hndl_tp *conn_hndl;
    int rc;

    conn_hndl = NULL;

    rc = cdb2_open(&conn_hndl, args.db, args.host, CDB2_DIRECT_CPU);
    if (rc) {
        log_text(LOG_FATAL, "cdb2_open() failed (reason: %s, rc: %d)",
                 cdb2_errstr(conn_hndl), rc);
        cdb2_close(conn_hndl);
        return DB_ERROR_FATAL;
    }

    sb_conn->ptr = conn_hndl;

    return DB_ERROR_NONE;
}

/* Close Comdb2 connection handle */
static int comdb2_drv_disconnect(db_conn_t *sb_conn)
{
    cdb2_close((cdb2_hndl_tp *)sb_conn->ptr);
    sb_conn->ptr = 0;

    return DB_ERROR_NONE;
}

/* Prepare a statement */
static int comdb2_drv_prepare(db_stmt_t *stmt, const char *query, size_t len)
{
    (void)len;

    if (!use_ps) {
        stmt->emulated = 1;
    }

    /* Create a copy of the query */
    stmt->query = strdup(query);

    return DB_ERROR_NONE;
}

/* DB-to-PgSQL bind types map */
typedef struct {
    db_bind_type_t db_type;
    int comdb2_type;
} db_comdb2_bind_map_t;

db_comdb2_bind_map_t db_comdb2_bind_map[] = {{DB_TYPE_TINYINT, CDB2_INTEGER},
                                             {DB_TYPE_SMALLINT, CDB2_INTEGER},
                                             {DB_TYPE_INT, CDB2_INTEGER},
                                             {DB_TYPE_BIGINT, CDB2_INTEGER},
                                             {DB_TYPE_FLOAT, CDB2_REAL},
                                             {DB_TYPE_DOUBLE, CDB2_REAL},
                                             {DB_TYPE_DATETIME, CDB2_DATETIME},
                                             {DB_TYPE_TIMESTAMP, CDB2_INTEGER},
                                             {DB_TYPE_CHAR, CDB2_CSTRING},
                                             {DB_TYPE_VARCHAR, CDB2_CSTRING},
                                             {DB_TYPE_NONE, 0}};

static int db_to_comdb2_type(db_bind_type_t type)
{
    int i;
    for (i = 0; db_comdb2_bind_map[i].db_type != DB_TYPE_NONE; i++) {
        if (db_comdb2_bind_map[i].db_type == type) {
            return db_comdb2_bind_map[i].comdb2_type;
        }
    }
    return -1;
}

static int comdb2_drv_bind_param(db_stmt_t *stmt, db_bind_t *params, size_t len)
{
    size_t sz;

    if (stmt->bound_param != NULL) {
        free(stmt->bound_param);
    }

    sz = len * sizeof(db_bind_t);

    stmt->bound_param = (db_bind_t *)malloc(sz);
    if (stmt->bound_param == NULL) {
        log_text(LOG_FATAL, "ERROR: exiting comdb2_drv_bind_param(), "
                            "memory allocation failure");
        return DB_ERROR_FATAL;
    }
    memcpy(stmt->bound_param, params, sz);
    stmt->bound_param_len = len;

    /*
      In case of prepared statements, we defer the binding of parameters
      in cdb2_drv_execute(), as parameters are cached in comdb2 connection
      handle and thus need to be cleared after cdb2_run_statement().
    */

    return DB_ERROR_NONE;
}

static int comdb2_drv_bind_result(db_stmt_t *stmt, db_bind_t *params,
                                  size_t len)
{
    /* Ignore, specific to MySQL/MariaDB? ? */
    (void)stmt;
    (void)params;
    (void)len;

    return DB_ERROR_NONE;
}

/* Forward declaration */
static db_error_t comdb2_drv_query(db_conn_t *sb_conn, const char *query,
                                   size_t len, db_result_t *rs);

static db_error_t comdb2_drv_execute(db_stmt_t *stmt, db_result_t *rs)
{
    db_conn_t *conn;
    db_bind_t *params;
    unsigned int len;

    cdb2_hndl_tp *conn_hndl;
    cdb2_effects_tp effects;

    char *buf = NULL;
    unsigned int buflen = 0;
    unsigned int j, vcnt;
    char need_realloc;
    int n;
    db_error_t rc;
    size_t i;
    int type;

    conn = stmt->connection;
    conn_hndl = conn->ptr;

    if (!stmt->emulated) {
        if (SB_UNLIKELY(args.verbose > 0)) {
            log_text(LOG_INFO, "db_execute(): %s", stmt->query);
        }

        params = stmt->bound_param;
        len = stmt->bound_param_len;

        /* Bind the paramaters */
        for (i = 0; i < len; i++) {
            type = db_to_comdb2_type(params[i].type);
            if (type == -1) {
                log_text(LOG_FATAL,
                         "comdb2_drv_bind_param(): unsupported parameter type "
                         "(reason: %s, rc: %d)",
                         cdb2_errstr(conn_hndl), rc);
                cdb2_close(conn_hndl);
                conn->ptr = 0;
                return DB_ERROR_FATAL;
            }

            if (params[i].is_null && *params[i].is_null) {
                rc = cdb2_bind_index(conn_hndl, i + 1, type, 0, 0);
            } else {
                rc = cdb2_bind_index(conn_hndl, i + 1, type, params[i].buffer,
                                     params[i].max_len);
            }
            if (rc) {
                rc = check_error(conn_hndl, "cdb2_bind_index()", stmt->query,
                                 &rs->counter, rc);
                cdb2_close(conn_hndl);
                conn->ptr = 0;
                return rc;
            }
        }

        rc = cdb2_run_statement(conn_hndl, stmt->query);
        if (rc) {
            rc = check_error(conn_hndl, "cdb2_run_statement()", stmt->query,
                             &rs->counter, rc);
            cdb2_close(conn_hndl);
            conn->ptr = 0;
            return rc;
        }

        rc = cdb2_get_effects(conn_hndl, &effects);
        if (rc) {
            rc = check_error(conn_hndl, "cdb2_get_effects()", stmt->query,
                             &rs->counter, rc);
            cdb2_close(conn_hndl);
            conn->ptr = 0;
            return rc;
        }

        if (effects.num_affected > 0) {
            rs->nrows = effects.num_affected;
            rs->counter = SB_CNT_WRITE;
        } else if (effects.num_selected > 0) {
            rs->nrows = effects.num_selected;
            rs->counter = SB_CNT_READ;
        }

        if (SB_UNLIKELY(args.verbose > 1)) {
            log_text(LOG_INFO,
                     "cdb2_get_effects(): affected: %d, selected: "
                     "%d, updated: %d, deleted: %d, inserted: %d",
                     effects.num_affected, effects.num_selected,
                     effects.num_updated, effects.num_deleted,
                     effects.num_inserted);
        }

        rc = cdb2_clearbindings(conn_hndl);
        if (rc) {
            rc = check_error(conn_hndl, "cdb2_clearbindings()", stmt->query,
                             &rs->counter, rc);
            cdb2_close(conn_hndl);
            conn->ptr = 0;
            return rc;
        }

        return DB_ERROR_NONE;
    }

    /*
      Use emulation
      Build the actual query string from parameters list.
      NOTE: The following logic has been copied from drv_mysql.c in an
      attempt to keep the code common across all drivers.
    */
    need_realloc = 1;
    vcnt = 0;
    for (i = 0, j = 0; stmt->query[i] != '\0'; i++) {
    again:
        if (j + 1 >= buflen || need_realloc) {
            buflen = (buflen > 0) ? buflen * 2 : 256;
            buf = realloc(buf, buflen);
            if (buf == NULL) {
                log_text(LOG_FATAL, "ERROR: exiting comdb2_drv_execute(), "
                                    "memory allocation failure");
                return DB_ERROR_FATAL;
            }
            need_realloc = 0;
        }

        if (stmt->query[i] != '?') {
            buf[j++] = stmt->query[i];
            continue;
        }

        n = db_print_value(stmt->bound_param + vcnt, buf + j,
                           (int)(buflen - j));
        if (n < 0) {
            need_realloc = 1;
            goto again;
        }
        j += (unsigned int)n;
        vcnt++;
    }
    buf[j] = '\0';

    rc = comdb2_drv_query(conn, buf, j, rs);

    free(buf);
    return DB_ERROR_NONE;
}

static int comdb2_drv_fetch(db_result_t *rs)
{
    (void)rs;

    return DB_ERROR_NONE;
}

static int comdb2_drv_fetch_row(db_result_t *rs, db_row_t *row)
{
    (void)rs;
    (void)row;
    assert(0);
    return DB_ERROR_NONE;
}

static int comdb2_drv_free_results(db_result_t *rs)
{
    (void)rs;
    return DB_ERROR_NONE;
}

static int comdb2_drv_close(db_stmt_t *stmt)
{
    (void)stmt;
    /* TODO: free resources */
    return DB_ERROR_NONE;
}

static db_error_t comdb2_drv_query(db_conn_t *sb_conn, const char *query,
                                   size_t len, db_result_t *rs)
{
    cdb2_hndl_tp *conn_hndl;
    cdb2_effects_tp effects;
    int rc;

    sb_conn->sql_errno = 0;
    sb_conn->sql_state = NULL;
    sb_conn->sql_errmsg = NULL;

    conn_hndl = sb_conn->ptr;

    (void)len;

    if (SB_UNLIKELY(args.verbose > 0)) {
        log_text(LOG_INFO, "db_query(): %s", query);
    }

    rc = cdb2_run_statement(conn_hndl, query);
    if (SB_UNLIKELY(rc != 0)) {
        rc = check_error(conn_hndl, "cdb2_run_statement()", query, &rs->counter,
                         rc);
        cdb2_close(conn_hndl);
        sb_conn->ptr = 0;
        return rc;
    }

    rc = cdb2_get_effects(conn_hndl, &effects);
    if (rc) {
        rc = check_error(conn_hndl, "cdb2_get_effects()", query, &rs->counter,
                         rc);
        cdb2_close(conn_hndl);
        sb_conn->ptr = 0;
        return rc;
    }

    if (effects.num_affected > 0) {
        rs->nrows = effects.num_affected;
        rs->counter = SB_CNT_WRITE;
    } else if (effects.num_selected > 0) {
        rs->nrows = effects.num_selected;
        rs->counter = SB_CNT_READ;
    }

    if (SB_UNLIKELY(args.verbose > 1)) {
        log_text(LOG_INFO,
                 "cdb2_get_effects(): affected: %d, selected: %d, "
                 "updated: %d, deleted: %d, inserted: %d",
                 effects.num_affected, effects.num_selected,
                 effects.num_updated, effects.num_deleted,
                 effects.num_inserted);
    }

    /* Retrieve all the records */
    while (CDB2_OK == (rc = cdb2_next_record(conn_hndl)))
        ;

    switch (rc) {
    case CDB2_OK_DONE:
        break; /* Result set exhausted */
    default:
        rc = check_error(conn_hndl, "cdb2_next_record()", query, &rs->counter,
                         rc);
        cdb2_close(conn_hndl);
        sb_conn->ptr = 0;
        return rc;
    }

    return DB_ERROR_NONE;
}

static int comdb2_drv_done(void)
{
    return DB_ERROR_NONE;
}

/* Comdb2 driver definition */
static db_driver_t comdb2_driver = {
    .sname = "comdb2",
    .lname = "Comdb2 driver",
    .args = comdb2_drv_args,
    .ops = {.init = comdb2_drv_init,
            /* .thread_init = 0, */
            .describe = comdb2_drv_describe,
            .connect = comdb2_drv_connect,
            .disconnect = comdb2_drv_disconnect,
            /* .reconnect = 0 */
            .prepare = comdb2_drv_prepare,
            .bind_param = comdb2_drv_bind_param,
            .bind_result = comdb2_drv_bind_result,
            .execute = comdb2_drv_execute,
            .fetch = comdb2_drv_fetch,
            .fetch_row = comdb2_drv_fetch_row,
            .free_results = comdb2_drv_free_results,
            .close = comdb2_drv_close,
            .query = comdb2_drv_query,
            /* .thread_done = 0, */
            .done = comdb2_drv_done}};

int register_driver_comdb2(sb_list_t *drivers)
{
    SB_LIST_ADD_TAIL(&comdb2_driver.listitem, drivers);
    return 0;
}
