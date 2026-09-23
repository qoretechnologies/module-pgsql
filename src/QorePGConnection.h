/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QorePGConnection.h

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

#ifndef _QORE_QOREPGCONNECTION_H
#define _QORE_QOREPGCONNECTION_H

#include <qore/safe_dslist>
#include <qore/QoreSandboxManager.h>

#include <atomic>
#include <climits>
#include <vector>
#include <string>

typedef std::vector<std::string> strvec_t;

class QoreColumnarResult;
#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
class QorePGBulkCopyState;
#endif

// necessary in order to avoid conflicts with qore's int64 type
#define HAVE_INT64

// undefine package constants to avoid extraneous warnings
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#ifdef WIN32
#include <winsock2.h>
#endif

#include <postgres_ext.h>         // for most basic types
#ifdef POSTGRESQL_SERVER_INCLUDES
#include <postgres.h>
#include <utils/nabstime.h>   // for abstime (AbsoluteTime), reltime (RelativeTime), tinterval (TimeInterval)
#include <utils/date.h>       // for date (DateADT)
#include <utils/timestamp.h>  // for interval (Interval*)
#include <storage/itemptr.h>  // for tid (ItemPointer)
#include <utils/date.h>       // for time (TimeADT), time with time zone (TimeTzADT),
#include <utils/timestamp.h>  // for timestamp (Timestamp*)
#include <utils/numeric.h>
#include <utils/inet.h>
#include <utils/geo_decls.h>  // for point, etc
#include <catalog/pg_type.h>
#include <pgtypes_numeric.h>
#else
// the following definitions are taken from PostgreSQL server header files
#define BOOLOID                 16
#define BYTEAOID                17
#define CHAROID                 18
#define NAMEOID                 19
#define INT8OID                 20
#define INT2OID                 21
#define INT2VECTOROID           22
#define INT4OID                 23
#define REGPROCOID              24
#define TEXTOID                 25
#define OIDOID                  26
#define TIDOID                  27
#define XIDOID                  28
#define CIDOID                  29
#define OIDVECTOROID            30
#define JSONOID                 114
#define XMLOID                  142
#define XMLARRAYOID             143
#define JSONARRAYOID            199
#define POINTOID                600
#define LSEGOID                 601
#define PATHOID                 602
#define BOXOID                  603
#define POLYGONOID              604
#define LINEOID                 628
#define FLOAT4OID               700
#define FLOAT8OID               701
#define ABSTIMEOID              702
#define RELTIMEOID              703
#define TINTERVALOID            704
#define UNKNOWNOID              705
#define CIRCLEOID               718
#define CASHOID                 790
#define MACADDROID              829
#define INETOID                 869
#define CIDROID                 650
#define INT4ARRAYOID            1007
#define TEXTARRAYOID            1009
#define FLOAT4ARRAYOID          1021
#define ACLITEMOID              1033
#define CSTRINGARRAYOID         1263
#define BPCHAROID               1042
#define VARCHAROID              1043
#define DATEOID                 1082
#define TIMEOID                 1083
#define TIMESTAMPOID            1114
#define TIMESTAMPTZOID          1184
#define INTERVALOID             1186
#define TIMETZOID               1266
#define BITOID                  1560
#define VARBITOID               1562
#define NUMERICOID              1700
#define REFCURSOROID            1790
#define REGPROCEDUREOID         2202
#define REGOPEROID              2203
#define REGOPERATOROID          2204
#define REGCLASSOID             2205
#define REGTYPEOID              2206
#define REGTYPEARRAYOID         2211
#define TSVECTOROID             3614
#define GTSVECTOROID            3642
#define TSQUERYOID              3615
#define REGCONFIGOID            3734
#define REGDICTIONARYOID        3769
#define RECORDOID               2249
#define RECORDARRAYOID          2287
#define CSTRINGOID              2275
#define ANYOID                  2276
#define ANYARRAYOID             2277
#define VOIDOID                 2278
#define TRIGGEROID              2279
#define LANGUAGE_HANDLEROID     2280
#define INTERNALOID             2281
#define OPAQUEOID               2282
#define ANYELEMENTOID           2283
#define ANYNONARRAYOID          2776
#define ANYENUMOID              3500
#define JSONBOID                3802
#define JSONBARRAYOID           3807
#define UUIDOID                 2950
#define UUIDARRAYOID            2951

typedef struct {
    double x, y;
} Point;

#define MAXDIM 6

typedef signed int int32;

typedef int32 AbsoluteTime;
typedef int32 RelativeTime;

typedef struct {
    int32 status;
    AbsoluteTime data[2];
} TimeIntervalData;

typedef TimeIntervalData *TimeInterval;

#define PGSQL_AF_INET   (AF_INET + 0)
#define PGSQL_AF_INET6  (AF_INET + 1)

typedef struct {
    Point p[2];
    double m;
} LSEG;

typedef struct {
    Point high, low;
} BOX;

typedef struct {
    Point center;
    double radius;
} CIRCLE;

typedef unsigned char NumericDigit;
#endif

#include <libpq-fe.h>

#include <map>

// missing defines for array types (from catalog/pg_type.h)
#define QPGT_CIRCLEARRAYOID       719
#define QPGT_MONEYARRAYOID        791
#define QPGT_BOOLARRAYOID         1000
#define QPGT_BYTEAARRAYOID        1001
#define QPGT_NAMEARRAYOID         1003
#define QPGT_INT2ARRAYOID         1005
#define QPGT_INT2VECTORARRAYOID   1006
#ifdef INT4ARRAYOID
#define QPGT_INT4ARRAYOID         INT4ARRAYOID
#else
#define QPGT_INT4ARRAYOID         1007
#endif
#define QPGT_REGPROCARRAYOID      1008
#define QPGT_TEXTARRAYOID         1009
#define QPGT_OIDARRAYOID          1028
#define QPGT_TIDARRAYOID          1010
#define QPGT_XIDARRAYOID          1011
#define QPGT_CIDARRAYOID          1012
#define QPGT_OIDVECTORARRAYOID    1013
#define QPGT_BPCHARARRAYOID       1014
#define QPGT_VARCHARARRAYOID      1015
#define QPGT_INT8ARRAYOID         1016
#define QPGT_POINTARRAYOID        1017
#define QPGT_LSEGARRAYOID         1018
#define QPGT_PATHARRAYOID         1019
#define QPGT_BOXARRAYOID          1020
#define QPGT_FLOAT4ARRAYOID       1021
#define QPGT_FLOAT8ARRAYOID       1022
#define QPGT_ABSTIMEARRAYOID      1023
#define QPGT_RELTIMEARRAYOID      1024
#define QPGT_TINTERVALARRAYOID    1025
#define QPGT_POLYGONARRAYOID      1027
#define QPGT_ACLITEMARRAYOID      1034
#define QPGT_MACADDRARRAYOID      1040
#define QPGT_INETARRAYOID         1041
#define QPGT_CIDRARRAYOID         651
#define QPGT_TIMESTAMPARRAYOID    1115
#define QPGT_DATEARRAYOID         1182
#define QPGT_TIMEARRAYOID         1183
#define QPGT_TIMESTAMPTZARRAYOID  1185
#define QPGT_INTERVALARRAYOID     1187
#define QPGT_NUMERICARRAYOID      1231
#define QPGT_TIMETZARRAYOID       1270
#define QPGT_BITARRAYOID          1561
#define QPGT_VARBITARRAYOID       1563
#define QPGT_REFCURSORARRAYOID    2201
#define QPGT_UUIDARRAYOID         2951
#define QPGT_REGPROCEDUREARRAYOID 2207
#define QPGT_REGOPERARRAYOID      2208
#define QPGT_REGOPERATORARRAYOID  2209
#define QPGT_REGCLASSARRAYOID     2210
#define QPGT_REGTYPEARRAYOID      2211
#define QPGT_ANYARRAYOID          2277

// NOTE: this seems to be the binary format for inet/cidr data from PGSQL
// however I can't find this definition anywhere in the header files!!!
// server/utils/inet.h has a different definition
struct qore_pg_inet_struct {
    unsigned char family;      // PGSQL_AF_INET or PGSQL_AF_INET6
    unsigned char bits;        // number of bits in netmask
    unsigned char type;        // 0 = inet, 1 = cidr */
    unsigned char length;      // length of the following field (NOTE: I added this field)
    unsigned char ipaddr[16];  // up to 128 bits of address
};

struct qore_pg_tuple_id {
    unsigned int block;
    unsigned short index;
};

struct qore_pg_bit {
    unsigned int size;
    unsigned char data[1];
};

struct qore_pg_tinterval {
    int status, t1, t2;
};

struct qore_pg_array_info {
    int dim;
    int lBound;
};

struct qore_pg_array_header {
    int ndim, flags, oid;
    struct qore_pg_array_info info[1];
};

struct qore_pg_numeric_base {
    // number of "digits" in the output - each "digit" is a "short" containing 4 decimal digits
    short ndigits = 0;
    // the exponent of the encoded number
    short weight = 0;
    // the sign of the encoded number
    short sign = 0;
    // the maximum number of decimal digits in the value returned by the server (can be 0 when sending)
    short dscale = 0;

    DLLLOCAL qore_pg_numeric_base() {
    }
};

struct qore_pg_numeric : public qore_pg_numeric_base {
    unsigned short digits[1];

    DLLLOCAL void convertToHost();
    DLLLOCAL QoreValue toOptimal() const;
    DLLLOCAL QoreStringNode* toString() const;
    DLLLOCAL QoreNumberNode* toNumber() const;
    DLLLOCAL void toStr(QoreString& str) const;

    // must be called on raw data before convertToHost() is called
    DLLLOCAL size_t rawSize() const;
};

//! The largest display scale the server's binary \c numeric representation can hold
#define PGSQL_MAX_DSCALE 0x3fff
//! The largest base-10000 digit count the server's binary \c numeric representation can hold
#define PGSQL_MAX_NDIGITS 0x7fff

//! Builds the binary \c numeric image sent to the server for a Qore number
/** The number of base-10000 digits needed is a property of the value, not a constant: %Qore
    number arithmetic raises the working precision to keep results exact, so a quotient of
    accumulated sums easily runs to hundreds of decimal digits.  The digit array is therefore
    sized from the value's string representation rather than fixed; getData() returns the
    contiguous header-plus-digits image in network byte order.
*/
struct qore_pg_numeric_out {
    DLLLOCAL qore_pg_numeric_out(const QoreNumberNode* n);

    //! Returns the binary numeric image to send to the server
    DLLLOCAL const char* getData() const {
        return reinterpret_cast<const char*>(buf.data());
    }

    //! Returns the size in bytes of the image returned by getData()
    DLLLOCAL int getSize() const {
        return static_cast<int>(buf.size() * sizeof(unsigned short));
    }

private:
    // number of "digits" in the output - each "digit" is a "short" containing 4 decimal digits
    short ndigits = 0;
    // the exponent of the encoded number
    short weight = 0;
    // the sign of the encoded number
    short sign = 0;
    // the number of decimal digits after the decimal point
    short dscale = 0;
    // the base-10000 digits, most significant first, in host byte order while building
    std::vector<unsigned short> digits;
    // the wire image: ndigits, weight, sign, dscale, then the digits, all in network byte order
    std::vector<unsigned short> buf;

    //! Appends one base-10000 digit; returns false when the wire format cannot hold any more
    DLLLOCAL bool addDigit(unsigned short digit);

    DLLLOCAL void convertToNet();
};

union qore_pg_time {
    int64 i;
    double f;
};

struct qore_pg_interval {
    qore_pg_time time;
    union {
        int month;
        struct {
            int day;
            int month;
        } with_day;
    } rest;
};

struct qore_pg_time_tz_adt {
    qore_pg_time time;
    int zone;
};

class QorePGConnection;
typedef QoreValue (*qore_pg_data_func_t)(char *data, int type, int size, QorePGConnection *conn, const QoreEncoding *enc);

typedef std::map<int, qore_pg_data_func_t> qore_pg_data_map_t;
typedef std::pair<int, qore_pg_data_func_t> qore_pg_array_data_info_t;
typedef std::map<int, qore_pg_array_data_info_t> qore_pg_array_data_map_t;
typedef std::map<int, int> qore_pg_array_type_map_t;

static inline void assign_point(Point &p, Point *raw) {
    p.x = MSBf8(raw->x);
    p.y = MSBf8(raw->y);
};

#define OPT_NUM_OPTIMAL 0  // return numeric as int64 if it fits or "number" if not
#define OPT_NUM_STRING  1  // always return numeric types as strings
#define OPT_NUM_NUMERIC 2  // always return numeric types as "number"

// return optimal numeric values if options are supported
#define OPT_NUM_DEFAULT OPT_NUM_OPTIMAL

// connection (libpq) option names, settable on the datasource
#define PGSQL_OPT_KEEPALIVES          "keepalives"
#define PGSQL_OPT_KEEPALIVES_IDLE     "keepalives-idle"
#define PGSQL_OPT_KEEPALIVES_INTERVAL "keepalives-interval"
#define PGSQL_OPT_KEEPALIVES_COUNT    "keepalives-count"
#define PGSQL_OPT_CONNECT_TIMEOUT     "connect-timeout"
#define PGSQL_OPT_TCP_USER_TIMEOUT    "tcp-user-timeout"
#define PGSQL_OPT_APPLICATION_NAME    "application-name"

// connection option defaults: TCP keepalives are enabled with an aggressive idle time so that
// PostgreSQL promptly detects and reaps backends orphaned by an unclean client exit (otherwise idle
// orphans persist until the OS keepalive default, often 2 hours, exhausting max_connections)
#define PGSQL_DEF_KEEPALIVES          true
#define PGSQL_DEF_KEEPALIVES_IDLE     60
#define PGSQL_DEF_KEEPALIVES_INTERVAL 10
#define PGSQL_DEF_KEEPALIVES_COUNT    3
#define PGSQL_DEF_CONNECT_TIMEOUT     0   // 0 = use the libpq default (no client-side connect timeout)
// maximum time in milliseconds that transmitted data may remain unacknowledged before the connection
// is closed (TCP_USER_TIMEOUT); keepalives only probe idle connections, so without this a query in
// flight when the server's host disappears silently blocks until the kernel's retransmission limit
// (about 15 minutes with the Linux default net.ipv4.tcp_retries2 = 15)
#define PGSQL_DEF_TCP_USER_TIMEOUT    30000
// first libpq version supporting the tcp_user_timeout connection parameter (PostgreSQL 12)
#define PGSQL_TCP_USER_TIMEOUT_MIN_LIBPQ 120000

//! RAII helper for PostgreSQL query cancellation
/** Registers a cancel callback with the sandbox manager before blocking operations.
    When requestInterrupt() is called, the callback will use PQcancel() to cancel the query.
*/
class QorePGCancelHelper {
public:
    DLLLOCAL QorePGCancelHelper(PGconn* conn);
    DLLLOCAL ~QorePGCancelHelper();

    // Non-copyable
    QorePGCancelHelper(const QorePGCancelHelper&) = delete;
    QorePGCancelHelper& operator=(const QorePGCancelHelper&) = delete;

private:
    PGconn* conn;
    QoreSandboxManagerHelper smh;
    // Use atomic pointer for thread safety with callback invocation
    std::atomic<PGcancel*> cancel_obj;
};

class QorePGConnection {
protected:
    Datasource* ds;
    PGconn* pc;
    const AbstractQoreZoneInfo* server_tz;
    // short server description without password for error / warning messages
    QoreStringMaker server_desc;
    bool interval_has_day, integer_datetimes;
    int numeric_support;

    // libpq connection options (applied to the conninfo at connect time)
    bool opt_keepalives = PGSQL_DEF_KEEPALIVES;
    int opt_keepalives_idle = PGSQL_DEF_KEEPALIVES_IDLE;
    int opt_keepalives_interval = PGSQL_DEF_KEEPALIVES_INTERVAL;
    int opt_keepalives_count = PGSQL_DEF_KEEPALIVES_COUNT;
    int opt_connect_timeout = PGSQL_DEF_CONNECT_TIMEOUT;
    int opt_tcp_user_timeout = PGSQL_DEF_TCP_USER_TIMEOUT;
    // true if tcp-user-timeout was set explicitly in the datasource options; an explicit value is
    // always passed to libpq, while the default is omitted when the client library cannot accept it
    bool opt_tcp_user_timeout_set = false;
    // libpq application_name reported to the server (visible in pg_stat_activity.application_name);
    // empty means unset (libpq default applies)
    std::string opt_application_name;

    // Dynamic extension type OIDs (discovered per-connection from pg_type)
    // 0 means the type is not available (extension not installed)
    Oid vector_oid = 0;
    Oid vector_array_oid = 0;
    Oid halfvec_oid = 0;
    Oid halfvec_array_oid = 0;
    Oid sparsevec_oid = 0;
    Oid sparsevec_array_oid = 0;

#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
    //! active driver-native COPY FROM STDIN protocol state
    QorePGBulkCopyState* bulk_copy = nullptr;
#endif

public:
    DLLLOCAL QorePGConnection(Datasource* d, const char *str, ExceptionSink *xsink);
    DLLLOCAL ~QorePGConnection();

    DLLLOCAL int commit(ExceptionSink *xsink);
    DLLLOCAL int rollback( ExceptionSink *xsink);
    DLLLOCAL QoreListNode* selectRows(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
#ifdef QDBI_METHOD_SELECT_TYPED
    DLLLOCAL QoreValue selectRowsTyped(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
#endif
    DLLLOCAL QoreHashNode* selectRow(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
    DLLLOCAL QoreValue select(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
#ifdef QDBI_METHOD_SELECT_TYPED
    DLLLOCAL QoreValue selectTyped(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
#endif
#ifdef QDBI_METHOD_SELECT_COLUMNAR
    DLLLOCAL QoreColumnarResult* selectColumnar(const QoreString *qstr, const QoreListNode *args,
        ExceptionSink *xsink);
#endif
    DLLLOCAL QoreValue exec(const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink);
    DLLLOCAL QoreValue execRaw(const QoreString *qstr, ExceptionSink *xsink);
    DLLLOCAL int begin_transaction(ExceptionSink *xsink);

    //! Returns true if the SQL is a COPY ... FROM STDIN command
    DLLLOCAL static bool isCopyFromStdin(const QoreString* qstr);

    //! Executes a COPY ... FROM STDIN command using the COPY protocol
    DLLLOCAL QoreValue copyFromStdin(const QoreString* qstr, const QoreListNode* args, ExceptionSink* xsink);
#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
    //! Starts a driver-native COPY FROM STDIN operation
    DLLLOCAL int bulkLoadBegin(const QoreString* table, const QoreListNode* columns,
        const QoreHashNode* options, ExceptionSink* xsink);

    //! Sends one hash-of-columns block to the active native COPY operation
    DLLLOCAL int bulkLoadRows(const QoreHashNode* rows, ExceptionSink* xsink);

    //! Finishes or aborts the active native COPY operation
    DLLLOCAL int bulkLoadEnd(bool success, ExceptionSink* xsink);
#endif
    DLLLOCAL bool has_interval_day() const { return interval_has_day; }
    DLLLOCAL bool has_integer_datetimes() const { return integer_datetimes; }
    DLLLOCAL int get_server_version() const;

    //! Discovers pgvector extension type OIDs from pg_type catalog
    /** Called at connection time; silently does nothing if pgvector is not installed.
    */
    DLLLOCAL void discoverExtensionTypes(ExceptionSink* xsink);

    //! Resolves an extension type name to its per-connection OID
    /** @return the OID, or 0 if the type is not available on this connection
    */
    DLLLOCAL Oid resolveExtensionTypeName(const char* type_name) const;

    //! Returns the array OID for a given extension scalar OID
    /** @return the array OID, or 0 if not found
    */
    DLLLOCAL Oid getExtensionArrayOid(Oid scalar_oid) const;

    //! Looks up a dynamic data conversion function for the given OID
    /** @return the conversion function, or nullptr if the OID is not a known extension type
    */
    DLLLOCAL qore_pg_data_func_t getExtensionDataFunc(Oid oid) const;

    //! Looks up a dynamic array data conversion entry for the given OID
    /** @param[out] element_oid set to the element OID if found
        @param[out] func set to the element conversion function if found
        @return true if the OID is a known extension array type
    */
    DLLLOCAL bool getExtensionArrayDataFunc(Oid oid, int& element_oid,
        qore_pg_data_func_t& func) const;

    DLLLOCAL Oid getVectorOid() const { return vector_oid; }
    DLLLOCAL Oid getVectorArrayOid() const { return vector_array_oid; }
    DLLLOCAL Oid getHalfvecOid() const { return halfvec_oid; }
    DLLLOCAL Oid getHalfvecArrayOid() const { return halfvec_array_oid; }
    DLLLOCAL Oid getSparsevecOid() const { return sparsevec_oid; }
    DLLLOCAL Oid getSparsevecArrayOid() const { return sparsevec_array_oid; }

    //! Returns true if the vector type is available on this connection
    DLLLOCAL bool hasVectorType() const { return vector_oid != 0; }

    DLLLOCAL const char* getServerDesc() const {
        return server_desc.c_str();
    }

    //! validates a tcp-user-timeout option value and stores it in \a out
    /** @return 0 for success, -1 if the value is out of range (exception raised)
    */
    DLLLOCAL static int parseTcpUserTimeoutOption(const QoreValue val, int& out, ExceptionSink* xsink) {
        int64 v = val.getAsBigInt();
        if (v < 0 || v > INT_MAX) {
            xsink->raiseException("DBI:PGSQL-OPTION-ERROR", "invalid '%s' value " QLLD "; expecting a "
                "number of milliseconds from 0 to %d (0 = use the operating system default)",
                PGSQL_OPT_TCP_USER_TIMEOUT, v, INT_MAX);
            return -1;
        }
        out = static_cast<int>(v);
        return 0;
    }

    DLLLOCAL int setOption(const char* opt, const QoreValue val, ExceptionSink* xsink) {
        if (!strcasecmp(opt, DBI_OPT_NUMBER_OPT)) {
            numeric_support = OPT_NUM_OPTIMAL;
            return 0;
        }
        if (!strcasecmp(opt, DBI_OPT_NUMBER_STRING)) {
            numeric_support = OPT_NUM_STRING;
            return 0;
        }
        if (!strcasecmp(opt, DBI_OPT_NUMBER_NUMERIC)) {
            numeric_support = OPT_NUM_NUMERIC;
            return 0;
        }
        // connection options: stored for introspection; they are applied to the libpq conninfo at
        // connect time, so a change made on an open datasource takes effect on the next connection
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES)) {
            opt_keepalives = val.getAsBool();
            return 0;
        }
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_IDLE)) {
            opt_keepalives_idle = static_cast<int>(val.getAsBigInt());
            return 0;
        }
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_INTERVAL)) {
            opt_keepalives_interval = static_cast<int>(val.getAsBigInt());
            return 0;
        }
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_COUNT)) {
            opt_keepalives_count = static_cast<int>(val.getAsBigInt());
            return 0;
        }
        if (!strcasecmp(opt, PGSQL_OPT_CONNECT_TIMEOUT)) {
            opt_connect_timeout = static_cast<int>(val.getAsBigInt());
            return 0;
        }
        if (!strcasecmp(opt, PGSQL_OPT_TCP_USER_TIMEOUT)) {
            return parseTcpUserTimeoutOption(val, opt_tcp_user_timeout, xsink);
        }
        if (!strcasecmp(opt, PGSQL_OPT_APPLICATION_NAME)) {
            QoreStringValueHelper str(val);
            opt_application_name = str->c_str();
            return 0;
        }
        assert(!strcasecmp(opt, DBI_OPT_TIMEZONE));
        assert(val.getType() == NT_STRING);
        QoreStringValueHelper str(val);
        const AbstractQoreZoneInfo* tz =
            find_create_timezone(str->c_str(), xsink);
        if (*xsink)
            return -1;
        server_tz = tz;
        return 0;
    }

    DLLLOCAL QoreValue getOption(const char* opt) {
        if (!strcasecmp(opt, DBI_OPT_NUMBER_OPT))
            return numeric_support == OPT_NUM_OPTIMAL;

        if (!strcasecmp(opt, DBI_OPT_NUMBER_STRING))
            return numeric_support == OPT_NUM_STRING;

        if (!strcasecmp(opt, DBI_OPT_NUMBER_NUMERIC))
            return numeric_support == OPT_NUM_NUMERIC;

        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES))
            return opt_keepalives;
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_IDLE))
            return static_cast<int64>(opt_keepalives_idle);
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_INTERVAL))
            return static_cast<int64>(opt_keepalives_interval);
        if (!strcasecmp(opt, PGSQL_OPT_KEEPALIVES_COUNT))
            return static_cast<int64>(opt_keepalives_count);
        if (!strcasecmp(opt, PGSQL_OPT_CONNECT_TIMEOUT))
            return static_cast<int64>(opt_connect_timeout);
        if (!strcasecmp(opt, PGSQL_OPT_TCP_USER_TIMEOUT))
            return static_cast<int64>(opt_tcp_user_timeout);
        if (!strcasecmp(opt, PGSQL_OPT_APPLICATION_NAME))
            return new QoreStringNode(opt_application_name.c_str());

        assert(!strcasecmp(opt, DBI_OPT_TIMEZONE));
        return new QoreStringNode(tz_get_region_name(server_tz));
    }

    DLLLOCAL int getNumeric() const { return numeric_support; }

    DLLLOCAL const AbstractQoreZoneInfo* getTZ() const {
        return server_tz;
    }

    DLLLOCAL int checkResult(PGresult* res, ExceptionSink* xsink) {
        ExecStatusType rc = PQresultStatus(res);
        if (rc != PGRES_COMMAND_OK && rc != PGRES_TUPLES_OK) {
            //printd(5, "PQresultStatus() returned %d\n", rc);
            return doError(res, xsink);
        }
        return 0;
    }

    DLLLOCAL int checkClearResult(bool lost_connection, PGresult*& res, ExceptionSink* xsink) {
        if (lost_connection) {
            // lost connection error is raised if a transaction was in progress before this call
            if (!res) {
                // issue #4368 if no transaction was in progress, raise a lost connection error here
                if (!xsink->isException()) {
                    doLostConnectionError(false, res, xsink);
                }
                return -1;
            }
        }
        int rc = checkResult(res, xsink);
        if (rc) {
            PQclear(res);
            res = nullptr;
        }
        return rc;
    }

    DLLLOCAL int doError(PGresult *res, ExceptionSink *xsink) {
        const char* err = PQerrorMessage(pc);
        const char* e = (!strncmp(err, "ERROR:  ", 8) || !strncmp(err, "FATAL:  ", 8)) ? err + 8 : err;
        QoreStringNode* desc = new QoreStringNode(e);
        desc->chomp();

        const QoreHashNode* arg = getExceptionArg(res, xsink);

        xsink->raiseExceptionArg("DBI:PGSQL:ERROR", arg, desc);
        return -1;
    }

    DLLLOCAL PGconn* get() const {
        return pc;
    }

    DLLLOCAL Datasource* getDs() const {
        return ds;
    }

    DLLLOCAL bool wasInTransaction() const {
        return ds->activeTransaction();
    }

    DLLLOCAL static QoreHashNode* getExceptionArg(const PGresult *res, ExceptionSink *xsink);

    DLLLOCAL static void doLostConnectionError(bool in_trans, const PGresult *res, ExceptionSink* xsink);
};

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

union parambuf {
    bool b;
    short i2;
    int i4;
    int64 i8;
    double f8;
    char* str;
    void* ptr;
    qore_pg_numeric_out* num;
    qore_pg_interval iv;

    DLLLOCAL void assign(short i) {
        i2 = htons(i);
    }
    DLLLOCAL void assign(int i) {
        i4 = htonl(i);
    }
    DLLLOCAL void assign(int64 i) {
        i8 = i8MSB(i);
    }
    DLLLOCAL void assign(double f) {
        f8 = f8MSB(f);
    }
};

class parambuf_list_t : public safe_dslist<parambuf *> {
public:
    DLLLOCAL ~parambuf_list_t() {
        std::for_each(begin(), end(), simple_delete<parambuf>());
    }
};

class QorePgsqlStatement {
protected:
    DLLLOCAL static qore_pg_data_map_t data_map;
    DLLLOCAL static qore_pg_array_data_map_t array_data_map;

    PGresult* res;
    int nParams, allocated;
    Oid *paramTypes;
    char **paramValues;
    int *paramLengths, *paramFormats;
    int *paramArray;
    parambuf_list_t parambuf_list;
    QorePGConnection *conn;
    const QoreEncoding *enc;
    //! expected array size for array bind validation; -1 = not yet set
    int array_size;

    DLLLOCAL QoreValue getValue(int row, int col, ExceptionSink *xsink);
    // returns 0 for OK, -1 for error
    DLLLOCAL int parse(QoreString *str, const QoreListNode *args, ExceptionSink *xsink);
    DLLLOCAL int add(QoreValue v, ExceptionSink *xsink);
    DLLLOCAL QoreListNode* getArray(int type, qore_pg_data_func_t func, char *&array_data, int current, int ndim, int dim[]);
    DLLLOCAL void reset();
    DLLLOCAL QoreHashNode* getSingleRowIntern(ExceptionSink* xsink, int row = 0);
    DLLLOCAL int execIntern(const char* sql, ExceptionSink* xsink);
    DLLLOCAL void setupColumnNames(strvec_t& cvec, int num_columns);

public:
    DLLLOCAL static qore_pg_array_type_map_t array_type_map;

    DLLLOCAL QorePgsqlStatement(QorePGConnection *r_conn, const QoreEncoding *r_enc);
    DLLLOCAL QorePgsqlStatement(Datasource* ds);
    DLLLOCAL ~QorePgsqlStatement();

    // returns 0 for OK, -1 for error
    DLLLOCAL int exec(const QoreString *str, const QoreListNode *args, ExceptionSink *xsink);

    // returns 0 for OK, -1 for error
    DLLLOCAL int exec(const char* cmd, ExceptionSink* xsink);

    DLLLOCAL void setupColumns(QoreHashNode& h, strvec_t& cvec, int num_columns);
    DLLLOCAL QoreHashNode* getOutputHash(ExceptionSink* xsink, bool cols = false, int* start = 0, int maxrows = -1);
#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
    DLLLOCAL QoreColumnarResult* getOutputColumnar(ExceptionSink* xsink, bool cols = false, int* start = 0,
        int maxrows = -1);
#endif
    DLLLOCAL QoreListNode* getOutputList(ExceptionSink* xsink, int* start = 0, int maxrows = -1);

    DLLLOCAL QoreHashNode* getSingleRow(ExceptionSink *xsink, int row = 0);
    DLLLOCAL int rowsAffected();
    DLLLOCAL bool hasResultData();
    DLLLOCAL bool checkIntegerDateTimes(ExceptionSink *xsink);
    DLLLOCAL QoreHashNode* describe(ExceptionSink* xsink);

    // static functions
    DLLLOCAL static void static_init();
};

class QorePgsqlPreparedStatement : public QorePgsqlStatement {
protected:
    QoreString* sql;
    QoreListNode* targs;
    // current row
    int crow;
    bool do_parse;
    bool parsed;

    DLLLOCAL int prepareIntern(const QoreListNode* args, ExceptionSink* xsink);

public:
    DLLLOCAL QorePgsqlPreparedStatement(Datasource* ds) : QorePgsqlStatement(ds), sql(0), targs(0), crow(-1), do_parse(false), parsed(false) {
    }

    DLLLOCAL ~QorePgsqlPreparedStatement() {
        assert(!sql);
    }

    // returns 0 for OK, -1 for error
    DLLLOCAL int prepare(const QoreString& sql, const QoreListNode* args, bool n_parse, ExceptionSink* xsink);
    DLLLOCAL int bind(const QoreListNode& l, ExceptionSink* xsink);
    DLLLOCAL int exec(ExceptionSink* xsink);
    DLLLOCAL QoreHashNode* fetchRow(ExceptionSink* xsink);
    DLLLOCAL QoreListNode* fetchRows(int rows, ExceptionSink* xsink);
    DLLLOCAL QoreHashNode* fetchColumns(int rows, ExceptionSink* xsink);
#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
    DLLLOCAL QoreColumnarResult* fetchColumnar(int rows, ExceptionSink* xsink);
#endif
    DLLLOCAL bool next();

    DLLLOCAL void reset(ExceptionSink *xsink);
};

class QorePGBindArray {
private:
    int ndim, size, allocated, elements;
    int dim[MAXDIM];
    char *ptr;
    qore_pg_array_header *hdr;
    qore_type_t type;
    int oid, arrayoid, format;
    QorePGConnection *conn;

    // returns -1 for exception, 0 for OK
    DLLLOCAL int check_type(QoreValue n, ExceptionSink *xsink);
    // returns -1 for exception, 0 for OK
    DLLLOCAL int check_oid(const QoreHashNode *h, ExceptionSink *xsink);
    // returns -1 for exception, 0 for OK
    DLLLOCAL int new_dimension(const QoreListNode *l, int current, ExceptionSink *xsink);
    // returns -1 for exception, 0 for OK
    DLLLOCAL int process_list(const QoreListNode *l, int current, const QoreEncoding *enc, ExceptionSink *xsink);
    // returns -1 for exception, 0 for OK
    DLLLOCAL int bind(QoreValue n, const QoreEncoding *enc, ExceptionSink *xsink);
    DLLLOCAL void check_size(int size);

public:
    DLLLOCAL QorePGBindArray(QorePGConnection *r_conn);
    DLLLOCAL ~QorePGBindArray();
    // returns -1 for exception, 0 for OK
    DLLLOCAL int create_data(const QoreListNode *l, int current, const QoreEncoding *enc, ExceptionSink *xsink);
    DLLLOCAL int getOid() const;
    DLLLOCAL int getArrayOid() const;
    DLLLOCAL int getSize() const;
    DLLLOCAL int getFormat() const { return format; }
    DLLLOCAL qore_pg_array_header *getHeader();
};

#endif
