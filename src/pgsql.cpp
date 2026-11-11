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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <mutex>

#include <netdb.h>
#include <sys/socket.h>
#include <string>
#include <vector>

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

//! Appends a keyword with a quoted value to a libpq connection string
/** The value is escaped as libpq requires, so that a value cannot end the quotes and add keywords of its own
*/
static void pgsql_append_conninfo(QoreString& out, const char* keyword, const char* value) {
    out.sprintf("%s='", keyword);
    for (const char* p = value; *p; ++p) {
        if (*p == '\\' || *p == '\'') {
            out.concat('\\');
        }
        out.concat(*p);
    }
    out.concat("' ");
}

//! Builds the libpq connection string of a datasource; the host and port are left out if \a host_port is false
static void pgsql_conninfo(Datasource* ds, QoreString& lstr, bool host_port = true) {
    if (ds->getUsername()) {
        pgsql_append_conninfo(lstr, "user", ds->getUsername());
    }
    if (ds->getPassword()) {
        pgsql_append_conninfo(lstr, "password", ds->getPassword());
    }
    if (ds->getDBName()) {
        pgsql_append_conninfo(lstr, "dbname", ds->getDBName());
    }
    if (host_port) {
        if (ds->getHostName()) {
            pgsql_append_conninfo(lstr, "host", ds->getHostName());
        }
        if (ds->getPort()) {
            lstr.sprintf("port=%d ", ds->getPort());
        }
    }
}

#ifdef QDBI_METHOD_GET_CONNECT_TARGETS
//! Returns the host that the loaded libpq uses for an entry without a host: its default UNIX socket directory, or
//! \c localhost for a libpq without UNIX sockets
/** libpq does not export its compiled-in default; it fills it in when it validates the connection options, so a
    connection started with an invalid \c sslmode fails there, without any I/O, with the default host in place.  The
    host and hostaddr are given (empty), so that neither PGHOST nor PGHOSTADDR applies, and a user and a password, so
    that libpq neither looks up the user nor reads the password file.

    @return 0 for success, -1 if an exception was raised
*/
static int pgsql_default_host(std::string& host, ExceptionSink* xsink) {
    static std::mutex m;
    static std::string cached;
    std::lock_guard<std::mutex> lock(m);
    if (cached.empty()) {
        std::unique_ptr<PGconn, decltype(&PQfinish)> pc(PQconnectStart("host='' hostaddr='' port='1' "
            "user='-' password='-' sslmode='invalid'"), PQfinish);
        if (!pc) {
            xsink->raiseException("DBI:PGSQL:ERROR", "the PostgreSQL client library could not allocate a "
                "connection");
            return -1;
        }
        const char* h = PQhost(pc.get());
        if (!h || !*h) {
            xsink->raiseException("NETWORK-ACCESS-DENIED", "cannot determine the default destination of the "
                "PostgreSQL client library, so a connection without a host cannot be checked against the sandbox");
            return -1;
        }
        cached = h;
    }
    host = cached;
    return 0;
}

//! Parses a port as libpq does (leading and trailing white space allowed, 1 - 65535)
/** @return the port, or 0 if it is invalid
*/
static int pgsql_parse_port(const std::string& value) {
    const char* start = value.c_str();
    char* end;
    errno = 0;
    long port = strtol(start, &end, 10);
    if (end == start || errno) {
        return 0;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (*end || port < 1 || port > 65535) {
        return 0;
    }
    return (int)port;
}

//! Splits a comma-separated libpq list
static std::vector<std::string> pgsql_split_list(const std::string& value) {
    std::vector<std::string> rv;
    size_t start = 0;
    while (true) {
        size_t end = value.find(',', start);
        rv.emplace_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return rv;
}

//! A destination of a libpq connection
struct PgsqlTarget {
    //! the host name as libpq uses it (for TLS, .pgpass and GSSAPI), or the socket directory
    std::string host;
    //! the numeric address (hostaddr), if given
    std::string addr;
    //! the port
    std::string port;
    //! true for a UNIX socket
    bool socket = false;
};

//! Determines the destinations that libpq connects to for a datasource, as libpq does (connectOptions2())
/** The connection string of the datasource is completed with libpq's defaults (PGHOST, PGHOSTADDR, PGPORT and the
    compiled-in values): every entry of a multi-host list is a destination; an entry with a \c hostaddr connects to
    that address, a host that is a directory to the UNIX socket \c .s.PGSQL.<port> in it, an empty host to that socket
    in libpq's default directory, and any other host by TCP.  A connection that uses a service file (\c service or
    PGSERVICE) is refused, as the service file can name any destination, and so is an abstract UNIX socket (a host
    starting with \c @), which no filesystem policy governs.

    @return 0 for success, -1 if an exception was raised
*/
static int pgsql_get_targets(Datasource* ds, std::vector<PgsqlTarget>& targets, ExceptionSink* xsink) {
    // the pinned connection lists every checked address of a host name as an entry of its own, which needs
    // multi-host lists (libpq 10), and the timeout restart tells such entries apart by their address (PQhostaddr(),
    // libpq 12)
#if defined(HAVE_PQLIBVERSION) && defined(HAVE_PQHOSTADDR) && defined(HAVE_PQCONNINFO)
    if (PQlibVersion() < 120000) {
        xsink->raiseException("NETWORK-ACCESS-DENIED", "the PostgreSQL client library version %d cannot connect to "
            "the addresses checked against the sandbox only; libpq 12 or later is required", PQlibVersion());
        return -1;
    }
#else
    xsink->raiseException("NETWORK-ACCESS-DENIED", "the pgsql driver was built with a PostgreSQL client library that "
        "cannot connect to the addresses checked against the sandbox only; libpq 12 or later is required");
    return -1;
#endif
    QoreString conninfo;
    pgsql_conninfo(ds, conninfo);
    char* err = nullptr;
    std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> parsed(PQconninfoParse(conninfo.c_str(), &err),
        PQconninfoFree);
    if (!parsed) {
        // the parse error can quote the connection string, which can contain the password
        xsink->raiseException("DBI:PGSQL:ERROR", "invalid PostgreSQL connection parameters%s",
            err ? "" : ": out of memory");
        if (err) {
            PQfreemem(err);
        }
        return -1;
    }
    std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> defaults(PQconndefaults(), PQconninfoFree);
    if (!defaults) {
        xsink->raiseException("DBI:PGSQL:ERROR", "could not allocate the PostgreSQL connection defaults");
        return -1;
    }
    // the value of a keyword: given in the connection string (also if empty, which blocks the default, as in libpq),
    // else libpq's default (an environment variable or the compiled-in value)
    auto value = [&](const char* keyword) -> std::string {
        for (const PQconninfoOption* p = parsed.get(); p->keyword; ++p) {
            if (!strcmp(p->keyword, keyword) && p->val) {
                return p->val;
            }
        }
        for (const PQconninfoOption* p = defaults.get(); p->keyword; ++p) {
            if (!strcmp(p->keyword, keyword)) {
                if (p->val) {
                    return p->val;
                }
                return p->compiled ? p->compiled : "";
            }
        }
        return std::string();
    };
    // the compiled-in port, which libpq uses for an empty element of a port list
    std::string default_port = "5432";
    for (const PQconninfoOption* p = defaults.get(); p->keyword; ++p) {
        if (!strcmp(p->keyword, "port")) {
            if (p->compiled && *p->compiled) {
                default_port = p->compiled;
            }
            break;
        }
    }

    std::string service = value("service");
    if (!service.empty()) {
        xsink->raiseException("NETWORK-ACCESS-DENIED", "the PostgreSQL connection uses the service '%s', whose "
            "destinations cannot be checked against the sandbox", service.c_str());
        return -1;
    }

    std::string host_value = value("host");
    std::string addr_value = value("hostaddr");
    std::vector<std::string> hosts, addrs;
    size_t count;
    if (!addr_value.empty()) {
        addrs = pgsql_split_list(addr_value);
        count = addrs.size();
        if (!host_value.empty()) {
            hosts = pgsql_split_list(host_value);
            if (hosts.size() != count) {
                xsink->raiseException("DBI:PGSQL:ERROR", "could not match %d host names to %d hostaddr values",
                    (int)hosts.size(), (int)count);
                return -1;
            }
        }
    } else {
        hosts = pgsql_split_list(host_value);
        count = hosts.size();
    }
    hosts.resize(count);
    addrs.resize(count);
    std::vector<std::string> ports = pgsql_split_list(value("port"));
    if (ports.size() == 1) {
        ports.resize(count, ports.front());
    } else if (ports.size() != count) {
        xsink->raiseException("DBI:PGSQL:ERROR", "could not match %d port numbers to %d hosts", (int)ports.size(),
            (int)count);
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        if (qore_check_cancel(xsink, "listing the destinations of a PostgreSQL connection")) {
            return -1;
        }
        PgsqlTarget t;
        // the port as libpq parses it, which names the socket and is passed to the pinned connection
        const std::string& port = ports[i].empty() ? default_port : ports[i];
        int portnum = pgsql_parse_port(port);
        if (!portnum) {
            xsink->raiseException("DBI:PGSQL:ERROR", "invalid PostgreSQL port '%s'", port.c_str());
            return -1;
        }
        t.port = std::to_string(portnum);
        t.addr = addrs[i];
        if (!t.addr.empty()) {
            // libpq accepts only a numeric address
            struct addrinfo hints = {};
            hints.ai_flags = AI_NUMERICHOST;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* ai = nullptr;
            if (getaddrinfo(t.addr.c_str(), nullptr, &hints, &ai)) {
                xsink->raiseException("DBI:PGSQL:ERROR", "invalid PostgreSQL hostaddr '%s': not a numeric address",
                    t.addr.c_str());
                return -1;
            }
            freeaddrinfo(ai);
        }
        t.host = hosts[i];
        if (t.addr.empty()) {
            if (t.host.empty()) {
                if (pgsql_default_host(t.host, xsink)) {
                    return -1;
                }
                // a libpq without UNIX sockets (Windows) connects to localhost by TCP
                t.socket = t.host[0] == '/';
            } else if (t.host[0] == '@') {
                xsink->raiseException("NETWORK-ACCESS-DENIED", "the PostgreSQL connection uses the abstract UNIX "
                    "socket '%s', which cannot be checked against the sandbox", t.host.c_str());
                return -1;
            } else if (t.host[0] == '/') {
                t.socket = true;
            }
        }
        targets.push_back(std::move(t));
    }
    return 0;
}

//! Returns the destinations that libpq connects to for a datasource, for the sandbox (qore#5747)
static QoreListNode* pgsql_get_connect_targets(Datasource* ds, ExceptionSink* xsink) {
    std::vector<PgsqlTarget> targets;
    if (pgsql_get_targets(ds, targets, xsink)) {
        return nullptr;
    }
    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
    for (const PgsqlTarget& t : targets) {
        ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);
        if (t.socket) {
            h->setKeyValue("socket", new QoreStringNode(t.host + "/.s.PGSQL." + t.port), xsink);
        } else {
            // a hostaddr is what libpq connects to
            h->setKeyValue("host", new QoreStringNode(t.addr.empty() ? t.host : t.addr), xsink);
            h->setKeyValue("port", atoi(t.port.c_str()), xsink);
        }
        rv->push(h.release(), xsink);
    }
    return rv.release();
}

//! Builds the host part of the connection string from the destinations that were checked against the sandbox
/** Each checked address of a TCP destination becomes an entry with that address as \c hostaddr, and the host name of
    the destination as \c host, so that libpq connects to exactly the checked addresses (also when it reconnects with
    PQreset() or restarts after a timeout) while TLS, .pgpass and GSSAPI still use the host name; a UNIX socket entry
    keeps its directory as \c host.  The destinations are determined again and must match the checked ones.

    @note a host with several addresses becomes several entries, so it has more weight with
    \c load_balance_hosts=random

    @return 0 for success, -1 if an exception was raised
*/
static int pgsql_checked_conninfo(Datasource* ds, const QoreListNode* checked, QoreString& lstr,
        ExceptionSink* xsink) {
    std::vector<PgsqlTarget> targets;
    if (pgsql_get_targets(ds, targets, xsink)) {
        return -1;
    }
    if (targets.size() != checked->size()) {
        xsink->raiseException("NETWORK-ACCESS-DENIED", "the destinations of the PostgreSQL connection changed after "
            "they were checked against the sandbox");
        return -1;
    }
    std::string host_list, addr_list, port_list;
    bool first = true;
    auto add = [&](const std::string& host, const std::string& addr, const std::string& port) {
        if (!first) {
            host_list += ',';
            addr_list += ',';
            port_list += ',';
        }
        first = false;
        host_list += host;
        addr_list += addr;
        port_list += port;
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        const PgsqlTarget& t = targets[i];
        QoreValue cv = checked->retrieveEntry(i);
        const QoreHashNode* c = cv.getType() == NT_HASH ? cv.get<const QoreHashNode>() : nullptr;
        if (!c) {
            xsink->raiseException("NETWORK-ACCESS-DENIED", "invalid checked destination of a PostgreSQL connection");
            return -1;
        }
        if (t.socket) {
            QoreValue path = c->getKeyValue("socket");
            if (path.getType() != NT_STRING
                || (t.host + "/.s.PGSQL." + t.port) != path.get<const QoreStringNode>()->c_str()) {
                xsink->raiseException("NETWORK-ACCESS-DENIED", "the destinations of the PostgreSQL connection "
                    "changed after they were checked against the sandbox");
                return -1;
            }
            add(t.host, "", t.port);
            continue;
        }
        QoreValue host = c->getKeyValue("host");
        if (host.getType() != NT_STRING || (t.addr.empty() ? t.host : t.addr) != host.get<const QoreStringNode>()->c_str()
            || c->getKeyValue("port").getAsBigInt() != atoi(t.port.c_str())) {
            xsink->raiseException("NETWORK-ACCESS-DENIED", "the destinations of the PostgreSQL connection changed "
                "after they were checked against the sandbox");
            return -1;
        }
        // the host name for TLS, .pgpass and GSSAPI: an entry given only as an address keeps it empty, as in libpq
        QoreValue av = c->getKeyValue("addresses");
        const QoreListNode* addresses = av.getType() == NT_LIST ? av.get<const QoreListNode>() : nullptr;
        if (!addresses || addresses->empty()) {
            // an entry without an address would connect to the default socket, which was not checked
            xsink->raiseException("NETWORK-ACCESS-DENIED", "a checked destination of a PostgreSQL connection has no "
                "address");
            return -1;
        }
        ConstListIterator ai(addresses);
        while (ai.next()) {
            add(t.host, ai.getValue().get<const QoreStringNode>()->c_str(), t.port);
        }
    }
    pgsql_append_conninfo(lstr, "host", host_list.c_str());
    pgsql_append_conninfo(lstr, "hostaddr", addr_list.c_str());
    pgsql_append_conninfo(lstr, "port", port_list.c_str());
    return 0;
}
#endif

static int qore_pgsql_open(Datasource* ds, ExceptionSink* xsink) {
    printd(5, "qore_pgsql_open() datasource %08p for DB=%s\n", ds,
        ds->getDBName() ? ds->getDBName() : "unknown");

    // string for connection arguments
    QoreString lstr;
#ifdef QDBI_METHOD_GET_CONNECT_TARGETS
    // a connection checked against a sandbox connects to the checked addresses only
    if (const QoreListNode* checked = ds->getCheckedConnectTargets()) {
        pgsql_conninfo(ds, lstr, false);
        if (pgsql_checked_conninfo(ds, checked, lstr, xsink)) {
            return -1;
        }
    } else {
        pgsql_conninfo(ds, lstr);
    }
#else
    pgsql_conninfo(ds, lstr);
#endif

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
#ifdef QDBI_METHOD_GET_CONNECT_TARGETS
    methods.add(QDBI_METHOD_GET_CONNECT_TARGETS, pgsql_get_connect_targets);
#endif
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
    methods.registerOption(DBI_OPT_TIMEZONE, "the time zone of the connection: date/time values are written and read in this zone, and it is set as the session's time zone (SET TIME ZONE) on every connection and reconnection, so server-side date/time functions use it too; the value is an IANA region name (ex: \"Europe/Prague\") or a UTC offset east of UTC in whole minutes (ex: \"+01:00\" or \"-05:30\"); if not set, the session keeps the server's time zone and the server's time zone is assumed to be the same as the client's", stringTypeInfo);

    // libpq connection options, applied to the connection at connect time; TCP keepalives are enabled
    // by default with an aggressive idle time so PostgreSQL promptly reaps backends orphaned by an
    // unclean client exit (otherwise idle orphans persist until the OS keepalive default, often 2
    // hours, and can exhaust max_connections)
    methods.registerOption(PGSQL_OPT_KEEPALIVES, "controls whether client-side TCP keepalives are used on the connection (default: True); disabling this turns off all keepalive settings below", boolTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_IDLE, "seconds of idle time before the first TCP keepalive probe is sent (libpq keepalives_idle; default: 60); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_INTERVAL, "seconds between TCP keepalive probes after the first (libpq keepalives_interval; default: 10); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_KEEPALIVES_COUNT, "number of unacknowledged TCP keepalive probes before the connection is considered dead (libpq keepalives_count; default: 3); ignored when 'keepalives' is False", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_CONNECT_TIMEOUT, "maximum time in seconds for each connection endpoint; a timed-out host is skipped and the remaining hosts are tried (libpq connect_timeout); 0 (the default) means no client-side limit", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_TCP_USER_TIMEOUT, "maximum time in milliseconds that transmitted data may remain unacknowledged before the connection is closed (libpq tcp_user_timeout, TCP_USER_TIMEOUT socket option; default: 30000); bounds how long a query in flight blocks when the server's host disappears without closing the connection, which keepalives do not cover; 0 means use the operating system default; requires libpq 12 or later (the default is not applied with an older client library)", bigIntTypeInfo);
    methods.registerOption(PGSQL_OPT_APPLICATION_NAME, "an application name reported to the server and shown in pg_stat_activity.application_name (libpq application_name); useful for identifying which client/pool owns each backend connection; unset by default (the libpq default applies)", stringTypeInfo);

    DBID_PGSQL = DBI.registerDriver("pgsql", methods, pgsql_caps);
}

static void pgsql_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
   qns->addInitialNamespace(pgsql_ns.copy());
}

static void pgsql_module_delete() {
}
