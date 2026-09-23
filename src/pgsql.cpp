/*
    pgsql.cpp

    Qore Programming Language

    Copyright 2003 - 2026 Qore Technologies, s.r.o.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "pgsql.h"

#include "QorePGConnection.h"
#include "QorePGMapper.h"

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
#include <qore/QoreColumnarResult.h>
#endif

#include <libpq-fe.h>

void init_pgsql_functions(QoreNamespace& ns);
void init_pgsql_constants(QoreNamespace& ns);

static void pgsql_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink);
static void pgsql_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink);
static void pgsql_module_delete();

extern "C" DLLEXPORT void pgsql_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = "pgsql";
    mod_info.version = QORE_MODULE_PACKAGE_VERSION;
    mod_info.desc = "PostgreSQL module";
    mod_info.author = "David Nichols";
    mod_info.url = "http://qore.org";
    mod_info.api_major = QORE_MODULE_API_MAJOR;
    mod_info.api_minor = QORE_MODULE_API_MINOR;
    mod_info.init = pgsql_module_init;
    mod_info.ns_init = pgsql_module_ns_init;
    mod_info.del = pgsql_module_delete;
    mod_info.license = QL_MIT;
    mod_info.license_str = "MIT";
}

static QoreNamespace pgsql_ns("PGSQL");

static int pgsql_caps = DBI_CAP_TRANSACTION_MANAGEMENT
   | DBI_CAP_CHARSET_SUPPORT
   | DBI_CAP_STORED_PROCEDURES
   | DBI_CAP_LOB_SUPPORT
   | DBI_CAP_BIND_BY_VALUE
   | DBI_CAP_HAS_EXECRAW
   | DBI_CAP_TIME_ZONE_SUPPORT
   | DBI_CAP_HAS_NUMBER_SUPPORT
   | DBI_CAP_SERVER_TIME_ZONE
   | DBI_CAP_AUTORECONNECT
   | DBI_CAP_HAS_ARRAY_BIND
#ifdef QDBI_METHOD_SELECT_TYPED
   | DBI_CAP_HAS_TYPED_SELECT
#endif
#ifdef QDBI_METHOD_SELECT_COLUMNAR
   | DBI_CAP_HAS_COLUMNAR_SELECT
#endif
;

DBIDriver *DBID_PGSQL = nullptr;

static int qore_pgsql_commit(Datasource* ds, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->commit(xsink);
}

static int qore_pgsql_rollback(Datasource* ds, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->rollback(xsink);
}

static int qore_pgsql_begin_transaction(Datasource* ds, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->begin_transaction(xsink);
}

static QoreValue qore_pgsql_select_rows(Datasource* ds, const QoreString *qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->selectRows(qstr, args, xsink);
}

#ifdef QDBI_METHOD_SELECT_TYPED
static QoreValue qore_pgsql_select_rows_typed(Datasource* ds, const QoreString* qstr, const QoreListNode* args,
        ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();

    return pc->selectRowsTyped(qstr, args, xsink);
}
#endif

static QoreHashNode* qore_pgsql_select_row(Datasource* ds, const QoreString *qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->selectRow(qstr, args, xsink);
}

static QoreValue qore_pgsql_select(Datasource* ds, const QoreString *qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->select(qstr, args, xsink);
}

#ifdef QDBI_METHOD_SELECT_TYPED
static QoreValue qore_pgsql_select_typed(Datasource* ds, const QoreString* qstr, const QoreListNode* args,
        ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();

    return pc->selectTyped(qstr, args, xsink);
}
#endif

#ifdef QDBI_METHOD_SELECT_COLUMNAR
static QoreColumnarResult* qore_pgsql_select_columnar(Datasource* ds, const QoreString* qstr,
        const QoreListNode* args, ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();

    return pc->selectColumnar(qstr, args, xsink);
}
#endif

static QoreValue qore_pgsql_exec(Datasource* ds, const QoreString *qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    return pc->exec(qstr, args, xsink);
}

static QoreValue qore_pgsql_execRaw(Datasource* ds, const QoreString *qstr, ExceptionSink* xsink) {
   QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();
   return pc->execRaw(qstr, xsink);
}

#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
static int qore_pgsql_bulk_load_begin(Datasource* ds, const QoreString* table, const QoreListNode* columns,
        const QoreHashNode* options, ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();
    return pc->bulkLoadBegin(table, columns, options, xsink);
}

static int qore_pgsql_bulk_load_rows(Datasource* ds, const QoreHashNode* rows, ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();
    return pc->bulkLoadRows(rows, xsink);
}

static int qore_pgsql_bulk_load_end(Datasource* ds, bool success, ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();
    return pc->bulkLoadEnd(success, xsink);
}
#endif

static int qore_pgsql_open(Datasource* ds, ExceptionSink* xsink) {
    printd(5, "qore_pgsql_open() datasource %08p for DB=%s\n", ds,
        ds->getDBName() ? ds->getDBName() : "unknown");

    // string for connection arguments
    QoreString lstr;
    if (ds->getUsername())
        lstr.sprintf("user='%s' ", ds->getUsername());

    if (ds->getPassword())
        lstr.sprintf("password='%s' ", ds->getPassword());

    if (ds->getDBName())
        lstr.sprintf("dbname='%s' ", ds->getDBName());

    if (ds->getHostName())
        lstr.sprintf("host='%s' ", ds->getHostName());

    if (ds->getPort())
        lstr.sprintf("port=%d ", ds->getPort());

    if (ds->getDBEncoding()) {
        const QoreEncoding *enc = QorePGMapper::getQoreEncoding(ds->getDBEncoding());
        ds->setQoreEncoding(enc);
    } else {
        char *enc = (char *)QorePGMapper::getPGEncoding(QCS_DEFAULT);
        if (!enc) {
            xsink->raiseException("DBI:PGSQL:UNKNOWN-CHARACTER-SET", "cannot find the PostgreSQL character encoding equivalent for '%s'", QCS_DEFAULT->getCode());
            return -1;
        }
        ds->setDBEncoding(enc);
        ds->setQoreEncoding(QCS_DEFAULT);
    }

    QorePGConnection* pc = new QorePGConnection(ds, lstr.c_str(), xsink);

    if (*xsink) {
        delete pc;
        return -1;
    }

    ds->setPrivateData((void *)pc);
    return 0;
}

static int qore_pgsql_close(Datasource* ds) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();

    delete pc;
    ds->setPrivateData(nullptr);
    return 0;
}

static QoreValue qore_pgsql_get_server_version(Datasource* ds, ExceptionSink* xsink) {
    QorePGConnection *pc = (QorePGConnection *)ds->getPrivateData();
    return pc->get_server_version();
}

static QoreValue qore_pgsql_get_client_version(const Datasource* ds, ExceptionSink* xsink) {
#ifdef HAVE_PQLIBVERSION
    return PQlibVersion();
#else
    xsink->raiseException("DBI:PGSQL-GET-CLIENT-VERSION-ERROR", "the version of the PostgreSQL client library that this module was compiled against did not support the PQlibVersion() function");
    return QoreValue();
#endif
}

static int pgsql_stmt_prepare(SQLStatement* stmt, const QoreString& str, const QoreListNode* args, ExceptionSink* xsink) {
   assert(!stmt->getPrivateData());

   QorePgsqlPreparedStatement* bg = new QorePgsqlPreparedStatement(stmt->getDatasource());
   stmt->setPrivateData(bg);

   return bg->prepare(str, args, true, xsink);
}

static int pgsql_stmt_prepare_raw(SQLStatement* stmt, const QoreString& str, ExceptionSink* xsink) {
   assert(!stmt->getPrivateData());

   QorePgsqlPreparedStatement* bg = new QorePgsqlPreparedStatement(stmt->getDatasource());
   stmt->setPrivateData(bg);

   return bg->prepare(str, 0, false, xsink);
}

static int pgsql_stmt_bind(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement* )stmt->getPrivateData();
   assert(bg);

   return bg->bind(l, xsink);
}

static int pgsql_stmt_bind_placeholders(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
   xsink->raiseException("DBI:PGSQL-BIND-PLACEHHODERS-ERROR", "binding placeholders is not necessary or supported with the pgsql driver");
   return -1;
}

static int pgsql_stmt_bind_values(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement* )stmt->getPrivateData();
   assert(bg);

   return bg->bind(l, xsink);
}

static int pgsql_stmt_exec(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement* )stmt->getPrivateData();
   assert(bg);

   return bg->exec(xsink);
}

static int pgsql_stmt_define(SQLStatement* stmt, ExceptionSink* xsink) {
   // define is a noop in the pgsql driver
   return 0;
}

static int pgsql_stmt_affected_rows(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->rowsAffected();
}

static QoreHashNode* pgsql_stmt_get_output(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->getOutputHash(xsink);
}

static QoreHashNode* pgsql_stmt_get_output_rows(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->getOutputHash(xsink);
}

static QoreHashNode* pgsql_stmt_fetch_row(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->fetchRow(xsink);
}

static QoreListNode* pgsql_stmt_fetch_rows(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->fetchRows(rows, xsink);
}

static QoreHashNode* pgsql_stmt_fetch_columns(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->fetchColumns(rows, xsink);
}

#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
static QoreColumnarResult* pgsql_stmt_fetch_columnar(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->fetchColumnar(rows, xsink);
}
#endif

static QoreHashNode* pgsql_stmt_describe(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->describe(xsink);
}

static bool pgsql_stmt_next(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   return bg->next();
}

static int pgsql_stmt_close(SQLStatement* stmt, ExceptionSink* xsink) {
   QorePgsqlPreparedStatement* bg = (QorePgsqlPreparedStatement*)stmt->getPrivateData();
   assert(bg);

   bg->reset(xsink);
   delete bg;
   stmt->setPrivateData(0);
   return *xsink ? -1 : 0;
}

static int pgsql_opt_set(Datasource* ds, const char* opt, const QoreValue val, ExceptionSink* xsink) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();
    return pc->setOption(opt, val, xsink);
}

static QoreValue pgsql_opt_get(const Datasource* ds, const char* opt) {
    QorePGConnection* pc = (QorePGConnection*)ds->getPrivateData();
    return pc->getOption(opt);
}

static void pgsql_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink) {
#ifdef HAVE_PQISTHREADSAFE
    if (!PQisthreadsafe()) {
        xsink.raiseException("MODULE-INIT-ERROR", "cannot load pgsql module; the PostgreSQL library on this system is not thread-safe");
        return;
    }
#endif

    init_pgsql_functions(pgsql_ns);
    init_pgsql_constants(pgsql_ns);

    QorePGMapper::static_init();
    QorePgsqlStatement::static_init();

    // register database functions with DBI subsystem
    qore_dbi_method_list methods;
    methods.add(QDBI_METHOD_OPEN, qore_pgsql_open);
    methods.add(QDBI_METHOD_CLOSE, qore_pgsql_close);
    methods.add(QDBI_METHOD_SELECT, qore_pgsql_select);
    methods.add(QDBI_METHOD_SELECT_ROWS, qore_pgsql_select_rows);
#ifdef QDBI_METHOD_SELECT_TYPED
    methods.add(QDBI_METHOD_SELECT_TYPED, qore_pgsql_select_typed);
    methods.add(QDBI_METHOD_SELECT_ROWS_TYPED, qore_pgsql_select_rows_typed);
#endif
#ifdef QDBI_METHOD_SELECT_COLUMNAR
    methods.add(QDBI_METHOD_SELECT_COLUMNAR, qore_pgsql_select_columnar);
#endif
    methods.add(QDBI_METHOD_SELECT_ROW, qore_pgsql_select_row);
    methods.add(QDBI_METHOD_EXEC, qore_pgsql_exec);
    methods.add(QDBI_METHOD_EXECRAW, qore_pgsql_execRaw);
    methods.add(QDBI_METHOD_COMMIT, qore_pgsql_commit);
    methods.add(QDBI_METHOD_ROLLBACK, qore_pgsql_rollback);
    methods.add(QDBI_METHOD_BEGIN_TRANSACTION, qore_pgsql_begin_transaction);
    methods.add(QDBI_METHOD_GET_SERVER_VERSION, qore_pgsql_get_server_version);
    methods.add(QDBI_METHOD_GET_CLIENT_VERSION, qore_pgsql_get_client_version);
#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
    methods.add(QDBI_METHOD_BULK_LOAD_BEGIN, qore_pgsql_bulk_load_begin);
    methods.add(QDBI_METHOD_BULK_LOAD_ROWS, qore_pgsql_bulk_load_rows);
    methods.add(QDBI_METHOD_BULK_LOAD_END, qore_pgsql_bulk_load_end);
#endif

    methods.add(QDBI_METHOD_STMT_PREPARE, pgsql_stmt_prepare);
    methods.add(QDBI_METHOD_STMT_PREPARE_RAW, pgsql_stmt_prepare_raw);
    methods.add(QDBI_METHOD_STMT_BIND, pgsql_stmt_bind);
    methods.add(QDBI_METHOD_STMT_BIND_PLACEHOLDERS, pgsql_stmt_bind_placeholders);
    methods.add(QDBI_METHOD_STMT_BIND_VALUES, pgsql_stmt_bind_values);
    methods.add(QDBI_METHOD_STMT_EXEC, pgsql_stmt_exec);
    methods.add(QDBI_METHOD_STMT_DEFINE, pgsql_stmt_define);
    methods.add(QDBI_METHOD_STMT_FETCH_ROW, pgsql_stmt_fetch_row);
    methods.add(QDBI_METHOD_STMT_FETCH_ROWS, pgsql_stmt_fetch_rows);
    methods.add(QDBI_METHOD_STMT_FETCH_COLUMNS, pgsql_stmt_fetch_columns);
#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
    methods.add(QDBI_METHOD_STMT_FETCH_COLUMNAR, pgsql_stmt_fetch_columnar);
#endif
    methods.add(QDBI_METHOD_STMT_DESCRIBE, pgsql_stmt_describe);
    methods.add(QDBI_METHOD_STMT_NEXT, pgsql_stmt_next);
    methods.add(QDBI_METHOD_STMT_CLOSE, pgsql_stmt_close);
    methods.add(QDBI_METHOD_STMT_AFFECTED_ROWS, pgsql_stmt_affected_rows);
    methods.add(QDBI_METHOD_STMT_GET_OUTPUT, pgsql_stmt_get_output);
    methods.add(QDBI_METHOD_STMT_GET_OUTPUT_ROWS, pgsql_stmt_get_output_rows);

    methods.add(QDBI_METHOD_OPT_SET, pgsql_opt_set);
    methods.add(QDBI_METHOD_OPT_GET, pgsql_opt_get);

    methods.registerOption(DBI_OPT_NUMBER_OPT, "when set, numeric/decimal values are returned as integers if possible, otherwise as arbitrary-precision number values; the argument is ignored; setting this option turns it on and turns off 'string-numbers' and 'numeric-numbers'");
    methods.registerOption(DBI_OPT_NUMBER_STRING, "when set, numeric/decimal values are returned as strings for backwards-compatibility; the argument is ignored; setting this option turns it on and turns off 'optimal-numbers' and 'numeric-numbers'");
    methods.registerOption(DBI_OPT_NUMBER_NUMERIC, "when set, numeric/decimal values are returned as arbitrary-precision number values; the argument is ignored; setting this option turns it on and turns off 'string-numbers' and 'optimal-numbers'");
    methods.registerOption(DBI_OPT_TIMEZONE, "set the server-side timezone, value must be a string in the format accepted by Timezone::constructor() on the client (ie either a region name or a UTC offset like \"+01:00\"), if not set the server's time zone will be assumed to be the same as the client's", stringTypeInfo);

    // libpq connection options, applied to the connection at connect time; TCP keepalives are enabled
    // by default with an aggressive idle time so PostgreSQL promptly reaps backends orphaned by an
    // unclean client exit (otherwise idle orphans persist until the OS keepalive default, often 2
    // hours, and can exhaust max_connections)
    methods.registerOption(PGSQL_OPT_KEEPALIVES, "controls whether client-side TCP keepalives are used on the connection (default: True); disabling this turns off all keepalive settings below", boolTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_IDLE, "seconds of idle time before the first TCP keepalive probe is sent (libpq keepalives_idle; default: 60); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_INTERVAL, "seconds between TCP keepalive probes after the first (libpq keepalives_interval; default: 10); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_COUNT, "number of unacknowledged TCP keepalive probes before the connection is considered dead (libpq keepalives_count; default: 3); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_CONNECT_TIMEOUT, "maximum time in seconds to wait when establishing a connection (libpq connect_timeout); 0 (the default) means use the libpq default (no client-side limit)", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_TCP_USER_TIMEOUT, "maximum time in milliseconds that transmitted data may remain unacknowledged before the connection is closed (libpq tcp_user_timeout, TCP_USER_TIMEOUT socket option; default: 30000); bounds how long a query in flight blocks when the server's host disappears without closing the connection, which keepalives do not cover; 0 means use the operating system default; requires libpq 12 or later (the default is not applied with an older client library)", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_APPLICATION_NAME, "an application name reported to the server and shown in pg_stat_activity.application_name (libpq application_name); useful for identifying which client/pool owns each backend connection; unset by default (the libpq default applies)", stringTypeInfo);

    DBID_PGSQL = DBI.registerDriver("pgsql", methods, pgsql_caps);
}

static void pgsql_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
   qns->addInitialNamespace(pgsql_ns.copy());
}

static void pgsql_module_delete() {
}
