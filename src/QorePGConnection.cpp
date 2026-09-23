/* -*- indent-tabs-mode: nil -*- */
/*
    QorePGConnection.cpp

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

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
#include <qore/QoreColumnarResult.h>
#endif

#if (defined _WIN32 || defined __WIN32__) && ! defined __CYGWIN__
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <cmath>
#include <limits>
#include <ctype.h>

#include <memory>
#include <typeinfo>
#include <unordered_set>

// Qore's native bulk-load DBI contract was added after the mutation-observer stream API.  Treat the
// new method codes as an equivalent feature probe so build-tree headers with a stale generated
// qore-version.h still compile the required stream reporting.
#if defined(QORE_HAVE_SQL_MUTATION_OBSERVER) || defined(QDBI_METHOD_BULK_LOAD_BEGIN)
#define QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER 1
#endif

// postgresql uses an epoch starting at 2000-01-01, which is
// 10,957 days after the UNIX and Qore epoch of 1970-01-01
// there are 86,400 seconds in the average day (w/o DST changes)
#define PGSQL_EPOCH_OFFSET (10957 * 86400)

static char* qpg_copy_string_buffer(const QoreString* str) {
    size_t len = str->size();
    char* rv = (char*)malloc(len + 1);
    memcpy(rv, str->c_str(), len);
    rv[len] = '\0';
    return rv;
}

//------------------------------------------------------------------------------
// QorePGCancelHelper implementation
//------------------------------------------------------------------------------

QorePGCancelHelper::QorePGCancelHelper(PGconn* conn)
    : conn(conn), cancel_obj(nullptr) {
    if (smh && conn) {
        // Get a cancel object that can be used from another thread
        PGcancel* co = PQgetCancel(conn);
        cancel_obj.store(co, std::memory_order_release);
        if (co) {
            // Register cancel callback
            smh->registerCancelCallback(this, [this]() -> bool {
                // Load pointer atomically - it may be set to nullptr by destructor
                PGcancel* co = this->cancel_obj.load(std::memory_order_acquire);
                if (co) {
                    char errbuf[256];
                    int result = PQcancel(co, errbuf, sizeof(errbuf));
                    return result != 0;
                }
                return false;
            });
        }
    }
}

QorePGCancelHelper::~QorePGCancelHelper() {
    // Get the cancel object and set to nullptr atomically before unregistering
    // to prevent use-after-free if a callback is currently being invoked
    PGcancel* co = cancel_obj.exchange(nullptr, std::memory_order_acq_rel);
    if (smh && co) {
        // Unregister the callback
        smh->unregisterCancelCallback(this);
    }
    if (co) {
        PQfreeCancel(co);
    }
}

// Helper function to check for interrupt before connection attempt
static PGconn* pgsql_connect_with_interrupt_check(const char* str, ExceptionSink* xsink) {
    if (qore_check_cancel(xsink)) {
        return nullptr;
    }
    return PQconnectdb(str);
}

// declare static members
qore_pg_data_map_t QorePgsqlStatement::data_map;
qore_pg_array_data_map_t QorePgsqlStatement::array_data_map;
qore_pg_array_type_map_t QorePgsqlStatement::array_type_map;

#ifdef DEBUG
void do_output(char* p, unsigned len) {
    for (unsigned i = 0; i < len; ++i)
        printd(0, "do_output: %2d: %02x\n", i, (int)p[i]);
}
#endif

void qore_pg_numeric::convertToHost() {
    ndigits = ntohs(ndigits);
    weight = ntohs(weight);
    sign = ntohs(sign);
    dscale = ntohs(dscale);

#ifdef DEBUG_1
    printd(0, "qpg_data_numeric::convertToHost() ndigits: %hd weight: %hd sign: %hd dscale: %hd\n", ndigits, weight, sign, dscale);
    for (unsigned i = 0; i < ndigits; ++i)
        printd(0, " + %hu\n", ntohs(digits[i]));
#endif
}

QoreValue qore_pg_numeric::toOptimal() const {
    QoreString str;
    toStr(str);

    //printd(5, "qore_pg_numeric::toOptimal() processing %s ndigits: %d weight: %d sign: %d cmp: %d\n", str.c_str(), ndigits, weight, sign, strcmp(str.c_str(), "-9223372036854775808"));

    // return an integer if the number can be converted to a 64-bit integer
    if ((ndigits <= (weight + 1)) && (ndigits < 4
                    || (ndigits == 5 &&
                        ((!sign && strcmp(str.c_str(), "9223372036854775807") <= 0)
                        ||(sign && strcmp(str.c_str(), "-9223372036854775808") <= 0)))))
        return str.toBigInt();

    return new QoreNumberNode(str.c_str());
}

QoreStringNode* qore_pg_numeric::toString() const {
    QoreStringNode* str = new QoreStringNode;
    toStr(*str);
    return str;
}

QoreNumberNode* qore_pg_numeric::toNumber() const {
    QoreString str;
    toStr(str);

    return new QoreNumberNode(str.c_str());
}

void qore_pg_numeric::toStr(QoreString& str) const {
    if (!ndigits) {
        str.concat('0');
        return;
    }

    if (sign)
        str.concat('-');

    //printd(5, "qore_pg_numeric::toStr() ndigits: %d dscale: %d\n", ndigits, dscale);

    int i;
    for (i = 0; i < ndigits; ++i) {
        if (i == weight + 1)
            str.concat('.');
        if (i || weight < 0)
            str.sprintf("%04d", ntohs(digits[i]));
        else
            str.sprintf("%d", ntohs(digits[i]));
        //printd(5, "qore_pg_numeric::toStr() digit %d: %d\n", i, ntohs(digits[i]));
    }

    //printd(5, "qore_pg_numeric::toStr() i: %d weight: %d str: %s\n", i, weight, str.c_str());

    // now add significant zeros for remaining decimal places
    if (weight >= i)
        str.addch('0', (weight - i + 1) * 4);
    else if (weight < -1) {
        for (int i = weight; i < -1; ++i)
            str.insert("0000", 0);
        str.prepend(".");
    }

    //printd(5, "qore_pg_numeric::toStr() str: '%s'\n", str.c_str());
}

size_t qore_pg_numeric::rawSize() const {
    return sizeof(qore_pg_numeric_base) + sizeof(short) * ntohs(ndigits);
}

qore_pg_numeric_out::qore_pg_numeric_out(const QoreNumberNode* n) {
    QoreString str;
    n->getStringRepresentation(str);

    //printd(5, "qore_pg_numeric_out::qore_pg_numeric_out() this %p str: '%s'\n", this, str.c_str());

    // populate structure
    int nsign = n->sign();
    if (nsign < 0) {
        // this is what the server sends for negative numbers
        sign = 0x4000;
        // remove the minus sign from the string
        str.trim_leading('-');
    }
    qore_offset_t di = str.find('.');
    if (di == -1)
        di = str.strlen();

    //printd(5, "find: '%s' di: %lld\n", str.c_str(), di);

    // reserve the exact number of base-10000 digits the value needs: one per four decimal
    // digits on each side of the decimal point, rounded up
    digits.reserve(((di + 3) / 4) + ((str.size() - di + 3) / 4));

    char buf[5];
    int i = 0;
    if (di != 1 || str[0] != '0') {
        while (i < di) {
            // get the remaining number of digits up to the decimal place
            int nd = (di - i) % 4;
            if (!nd)
                nd = 4;
            for (int j = 0; j < nd; ++j)
                buf[j] = (str.c_str() + i)[j];
            buf[nd] = '\0';
            if (!addDigit(atoi(buf))) {
                break;
            }
            if (ndigits > 1)
                ++weight;
            //printd(5, "adding digits: '%s' (%d)\n", buf, atoi(buf));
            i += nd;
        }
    } else {
        weight = -1;
        i = 1;
    }

    // now add digits after the decimal point
    if (i != (int)str.size()) {
        // skip decimal point
        ++i;
        di = str.size();
        // the display scale is the total number of digits after the decimal point; the server
        // cannot represent more than PGSQL_MAX_DSCALE of them, so the rest are dropped, which
        // is what the server itself does with a value that overflows the column's scale
        qore_offset_t frac = di - i;
        if (frac > PGSQL_MAX_DSCALE) {
            frac = PGSQL_MAX_DSCALE;
            di = i + frac;
        }
        dscale = (short)frac;
        while (i < di) {
            int nd = di - i;
            if (nd > 4)
                nd = 4;
            for (int j = 0; j < nd; ++j)
                buf[j] = (str.c_str() + i)[j];
            while (nd < 4)
                buf[nd++] = '0';
            buf[nd] = '\0';
            if (!addDigit(atoi(buf))) {
                break;
            }
            //printd(5, "adding (after decimal point) digits: '%s' (%d)\n", buf, atoi(buf));
            i += 4;
        }
    } else {
        // trim off trailing zeros when there are no digits after the decimal point
        while (ndigits && !digits[ndigits - 1]) {
            --ndigits;
            digits.pop_back();
        }
    }

    convertToNet();
}

bool qore_pg_numeric_out::addDigit(unsigned short digit) {
    // ndigits is a signed 16-bit field on the wire, so a value needing more digits than that
    // cannot be sent at all; stop rather than wrap the count into a negative number
    if (ndigits == PGSQL_MAX_NDIGITS) {
        return false;
    }
    digits.push_back(digit);
    ++ndigits;
    return true;
}

void qore_pg_numeric_out::convertToNet() {
    assert(ndigits >= 0 && (size_t)ndigits <= digits.size());

    printd(5, "qore_pg_numeric_out::convertToNet() ndigits: %hd weight: %hd sign: %hd dscale: %hd\n",
        ndigits, weight, sign, dscale);

    buf.reserve(4 + ndigits);
    buf.push_back(htons(ndigits));
    buf.push_back(htons(weight));
    buf.push_back(htons(sign));
    buf.push_back(htons(dscale));
    for (unsigned i = 0; i < (unsigned)ndigits; ++i) {
        //printd(5, " + %hu\n", digits[i]);
        buf.push_back(htons(digits[i]));
    }

    //do_output(getData(), getSize());
}

// bind functions
static QoreValue qpg_data_bool(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    return *((bool*)data);
}

static QoreValue qpg_data_bytea(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    void* dc = malloc(len);
    memcpy(dc, data, len);
    return new BinaryNode(dc, len);
}

static QoreValue qpg_data_char(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    QoreStringNode* rv = new QoreStringNode(data, len);
    rv->trim_trailing();
    return rv;
}

static QoreValue qpg_data_int8(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    return MSBi8(*((uint64_t *)data));
}

static QoreValue qpg_data_int4(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    // Cast to signed to handle negative values correctly
    return (int32_t)ntohl(*((uint32_t *)data));
}

static QoreValue qpg_data_int2(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    // Cast to signed to handle negative values correctly
    return (int16_t)ntohs(*((uint16_t *)data));
}

static QoreValue qpg_data_text(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    return new QoreStringNode((char*)data, len, enc);
}

static QoreValue qpg_data_jsonb(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    // skip initial 0x01 byte at the beginning of JSONB data returned
    if (data[0] == 1) {
        ++data;
        --len;
    }
    return new QoreStringNode((char*)data, len, enc);
}

static QoreValue qpg_data_float4(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    float fv = MSBf4(*((float *)data));
    return (double)fv;
}

static QoreValue qpg_data_float8(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    double fv = MSBf8(*((double *)data));
    return fv;
}

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
static int64_t qpg_read_int8(const char* data) {
    uint64_t raw;
    memcpy(&raw, data, sizeof(raw));
    return MSBi8(raw);
}

static int32_t qpg_read_int4(const char* data) {
    uint32_t raw;
    memcpy(&raw, data, sizeof(raw));
    return static_cast<int32_t>(ntohl(raw));
}

static int16_t qpg_read_int2(const char* data) {
    uint16_t raw;
    memcpy(&raw, data, sizeof(raw));
    return static_cast<int16_t>(ntohs(raw));
}

static float qpg_read_float4(const char* data) {
    float raw;
    memcpy(&raw, data, sizeof(raw));
    return MSBf4(raw);
}

static double qpg_read_float8(const char* data) {
    double raw;
    memcpy(&raw, data, sizeof(raw));
    return MSBf8(raw);
}

static bool qpg_get_columnar_buffer_type(Oid oid, QoreBufferElementType& buffer_type,
        QoreColumnarColumnType& column_type) {
    switch (oid) {
        case BOOLOID:
            buffer_type = QoreBufferElementType::Bool;
            column_type = QoreColumnarColumnType::Bool;
            return true;
        case INT2OID:
            buffer_type = QoreBufferElementType::Int16;
            column_type = QoreColumnarColumnType::Int;
            return true;
        case INT4OID:
        case OIDOID:
        case XIDOID:
        case CIDOID:
            buffer_type = QoreBufferElementType::Int32;
            column_type = QoreColumnarColumnType::Int;
            return true;
        case INT8OID:
            buffer_type = QoreBufferElementType::Int64;
            column_type = QoreColumnarColumnType::Int;
            return true;
        case FLOAT4OID:
            buffer_type = QoreBufferElementType::Float32;
            column_type = QoreColumnarColumnType::Float;
            return true;
        case FLOAT8OID:
            buffer_type = QoreBufferElementType::Float64;
            column_type = QoreColumnarColumnType::Float;
            return true;
        case CHAROID:
        case BPCHAROID:
        case TEXTOID:
        case VARCHAROID:
        case NAMEOID:
        case UNKNOWNOID:
        case XMLOID:
        case JSONOID:
        case JSONBOID:
        case UUIDOID:
            buffer_type = QoreBufferElementType::String;
            column_type = QoreColumnarColumnType::String;
            return true;
        default:
            return false;
    }
}

static QoreColumnarColumnType qpg_get_columnar_column_type(Oid oid, QorePGConnection* conn) {
    switch (oid) {
        case BOOLOID:
            return QoreColumnarColumnType::Bool;
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case OIDOID:
        case XIDOID:
        case CIDOID:
            return QoreColumnarColumnType::Int;
        case FLOAT4OID:
        case FLOAT8OID:
            return QoreColumnarColumnType::Float;
        case NUMERICOID:
        case CASHOID:
            return QoreColumnarColumnType::Number;
        case BYTEAOID:
            return QoreColumnarColumnType::Binary;
        case ABSTIMEOID:
        case RELTIMEOID:
        case DATEOID:
        case TIMEOID:
        case TIMETZOID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
        case INTERVALOID:
        case TINTERVALOID:
            return QoreColumnarColumnType::Date;
        case CHAROID:
        case BPCHAROID:
        case TEXTOID:
        case VARCHAROID:
        case NAMEOID:
        case UNKNOWNOID:
        case XMLOID:
        case JSONOID:
        case JSONBOID:
        case UUIDOID:
            return QoreColumnarColumnType::String;
        default:
            if ((conn->getVectorOid() && oid == conn->getVectorOid())
                    || (conn->getHalfvecOid() && oid == conn->getHalfvecOid())
                    || (conn->getSparsevecOid() && oid == conn->getSparsevecOid())) {
                return QoreColumnarColumnType::Auto;
            }
            return QoreColumnarColumnType::Auto;
    }
}

static bool qpg_get_numeric_typmod(int fmod, int32_t& precision, int32_t& scale) {
    if (fmod < 0) {
        return false;
    }

    int typmod = fmod - 4;
    if (typmod < 0) {
        return false;
    }

    precision = static_cast<int32_t>((typmod >> 16) & 0xffff);
    scale = static_cast<int32_t>(static_cast<int16_t>(typmod & 0xffff));
    return precision > 0 && scale >= 0 && scale <= precision;
}

static std::string qpg_get_columnar_native_type(Oid oid, int fmod, QorePGConnection* conn) {
    switch (oid) {
        case BOOLOID: return "boolean";
        case INT2OID: return "smallint";
        case INT4OID: return "integer";
        case INT8OID: return "bigint";
        case OIDOID: return "oid";
        case XIDOID: return "xid";
        case CIDOID: return "cid";
        case FLOAT4OID: return "real";
        case FLOAT8OID: return "double precision";
        case NUMERICOID: {
            int32_t precision = 0;
            int32_t scale = 0;
            if (qpg_get_numeric_typmod(fmod, precision, scale)) {
                return "numeric(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
            }
            return "numeric";
        }
        case CASHOID: return "money";
        case BYTEAOID: return "bytea";
        case CHAROID: return "char";
        case BPCHAROID: return "bpchar";
        case TEXTOID: return "text";
        case VARCHAROID: return "varchar";
        case NAMEOID: return "name";
        case UNKNOWNOID: return "unknown";
        case XMLOID: return "xml";
        case JSONOID: return "json";
        case JSONBOID: return "jsonb";
        case UUIDOID: return "uuid";
        case DATEOID: return "date";
        case TIMEOID: return "time";
        case TIMETZOID: return "timetz";
        case TIMESTAMPOID: return "timestamp";
        case TIMESTAMPTZOID: return "timestamptz";
        case INTERVALOID: return "interval";
        case ABSTIMEOID: return "abstime";
        case RELTIMEOID: return "reltime";
        case TINTERVALOID: return "tinterval";
        default:
            if (conn->getVectorOid() && oid == conn->getVectorOid()) {
                return "vector";
            }
            if (conn->getHalfvecOid() && oid == conn->getHalfvecOid()) {
                return "halfvec";
            }
            if (conn->getSparsevecOid() && oid == conn->getSparsevecOid()) {
                return "sparsevec";
            }
            return std::string();
    }
}
#endif

static QoreValue qpg_data_abstime(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    int val = ntohl(*((uint32_t *)data));
    return DateTimeNode::makeAbsolute(conn->getTZ(), (int64)val, 0);
}

static QoreValue qpg_data_reltime(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    int val = ntohl(*((uint32_t *)data));
    return new DateTimeNode(0, 0, 0, 0, 0, val, 0, true);
}

static QoreValue qpg_data_timestamptz(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    if (conn->has_integer_datetimes()) {
        int64 val = MSBi8(*((uint64_t *)data));
        int64 secs = val / 1000000;
        int us = val % 1000000;
        secs += PGSQL_EPOCH_OFFSET;
        return DateTimeNode::makeAbsolute(conn->getTZ(), secs, us);
    }
    double fv = MSBf8(*((double *)data));
    int64 nv = (int64)fv;
    int us = (int)((fv - (double)nv) * 1000000.0);
    nv += PGSQL_EPOCH_OFFSET;
    return DateTimeNode::makeAbsolute(conn->getTZ(), nv, us);
}

static QoreValue qpg_data_timestamp(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    if (conn->has_integer_datetimes()) {
        int64 val = MSBi8(*((uint64_t *)data));

        // convert from u-secs to seconds and milliseconds
        int64 secs = val / 1000000;
        int us = val % 1000000;
        secs += PGSQL_EPOCH_OFFSET;
        return DateTimeNode::makeAbsoluteLocal(conn->getTZ(), secs, us);
    }
    double fv = MSBf8(*((double *)data));
    int64 nv = (int64)fv;
    int us = (int)((fv - (double)nv) * 1000000.0);
    nv += PGSQL_EPOCH_OFFSET;
    return DateTimeNode::makeAbsolute(conn->getTZ(), nv, us);
}

// the DATEOID format is a signed 32-bit integer giving the day offset from 2000-01-01
static QoreValue qpg_data_date(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    int32_t val = ntohl(*((int32_t*)data));
    int64 v = (static_cast<int64>(val) + 10957) * 86400;
    return new DateTimeNode(v);
}

static QoreValue qpg_data_interval(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    int64 secs;
    int hours;
    int minutes;

    qore_pg_interval *iv = (qore_pg_interval *)data;
    // create interval with microsecond resolution
    int64 us;
    if (conn->has_integer_datetimes()) {
        us = MSBi8(iv->time.i);
        secs = us / 1000000;
        us %= 1000000;
        //printd(5, "got interval %lld: secs: %lld us: %lld (day: %d)\n", MSBi8(iv->time.i), secs, us, conn->has_interval_day());
    } else {
        double f = MSBf8(iv->time.f);
        secs = (int64)f;
        us = (int)((f - (double)secs) * 1000000.0);
    }
    hours = secs / 3600;
    if (hours)
        secs -= hours * 3600;
    minutes = secs / 60;
    if (minutes)
        secs -= minutes* 60;
    if (conn->has_interval_day())
        return DateTimeNode::makeRelative(0, ntohl(iv->rest.with_day.month), ntohl(iv->rest.with_day.day), hours, minutes, secs, us);
    return DateTimeNode::makeRelative(0, ntohl(iv->rest.month), 0, hours, minutes, secs, us);
}

static QoreValue qpg_data_time(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    int64 secs;
    int us;
    if (conn->has_integer_datetimes()) {
        int64 val = MSBi8(*((uint64_t *)data));
        secs = val / 1000000;
        us = val % 1000000;
    } else {
        double val = MSBf8(*((double *)data));
        secs = (int64)val;
        us = (int)((val - (double)secs) * 1000000.0);
    }
    //printd(5, "qpg_data_time() %lld.%06d\n", secs, us);
    // create the date/time value from an offset in the current time zone
    return DateTimeNode::makeAbsoluteLocal(conn->getTZ(), secs, us);
}

static QoreValue qpg_data_timetz(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    qore_pg_time_tz_adt *tm = (qore_pg_time_tz_adt *)data;
    int64 secs;
    // postgresql gives the time zone in seconds west of UTC
    int zone = ntohl(tm->zone);

    int us;
    if (conn->has_integer_datetimes()) {
        int64 val = MSBi8(tm->time.i);
        secs = val / 1000000;
        us = val % 1000000;
    } else {
        double val = MSBf8(tm->time.f);
        secs = (int64)val;
        us = (int)((val - (double)secs) * 1000000.0);
    }
    //printd(5, "zone: %d secs: %lld.%06d\n", zone, secs, us);

    return DateTimeNode::makeAbsoluteLocal(findCreateOffsetZone(-zone), secs, us);
}

static QoreValue qpg_data_tinterval(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    //printd(5, "qpg_data_tinterval(row: %d, col: %d, type: %d) this: %p len: %d\n", row, col, type, this, len);
    TimeIntervalData *td = (TimeIntervalData*)data;

    int64 i = (int64)(int)ntohl(td->data[0]);
    DateTime dt(i);
    QoreStringNode* str = new QoreStringNode();
    str->sprintf("[\"%04d-%02d-%02d %02d:%02d:%02d\" ", dt.getYear(), dt.getMonth(), dt.getDay(), dt.getHour(), dt.getMinute(), dt.getSecond());
    dt.setDate((int64)ntohl(td->data[1]));
    str->sprintf("\"%04d-%02d-%02d %02d:%02d:%02d\"]", dt.getYear(), dt.getMonth(), dt.getDay(), dt.getHour(), dt.getMinute(), dt.getSecond());

    // NOTE: ignoring tinverval->status, it is assumed that any value here will be valid
    //printd(5, "status: %d s0: %d s1: %d\n", ntohl(td->status), s0, s1);

    return str;
}

static QoreValue qpg_data_numeric(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    // issue #4249: we cannot write directly to the data, as we may be called multiple times on the same data
    qore_pg_numeric* num = reinterpret_cast<qore_pg_numeric*>(data);
    size_t size = num->rawSize();
    qore_pg_numeric* nd = (qore_pg_numeric*)malloc(size);
    ON_BLOCK_EXIT(free, nd);
    memcpy(nd, num, size);

    nd->convertToHost();
    int nc = conn->getNumeric();
    if (nc == OPT_NUM_OPTIMAL)
        return nd->toOptimal();
    if (nc == OPT_NUM_NUMERIC)
        return nd->toNumber();
    assert(nc == OPT_NUM_STRING);
    return nd->toString();
}

static QoreValue qpg_data_cash(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    double f = (double)ntohl(*((uint32_t*)data)) / 100.0;
    //printd(5, "qpg_data_cash() f: %g\n", f);
    return f;
}

static QoreValue qpg_data_macaddr(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    QoreStringNode* str = new QoreStringNode;
    for (int i = 0; i < 5; i++) {
        str->concatHex((char*)data + i, 1);
        str->concat(':');
    }
    str->concatHex((char*)data+5, 1);
    return str;
}

static QoreValue qpg_data_inet(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    qore_pg_inet_struct *is = (qore_pg_inet_struct *)data;

    QoreStringNode* str = new QoreStringNode();
    if (is->family == PGSQL_AF_INET) {
        for (int i = 0, e = is->length - 1; i < e; i++)
            str->sprintf("%d.", is->ipaddr[i]);
        str->sprintf("%d/%d", is->ipaddr[3], is->bits);
    } else {
        short *sp;
        int i, val, e, last = 0;
        if (type == CIDROID)
            e = is->bits / 8;
        else
            e = is->length;
        if (e == 16) {
            e -= 2;
            last = 1;
        }
        for (i = 0; i < e; i += 2) {
            sp = (short *)&is->ipaddr[i];
            val = ntohs(*sp);
            str->sprintf("%x:", val);
        }
        if (last) {
            sp = (short *)&is->ipaddr[i];
            val = ntohs(*sp);
            str->sprintf("%x", val);
        } else
            str->concat(':');
        str->sprintf("/%d", is->bits);
    }
    return str;
}

static QoreValue qpg_data_tid(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    qore_pg_tuple_id *ti = (qore_pg_tuple_id *)data;
    unsigned block = ntohl(ti->block);
    unsigned index = ntohs(ti->index);
    QoreStringNode* str = new QoreStringNode;
    str->sprintf("(%u,%u)", block, index);
    return str;
}

static QoreValue qpg_data_bit(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    qore_pg_bit *bp = (qore_pg_bit *)data;
    int num = (ntohl(bp->size) - 1) / 8 + 1;
    BinaryNode* b = new BinaryNode;
    b->append(bp->data, num);
    return b;
}

static QoreValue qpg_data_point(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    Point p;
    assign_point(p, (Point*)data);
    QoreStringNode* str = new QoreStringNode;
    str->sprintf("%g,%g", p.x, p.y);
    return str;
}

static QoreValue qpg_data_lseg(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    Point p;
    assign_point(p, &((LSEG *)data)->p[0]);
    QoreStringNode* str = new QoreStringNode;
    str->sprintf("(%g,%g),", p.x, p.y);
    assign_point(p, &((LSEG *)data)->p[1]);
    str->sprintf("(%g,%g)", p.x, p.y);
    return str;
}

// NOTE: This is functionally identical to LSEG above
static QoreValue qpg_data_box(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    Point p0, p1;
    assign_point(p0, &((BOX *)data)->high);
    assign_point(p1, &((BOX *)data)->low);
    QoreStringNode* str = new QoreStringNode;
    str->sprintf("(%g,%g),(%g,%g)", p0.x, p0.y, p1.x, p1.y);
    return str;
}

static QoreValue qpg_data_path(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    unsigned npts = ntohl(*((int*)((char*)data + 1)));
    bool closed = ntohl(*((char*)data));
    //printd(5, "npts: %d closed: %d\n", npts, closed);
    QoreStringNode* str = new QoreStringNode();
    str->concat(closed ? '(' : '[');
    Point p;
    for (unsigned i = 0; i < npts; ++i) {
        assign_point(p, (Point*)(((char*)data) + 5 + (sizeof(Point)) * i));
        str->sprintf("(%g,%g)", p.x, p.y);
        if (i != (npts - 1))
            str->concat(',');
    }
    str->concat(closed ? ')' : ']');
    return str;
}

static QoreValue qpg_data_polygon(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    unsigned npts = ntohl(*((int*)data));
    QoreStringNode* str = new QoreStringNode('(');
    Point p;
    for (unsigned i = 0; i < npts; ++i) {
        assign_point(p, (Point*)(((char*)data) + 4 + (sizeof(Point)) * i));
        str->sprintf("(%g,%g)", p.x, p.y);
        if (i != (npts - 1))
            str->concat(',');
    }
    str->concat(')');
    return str;
}

static QoreValue qpg_data_circle(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    //printd(5, "qpg_data_circle(row: %d, col: %d, type: %d) this: %p len: %d\n", row, col, type, this, len);
    QoreStringNode* str = new QoreStringNode;
    Point p;
    assign_point(p, &((CIRCLE *)data)->center);
    double radius = MSBf8(((CIRCLE *)data)->radius);
    str->sprintf("<(%g,%g),%g>", p.x, p.y, radius);
    return str;
}

static QoreValue qpg_data_uuid(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    // UUID is 16 bytes in binary format
    if (len != 16) {
        return new QoreStringNode(data, len, enc);
    }
    unsigned char* uuid = (unsigned char*)data;
    QoreStringNode* str = new QoreStringNode;
    str->sprintf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5],
        uuid[6], uuid[7],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return str;
}

//! Converts an IEEE 754 half-precision (16-bit) float to double
static double half_to_double(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;

    double result;
    if (exponent == 0) {
        // subnormal or zero
        result = std::ldexp((double)mantissa, -24);
    } else if (exponent == 31) {
        // inf or NaN
        if (mantissa == 0) {
            result = std::numeric_limits<double>::infinity();
        } else {
            result = std::numeric_limits<double>::quiet_NaN();
        }
    } else {
        // normalized
        result = std::ldexp((double)(mantissa + 1024), exponent - 25);
    }
    return sign ? -result : result;
}

//! Converts pgvector binary format to Qore list<float>
/** Binary format: [int16: dim][int16: unused][float4 x dim: elements]
*/
static QoreValue qpg_data_vector(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    if (len < 4) {
        return new QoreStringNode(data, len, enc);
    }
    int16_t dim = ntohs(*((int16_t*)data));
    // skip unused field at data + 2

    if (dim < 0) {
        return new QoreStringNode(data, len, enc);
    }
    int expected_len = 4 + dim * (int)sizeof(float);
    if (len < expected_len) {
        return new QoreStringNode(data, len, enc);
    }

    ReferenceHolder<QoreListNode> l(new QoreListNode(floatTypeInfo), nullptr);
    float* elements = (float*)(data + 4);
    for (int i = 0; i < dim; ++i) {
        float val = MSBf4(elements[i]);
        l->push((double)val, nullptr);
    }
    return l.release();
}

//! Converts pgvector halfvec binary format to Qore list<float>
/** Binary format: [int16: dim][int16: unused][uint16 (IEEE 754 half) x dim: elements]
*/
static QoreValue qpg_data_halfvec(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    if (len < 4) {
        return new QoreStringNode(data, len, enc);
    }
    int16_t dim = ntohs(*((int16_t*)data));
    // skip unused field at data + 2

    if (dim < 0) {
        return new QoreStringNode(data, len, enc);
    }
    int expected_len = 4 + dim * 2;
    if (len < expected_len) {
        return new QoreStringNode(data, len, enc);
    }

    ReferenceHolder<QoreListNode> l(new QoreListNode(floatTypeInfo), nullptr);
    uint16_t* elements = (uint16_t*)(data + 4);
    for (int i = 0; i < dim; ++i) {
        uint16_t raw = ntohs(elements[i]);
        double val = half_to_double(raw);
        l->push(val, nullptr);
    }
    return l.release();
}

//! Converts pgvector sparsevec binary format to Qore hash
/** Binary format: [int32: dim][int32: nnz][int32 x nnz: indices][float4 x nnz: values]
*/
static QoreValue qpg_data_sparsevec(char* data, int type, int len, QorePGConnection* conn, const QoreEncoding* enc) {
    if (len < 8) {
        return new QoreStringNode(data, len, enc);
    }
    int32_t dim = ntohl(*((int32_t*)data));
    int32_t nnz = ntohl(*((int32_t*)(data + 4)));

    if (nnz < 0) {
        return new QoreStringNode(data, len, enc);
    }
    int expected_len = 8 + nnz * (int)(sizeof(int32_t) + sizeof(float));
    if (len < expected_len) {
        return new QoreStringNode(data, len, enc);
    }

    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), nullptr);
    h->setKeyValue("dim", (int64)dim, nullptr);
    h->setKeyValue("nnz", (int64)nnz, nullptr);

    ReferenceHolder<QoreListNode> indices(new QoreListNode(bigIntTypeInfo), nullptr);
    ReferenceHolder<QoreListNode> values(new QoreListNode(floatTypeInfo), nullptr);

    int32_t* idx_ptr = (int32_t*)(data + 8);
    float* val_ptr = (float*)(data + 8 + nnz * sizeof(int32_t));

    for (int i = 0; i < nnz; ++i) {
        indices->push((int64)ntohl(idx_ptr[i]), nullptr);
        float val = MSBf4(val_ptr[i]);
        values->push((double)val, nullptr);
    }

    h->setKeyValue("indices", indices.release(), nullptr);
    h->setKeyValue("values", values.release(), nullptr);
    return h.release();
}

//! Converts a Qore list of floats to pgvector text format "[1.5,2.3,4.1]"
static QoreString* qpg_vector_to_text(const QoreListNode* l) {
    std::unique_ptr<QoreString> str(new QoreString("["));
    ConstListIterator li(l);
    while (li.next()) {
        if (!li.first()) {
            str->concat(',');
        }
        QoreValue v = li.getValue();
        str->sprintf("%.9g", v.getAsFloat());
    }
    str->concat(']');
    return str.release();
}

// static initialization
void QorePgsqlStatement::static_init() {
    data_map[BOOLOID]        = qpg_data_bool;
    data_map[BYTEAOID]       = qpg_data_bytea;
    data_map[CHAROID]        = qpg_data_char;
    data_map[BPCHAROID]      = qpg_data_char;

    // treat UNKNOWNOID as string
    data_map[UNKNOWNOID]     = qpg_data_char;

    data_map[INT8OID]        = qpg_data_int8;
    data_map[INT4OID]        = qpg_data_int4;
    data_map[OIDOID]         = qpg_data_int4;
    data_map[XIDOID]         = qpg_data_int4;
    data_map[CIDOID]         = qpg_data_int4;
    //data_map[REGPROCOID]     = qpg_data_int4;
    data_map[INT2OID]        = qpg_data_int2;
    data_map[TEXTOID]        = qpg_data_text;
    data_map[VARCHAROID]     = qpg_data_text;
    data_map[NAMEOID]        = qpg_data_text;
    data_map[FLOAT4OID]      = qpg_data_float4;
    data_map[FLOAT8OID]      = qpg_data_float8;
    data_map[ABSTIMEOID]     = qpg_data_abstime;
    data_map[RELTIMEOID]     = qpg_data_reltime;

    data_map[TIMESTAMPOID]   = qpg_data_timestamp;
    data_map[TIMESTAMPTZOID] = qpg_data_timestamptz;
    data_map[DATEOID]        = qpg_data_date;
    data_map[INTERVALOID]    = qpg_data_interval;
    data_map[TIMEOID]        = qpg_data_time;
    data_map[TIMETZOID]      = qpg_data_timetz;
    data_map[TINTERVALOID]   = qpg_data_tinterval;
    data_map[NUMERICOID]     = qpg_data_numeric;
    data_map[CASHOID]        = qpg_data_cash;
    data_map[MACADDROID]     = qpg_data_macaddr;
    data_map[INETOID]        = qpg_data_inet;
    data_map[CIDROID]        = qpg_data_inet;
    data_map[TIDOID]         = qpg_data_tid;
    data_map[BITOID]         = qpg_data_bit;
    data_map[VARBITOID]      = qpg_data_bit;
    data_map[POINTOID]       = qpg_data_point;
    data_map[LSEGOID]        = qpg_data_lseg;
    data_map[BOXOID]         = qpg_data_box;
    data_map[PATHOID]        = qpg_data_path;
    data_map[POLYGONOID]     = qpg_data_polygon;
    data_map[CIRCLEOID]      = qpg_data_circle;
    data_map[XMLOID]         = qpg_data_text;
    data_map[JSONOID]        = qpg_data_text;
    data_map[JSONBOID]       = qpg_data_jsonb;
    data_map[UUIDOID]        = qpg_data_uuid;

    //data_map[INT2VECTOROID]  = qpg_data_int2vector;
    //data_map[OIDVECTOROID]   = qpg_data_oidvector;

    //array_data_map[INT2VECTOROID] = std::make_pair(INT2OID, qpg_data_int2);
    // NOTE: the casts are necessary with SunPro CC 5.8...
    array_data_map[QPGT_INT4ARRAYOID]         = std::make_pair(INT4OID, (qore_pg_data_func_t)qpg_data_int4);
    array_data_map[QPGT_CIRCLEARRAYOID]       = std::make_pair(CIRCLEOID, (qore_pg_data_func_t)qpg_data_circle);
    array_data_map[QPGT_MONEYARRAYOID]        = std::make_pair(CASHOID, (qore_pg_data_func_t)qpg_data_cash);
    array_data_map[QPGT_BOOLARRAYOID]         = std::make_pair(BOOLOID, (qore_pg_data_func_t)qpg_data_bool);
    array_data_map[QPGT_BYTEAARRAYOID]        = std::make_pair(BYTEAOID, (qore_pg_data_func_t)qpg_data_bytea);
    array_data_map[QPGT_NAMEARRAYOID]         = std::make_pair(NAMEOID, (qore_pg_data_func_t)qpg_data_text);
    array_data_map[QPGT_INT2ARRAYOID]         = std::make_pair(INT2OID, (qore_pg_data_func_t)qpg_data_int2);
    //array_data_map[QPGT_INT2VECTORARRAYOID]   = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGPROCARRAYOID]      = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    array_data_map[QPGT_TEXTARRAYOID]         = std::make_pair(TEXTOID, (qore_pg_data_func_t)qpg_data_text);
    array_data_map[QPGT_OIDARRAYOID]          = std::make_pair(OIDOID, (qore_pg_data_func_t)qpg_data_int4);
    array_data_map[QPGT_TIDARRAYOID]          = std::make_pair(TIDOID, (qore_pg_data_func_t)qpg_data_tid);
    array_data_map[QPGT_XIDARRAYOID]          = std::make_pair(XIDOID, (qore_pg_data_func_t)qpg_data_int4);
    array_data_map[QPGT_CIDARRAYOID]          = std::make_pair(CIDOID, (qore_pg_data_func_t)qpg_data_int4);
    //array_data_map[QPGT_OIDVECTORARRAYOID]    = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    array_data_map[QPGT_BPCHARARRAYOID]       = std::make_pair(BPCHAROID, (qore_pg_data_func_t)qpg_data_char);
    array_data_map[QPGT_VARCHARARRAYOID]      = std::make_pair(VARCHAROID, (qore_pg_data_func_t)qpg_data_text);
    array_data_map[QPGT_INT8ARRAYOID]         = std::make_pair(INT8OID, (qore_pg_data_func_t)qpg_data_int8);
    array_data_map[QPGT_POINTARRAYOID]        = std::make_pair(POINTOID, (qore_pg_data_func_t)qpg_data_point);
    array_data_map[QPGT_LSEGARRAYOID]         = std::make_pair(LSEGOID, (qore_pg_data_func_t)qpg_data_lseg);
    array_data_map[QPGT_PATHARRAYOID]         = std::make_pair(PATHOID, (qore_pg_data_func_t)qpg_data_path);
    array_data_map[QPGT_BOXARRAYOID]          = std::make_pair(BOXOID, (qore_pg_data_func_t)qpg_data_box);
    array_data_map[QPGT_FLOAT4ARRAYOID]       = std::make_pair(FLOAT4OID, (qore_pg_data_func_t)qpg_data_float4);
    array_data_map[QPGT_FLOAT8ARRAYOID]       = std::make_pair(FLOAT8OID, (qore_pg_data_func_t)qpg_data_float8);
    array_data_map[QPGT_ABSTIMEARRAYOID]      = std::make_pair(ABSTIMEOID, (qore_pg_data_func_t)qpg_data_abstime);
    array_data_map[QPGT_RELTIMEARRAYOID]      = std::make_pair(RELTIMEOID, (qore_pg_data_func_t)qpg_data_reltime);
    array_data_map[QPGT_TINTERVALARRAYOID]    = std::make_pair(TINTERVALOID, (qore_pg_data_func_t)qpg_data_tinterval);
    array_data_map[QPGT_POLYGONARRAYOID]      = std::make_pair(POLYGONOID, (qore_pg_data_func_t)qpg_data_polygon);
    //array_data_map[QPGT_ACLITEMARRAYOID]      = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    array_data_map[QPGT_MACADDRARRAYOID]      = std::make_pair(MACADDROID, (qore_pg_data_func_t)qpg_data_macaddr);
    array_data_map[QPGT_INETARRAYOID]         = std::make_pair(INETOID, (qore_pg_data_func_t)qpg_data_inet);
    array_data_map[QPGT_CIDRARRAYOID]         = std::make_pair(CIDROID, (qore_pg_data_func_t)qpg_data_inet);
    array_data_map[QPGT_TIMESTAMPARRAYOID]    = std::make_pair(TIMESTAMPOID, (qore_pg_data_func_t)qpg_data_timestamp);
    array_data_map[QPGT_DATEARRAYOID]         = std::make_pair(DATEOID, (qore_pg_data_func_t)qpg_data_date);
    array_data_map[QPGT_TIMEARRAYOID]         = std::make_pair(TIMEOID, (qore_pg_data_func_t)qpg_data_time);
    array_data_map[QPGT_TIMESTAMPTZARRAYOID]  = std::make_pair(TIMESTAMPTZOID, (qore_pg_data_func_t)qpg_data_timestamptz);
    array_data_map[QPGT_INTERVALARRAYOID]     = std::make_pair(INTERVALOID, (qore_pg_data_func_t)qpg_data_interval);
    array_data_map[QPGT_NUMERICARRAYOID]      = std::make_pair(NUMERICOID, (qore_pg_data_func_t)qpg_data_numeric);
    array_data_map[QPGT_TIMETZARRAYOID]       = std::make_pair(TIMETZOID, (qore_pg_data_func_t)qpg_data_timetz);
    array_data_map[QPGT_BITARRAYOID]          = std::make_pair(BITOID, (qore_pg_data_func_t)qpg_data_bit);
    array_data_map[QPGT_VARBITARRAYOID]       = std::make_pair(VARBITOID, (qore_pg_data_func_t)qpg_data_bit);
    //array_data_map[QPGT_REFCURSORARRAYOID]    = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGPROCEDUREARRAYOID] = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGOPERARRAYOID]      = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGOPERATORARRAYOID]  = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGCLASSARRAYOID]     = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_REGTYPEARRAYOID]      = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    //array_data_map[QPGT_ANYARRAYOID]          = std::make_pair(OID, (qore_pg_data_func_t)qpg_data_);
    array_data_map[XMLARRAYOID]              = std::make_pair(XMLOID, (qore_pg_data_func_t)qpg_data_text);
    array_data_map[JSONARRAYOID]             = std::make_pair(JSONOID, (qore_pg_data_func_t)qpg_data_text);
    array_data_map[JSONBARRAYOID]            = std::make_pair(JSONBOID, (qore_pg_data_func_t)qpg_data_jsonb);
    array_data_map[QPGT_UUIDARRAYOID]        = std::make_pair(UUIDOID, (qore_pg_data_func_t)qpg_data_uuid);

    array_type_map[INT4OID]                      = QPGT_INT4ARRAYOID;
    array_type_map[CIRCLEOID]                    = QPGT_CIRCLEARRAYOID;
    array_type_map[CASHOID]                      = QPGT_MONEYARRAYOID;
    array_type_map[BOOLOID]                      = QPGT_BOOLARRAYOID;
    array_type_map[BYTEAOID]                     = QPGT_BYTEAARRAYOID;
    array_type_map[NAMEOID]                      = QPGT_NAMEARRAYOID;
    array_type_map[INT2OID]                      = QPGT_INT2ARRAYOID;
    array_type_map[TEXTOID]                      = QPGT_TEXTARRAYOID;
    array_type_map[OIDOID]                       = QPGT_OIDARRAYOID;
    array_type_map[TIDOID]                       = QPGT_TIDARRAYOID;
    array_type_map[XIDOID]                       = QPGT_XIDARRAYOID;
    array_type_map[CIDOID]                       = QPGT_CIDARRAYOID;
    array_type_map[BPCHAROID]                    = QPGT_BPCHARARRAYOID;
    array_type_map[VARCHAROID]                   = QPGT_VARCHARARRAYOID;
    array_type_map[INT8OID]                      = QPGT_INT8ARRAYOID;
    array_type_map[POINTOID]                     = QPGT_POINTARRAYOID;
    array_type_map[LSEGOID]                      = QPGT_LSEGARRAYOID;
    array_type_map[PATHOID]                      = QPGT_PATHARRAYOID;
    array_type_map[BOXOID]                       = QPGT_BOXARRAYOID;
    array_type_map[FLOAT4OID]                    = QPGT_FLOAT4ARRAYOID;
    array_type_map[FLOAT8OID]                    = QPGT_FLOAT8ARRAYOID;
    array_type_map[ABSTIMEOID]                   = QPGT_ABSTIMEARRAYOID;
    array_type_map[RELTIMEOID]                   = QPGT_RELTIMEARRAYOID;
    array_type_map[TINTERVALOID]                 = QPGT_TINTERVALARRAYOID;
    array_type_map[POLYGONOID]                   = QPGT_POLYGONARRAYOID;
    array_type_map[MACADDROID]                   = QPGT_MACADDRARRAYOID;
    array_type_map[INETOID]                      = QPGT_INETARRAYOID;
    array_type_map[CIDROID]                      = QPGT_CIDRARRAYOID;
    array_type_map[TIMESTAMPOID]                 = QPGT_TIMESTAMPARRAYOID;
    array_type_map[DATEOID]                      = QPGT_DATEARRAYOID;
    array_type_map[TIMEOID]                      = QPGT_TIMEARRAYOID;
    array_type_map[TIMESTAMPTZOID]               = QPGT_TIMESTAMPTZARRAYOID;
    array_type_map[INTERVALOID]                  = QPGT_INTERVALARRAYOID;
    array_type_map[NUMERICOID]                   = QPGT_NUMERICARRAYOID;
    array_type_map[TIMETZOID]                    = QPGT_TIMETZARRAYOID;
    array_type_map[BITOID]                       = QPGT_BITARRAYOID;
    array_type_map[VARBITOID]                    = QPGT_VARBITARRAYOID;
    array_type_map[XMLOID]                       = XMLARRAYOID;
    array_type_map[JSONOID]                      = JSONARRAYOID;
    array_type_map[JSONBOID]                     = JSONBARRAYOID;
    array_type_map[UUIDOID]                      = QPGT_UUIDARRAYOID;
}

QorePgsqlStatement::QorePgsqlStatement(QorePGConnection* r_conn, const QoreEncoding* r_enc)
    : res(0), nParams(0), allocated(0), paramTypes(0), paramValues(0),
        paramLengths(0), paramFormats(0), paramArray(0), conn(r_conn), enc(r_enc), array_size(-1) {
}

QorePgsqlStatement::QorePgsqlStatement(Datasource* ds)
    : res(0), nParams(0), allocated(0), paramTypes(0), paramValues(0),
        paramLengths(0), paramFormats(0), paramArray(0), conn((QorePGConnection*)ds->getPrivateData()),
        enc(ds->getQoreEncoding()), array_size(-1) {
}

QorePgsqlStatement::~QorePgsqlStatement() {
    reset();
}

void QorePgsqlStatement::reset() {
    if (res) {
        PQclear(res);
        res = 0;
    }

    if (allocated) {
        parambuf_list_t::iterator i = parambuf_list.begin();
        for (int j = 0; j < nParams; ++i, ++j) {
            //printd(5, "QorePgsqlStatement::reset() deleting type %d (NUMERICOID = %d)\n", paramTypes[j], NUMERICOID);
            if (paramFormats[j] == 0 && !paramArray[j] && (*i)->str) {
                // free text-format scalar bind string buffers (TEXTOID, typed text binds, etc.)
                free((*i)->str);
            } else if (paramTypes[j] == NUMERICOID && (*i)->num) {
                //printd(5, "QorePgsqlStatement::reset() deleting num: %p\n", (*i)->num);
                delete (*i)->num;
            } else if (paramArray[j] && (*i)->ptr) {
                free((*i)->ptr);
            }
            delete *i;
        }
        // delete any remaining parambufs beyond nParams (e.g. from an error in add())
        while (i != parambuf_list.end()) {
            delete *i;
            ++i;
        }

        parambuf_list.clear();

        free(paramTypes);
        paramTypes = 0;

        free(paramValues);
        paramValues = 0;

        free(paramLengths);
        paramLengths = 0;

        free(paramFormats);
        paramFormats = 0;

        free(paramArray);
        paramArray = 0;

        allocated = 0;
        nParams = 0;
    }
    array_size = -1;
}

int QorePgsqlStatement::rowsAffected() {
   assert(res);
   return atoi(PQcmdTuples(res));
}

bool QorePgsqlStatement::hasResultData() {
   return PQnfields(res);
}

QoreListNode* QorePgsqlStatement::getArray(int type, qore_pg_data_func_t func, char*& array_data, int current,
        int ndim, int dim[]) {
    //printd(5, "getArray(type: %d, array_data: %p, current: %d, ndim: %d, dim[%d]: %d)\n", type, array_data, current,
    //  ndim, current, dim[current]);
    QoreListNode* l = new QoreListNode(autoTypeInfo);

    if (current != (ndim - 1)) {
        for (int i = 0; i < dim[current]; ++i)
            l->push(getArray(type, func, array_data, current + 1, ndim, dim), nullptr);
    } else
        for (int i = 0; i < dim[current]; ++i) {
            int length = ntohl(*((uint32_t*)(array_data)));
            //printd(5, "length: %d\n", length);
            array_data += 4;
            if (length == -1) // NULL value
                l->push(null(), nullptr);
            else {
                l->push(func(array_data, type, length, conn, enc), nullptr);
                array_data += length;
            }
        }

    return l;
}

// converts from PostgreSQL data types to Qore data
QoreValue QorePgsqlStatement::getValue(int row, int col, ExceptionSink *xsink) {
    void* data = PQgetvalue(res, row, col);
    int type = PQftype(res, col);
    //int mod = PQfmod(res, col);
    int len = PQgetlength(res, row, col);

    //printd(5, "QorePgsqlStatement::getValue(row: %d, col: %d) type: %d this: %p len: %d\n", row, col, type, this, len);
    //do_output((char*)data, len);
    assert(row >= 0);

    if (PQgetisnull(res, row, col))
        return null();

    qore_pg_data_map_t::const_iterator i = data_map.find(type);
    if (i != data_map.end()) {
        return i->second((char*)data, type, len, conn, enc);
    }

    // check per-connection extension types (pgvector, etc.)
    qore_pg_data_func_t ext_func = conn->getExtensionDataFunc(type);
    if (ext_func) {
        return ext_func((char*)data, type, len, conn, enc);
    }

    // otherwise, see if it's an array
    qore_pg_array_data_map_t::const_iterator ai = array_data_map.find(type);
    if (ai == array_data_map.end()) {
        // check per-connection extension array types
        int ext_elem_oid;
        qore_pg_data_func_t ext_arr_func;
        if (conn->getExtensionArrayDataFunc(type, ext_elem_oid, ext_arr_func)) {
            qore_pg_array_header* ah = (qore_pg_array_header*)data;
            int ndim = ntohl(ah->ndim);
            int dim[ndim];
            for (int di = 0; di < ndim; ++di) {
                dim[di] = ntohl(ah->info[di].dim);
            }
            char* array_data = ((char*)data) + 12 + 8 * ndim;
            return getArray(ext_elem_oid, ext_arr_func, array_data, 0, ndim, dim);
        }
        xsink->raiseException("DBI:PGSQL:TYPE-ERROR", "don't know how to handle type ID: %d", type);
        return QoreValue();
    }

    //printd(5, "QorePgsqlStatement::getValue(row: %d, col: %d) ARRAY type: %d this: %p len: %d\n", row, col, type, this, len);
    qore_pg_array_header *ah = (qore_pg_array_header *)data;
    int ndim = ntohl(ah->ndim);
    //int oid  = ntohl(ah->oid);
    //printd(5, "array dimensions %d, oid: %d\n", ndim, oid);
    int dim[ndim];
    //int lBound[ndim];
    for (int i = 0; i < ndim; ++i) {
        dim[i]    = ntohl(ah->info[i].dim);
        //lBound[i] = ntohl(ah->info[i].lBound);
        //printd(5, "%d: dim: %d lBound: %d\n", i, dim[i], lBound[i]);
    }

    char* array_data = ((char*)data) + 12 + 8 * ndim;
    return getArray(ai->second.first, ai->second.second, array_data, 0, ndim, dim);
}

void QorePgsqlStatement::setupColumnNames(strvec_t& cvec, int num_columns) {
    std::unordered_set<std::string> used_names;
    used_names.reserve(num_columns);
    for (int i = 0; i < num_columns; ++i) {
        const char* name = PQfname(res, i);
        std::string col_name = name;

        unsigned num = 1;
        while (used_names.find(col_name) != used_names.end()) {
            QoreStringMaker tmp("%s_%d", name, num++);
            col_name = tmp.c_str();
        }
        used_names.insert(col_name);
        cvec.push_back(col_name);
    }
}

void QorePgsqlStatement::setupColumns(QoreHashNode& h, strvec_t& cvec, int num_columns) {
    setupColumnNames(cvec, num_columns);
    for (const std::string& name : cvec) {
        HashAssignmentHelper hah(h, name.c_str());
        hah.assign(new QoreListNode(autoTypeInfo), 0);
    }
}

QoreHashNode* QorePgsqlStatement::getOutputHash(ExceptionSink* xsink, bool cols, int* start, int maxrows) {
    assert(res);
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);

    int num_columns = PQnfields(res);

    //printd(5, "QorePgsqlStatement::getOutputHash() num_columns: %d num_rows: %d\n", num_columns, PQntuples(res));

    int i = start ? *start : 0;
    maxrows += i;

    int nt = PQntuples(res);
    int max = maxrows < 0 ? nt : (maxrows > nt ? nt : maxrows);

    strvec_t cvec;

    if (cols || (i < max)) {
        // assign unique column names
        cvec.reserve(num_columns);
        setupColumns(**h, cvec, num_columns);
    }

    for (; i < max; ++i) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((i % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }
        for (int j = 0; j < num_columns; ++j) {
            ValueHolder n(getValue(i, j, xsink), xsink);
            if (!n || *xsink)
                return nullptr;

            QoreListNode* l = h->getKeyValue(cvec[j].c_str()).get<QoreListNode>();
            l->push(n.release(), xsink);
        }
    }
    if (start)
        *start = i;
    return h.release();
}

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
QoreColumnarResult* QorePgsqlStatement::getOutputColumnar(ExceptionSink* xsink, bool cols, int* start,
        int maxrows) {
    assert(res);

    int num_columns = PQnfields(res);
    int i = start ? *start : 0;

    int nt = PQntuples(res);
    int max = maxrows < 0 ? nt : (i + maxrows > nt ? nt : i + maxrows);
    int row_count = max - i;

    strvec_t cvec;
    if (cols || (i < max)) {
        cvec.reserve(num_columns);
        setupColumnNames(cvec, num_columns);
    }

    ReferenceHolder<QoreColumnarResult> rv(new QoreColumnarResult, xsink);
    for (int j = 0; j < num_columns && !cvec.empty(); ++j) {
        if (j && !(j % 100) && qore_check_cancel(xsink, "building PostgreSQL columnar result")) {
            return nullptr;
        }

        Oid oid = PQftype(res, j);
        int fmod = PQfmod(res, j);
        QoreBufferElementType buffer_type = QoreBufferElementType::Invalid;
        QoreColumnarColumnType column_type = QoreColumnarColumnType::Auto;
        std::string native_type = qpg_get_columnar_native_type(oid, fmod, conn);
        if (qpg_get_columnar_buffer_type(oid, buffer_type, column_type)) {
            bool nullable = false;
            for (int r = i; r < max; ++r) {
                if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                        "scanning PostgreSQL column nulls")) {
                    return nullptr;
                }
                if (PQgetisnull(res, r, j)) {
                    nullable = true;
                    break;
                }
            }

            if (buffer_type == QoreBufferElementType::String) {
                ReferenceHolder<QoreListNode> list(new QoreListNode(autoTypeInfo), xsink);
                for (int r = i; r < max; ++r) {
                    if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                            "building PostgreSQL string column")) {
                        return nullptr;
                    }
                    ValueHolder n(getValue(r, j, xsink), xsink);
                    if (*xsink) {
                        return nullptr;
                    }
                    list->push(n.release(), xsink);
                    if (*xsink) {
                        return nullptr;
                    }
                }

                ReferenceHolder<QoreBufferNode> buffer(new QoreBufferNode(buffer_type, nullable, *list, xsink),
                    xsink);
                if (*xsink) {
                    return nullptr;
                }
                if (rv->addColumn(cvec[j].c_str(), buffer.release(), column_type, buffer_type, nullable,
                        native_type.c_str(), xsink)) {
                    return nullptr;
                }
                continue;
            }

            ReferenceHolder<QoreBufferNode> buffer(new QoreBufferNode(buffer_type, nullable, row_count), xsink);
            switch (buffer_type) {
                case QoreBufferElementType::Bool:
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL boolean column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else if (buffer->setEntry(out, QoreValue(PQgetvalue(res, r, j)[0] != 0), xsink)) {
                            return nullptr;
                        }
                    }
                    break;
                case QoreBufferElementType::Int16: {
                    int16_t* dest = static_cast<int16_t*>(buffer->getRawData());
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL integer column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else {
                            dest[out] = qpg_read_int2(PQgetvalue(res, r, j));
                        }
                    }
                    break;
                }
                case QoreBufferElementType::Int32: {
                    int32_t* dest = static_cast<int32_t*>(buffer->getRawData());
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL integer column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else {
                            dest[out] = qpg_read_int4(PQgetvalue(res, r, j));
                        }
                    }
                    break;
                }
                case QoreBufferElementType::Int64: {
                    int64_t* dest = static_cast<int64_t*>(buffer->getRawData());
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL integer column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else {
                            dest[out] = qpg_read_int8(PQgetvalue(res, r, j));
                        }
                    }
                    break;
                }
                case QoreBufferElementType::Float32: {
                    float* dest = static_cast<float*>(buffer->getRawData());
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL floating-point column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else {
                            dest[out] = qpg_read_float4(PQgetvalue(res, r, j));
                        }
                    }
                    break;
                }
                case QoreBufferElementType::Float64: {
                    double* dest = static_cast<double*>(buffer->getRawData());
                    for (int r = i; r < max; ++r) {
                        if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                                "building PostgreSQL floating-point column")) {
                            return nullptr;
                        }
                        size_t out = static_cast<size_t>(r - i);
                        if (PQgetisnull(res, r, j)) {
                            if (buffer->setEntry(out, QoreValue(), xsink)) {
                                return nullptr;
                            }
                        } else {
                            dest[out] = qpg_read_float8(PQgetvalue(res, r, j));
                        }
                    }
                    break;
                }
                default:
                    assert(false);
                    break;
            }

            if (rv->addColumn(cvec[j].c_str(), buffer.release(), column_type, buffer_type, nullable,
                    native_type.c_str(), xsink)) {
                return nullptr;
            }
            continue;
        }

        ReferenceHolder<QoreListNode> list(new QoreListNode(autoTypeInfo), xsink);
        bool nullable = false;
        for (int r = i; r < max; ++r) {
            if (r != i && !((r - i) % 100) && qore_check_cancel(xsink,
                    "building PostgreSQL columnar list column")) {
                return nullptr;
            }
            if (PQgetisnull(res, r, j)) {
                nullable = true;
            }
            ValueHolder n(getValue(r, j, xsink), xsink);
            if (!n || *xsink) {
                return nullptr;
            }
            list->push(n.release(), xsink);
            if (*xsink) {
                return nullptr;
            }
        }

        if (rv->addColumn(cvec[j].c_str(), list.release(), qpg_get_columnar_column_type(oid, conn),
                QoreBufferElementType::Invalid, nullable, native_type.c_str(), xsink)) {
            return nullptr;
        }
    }

    if (start) {
        *start = max;
    }
    return rv.release();
}
#endif

QoreHashNode* QorePgsqlStatement::getSingleRow(ExceptionSink* xsink, int row) {
    int e = PQntuples(res);
    if (!e)
        return nullptr;
    if (e > 1) {
        xsink->raiseException("DBI-SELECT-ROW-ERROR", "%s: SQL passed to selectRow() returned more than 1 row (%d "
            "rows in result set)", conn->getServerDesc(), e);
        return nullptr;
    }

    return getSingleRowIntern(xsink, row);
}

QoreHashNode* QorePgsqlStatement::getSingleRowIntern(ExceptionSink* xsink, int row) {
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);

    int num_columns = PQnfields(res);

    for (int j = 0; j < num_columns; ++j) {
        ValueHolder n(getValue(row, j, xsink), xsink);
        if (!n || *xsink)
            return nullptr;

        const char* name = PQfname(res, j);
        HashAssignmentHelper hah(**h, name);
        if (!hah.get().isNothing()) {
            // find a unique column name
            unsigned num = 1;
            while (true) {
                QoreStringMaker tmp("%s_%d", name, num);
                hah.reassign(tmp.c_str());
                if (!hah.get().isNothing()) {
                    ++num;
                    continue;
                }
                break;
            }
        }

        hah.assign(n.release(), xsink);
    }
    return h.release();
}

QoreListNode* QorePgsqlStatement::getOutputList(ExceptionSink *xsink, int* start, int maxrows) {
    ReferenceHolder<QoreListNode> l(new QoreListNode(autoTypeInfo), xsink);

    int num_columns = PQnfields(res);

    printd(5, "QorePgsqlStatement::getOutputList() num_columns: %d num_rows: %d\n", num_columns, PQntuples(res));

    int i = start ? *start : 0;
    maxrows += i;

    int nt = PQntuples(res);
    int max = maxrows < 0 ? nt : (maxrows > nt ? nt : maxrows);

    for (; i < max; ++i) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((i % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }
        ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);
        for (int j = 0; j < num_columns; ++j) {
            ValueHolder n(getValue(i, j, xsink), xsink);
            if (!n || *xsink)
                return nullptr;

            const char* name = PQfname(res, j);
            HashAssignmentHelper hah(**h, name);
            if (!hah.get().isNothing()) {
                // find a unique column name
                unsigned num = 1;
                while (true) {
                    QoreStringMaker tmp("%s_%d", name, num);
                    hah.reassign(tmp.c_str());
                    if (!hah.get().isNothing()) {
                        ++num;
                        continue;
                    }
                    break;
                }
            }

            hah.assign(n.release(), xsink);
        }
        l->push(h.release(), xsink);
    }
    if (start)
        *start = i;

    return l.release();
}

static int check_hash_type(const QoreHashNode* h, ExceptionSink *xsink) {
    QoreValue t = h->getKeyValue("^pgtype^");
    if (t.isNothing()) {
        xsink->raiseException("DBI:PGSQL:BIND-ERROR", "missing '^pgtype^' value in bind hash");
        return -1;
    }
    if (t.getType() != NT_INT) {
        xsink->raiseException("DBI:PGSQL:BIND-ERROR", "'^pgtype^' key contains '%s' value, expecting integer",
            t.getTypeName());
        return -1;
    }
    return (int)t.getAsBigInt();
}

//! Resolves a PostgreSQL type name string to its OID
/** Handles type names with optional size modifiers like "bit(8)" or "numeric(10,2)"
    and array suffixes like "integer[]" or "bit varying(16)[]".

    @param type_name the PostgreSQL type name (case-insensitive)
    @param is_array set to true if the type name has an array suffix "[]"
    @param conn the connection context for resolving extension types (pgvector, etc.)
    @param xsink exception sink for error reporting
    @return the base scalar OID, or (Oid)-1 on error
*/
static Oid resolve_pg_type_name(const char* type_name, bool& is_array, QorePGConnection* conn,
        ExceptionSink* xsink) {
    is_array = false;

    // make a lowercase copy and strip whitespace
    QoreString tname(type_name);
    tname.trim();
    tname.tolwr();

    // check for array suffix "[]"
    if (tname.strlen() >= 2 && !strcmp(tname.c_str() + tname.strlen() - 2, "[]")) {
        is_array = true;
        tname.terminate(tname.strlen() - 2);
        tname.trim();
    }

    // strip parenthesized modifiers like (8) from "bit(8)" or (10,2) from "numeric(10,2)"
    const char* paren = strchr(tname.c_str(), '(');
    QoreString base_name;
    if (paren) {
        base_name.concat(tname.c_str(), paren - tname.c_str());
        base_name.trim();
    } else {
        base_name = tname;
    }

    const char* bn = base_name.c_str();

    // integer types
    if (!strcmp(bn, "int4") || !strcmp(bn, "integer") || !strcmp(bn, "int") || !strcmp(bn, "serial")) {
        return INT4OID;
    }
    if (!strcmp(bn, "int8") || !strcmp(bn, "bigint") || !strcmp(bn, "bigserial")) {
        return INT8OID;
    }
    if (!strcmp(bn, "int2") || !strcmp(bn, "smallint") || !strcmp(bn, "smallserial")) {
        return INT2OID;
    }

    // string/char types
    if (!strcmp(bn, "text")) {
        return TEXTOID;
    }
    if (!strcmp(bn, "varchar") || !strcmp(bn, "character varying")) {
        return VARCHAROID;
    }
    if (!strcmp(bn, "bpchar") || !strcmp(bn, "character") || !strcmp(bn, "char")) {
        return BPCHAROID;
    }
    if (!strcmp(bn, "name")) {
        return NAMEOID;
    }

    // boolean
    if (!strcmp(bn, "boolean") || !strcmp(bn, "bool")) {
        return BOOLOID;
    }

    // numeric
    if (!strcmp(bn, "numeric") || !strcmp(bn, "decimal")) {
        return NUMERICOID;
    }

    // float types
    if (!strcmp(bn, "float4") || !strcmp(bn, "real")) {
        return FLOAT4OID;
    }
    if (!strcmp(bn, "float8") || !strcmp(bn, "double precision") || !strcmp(bn, "float")) {
        return FLOAT8OID;
    }

    // binary
    if (!strcmp(bn, "bytea")) {
        return BYTEAOID;
    }

    // date/time types
    if (!strcmp(bn, "date")) {
        return DATEOID;
    }
    if (!strcmp(bn, "time") || !strcmp(bn, "time without time zone")) {
        return TIMEOID;
    }
    if (!strcmp(bn, "timetz") || !strcmp(bn, "time with time zone")) {
        return TIMETZOID;
    }
    if (!strcmp(bn, "timestamp") || !strcmp(bn, "timestamp without time zone")) {
        return TIMESTAMPOID;
    }
    if (!strcmp(bn, "timestamptz") || !strcmp(bn, "timestamp with time zone")) {
        return TIMESTAMPTZOID;
    }
    if (!strcmp(bn, "interval")) {
        return INTERVALOID;
    }

    // bit types
    if (!strcmp(bn, "bit")) {
        return BITOID;
    }
    if (!strcmp(bn, "varbit") || !strcmp(bn, "bit varying")) {
        return VARBITOID;
    }

    // network types
    if (!strcmp(bn, "macaddr")) {
        return MACADDROID;
    }
    if (!strcmp(bn, "inet")) {
        return INETOID;
    }
    if (!strcmp(bn, "cidr")) {
        return CIDROID;
    }

    // oid
    if (!strcmp(bn, "oid")) {
        return OIDOID;
    }

    // json types
    if (!strcmp(bn, "json")) {
        return JSONOID;
    }
    if (!strcmp(bn, "jsonb")) {
        return JSONBOID;
    }

    // uuid
    if (!strcmp(bn, "uuid")) {
        return UUIDOID;
    }

    // xml
    if (!strcmp(bn, "xml")) {
        return XMLOID;
    }

    // geometric types
    if (!strcmp(bn, "point")) {
        return POINTOID;
    }
    if (!strcmp(bn, "lseg")) {
        return LSEGOID;
    }
    if (!strcmp(bn, "path")) {
        return PATHOID;
    }
    if (!strcmp(bn, "box")) {
        return BOXOID;
    }
    if (!strcmp(bn, "polygon")) {
        return POLYGONOID;
    }
    if (!strcmp(bn, "circle")) {
        return CIRCLEOID;
    }
    if (!strcmp(bn, "line")) {
        return LINEOID;
    }

    // money
    if (!strcmp(bn, "money")) {
        return CASHOID;
    }

    // check per-connection extension types (pgvector, etc.)
    if (conn) {
        Oid ext_oid = conn->resolveExtensionTypeName(bn);
        if (ext_oid) {
            return ext_oid;
        }
    }

    xsink->raiseException("DBI:PGSQL:BIND-ERROR",
        "unknown PostgreSQL type name '%s'", type_name);
    return (Oid)-1;
}

//! Returns the appropriate date format string for a PostgreSQL type OID
static const char* get_pg_date_format(Oid base_oid) {
    switch (base_oid) {
        case TIMEOID:
            return "HH:mm:SS.xx";
        case TIMETZOID:
            return "HH:mm:SS.xxZ";
        case DATEOID:
            return "YYYY-MM-DD";
        default:
            // TIMESTAMPOID, TIMESTAMPTZOID, INTERVALOID, and anything else: use ISO 8601
            return "IF";
    }
}

//! Builds a PostgreSQL text-format array literal from a Qore list
/** Converts a Qore list to the PostgreSQL text array literal format: {val1,val2,...}
    NULL/NOTHING values become unquoted NULL; all other values are quoted with
    backslash and double-quote escaping.

    @param l the list of values to convert
    @param base_oid the base type OID for date/time formatting
    @param enc the character encoding for string conversion
    @param conn the connection context for resolving extension types
    @param xsink exception sink for error reporting
    @return a new QoreString containing the text array literal, or nullptr on error
*/
static QoreString* build_text_array_literal(const QoreListNode* l, Oid base_oid, const QoreEncoding* enc,
        QorePGConnection* conn, ExceptionSink* xsink) {
    // check if this is a vector/halfvec array (elements are lists of floats)
    bool is_vector_array = conn
        && (base_oid == conn->getVectorOid() || base_oid == conn->getHalfvecOid());

    std::unique_ptr<QoreString> result(new QoreString("{"));
    const char* date_fmt = get_pg_date_format(base_oid);

    ConstListIterator li(l);
    while (li.next()) {
        if (!li.first()) {
            result->concat(',');
        }
        QoreValue elem = li.getValue();
        if (elem.isNullOrNothing()) {
            result->concat("NULL");
        } else {
            result->concat('"');
            if (is_vector_array && elem.getType() == NT_LIST) {
                // vector/halfvec element: format as [v1,v2,...] with escaping
                std::unique_ptr<QoreString> vec_str(qpg_vector_to_text(elem.get<const QoreListNode>()));
                const char* s = vec_str->c_str();
                while (*s) {
                    if (*s == '"' || *s == '\\') {
                        result->concat('\\');
                    }
                    result->concat(*s);
                    ++s;
                }
            } else if (elem.getType() == NT_DATE) {
                // use type-appropriate format for date/time values so PostgreSQL can parse them
                QoreString datestr;
                elem.get<const DateTimeNode>()->format(datestr, date_fmt);
                result->concat(datestr.c_str());
            } else if (elem.getType() == NT_FLOAT) {
                // use 17 significant digits for exact IEEE 754 double round-tripping
                QoreString fstr;
                fstr.sprintf("%.17g", elem.getAsFloat());
                result->concat(fstr.c_str());
            } else if (elem.getType() == NT_BINARY) {
                // convert binary to PostgreSQL hex format: \x followed by hex bytes
                const BinaryNode* b = elem.get<const BinaryNode>();
                result->concat("\\\\x");
                const unsigned char* data = (const unsigned char*)b->getPtr();
                for (size_t i = 0; i < b->size(); ++i) {
                    result->sprintf("%02x", data[i]);
                }
            } else {
                QoreStringValueHelper str(elem);
                TempEncodingHelper tmp(*str, enc, xsink);
                if (!tmp) {
                    return nullptr;
                }
                const char* s = tmp->c_str();
                while (*s) {
                    if (*s == '"' || *s == '\\') {
                        result->concat('\\');
                    }
                    result->concat(*s);
                    ++s;
                }
            }
            result->concat('"');
        }
    }
    result->concat('}');
    return result.release();
}

int QorePgsqlStatement::add(QoreValue v_arg, ExceptionSink *xsink) {
    // The bind arguments arrive as elements of a list, and a member assigned with the weak
    // reference operator ":=" or the opaque reference operator "@=" is stored as the
    // reference itself, so reading the list yields that rather than its target.  Resolve it
    // before dispatching on the type, or the value binds as the wrong type or not at all.
    QoreValue v = v_arg.resolveIndirect();
    parambuf* pb = new parambuf();
    parambuf_list.push_back(pb);

    //printd(5, "QorePgsqlStatement::add() this: %p nparams: %d, v: %s\n", this, nParams, v.getFullTypeName());
    if (nParams == allocated) {
        if (!allocated)
            allocated = 5;
        else
            allocated <<= 1;
        paramTypes   = (Oid*)realloc(paramTypes,    sizeof(Oid) * allocated);
        paramValues  = (char**)realloc(paramValues, sizeof(char*) * allocated);
        paramLengths = (int*)realloc(paramLengths,  sizeof(int) * allocated);
        paramFormats = (int*)realloc(paramFormats,  sizeof(int) * allocated);
        paramArray   = (int*)realloc(paramArray,    sizeof(int) * allocated);
        //printd(5, "allocated: %d, nparams: %d\n", allocated, nParams);
    }

    paramArray[nParams] = 0;
    paramFormats[nParams] = 1;

    if (v.isNullOrNothing()) {
        paramTypes[nParams] = 0;
        paramValues[nParams] = 0;
        ++nParams;
        return 0;
    }

    qore_type_t ntype = v.getType();
    if (ntype == NT_INT) {
        int64 i = v.getAsBigInt();
        if (i <= 32767 && i > -32768) {
            //printd(5, "i2: %d\n", (int)b->val);
            paramTypes[nParams]   = INT2OID;
            pb->assign((short)i);
            paramValues[nParams]  = (char*)&pb->i2;
            paramLengths[nParams] = sizeof(short);
        } else if (i <= 2147483647 && i >= -2147483647) {
            //printd(5, "i4: %d (%d, %d)\n", (int)b->val, sizeof(uint32_t), sizeof(int));
            paramTypes[nParams]   = INT4OID;
            pb->assign((int)i);
            //pb->i4 = htonl((int)b->val);
            paramValues[nParams]  = (char*)&pb->i4;
            paramLengths[nParams] = sizeof(int);
        } else {
            //printd(5, "i8: %lld\n", b->val);
            paramTypes[nParams]   = INT8OID;
            pb->assign(i);
            paramValues[nParams]  = (char*)&pb->i8;
            paramLengths[nParams] = sizeof(int64);
        }
        ++nParams;
        return 0;
    }

    if (ntype == NT_FLOAT) {
        paramTypes[nParams]   = FLOAT8OID;
        pb->assign(v.getAsFloat());
        paramValues[nParams]  = (char*)&pb->f8;
        paramLengths[nParams] = sizeof(double);

        ++nParams;
        return 0;
    }

    if (ntype == NT_NUMBER) {
        paramTypes[nParams]   = NUMERICOID;
        // create output numeric buffer structure
        pb->num = new qore_pg_numeric_out(v.get<const QoreNumberNode>());
        paramValues[nParams]  = (char*)pb->num->getData();
        paramLengths[nParams] = pb->num->getSize();

        ++nParams;
        return 0;
    }

    if (ntype == NT_STRING) {
        QoreStringValueHelper str(v);
        paramTypes[nParams] = TEXTOID;
        pb->str = NULL;
        TempEncodingHelper tmp(*str, enc, xsink);
        if (!tmp)
            return -1;

        paramLengths[nParams] = tmp->strlen();
        pb->str = qpg_copy_string_buffer(*tmp);
        paramValues[nParams] = pb->str;
        paramFormats[nParams] = 0;

        ++nParams;
        return 0;
    }

    if (ntype == NT_BOOLEAN) {
        paramTypes[nParams]   = BOOLOID;
        pb->b = v.getAsBool();
        paramValues[nParams]  = (char*)&pb->b;
        paramLengths[nParams] = sizeof(bool);

        ++nParams;
        return 0;
    }

    if (ntype == NT_DATE) {
        const DateTimeNode* d = v.get<const DateTimeNode>();
        if (d->isRelative()) {
            paramTypes[nParams] = INTERVALOID;

            int day_seconds;
            if (conn->has_interval_day()) {
                pb->iv.rest.with_day.month = htonl(d->getMonth());
                pb->iv.rest.with_day.day   = htonl(d->getDay());
                day_seconds = 0;
            } else {
                pb->iv.rest.month = htonl(d->getMonth());
                day_seconds = d->getDay() * 3600 * 24;
            }

            if (conn->has_integer_datetimes()) {
                pb->iv.time.i = i8MSB((((int64)d->getYear() * 365 * 24 * 3600) + (int64)d->getHour() * 3600 + (int64)d->getMinute() * 60 + (int64)d->getSecond() + day_seconds) * 1000000 + (int64)d->getMicrosecond());
                //printd(5, "binding interval %lld\n", MSBi8(pb->iv.time.i));
            } else
                pb->iv.time.f = f8MSB((double)((d->getYear() * 365 * 24 * 3600) + d->getHour() * 3600 + d->getMinute() * 60 + d->getSecond() + day_seconds) + (double)d->getMicrosecond() / 1000000.0);

            paramValues[nParams] = (char*)&pb->iv;
            paramLengths[nParams] = conn->has_interval_day() ? 16 : 12;
        } else {
            paramTypes[nParams] = TIMESTAMPTZOID;

            if (conn->has_integer_datetimes()) {
                // get number of seconds offset from jan 1 2000 then make it microseconds and add ms
                int64 val = (d->getEpochSecondsUTC() - PGSQL_EPOCH_OFFSET) * 1000000 + d->getMicrosecond();
                //printd(5, "timestamp int64 time: %lld\n", val);
                pb->assign(val);
                paramValues[nParams] = (char*)&pb->i8;
                paramLengths[nParams] = sizeof(int64);
            } else {
                double val = (double)((double)d->getEpochSecondsUTC() - PGSQL_EPOCH_OFFSET) + (double)(d->getMicrosecond() / 1000000.0);
                //printd(5, "timestamp double time: %9g\n", val);
                pb->assign(val);
                paramValues[nParams] = (char*)&pb->f8;
                paramLengths[nParams] = sizeof(double);
            }
        }
        ++nParams;
        //printd(5, "QorePgsqlStatement::add() this: %p nParams: %d\n", this, nParams);
        return 0;
    }

    if (ntype == NT_BINARY) {
        const BinaryNode* b = v.get<const BinaryNode>();
        paramTypes[nParams] = BYTEAOID;
        paramValues[nParams] = (char*)b->getPtr();
        paramLengths[nParams] = b->size();

        ++nParams;
        return 0;
    }

    if (ntype == NT_LIST) {
        const QoreListNode* l = v.get<const QoreListNode>();
        if (l->empty()) {
            paramTypes[nParams] = 0;
            paramValues[nParams] = 0;
            ++nParams;
            // validate array size consistency
            if (array_size == -1) {
                array_size = 0;
            } else if (array_size != 0) {
                xsink->raiseException("DBI:PGSQL:ARRAY-BIND-ERROR", "%s: array bind size mismatch: "
                    "expected %d elements, but got an empty list", conn->getServerDesc(), array_size);
                return -1;
            }
            return 0;
        }
        // validate array size consistency
        int lsize = (int)l->size();
        if (array_size == -1) {
            array_size = lsize;
        } else if (array_size != lsize) {
            xsink->raiseException("DBI:PGSQL:ARRAY-BIND-ERROR", "%s: array bind size mismatch: "
                "expected %d elements, but got %d", conn->getServerDesc(), array_size, lsize);
            return -1;
        }
        std::unique_ptr<QorePGBindArray> ba(new QorePGBindArray(conn));
        if (ba->create_data(l, 0, enc, xsink)) {
            return -1;
        }
        paramArray[nParams] = 1;
        paramTypes[nParams] = ba->getArrayOid();
        paramLengths[nParams] = ba->getSize();
        pb->ptr = ba->getHeader();
        paramValues[nParams] = (char*)pb->ptr;
        paramFormats[nParams] = ba->getFormat();
        ++nParams;
        return 0;
    }

    if (ntype == NT_HASH) {
        const QoreHashNode* vh = v.get<const QoreHashNode>();
        // first see if it should be an array bind
        if (vh->existsKey("^pgarray^")) {
            QoreValue t = vh->getKeyValue("^value^");
            switch (t.getType()) {
                case NT_NOTHING:
                    paramTypes[nParams] = 0;
                    paramValues[nParams] = 0;
                    break;

                case NT_LIST: {
                    std::unique_ptr<QorePGBindArray> ba(new QorePGBindArray(conn));
                    const QoreListNode* l = t.get<const QoreListNode>();
                    if (ba->create_data(l, 0, enc, xsink))
                        return -1;

                    paramArray[nParams] = 1;
                    paramTypes[nParams] = ba->getArrayOid();
                    paramLengths[nParams] = ba->getSize();
                    pb->ptr = ba->getHeader();
                    paramValues[nParams] = (char*)pb->ptr;
                    paramFormats[nParams] = ba->getFormat();
                    //printd(5, "QorePgsqlStatement::add() array size: %d, arrayoid: %d, data: %p\n", ba->getSize(), ba->getArrayOid(), pb->ptr);
                    break;
                }

                default:
                    paramTypes[nParams] = 0;
                    paramValues[nParams] = 0;
                    xsink->raiseException("DBI:PGSQL:EXEC-EXCEPTION", "%s: expecting type 'list' for array bind; for "
                        "type '%s' instead; use pgsql_bind_array() to bind array values with this driver",
                        conn->getServerDesc(), t.getTypeName());
                    ++nParams;
                    return -1;
            }

            nParams++;
            return 0;
        }

        // check for ^pgtype^ key: accepts integer OID or string type name
        QoreValue pgtype_val = vh->getKeyValue("^pgtype^");
        if (pgtype_val.isNothing()) {
            xsink->raiseException("DBI:PGSQL:BIND-ERROR", "missing '^pgtype^' value in bind hash");
            ++nParams;
            return -1;
        }

        if (pgtype_val.getType() == NT_STRING) {
            // string type name: resolve to OID, supports array types like "bit(8)[]"
            QoreStringValueHelper type_name(pgtype_val);
            bool is_array = false;
            Oid base_oid = resolve_pg_type_name(type_name->c_str(), is_array, conn, xsink);
            if (*xsink) {
                ++nParams;
                return -1;
            }

            QoreValue val = vh->getKeyValue("^value^");

            if (is_array) {
                // typed array binding
                if (val.isNullOrNothing()) {
                    paramTypes[nParams] = 0;
                    paramValues[nParams] = 0;
	                } else if (val.getType() != NT_LIST) {
	                    xsink->raiseException("DBI:PGSQL:BIND-ERROR",
	                        "'^pgtype^' specifies array type '%s' but '^value^' is type '%s', expecting list",
	                        type_name->c_str(), val.getTypeName());
                    ++nParams;
                    return -1;
                } else {
                    // look up array OID from base OID
                    Oid array_oid = 0;
                    qore_pg_array_type_map_t::const_iterator ai = array_type_map.find(base_oid);
                    if (ai != array_type_map.end()) {
                        array_oid = ai->second;
                    } else if (conn) {
                        array_oid = conn->getExtensionArrayOid(base_oid);
                    }
	                    if (!array_oid) {
	                        xsink->raiseException("DBI:PGSQL:BIND-ERROR",
	                            "cannot find array OID for base type '%s' (OID %d)",
	                            type_name->c_str(), (int)base_oid);
                        ++nParams;
                        return -1;
                    }

                    // validate array size consistency
                    const QoreListNode* l = val.get<const QoreListNode>();
                    int lsize = (int)l->size();
                    if (array_size == -1) {
                        array_size = lsize;
                    } else if (array_size != lsize) {
                        xsink->raiseException("DBI:PGSQL:ARRAY-BIND-ERROR",
                            "%s: array bind size mismatch: expected %d elements, but got %d",
                            conn->getServerDesc(), array_size, lsize);
                        ++nParams;
                        return -1;
                    }

                    // build text array literal: {val1,val2,...}
                    QoreString* array_str = build_text_array_literal(l, base_oid, enc,
                        conn, xsink);
                    if (*xsink) {
                        ++nParams;
                        return -1;
                    }

                    paramArray[nParams] = 1;
                    paramTypes[nParams] = array_oid;
                    paramLengths[nParams] = array_str->strlen();
                    pb->str = array_str->giveBuffer();
                    delete array_str;
                    paramValues[nParams] = pb->str;
                }
                paramFormats[nParams] = 0;  // text format
                ++nParams;
                return 0;
            } else {
                // typed scalar binding
                if (val.isNullOrNothing()) {
                    paramTypes[nParams] = 0;
                    paramValues[nParams] = 0;
                } else if (val.getType() == NT_LIST && conn
                        && (base_oid == conn->getVectorOid()
                            || base_oid == conn->getHalfvecOid())) {
                    // vector/halfvec type: convert list to "[1.5,2.3,...]" text format
                    paramTypes[nParams] = base_oid;
                    QoreString* vec_str = qpg_vector_to_text(val.get<const QoreListNode>());
                    paramLengths[nParams] = vec_str->strlen();
                    pb->str = vec_str->giveBuffer();
                    delete vec_str;
                    paramValues[nParams] = pb->str;
                } else {
                    paramTypes[nParams] = base_oid;
                    QoreStringValueHelper str(val);
                    TempEncodingHelper tmp(*str, enc, xsink);
                    if (!tmp) {
                        ++nParams;
                        return -1;
                    }
                    paramLengths[nParams] = tmp->strlen();
                    pb->str = qpg_copy_string_buffer(*tmp);
                    paramValues[nParams] = pb->str;
                }
                paramFormats[nParams] = 0;  // text format
                ++nParams;
                return 0;
            }
        }

        if (pgtype_val.getType() != NT_INT) {
            xsink->raiseException("DBI:PGSQL:BIND-ERROR",
                "'^pgtype^' key contains '%s' value, expecting integer or string",
                pgtype_val.getTypeName());
            ++nParams;
            return -1;
        }

        // existing integer OID path
        Oid type = (Oid)pgtype_val.getAsBigInt();
        QoreValue t = vh->getKeyValue("^value^");
        if (t.isNullOrNothing()) {
            paramTypes[nParams] = 0;
            paramValues[nParams] = 0;
        } else {
            paramTypes[nParams] = type;

            QoreStringValueHelper str(t);
            paramLengths[nParams] = str->strlen();
            pb->str = qpg_copy_string_buffer(*str);
            paramValues[nParams] = pb->str;
        }
        paramFormats[nParams] = 0;

        ++nParams;
        return 0;
    }

    paramTypes[nParams] = 0;
    paramValues[nParams] = 0;
    xsink->raiseException("DBI:PGSQL:EXEC-EXCEPTION", "%s: don't know how to bind type '%s'", conn->getServerDesc(),
        v.getTypeName());

    nParams++;
    return -1;
}

QorePGBindArray::QorePGBindArray(QorePGConnection* r_conn) : ndim(0), size(0), allocated(0), elements(0),
        ptr(NULL), hdr(NULL), type(-1), oid(0), arrayoid(0), format(1), conn(r_conn) {
}

QorePGBindArray::~QorePGBindArray() {
    if (hdr)
        free(hdr);
}

int QorePGBindArray::getOid() const {
    return oid;
}

int QorePGBindArray::getArrayOid() const {
    return arrayoid;
}

int QorePGBindArray::getSize() const {
    return size;
}

qore_pg_array_header *QorePGBindArray::getHeader() {
    qore_pg_array_header *rv = hdr;
    hdr = NULL;
    return rv;
}

int QorePGBindArray::check_type(QoreValue n, ExceptionSink *xsink) {
    // skip null types - NULLs are supported in arrays on modern PostgreSQL
    if (n.isNullOrNothing()) {
        return 0;
    }
    qore_type_t t = n.getType();
    if (type == -1) {
        type = t;
        // check that type is supported
        if (type == NT_INT) {
            arrayoid = QPGT_INT8ARRAYOID;
            oid = INT8OID;
            return 0;
        }

        if (type == NT_FLOAT) {
            arrayoid = QPGT_FLOAT8ARRAYOID;
            oid = FLOAT8OID;
            return 0;
        }

        if (type == NT_BOOLEAN) {
            arrayoid = QPGT_BOOLARRAYOID;
            oid = BOOLOID;
            return 0;
        }

        if (type == NT_STRING) {
            arrayoid = QPGT_TEXTARRAYOID;
            oid = TEXTOID;
            //format = 0;
            return 0;
        }

        if (type == NT_DATE) {
            const DateTimeNode* date = n.get<const DateTimeNode>();
            if (date->isRelative()) {
                arrayoid = QPGT_INTERVALARRAYOID;
                oid = INTERVALOID;
            }
            else {
                arrayoid = QPGT_TIMESTAMPTZARRAYOID;
                oid = TIMESTAMPTZOID;
            }
            return 0;
        }

        if (type == NT_NUMBER) {
            arrayoid = QPGT_NUMERICARRAYOID;
            oid = NUMERICOID;
            return 0;
        }

        if (type == NT_BINARY) {
            arrayoid = QPGT_BYTEAARRAYOID;
            oid = BYTEAOID;
            return 0;
        }

        /*
        if (type == NT_HASH) {
            format = 0;
            oid = check_hash_type(n->valx.hash, xsink);
            if (oid < 0)
                return -1;
            qore_pg_array_type_map_t::const_iterator i = QorePgsqlStatement::array_type_map.find(oid);
            if (i == QorePgsqlStatement::array_type_map.end()) {
                xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "don't know how to bind arrays of typeid %d", oid);
                return -1;
            }
            arrayoid = i->second;
            return 0;
        }
        */

        xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "don't know how to bind arrays of type '%s'", n.getTypeName());
        return -1;
    }

    if (t != type) {
        xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "array elements must be all of the same type for binding");
        return -1;
    }

    if (type == NT_DATE) {
        const DateTimeNode* date = n.get<const DateTimeNode>();
        if (date) {
            if (date->isRelative() && (oid == TIMESTAMPOID || oid == TIMESTAMPTZOID)) {
                xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "array type was set to TIMESTAMP or TIMESTAMPTZ, but a relative date/time is present in the list");
                return -1;
            }
            if (date->isAbsolute() && (oid == INTERVALOID)) {
                xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "array type was set to INTERVAL, but an absolute date/time is present in the list");
                return -1;
            }
        }
    }
    return 0;
}

int QorePGBindArray::check_oid(const QoreHashNode* h, ExceptionSink *xsink) {
    int o = check_hash_type(h, xsink);
    if (o < 0)
        return -1;

    if (!oid)
        oid = o;
    else if (o != oid) {
        xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "array elements must be all of the same type for binding");
        return -1;
    }
    return 0;
}

int QorePGBindArray::new_dimension(const QoreListNode* l, int current, ExceptionSink *xsink) {
    if (current >= MAXDIM) {
        xsink->raiseException("DBI:PGSQL:ARRAY-ERROR", "array exceeds maximum number of dimensions (%d)", MAXDIM);
        return -1;
    }
    ndim++;
    int len = l->size();
    if (!elements)
        elements = len;
    else
        elements *= len;
    dim[current] = len;
    return 0;
}

int QorePGBindArray::create_data(const QoreListNode* l, int current, const QoreEncoding* enc, ExceptionSink *xsink) {
    if (new_dimension(l, current, xsink))
        return -1;
    return process_list(l, current, enc, xsink);
}

void QorePGBindArray::check_size(int len) {
    int space = (len == -1 ? 4 : len + 4);
    if (allocated < (size + space)) {
        int offset = 12 + 8 * ndim;
        bool init = !allocated;
        if (init)
            size = offset;
        allocated = size + space + (allocated / 3);
        hdr = (qore_pg_array_header *)realloc(hdr, allocated);
        // setup header
        //printd(5, "check_size(len: %d) ndim: %d, allocated: %d, offset: %d hdr: %p\n", len, ndim, allocated, 12 + 8 * ndim, hdr);
        if (init) {
            hdr->ndim  = htonl(ndim);
            hdr->flags = 0; // htonl(0);
            hdr->oid   = htonl(oid);
            for (int i = 0; i < ndim; i++) {
                //printd(5, "this: %p initializing header addr: %p (offset: %d) ndim: %d, oid: %d, dim[%d]: %d, lBound[%d]=1\n",
                //       this, &hdr->info[i].dim, (char*)&hdr->info[i].dim - (char*)hdr, ndim, oid, i, dim[i], i);
                hdr->info[i].dim = htonl(dim[i]);
                hdr->info[i].lBound = htonl(1);
            }
        }
        ptr = (char*)hdr + size;
    }
    //printd(5, "check_size(len: %d) space: %d ndim: %d, allocated: %d, size: %d (%d) offset: %d hdr: %p ptr: %p\n", len, space, ndim, allocated, size, ptr-(char*)hdr, 12 + 8 * ndim, hdr, ptr);
    int* length = (int*)ptr;
    *length = htonl(len);
    ptr += 4;
    size += space;
}

int QorePGBindArray::bind(QoreValue n, const QoreEncoding* enc, ExceptionSink* xsink) {
    // bind a NULL for NOTHING or NULL
    if (n.isNullOrNothing()) {
        check_size(-1);
        return 0;
    }

    if (type == NT_INT) {
        check_size(8);
        int64* i8 = (int64*)ptr;
        *i8 = i8MSB(n.getAsBigInt());
        ptr += 8;
        return 0;
    }

    if (type == NT_FLOAT) {
        check_size(8);
        double *f8 = (double *)ptr;
        *f8 = f8MSB(n.getAsFloat());
        ptr += 8;
        return 0;
    }

    if (type == NT_BOOLEAN) {
        check_size(sizeof(bool));
        bool *b = (bool *)ptr;
        *b = n.getAsBool();
        ptr += sizeof(bool);
        return 0;
    }

    if (type == NT_NUMBER) {
        qore_pg_numeric_out num(n.get<const QoreNumberNode>());
        int len = num.getSize();
        check_size(len);
        memcpy(ptr, num.getData(), len);
        ptr += len;
        return 0;
    }

    if (type == NT_STRING) {
        QoreStringValueHelper str(n);
        TempEncodingHelper tmp(*str, enc, xsink);
        if (!tmp)
            return -1;

        int len = tmp->strlen();
        check_size(len);
        memcpy(ptr, tmp->c_str(), len);
        ptr += len;
        return 0;
    }

    if (type == NT_DATE) {
        const DateTimeNode* d = n.get<const DateTimeNode>();
        if (d->isRelative()) {
            int d_size = conn->has_interval_day() ? 16 : 12;
            check_size(d_size);
            qore_pg_interval *i = (qore_pg_interval *)ptr;

            if (conn->has_interval_day()) {
                i->rest.with_day.month = htonl(d->getMonth());
                i->rest.with_day.day = htonl(d->getDay());
            }
            else
                i->rest.month = htonl(d->getMonth());

            if (conn->has_integer_datetimes()) {
                i->time.i = i8MSB(((d->getYear() * 365 * 24 * 3600) + d->getHour() * 3600 + d->getMinute() * 60
                    + d->getSecond()) * 1000000 + d->getMicrosecond());
            } else {
                i->time.f = f8MSB((double)((d->getYear() * 365 * 24 * 3600) + d->getHour() * 3600
                    + d->getMinute() * 60 + d->getSecond()) + (double)d->getMicrosecond() / 1000000.0);
            }

            ptr += d_size;
        } else {
            check_size(8);

            if (conn->has_integer_datetimes()) {
                int64 *i = (int64 *)ptr;
                // get number of seconds offset from jan 1 2000 then make it microseconds and add ms
                *i = i8MSB((d->getEpochSecondsUTC() - PGSQL_EPOCH_OFFSET) * 1000000 + d->getMicrosecond());
            } else {
                double *f = (double *)ptr;
                *f = f8MSB((double)((double)d->getEpochSecondsUTC() - PGSQL_EPOCH_OFFSET)
                    + (double)(d->getMicrosecond() / 1000000.0));
            }
            ptr += 8;
        }
        return 0;
    }

    if (type == NT_BINARY) {
        const BinaryNode* b = n.get<const BinaryNode>();
        size_t len = b->size();
        check_size(len);
        memcpy(ptr, b->getPtr(), len);
        ptr += len;
        return 0;
    }

    if (type == NT_HASH) {
        const QoreHashNode* h = n.get<const QoreHashNode>();
        QoreValue t = h->getKeyValue("^value^");
        if (t.isNullOrNothing())
            check_size(-1);
        else {
            QoreStringValueHelper tmp(t, enc, xsink);
            if (*xsink)
                return -1;
            int len = tmp->strlen();
            check_size(len);
            memcpy(ptr, tmp->c_str(), len);
            ptr += len;
        }
        return 0;
    }
    return 0;
}

int QorePGBindArray::process_list(const QoreListNode* l, int current, const QoreEncoding* enc, ExceptionSink *xsink) {
    ConstListIterator li(l);
    while (li.next()) {
        qore_type_t ntype;
        // see QorePgsqlStatement::add(): an array element may be stored as a weak or opaque
        // reference, and the iterator yields what the list holds
        QoreValue n = li.getValue().resolveIndirect();
        ntype = n.getType();
        if (type == NT_LIST) {
            const QoreListNode* l = n.get<const QoreListNode>();
            if (li.first())
                if (new_dimension(l, current + 1, xsink))
                    return -1;
            if (process_list(l, current + 1, enc, xsink))
                return -1;
        } else {
            if (check_type(n, xsink))
                return -1;
            if (ntype == NT_HASH && check_oid(n.get<const QoreHashNode>(), xsink))
                return -1;

            if (bind(n, enc, xsink))
                return -1;
        }
    }
    if (!oid) {
        // all-NULL array: default to TEXT type so the array can still be bound
        oid = TEXTOID;
        arrayoid = QPGT_TEXTARRAYOID;
        // update the header OID since it was written as 0 during initial allocation
        if (hdr) {
            hdr->oid = htonl(oid);
        }
    }
    return 0;
}

#define QPDC_LINE 1
#define QPDC_BLOCK 2

int QorePgsqlStatement::parse(QoreString* str, const QoreListNode* args, ExceptionSink* xsink) {
    char quote = 0;
    const char* p = str->c_str();
    QoreString tmp;
    int index = 0;
    int comment = 0;

    while (*p) {
        if (!quote) {
            if (!comment) {
                if ((*p) == '-' && (*(p+1)) == '-') {
                    comment = QPDC_LINE;
                    p += 2;
                    continue;
                }

                if ((*p) == '/' && (*(p+1)) == '*') {
                    comment = QPDC_BLOCK;
                    p += 2;
                    continue;
                }
            } else {
                if (comment == QPDC_LINE) {
                    if ((*p) == '\n' || ((*p) == '\r'))
                        comment = 0;
                    ++p;
                    continue;
                }

                assert(comment == QPDC_BLOCK);
                if ((*p) == '*' && (*(p+1)) == '/') {
                    comment = 0;
                    p += 2;
                    continue;
                }

                ++p;
                continue;
            }

            if ((*p) == '%' && (p == str->c_str() || !isalnum(*(p-1)))) { // found value marker
                int offset = p - str->c_str();

                p++;
                QoreValue v = args ? args->retrieveEntry(index++) : QoreValue();
                if ((*p) == 'd') {
                    DBI_concat_numeric(&tmp, v);
                    str->replace(offset, 2, tmp.c_str());
                    p = str->c_str() + offset + tmp.strlen();
                    tmp.clear();
                    continue;
                }
                if ((*p) == 's') {
                    if (DBI_concat_string(&tmp, v, xsink))
                        return -1;
                    str->replace(offset, 2, tmp.c_str());
                    p = str->c_str() + offset + tmp.strlen();
                    tmp.clear();
                    continue;
                }
                if ((*p) != 'v') {
                    xsink->raiseException("DBI-EXEC-PARSE-EXCEPTION", "invalid value specification (expecting '%%v' or '%%d', got %%%c)", *p);
                    return -1;
                }
                p++;
                if (isalpha(*p)) {
                    xsink->raiseException("DBI-EXEC-PARSE-EXCEPTION", "invalid value specification (expecting '%%v' or '%%d', got %%v%c*)", *p);
                    return -1;
                }

                // replace value marker with "$<num>"
                // find byte offset in case string buffer is reallocated with replace()
                tmp.sprintf("$%d", nParams + 1);
                str->replace(offset, 2, tmp.c_str());
                p = str->c_str() + offset + tmp.strlen();
                tmp.clear();
                if (add(v, xsink))
                    return -1;
                continue;
            }

            // allow escaping of '%' characters
            if ((*p) == '\\' && (*(p+1) == ':' || *(p+1) == '%')) {
                str->splice(p - str->c_str(), 1, xsink);
                p += 2;
                continue;
            }
        }

        if (((*p) == '\'') || ((*p) == '\"')) {
            if (!quote)
                quote = *p;
            else if (quote == (*p))
                quote = 0;
            p++;
            continue;
        }

        p++;
    }
    return 0;
}

// hackish way to determine if a pre release 8 server is using int8 or float8 types for datetime values
bool QorePgsqlStatement::checkIntegerDateTimes(ExceptionSink *xsink) {
    // Check for interrupt before query execution
    if (qore_check_cancel(xsink)) {
        return false;
    }

    PGresult* tres = PQexecParams(conn->get(), "select '00:00'::time as \"a\"", 0, NULL, NULL, NULL, NULL, 1);
    if (!tres) {
        xsink->raiseException("DBI:PGSQL:ERROR", "Error determining binary date/time format: PQexecParams() returned "
            "NULL");
        return false;
    }
    // make sure and delete the result when we exit
    ON_BLOCK_EXIT(PQclear, tres);

    ExecStatusType rc = PQresultStatus(tres);
    if (rc != PGRES_COMMAND_OK && rc != PGRES_TUPLES_OK) {
        const char* err = PQerrorMessage(conn->get());
        const char* e;
        if (!strncmp(err, "ERROR:  ", 8) || !strncmp(err, "FATAL:  ", 8))
            e = err + 8;
        else
            e = err;
        QoreString desc(e);
        desc.chomp();
        xsink->raiseException("DBI:PGSQL:ERROR", "Error determining binary date/time format: %s", desc.c_str());
        return false;
    }

    // ensure that the result format is what we expect
    if (PQnfields(tres) != 1) {
        xsink->raiseException("DBI:PGSQL:ERROR", "Error determining binary date/time format; expecting 1 column in "
            "test query, got %d", PQnfields(tres));
        return false;
    }
    if (PQntuples(tres) != 1) {
        xsink->raiseException("DBI:PGSQL:ERROR", "Error determining binary date/time format; expecting 1 row in test "
            "query, got %d", PQntuples(tres));
        return false;
    }

    void* data = PQgetvalue(tres, 0, 0);
    int64 val = MSBi8(*((uint64_t*)data));

    return val == 0;
}

int QorePgsqlStatement::execIntern(const char* sql, ExceptionSink* xsink) {
    assert(!res);
    //printd(5, "QorePgsqlStatement::execIntern() this: %p sql: %s nParams: %d\n", this, sql, nParams);

    // Check for interrupt before query execution
    if (qore_check_cancel(xsink)) {
        return -1;
    }

    // Use cancel helper to enable query cancellation during blocking call
    {
        QorePGCancelHelper cancel_helper(conn->get());
        res = PQexecParams(conn->get(), sql, nParams, paramTypes, paramValues, paramLengths, paramFormats, 1);
    }
    ExecStatusType rc = PQresultStatus(res);
    //printd(5, "QorePgsqlStatement::execIntern() rc: %d\n", rc);
    if (rc == PGRES_COMMAND_OK || rc == PGRES_TUPLES_OK) {
        return 0;
    }

    bool lost_connection = false;
    // check if we have disconnected from the server
    if (rc == PGRES_FATAL_ERROR) {
        ConnStatusType cs = PQstatus(conn->get());
        //printd(5, "QorePgsqlStatement::execIntern() this: %p status: %d (OK: %d, BAD: %d)\n", this, cs,
        //    CONNECTION_OK, CONNECTION_BAD);
        // try to reestablish the connection
        if (cs == CONNECTION_BAD) {
            lost_connection = true;
            // first check if a transaction was in progress
            bool in_trans = conn->wasInTransaction();
            if (in_trans) {
                QorePGConnection::doLostConnectionError(true, res, xsink);
            }

            printd(5, "QorePgsqlStatement::execIntern() this: %p connection to server lost (transaction status: %d); "
                "trying to reconnect; current sql: %s\n", this, in_trans, sql);

            // Check for interrupt before reconnection attempt
            if (qore_check_cancel(xsink)) {
                return -1;
            }

            PQreset(conn->get());

            // only execute again if the connection was not aborted while in a transaction
            if (!in_trans) {
                // Check for interrupt before re-executing query
                if (qore_check_cancel(xsink)) {
                    PQclear(res);
                    res = nullptr;
                    return -1;
                }
                PQclear(res);
                // Use cancel helper to enable query cancellation during blocking call
                {
                    QorePGCancelHelper cancel_helper(conn->get());
                    res = PQexecParams(conn->get(), sql, nParams, paramTypes, paramValues, paramLengths, paramFormats, 1);
                }
            }
        }
    }

    return conn->checkClearResult(lost_connection, res, xsink);
}

int QorePgsqlStatement::exec(const QoreString* str, const QoreListNode* args, ExceptionSink *xsink) {
    // convert string to required character encoding or copy
    std::unique_ptr<QoreString> qstr(str->convertEncoding(enc, xsink));
    if (!qstr.get())
        return -1;

    if (parse(qstr.get(), args, xsink))
        return -1;

    printd(5, "QorePgsqlStatement::exec() nParams: %d args: %p (len: %d) sql: %s\n", nParams, args,
        args ? args->size() : 0, qstr->c_str());

    return execIntern(qstr->c_str(), xsink);
}

int QorePgsqlStatement::exec(const char* cmd, ExceptionSink *xsink) {
    assert(!nParams && !paramTypes && !paramValues && !paramLengths && !paramFormats);
    return execIntern(cmd, xsink);
}

// filter out notices that are not errors
static void custom_notice_processor(void* ptr, const char* message) {
    QorePGConnection* pc = (QorePGConnection*)ptr;
    // Only print errors, not warnings
    if (strstr(message, "ERROR:") != nullptr) {
        fprintf(stderr, "%s: %s", pc->getServerDesc(), message);
    }
}

// returns true if the libpq client library in use accepts the tcp_user_timeout connection parameter
static bool pgsql_libpq_has_tcp_user_timeout() {
#ifdef HAVE_PQLIBVERSION
    return PQlibVersion() >= PGSQL_TCP_USER_TIMEOUT_MIN_LIBPQ;
#else
    // PQlibVersion() was added in PostgreSQL 9.1, so this library predates tcp_user_timeout
    return false;
#endif
}

QorePGConnection::QorePGConnection(Datasource* d, const char* str, ExceptionSink *xsink)
        : ds(d), pc(nullptr), server_tz(currentTZ()),
            server_desc("%s:", d->getDriverName()),
            interval_has_day(false),
            integer_datetimes(false),
            numeric_support(OPT_NUM_DEFAULT) {
    // resolve libpq connection options from the datasource configuration (falling back to defaults);
    // getOptionHash() returns the raw configured options and does not raise for unset options while
    // the datasource is being opened (Datasource::getOption() would)
    {
        ReferenceHolder<QoreHashNode> opths(d->getOptionHash(), xsink);
        if (opths) {
            QoreValue v = opths->getKeyValue(PGSQL_OPT_KEEPALIVES);
            if (!v.isNothing())
                opt_keepalives = v.getAsBool();
            v = opths->getKeyValue(PGSQL_OPT_KEEPALIVES_IDLE);
            if (!v.isNothing())
                opt_keepalives_idle = static_cast<int>(v.getAsBigInt());
            v = opths->getKeyValue(PGSQL_OPT_KEEPALIVES_INTERVAL);
            if (!v.isNothing())
                opt_keepalives_interval = static_cast<int>(v.getAsBigInt());
            v = opths->getKeyValue(PGSQL_OPT_KEEPALIVES_COUNT);
            if (!v.isNothing())
                opt_keepalives_count = static_cast<int>(v.getAsBigInt());
            v = opths->getKeyValue(PGSQL_OPT_CONNECT_TIMEOUT);
            if (!v.isNothing())
                opt_connect_timeout = static_cast<int>(v.getAsBigInt());
            v = opths->getKeyValue(PGSQL_OPT_TCP_USER_TIMEOUT);
            if (!v.isNothing()) {
                if (parseTcpUserTimeoutOption(v, opt_tcp_user_timeout, xsink)) {
                    return;
                }
                opt_tcp_user_timeout_set = true;
            }
            v = opths->getKeyValue(PGSQL_OPT_APPLICATION_NAME);
            if (!v.isNothing()) {
                QoreStringValueHelper str(v);
                opt_application_name = str->c_str();
            }
        }
    }

    // apply the connection options to the conninfo; enabling TCP keepalives with an aggressive idle
    // time lets PostgreSQL promptly detect and reap backends orphaned by an unclean client exit
    // (without this, idle orphans persist until the OS keepalive default, often 2 hours, which can
    // exhaust max_connections)
    QoreString conninfo(str);
    conninfo.sprintf(" keepalives=%d", opt_keepalives ? 1 : 0);
    if (opt_keepalives) {
        if (opt_keepalives_idle > 0)
            conninfo.sprintf(" keepalives_idle=%d", opt_keepalives_idle);
        if (opt_keepalives_interval > 0)
            conninfo.sprintf(" keepalives_interval=%d", opt_keepalives_interval);
        if (opt_keepalives_count > 0)
            conninfo.sprintf(" keepalives_count=%d", opt_keepalives_count);
    }
    if (opt_connect_timeout > 0)
        conninfo.sprintf(" connect_timeout=%d", opt_connect_timeout);
    // bound the time transmitted data may remain unacknowledged (TCP_USER_TIMEOUT); keepalives only
    // probe idle connections, so without this a query in flight when the server's host disappears
    // silently (no RST) blocks until the kernel's retransmission limit (~15 minutes on Linux).
    // libpq accepts tcp_user_timeout only from PostgreSQL 12 and fails the connection with "invalid
    // connection option" otherwise, so the default is omitted with an older client library, while an
    // explicitly-set value is always passed so that libpq reports the problem
    if (opt_tcp_user_timeout > 0 && (opt_tcp_user_timeout_set || pgsql_libpq_has_tcp_user_timeout())) {
        conninfo.sprintf(" tcp_user_timeout=%d", opt_tcp_user_timeout);
    }
    // report an application_name to the server (visible in pg_stat_activity.application_name) so the
    // owning client/pool of each backend can be identified; libpq conninfo quoting requires the value
    // to be single-quoted with any embedded backslash or single-quote backslash-escaped
    if (!opt_application_name.empty()) {
        QoreString appname_esc;
        for (const char* p = opt_application_name.c_str(); *p; ++p) {
            if (*p == '\\' || *p == '\'')
                appname_esc.concat('\\');
            appname_esc.concat(*p);
        }
        conninfo.sprintf(" application_name='%s'", appname_esc.c_str());
    }

    pc = pgsql_connect_with_interrupt_check(conninfo.c_str(), xsink);

    // Check if connection was interrupted or failed
    if (!pc || PQstatus(pc) != CONNECTION_OK) {
        if (!*xsink) {
            doError(nullptr, xsink);
        }
        return;
    }

    {
        const char* tstr = d->getUsername();
        if (tstr && *tstr) {
            server_desc.concat(tstr);
        }
        tstr = d->getDBName();
        if (tstr && *tstr) {
            server_desc.sprintf("@%s", tstr);
        }
        tstr = d->getHostName();
        if (tstr && *tstr) {
            server_desc.sprintf("/%s", tstr);
        }
        int port = d->getPort();
        if (port > 0) {
            server_desc.sprintf(":%d", port);
        }
    }

    const char* pstr;
    // get server version to encode/decode binary values properly
    int server_version = PQserverVersion(pc);
    //printd(5, "version: %d\n", server_version);
    interval_has_day = server_version >= 80100 ? true : false;
    pstr = PQparameterStatus(pc, "integer_datetimes");

    if (!pstr || !pstr[0]) {
        // encoding does not matter here; we are only getting an integer
        QorePgsqlStatement res(this, QCS_DEFAULT);
        integer_datetimes = res.checkIntegerDateTimes(xsink);
    } else {
        integer_datetimes = strcmp(pstr, "off");
    }

    if (PQsetClientEncoding(pc, ds->getDBEncoding())) {
        xsink->raiseException("DBI:PGSQL:ENCODING-ERROR", "%s: invalid PostgreSQL encoding '%s'", server_desc.c_str(),
            ds->getDBEncoding());
    }

    PQsetNoticeProcessor(pc, custom_notice_processor, this);

    // Discover extension types (pgvector, etc.) - non-fatal if it fails
    if (!*xsink) {
        discoverExtensionTypes(xsink);
        if (*xsink) {
            xsink->clear();
        }
    }
}

void QorePGConnection::discoverExtensionTypes(ExceptionSink* xsink) {
    const char* sql = "SELECT typname, oid, typarray FROM pg_type "
                      "WHERE typname IN ('vector', 'halfvec', 'sparsevec')";

    PGresult* res = PQexecParams(pc, sql, 0, nullptr, nullptr, nullptr, nullptr, 0);
    if (!res) {
        return;
    }

    ExecStatusType rc = PQresultStatus(res);
    if (rc != PGRES_TUPLES_OK) {
        PQclear(res);
        return;
    }

    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        const char* typname = PQgetvalue(res, i, 0);
        Oid oid = (Oid)atoi(PQgetvalue(res, i, 1));
        Oid typarray = (Oid)atoi(PQgetvalue(res, i, 2));

        if (!strcmp(typname, "vector")) {
            vector_oid = oid;
            vector_array_oid = typarray;
        } else if (!strcmp(typname, "halfvec")) {
            halfvec_oid = oid;
            halfvec_array_oid = typarray;
        } else if (!strcmp(typname, "sparsevec")) {
            sparsevec_oid = oid;
            sparsevec_array_oid = typarray;
        }
    }
    PQclear(res);
}

Oid QorePGConnection::resolveExtensionTypeName(const char* type_name) const {
    if (!strcmp(type_name, "vector")) {
        return vector_oid;
    }
    if (!strcmp(type_name, "halfvec")) {
        return halfvec_oid;
    }
    if (!strcmp(type_name, "sparsevec")) {
        return sparsevec_oid;
    }
    return 0;
}

Oid QorePGConnection::getExtensionArrayOid(Oid scalar_oid) const {
    if (scalar_oid && scalar_oid == vector_oid) {
        return vector_array_oid;
    }
    if (scalar_oid && scalar_oid == halfvec_oid) {
        return halfvec_array_oid;
    }
    if (scalar_oid && scalar_oid == sparsevec_oid) {
        return sparsevec_array_oid;
    }
    return 0;
}

qore_pg_data_func_t QorePGConnection::getExtensionDataFunc(Oid oid) const {
    if (oid && oid == vector_oid) {
        return qpg_data_vector;
    }
    if (oid && oid == halfvec_oid) {
        return qpg_data_halfvec;
    }
    if (oid && oid == sparsevec_oid) {
        return qpg_data_sparsevec;
    }
    return nullptr;
}

bool QorePGConnection::getExtensionArrayDataFunc(Oid oid, int& element_oid,
        qore_pg_data_func_t& func) const {
    if (oid && oid == vector_array_oid) {
        element_oid = vector_oid;
        func = qpg_data_vector;
        return true;
    }
    if (oid && oid == halfvec_array_oid) {
        element_oid = halfvec_oid;
        func = qpg_data_halfvec;
        return true;
    }
    if (oid && oid == sparsevec_array_oid) {
        element_oid = sparsevec_oid;
        func = qpg_data_sparsevec;
        return true;
    }
    return false;
}

QorePGConnection::~QorePGConnection() {
#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
    if (bulk_copy) {
        ExceptionSink xsink;
        bulkLoadEnd(false, &xsink);
    }
#endif
    if (pc)
        PQfinish(pc);
}

int QorePGConnection::commit(ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    return res.exec("commit", xsink);
}

int QorePGConnection::rollback(ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    return res.exec("rollback", xsink);
}

int QorePGConnection::begin_transaction(ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    return res.exec("begin", xsink);
}

QoreListNode* QorePGConnection::selectRows(const QoreString* qstr, const QoreListNode* args, ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink))
        return NULL;

    return res.getOutputList(xsink);
}

#ifdef QDBI_METHOD_SELECT_TYPED
QoreValue QorePGConnection::selectRowsTyped(const QoreString* qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink)) {
        return QoreValue();
    }

    ReferenceHolder<QoreListNode> rows(res.getOutputList(xsink), xsink);
    if (*xsink || !rows) {
        return QoreValue();
    }

    ReferenceHolder<QoreHashNode> desc(res.describe(xsink), xsink);
    if (*xsink) {
        return QoreValue();
    }

    QoreListNode* rv = qore_dbi_make_typed_select_rows_result(ds, *rows, *desc, xsink);
    return rv ? QoreValue(rv) : QoreValue();
}
#endif

QoreHashNode* QorePGConnection::selectRow(const QoreString* qstr, const QoreListNode* args, ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink))
        return NULL;

    return res.getSingleRow(xsink);
}

QoreValue QorePGConnection::select(const QoreString* qstr, const QoreListNode* args, ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink))
        return QoreValue();

    if (res.hasResultData())
        return res.getOutputHash(xsink, true);

    return res.rowsAffected();
}

#ifdef QDBI_METHOD_SELECT_TYPED
QoreValue QorePGConnection::selectTyped(const QoreString* qstr, const QoreListNode* args, ExceptionSink* xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink)) {
        return QoreValue();
    }

    if (!res.hasResultData()) {
        return res.rowsAffected();
    }

    ReferenceHolder<QoreHashNode> columns(res.getOutputHash(xsink, true), xsink);
    if (*xsink || !columns) {
        return QoreValue();
    }

    ReferenceHolder<QoreHashNode> desc(res.describe(xsink), xsink);
    if (*xsink) {
        return QoreValue();
    }

    QoreHashNode* rv = qore_dbi_make_typed_select_result(ds, *columns, *desc, xsink);
    return rv ? QoreValue(rv) : QoreValue();
}
#endif

#ifdef QDBI_METHOD_SELECT_COLUMNAR
QoreColumnarResult* QorePGConnection::selectColumnar(const QoreString* qstr, const QoreListNode* args,
        ExceptionSink* xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink)) {
        return nullptr;
    }

    if (!res.hasResultData()) {
        xsink->raiseException("COLUMNAR-RESULT-ERROR",
            "Datasource::selectColumnar() requires an SQL statement returning result columns");
        return nullptr;
    }

    return res.getOutputColumnar(xsink, true);
}
#endif

// static
bool QorePGConnection::isCopyFromStdin(const QoreString* qstr) {
    const char* p = qstr->c_str();
    // skip leading whitespace
    while (*p && isspace(*p)) {
        ++p;
    }
    // check for "COPY" (case-insensitive)
    if (strncasecmp(p, "copy", 4) != 0) {
        return false;
    }
    p += 4;
    if (!isspace(*p)) {
        return false;
    }
    // search for "FROM" and "STDIN" (case-insensitive)
    bool found_from = false;
    bool found_stdin = false;
    while (*p) {
        if (isspace(*p)) {
            ++p;
            continue;
        }
        if (!found_from && strncasecmp(p, "from", 4) == 0 && (isspace(p[4]) || p[4] == '\0')) {
            found_from = true;
            p += 4;
            continue;
        }
        if (found_from && strncasecmp(p, "stdin", 5) == 0 && (isspace(p[5]) || p[5] == '\0' || p[5] == ';')) {
            found_stdin = true;
            break;
        }
        // skip the current token
        while (*p && !isspace(*p)) {
            ++p;
        }
    }
    return found_from && found_stdin;
}

// Helper: escape a string value for COPY text format
static int appendCopyEscapedString(QoreString& buf, const char* p, size_t len, ExceptionSink* xsink) {
    for (size_t i = 0; i < len; ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "pgsql COPY string escaping")) {
            return -1;
        }
        switch (p[i]) {
            case '\\':
                buf.concat("\\\\");
                break;
            case '\t':
                buf.concat("\\t");
                break;
            case '\n':
                buf.concat("\\n");
                break;
            case '\r':
                buf.concat("\\r");
                break;
            default:
                buf.concat(p[i]);
                break;
        }
    }
    return 0;
}

// Helper: format a binary value as hex for COPY text format
static int appendCopyBinaryHex(QoreString& buf, const BinaryNode* b, ExceptionSink* xsink) {
    buf.concat("\\\\x");
    const unsigned char* p = (const unsigned char*)b->getPtr();
    for (size_t i = 0; i < b->size(); ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "pgsql COPY binary encoding")) {
            return -1;
        }
        buf.sprintf("%02x", p[i]);
    }
    return 0;
}

//! appends one Qore value in PostgreSQL COPY text format
/** @return 0 for success, -1 when conversion failed and an exception is present in \a xsink
*/
static int appendCopyValue(QoreString& buf, QoreValue value, const QoreEncoding* enc, ExceptionSink* xsink) {
    if (value.isNullOrNothing()) {
        buf.concat("\\N");
        return 0;
    }

    switch (value.getType()) {
        case NT_INT:
            buf.sprintf("%lld", value.getAsBigInt());
            break;

        case NT_FLOAT:
            buf.sprintf("%.17g", value.getAsFloat());
            break;

        case NT_NUMBER: {
            QoreString tmp;
            value.get<const QoreNumberNode>()->getStringRepresentation(tmp);
            buf.concat(tmp.c_str());
            break;
        }

        case NT_BOOLEAN:
            buf.concat(value.getAsBool() ? "t" : "f");
            break;

        case NT_STRING: {
            QoreStringValueHelper str(value);
            TempEncodingHelper tmp(*str, enc, xsink);
            if (!tmp) {
                return -1;
            }
            if (appendCopyEscapedString(buf, tmp->c_str(), tmp->strlen(), xsink)) {
                return -1;
            }
            break;
        }

        case NT_DATE: {
            const DateTimeNode* d = value.get<const DateTimeNode>();
            QoreString tmp;
            if (d->isRelative()) {
                d->getStringRepresentation(tmp);
            } else {
                // Qore's "IF" ISO format is accepted by PostgreSQL COPY
                d->format(tmp, "IF");
            }
            buf.concat(tmp.c_str());
            break;
        }

        case NT_BINARY:
            if (appendCopyBinaryHex(buf, value.get<const BinaryNode>(), xsink)) {
                return -1;
            }
            break;

        default: {
            QoreStringValueHelper str(value, enc, xsink);
            if (*xsink) {
                return -1;
            }
            if (appendCopyEscapedString(buf, str->c_str(), str->strlen(), xsink)) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
//! report COPY progress on byte boundaries rather than per row
/** so that the reporting cost does not scale with the row count when rows are narrow
*/
static const int64 copy_stream_report_bytes = 65536;

//! reports a bounded \c COPY stream to the datasource mutation observer
/** The \c COPY payload is serialized by the caller and only byte counts are retained for reporting;
    the payload is never buffered merely to measure it.  The legacy SQL path sends one row at a time,
    while the driver-neutral native path sends one BulkSqlUtil block at a time.

    Normal paths report the stream end boundary explicitly so observer errors can propagate to the
    caller.  The destructor is a final backstop, and the started flag guarantees exactly one terminal
    boundary on every path.

    @since pgsql 3.4
*/
class QorePGCopyStreamHelper {
public:
    DLLLOCAL QorePGCopyStreamHelper(Datasource* ds, bool enabled = true) : ds(ds),
            active(enabled && ds->sqlMutationObserverActive()) {
    }

    DLLLOCAL ~QorePGCopyStreamHelper() {
        if (started) {
            // Normal paths finish explicitly so observer failures can be propagated.  This is only
            // a last-resort backstop for connection teardown or unexpected C++ unwinding.
            ExceptionSink xsink;
            ds->reportMutationStreamEnd(consumed, false, &xsink);
        }
    }

    //! reports the start of the stream
    /** @return 0 to continue, -1 if the consumer rejected the stream, in which case an exception has
        been raised and no data may be sent
    */
    DLLLOCAL int begin(ExceptionSink* xsink) {
        if (!active) {
            return 0;
        }
        // the size of the payload is not known before it is serialized, so 0 is reported here; the
        // core then falls back to the "max_growth_bytes" value of the producer's declaration, if any
        if (ds->reportMutationStreamBegin(0, xsink)) {
            return -1;
        }
        started = true;
        return 0;
    }

    //! accounts for a row that has been sent and reports progress when the interval has elapsed
    /** @return 0 to continue, -1 if the consumer stopped the stream, in which case an exception has
        been raised and the stream must be aborted
    */
    DLLLOCAL int addBytes(size_t bytes, ExceptionSink* xsink) {
        if (!started) {
            return 0;
        }
        consumed += (int64)bytes;
        if (consumed - reported < copy_stream_report_bytes) {
            return 0;
        }
        reported = consumed;
        if (ds->reportMutationStreamProgress(consumed, xsink)) {
            ok = false;
            return -1;
        }
        return 0;
    }

    //! marks the stream as failed; the end boundary reports the failure
    DLLLOCAL void setError() {
        ok = false;
    }

    //! reports the terminal boundary exactly once
    DLLLOCAL int finish(bool success, ExceptionSink* xsink) {
        if (!started) {
            return 0;
        }
        started = false;
        ok = ok && success;
        return ds->reportMutationStreamEnd(consumed, ok, xsink);
    }

private:
    Datasource* ds;
    //! true if a mutation observer wants stream events
    bool active;
    //! true once the start boundary has been delivered
    bool started = false;
    //! false if the stream did not complete successfully
    bool ok = true;
    //! total bytes sent to the server
    int64 consumed = 0;
    //! total bytes reported to the observer so far
    int64 reported = 0;
};
#endif

#ifdef QDBI_METHOD_BULK_LOAD_BEGIN
//! persistent state for the driver-neutral native bulk-load protocol
class QorePGBulkCopyState {
public:
    DLLLOCAL QorePGBulkCopyState(Datasource* ds, size_t columns, bool stream_bounds)
            : columns(columns)
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
            , stream(ds, stream_bounds)
#endif
    {
    }

    size_t columns;
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    QorePGCopyStreamHelper stream;
#endif
};

//! validates a native COPY row block and returns its logical row count
static int64 qpgBulkCopyRowCount(const QoreHashNode* rows, size_t expected_columns, ExceptionSink* xsink) {
    if (rows->size() != expected_columns) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "native COPY row block has %zu columns; expected %zu",
            rows->size(), expected_columns);
        return -1;
    }

    int64 count = -1;
    size_t column = 0;
    ConstHashIterator hi(rows);
    while (hi.next()) {
        if (column && !(column % 100) && qore_check_cancel(xsink, "pgsql native COPY row validation")) {
            return -1;
        }
        QoreValue value = hi.get();
        if (value.getType() == NT_LIST) {
            int64 size = value.get<const QoreListNode>()->size();
            if (count < 0) {
                count = size;
            } else if (count != size) {
                xsink->raiseException("DBI:PGSQL:COPY-ERROR", "native COPY column '%s' has " QLLD
                    " rows; expected " QLLD, hi.getKey(), size, count);
                return -1;
            }
        }
        ++column;
    }
    return count < 0 ? 1 : count;
}

//! aborts a COPY operation whose stream-begin boundary was rejected and drains the server result
static void qpgAbortRejectedCopy(PGconn* pc) {
    QorePGCancelHelper cancel_helper(pc);
    PQputCopyEnd(pc, "rejected by the datasource mutation observer");
    while (PGresult* result = PQgetResult(pc)) {
        PQclear(result);
    }
}

int QorePGConnection::bulkLoadBegin(const QoreString* table, const QoreListNode* columns,
        const QoreHashNode* options, ExceptionSink* xsink) {
    if (bulk_copy) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s native COPY is already active", server_desc.c_str());
        return -1;
    }

    bool stream_bounds = true;
    if (options) {
        QoreValue value = options->getKeyValue("stream_bounds");
        if (!value.isNothing()) {
            if (value.getType() != NT_BOOLEAN) {
                xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s native COPY option 'stream_bounds' must be "
                    "boolean", server_desc.c_str());
                return -1;
            }
            stream_bounds = value.getAsBool();
        }
    }

    QoreString query("COPY ");
    query.concat(table->c_str(), table->size());
    query.concat(" (");
    size_t column = 0;
    ConstListIterator li(columns);
    while (li.next()) {
        if (column && !(column % 100) && qore_check_cancel(xsink, "pgsql native COPY column rendering")) {
            return -1;
        }
        QoreValue value = li.getValue();
        if (value.getType() != NT_STRING) {
            xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s native COPY column %d has type '%s'; expected "
                "string", server_desc.c_str(), static_cast<int>(column + 1), value.getTypeName());
            return -1;
        }
        if (column) {
            query.concat(", ");
        }
        // note: column names are short enough to be held in inline short string storage (ex:
        // "id"), which has no QoreStringNode, so the data helper must be used to read the bytes
        QoreStringDataHelper name(value);
        query.concat(name.c_str(), name.size());
        ++column;
    }
    query.concat(") FROM STDIN");

    std::unique_ptr<QoreString> encoded(query.convertEncoding(ds->getQoreEncoding(), xsink));
    if (!encoded || qore_check_cancel(xsink, "pgsql native COPY begin")) {
        return -1;
    }

    PGresult* result;
    {
        QorePGCancelHelper cancel_helper(pc);
        result = PQexec(pc, encoded->c_str());
    }
    if (!result) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s PQexec() returned NULL for native COPY",
            server_desc.c_str());
        return -1;
    }
    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_COPY_IN) {
        doError(result, xsink);
        PQclear(result);
        return -1;
    }
    PQclear(result);

    std::unique_ptr<QorePGBulkCopyState> state(new QorePGBulkCopyState(ds, column, stream_bounds));
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    if (state->stream.begin(xsink)) {
        qpgAbortRejectedCopy(pc);
        return -1;
    }
#endif
    bulk_copy = state.release();
    return 0;
}

int QorePGConnection::bulkLoadRows(const QoreHashNode* rows, ExceptionSink* xsink) {
    if (!bulk_copy) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s no native COPY operation is active",
            server_desc.c_str());
        return -1;
    }

    int64 row_count = qpgBulkCopyRowCount(rows, bulk_copy->columns, xsink);
    if (*xsink || row_count < 0) {
        return -1;
    }
    if (!row_count) {
        return 0;
    }

    size_t num_columns = bulk_copy->columns;
    std::vector<const QoreListNode*> column_lists(num_columns);
    std::vector<bool> column_is_list(num_columns);
    std::vector<QoreValue> column_scalars(num_columns);
    {
        ConstHashIterator hi(rows);
        size_t column = 0;
        while (hi.next()) {
            if (column && !(column % 100)
                && qore_check_cancel(xsink, "pgsql native COPY column collection")) {
                return -1;
            }
            QoreValue value = hi.get();
            if (value.getType() == NT_LIST) {
                column_lists[column] = value.get<const QoreListNode>();
                column_is_list[column] = true;
            } else {
                column_scalars[column] = value;
                column_is_list[column] = false;
            }
            ++column;
        }
    }

    const QoreEncoding* enc = ds->getQoreEncoding();
    QoreString block;
    QoreString row;
    for (int64 i = 0; i < row_count; ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "pgsql native COPY row serialization")) {
            return -1;
        }
        row.clear();
        for (size_t column = 0; column < num_columns; ++column) {
            if (column && !(column % 100)
                && qore_check_cancel(xsink, "pgsql native COPY value serialization")) {
                return -1;
            }
            if (column) {
                row.concat('\t');
            }
            QoreValue value = column_is_list[column]
                ? column_lists[column]->retrieveEntry(i)
                : column_scalars[column];
            if (appendCopyValue(row, value, enc, xsink)) {
                return -1;
            }
        }
        row.concat('\n');
        block.concat(row.c_str(), row.size());
    }

    if (block.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s native COPY block has %zu bytes; libpq accepts at most "
            "%d bytes per block", server_desc.c_str(), block.size(), std::numeric_limits<int>::max());
        return -1;
    }

    int put_rc;
    {
        QorePGCancelHelper cancel_helper(pc);
        put_rc = PQputCopyData(pc, block.c_str(), static_cast<int>(block.size()));
    }
    if (put_rc != 1) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s PQputCopyData() failed for native COPY: %s",
            server_desc.c_str(), PQerrorMessage(pc));
        return -1;
    }

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    if (bulk_copy->stream.addBytes(block.size(), xsink)) {
        return -1;
    }
#endif
    return 0;
}

int QorePGConnection::bulkLoadEnd(bool success, ExceptionSink* xsink) {
    if (!bulk_copy) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s no native COPY operation is active",
            server_desc.c_str());
        return -1;
    }

    // Clear connection ownership first so callbacks, cleanup errors, or connection teardown cannot
    // attempt to end the same protocol session twice.
    std::unique_ptr<QorePGBulkCopyState> state(bulk_copy);
    bulk_copy = nullptr;

    bool cleanup_ok = true;
    int end_rc;
    {
        QorePGCancelHelper cancel_helper(pc);
        end_rc = PQputCopyEnd(pc, success ? nullptr : "cancelled by the native bulk-load caller");
    }
    if (end_rc != 1) {
        cleanup_ok = false;
        if (!*xsink) {
            xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s PQputCopyEnd() failed for native COPY: %s",
                server_desc.c_str(), PQerrorMessage(pc));
        }
    }

    PGresult* result = nullptr;
    if (end_rc == 1) {
        QorePGCancelHelper cancel_helper(pc);
        result = PQgetResult(pc);
    }
    if (success) {
        if (!result) {
            cleanup_ok = false;
            if (!*xsink) {
                xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s PQgetResult() returned NULL after native COPY",
                    server_desc.c_str());
            }
        } else if (PQresultStatus(result) != PGRES_COMMAND_OK) {
            cleanup_ok = false;
            doError(result, xsink);
        }
    }
    if (result) {
        PQclear(result);
    }

    // Drain any additional results before returning the connection to ordinary DBI use.
    if (end_rc == 1) {
        QorePGCancelHelper cancel_helper(pc);
        while (PGresult* extra = PQgetResult(pc)) {
            PQclear(extra);
        }
    }

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    if (state->stream.finish(success && cleanup_ok, xsink)) {
        cleanup_ok = false;
    }
#endif
    return cleanup_ok && !*xsink ? 0 : -1;
}
#endif

QoreValue QorePGConnection::copyFromStdin(const QoreString* qstr, const QoreListNode* args, ExceptionSink* xsink) {
    if (!args || args->empty()) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: COPY FROM STDIN requires a hash-of-lists argument "
            "with the data to copy", server_desc.c_str());
        return QoreValue();
    }

    // get the data hash from the first argument
    QoreValue v = args->retrieveEntry(0);
    if (v.getType() != NT_HASH) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: COPY FROM STDIN requires a hash-of-lists argument; "
            "got type '%s'", server_desc.c_str(), v.getTypeName());
        return QoreValue();
    }

    const QoreHashNode* data = v.get<const QoreHashNode>();
    if (!data->size()) {
        return 0;
    }

    // determine number of rows from first list
    int num_cols = (int)data->size();
    int num_rows = 0;
    {
        ConstHashIterator hi(data);
        if (hi.next()) {
            QoreValue fv = hi.get();
            if (fv.getType() == NT_LIST) {
                num_rows = (int)fv.get<const QoreListNode>()->size();
            } else {
                num_rows = 1;
            }
        }
    }

    if (!num_rows) {
        return 0;
    }

    // convert SQL to the connection encoding
    std::unique_ptr<QoreString> sql(qstr->convertEncoding(ds->getQoreEncoding(), xsink));
    if (!sql.get()) {
        return QoreValue();
    }

    // Check for interrupt before COPY execution
    if (qore_check_cancel(xsink)) {
        return QoreValue();
    }

    // Execute the COPY command
    PGresult* res;
    {
        QorePGCancelHelper cancel_helper(pc);
        res = PQexec(pc, sql->c_str());
    }

    if (!res) {
        xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: PQexec() returned NULL for COPY command",
            server_desc.c_str());
        return QoreValue();
    }

    ExecStatusType rc = PQresultStatus(res);
    if (rc != PGRES_COPY_IN) {
        doError(res, xsink);
        PQclear(res);
        return QoreValue();
    }
    PQclear(res);
    res = nullptr;

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    // the server is now in COPY IN mode; report the bounded stream to the datasource mutation
    // observer, if any, so that a consumer can account for or stop the write while it streams
    QorePGCopyStreamHelper csh(ds);
    if (csh.begin(xsink)) {
        // the consumer rejected the stream: end the COPY without sending any data
        PQputCopyEnd(pc, "rejected by the datasource mutation observer");
        PGresult* rej_res = PQgetResult(pc);
        if (rej_res) {
            PQclear(rej_res);
        }
        return QoreValue();
    }
#endif

    const QoreEncoding* enc = ds->getQoreEncoding();

    // collect column lists for iteration
    std::vector<const QoreListNode*> col_lists(num_cols);
    std::vector<bool> col_is_list(num_cols);
    std::vector<QoreValue> col_scalars(num_cols);
    {
        ConstHashIterator hi(data);
        int idx = 0;
        while (hi.next()) {
            QoreValue cv = hi.get();
            if (cv.getType() == NT_LIST) {
                col_lists[idx] = cv.get<const QoreListNode>();
                col_is_list[idx] = true;
            } else {
                col_scalars[idx] = cv;
                col_is_list[idx] = false;
            }
            ++idx;
        }
    }

    // send data rows
    bool error = false;
    QoreString row_buf;
    {
        QorePGCancelHelper cancel_helper(pc);
        for (int i = 0; i < num_rows; ++i) {
            // check for interrupt periodically
            if ((i % 1000) == 0 && qore_check_cancel(xsink)) {
                error = true;
                break;
            }

            row_buf.clear();

            for (int j = 0; j < num_cols; ++j) {
                if (j > 0) {
                    row_buf.concat('\t');
                }

                QoreValue cell;
                if (col_is_list[j]) {
                    cell = col_lists[j]->retrieveEntry(i);
                } else {
                    cell = col_scalars[j];
                }

                if (appendCopyValue(row_buf, cell, enc, xsink)) {
                    error = true;
                    break;
                }
            }

            if (error) {
                break;
            }

            row_buf.concat('\n');

            if (row_buf.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                error = true;
                xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: COPY row has %zu bytes; libpq accepts at "
                    "most %d bytes per call", server_desc.c_str(), row_buf.size(),
                    std::numeric_limits<int>::max());
                break;
            }

            int put_rc = PQputCopyData(pc, row_buf.c_str(), static_cast<int>(row_buf.size()));
            if (put_rc < 0) {
                error = true;
                xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: PQputCopyData() failed: %s",
                    server_desc.c_str(), PQerrorMessage(pc));
                break;
            }

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
            // report the bytes sent so far; the consumer can stop a stream that has exceeded what it
            // will admit
            if (csh.addBytes(row_buf.strlen(), xsink)) {
                error = true;
                break;
            }
#endif
        }
    }

#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    if (error) {
        csh.setError();
    }
#endif

    // end COPY
    int end_rc;
    if (error) {
        end_rc = PQputCopyEnd(pc, "cancelled");
    } else {
        end_rc = PQputCopyEnd(pc, nullptr);
    }

    if (end_rc < 0) {
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.setError();
#endif
        if (!*xsink) {
            xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: PQputCopyEnd() failed: %s",
                server_desc.c_str(), PQerrorMessage(pc));
        }
    }

    // consume result
    PGresult* end_res = PQgetResult(pc);
    if (error) {
        // just consume and clear
        if (end_res) {
            PQclear(end_res);
        }
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.finish(false, xsink);
#endif
        return QoreValue();
    }

    if (!end_res) {
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.setError();
#endif
        if (!*xsink) {
            xsink->raiseException("DBI:PGSQL:COPY-ERROR", "%s: PQgetResult() returned NULL after COPY",
                server_desc.c_str());
        }
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.finish(false, xsink);
#endif
        return QoreValue();
    }

    rc = PQresultStatus(end_res);
    if (rc != PGRES_COMMAND_OK) {
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.setError();
#endif
        doError(end_res, xsink);
        PQclear(end_res);
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
        csh.finish(false, xsink);
#endif
        return QoreValue();
    }

    int rows = atoi(PQcmdTuples(end_res));
    PQclear(end_res);
#ifdef QORE_PGSQL_HAVE_SQL_MUTATION_OBSERVER
    if (csh.finish(true, xsink)) {
        return QoreValue();
    }
#endif
    return rows;
}

QoreValue QorePGConnection::exec(const QoreString* qstr, const QoreListNode* args, ExceptionSink *xsink) {
    // check for COPY ... FROM STDIN pattern
    if (isCopyFromStdin(qstr)) {
        return copyFromStdin(qstr, args, xsink);
    }

    QorePgsqlStatement res(this, ds->getQoreEncoding());
    if (res.exec(qstr, args, xsink))
        return QoreValue();

    if (res.hasResultData())
        return res.getOutputHash(xsink);

    return res.rowsAffected();
}

QoreValue QorePGConnection::execRaw(const QoreString* qstr, ExceptionSink *xsink) {
    QorePgsqlStatement res(this, ds->getQoreEncoding());
    // convert string to required character encoding or copy
    std::unique_ptr<QoreString> ccstr(qstr->convertEncoding(ds->getQoreEncoding(), xsink));

    if (res.exec(ccstr->c_str(), xsink))
        return QoreValue();

    if (res.hasResultData())
        return res.getOutputHash(xsink);

    return res.rowsAffected();
}

int QorePGConnection::get_server_version() const {
    return PQserverVersion(pc);
}

// static
QoreHashNode* QorePGConnection::getExceptionArg(const PGresult *res, ExceptionSink *xsink) {
    QoreHashNode* arg = new QoreHashNode(autoTypeInfo);
    if (res) {
        // The caller should not free the result directly (char* results).
        // It will be freed when the associated PGresult handle is passed to PQclear.
        const char* sql_state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        const char* sql_diag = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
        arg->setKeyValue("alterr", new QoreStringNode(sql_state), xsink);
        arg->setKeyValue("alterr_diag", new QoreStringNode(sql_diag), xsink);
    } else {
        // ensure that alterr key is always presented
        arg->setKeyValue("alterr", new QoreStringNode(""), xsink);
    }

    return arg;
}

// static
void QorePGConnection::doLostConnectionError(bool in_trans, const PGresult* res, ExceptionSink* xsink) {
    const QoreHashNode* arg = getExceptionArg(res, xsink);
    xsink->raiseExceptionArg("DBI:PGSQL:CONNECTION-ERROR", arg, in_trans
        ? "connection to PostgreSQL database server lost while in a transaction; transaction has been lost"
        : "connection to PostgreSQL database server lost while not in a transaction");
}

int QorePgsqlPreparedStatement::prepare(const QoreString& n_sql, const QoreListNode* args, bool n_parse,
        ExceptionSink* xsink) {
    assert(!sql);
    // create copy of string and convert encoding if necessary
    sql = n_sql.convertEncoding(enc, xsink);
    if (*xsink) {
        return -1;
    }

    if (args) {
        targs = args->listRefSelf();
    }

    do_parse = n_parse;
    parsed = false;

    return 0;
}

int QorePgsqlPreparedStatement::bind(const QoreListNode &l, ExceptionSink *xsink) {
    if (targs) {
        targs->deref(xsink);
        targs = 0;
        if (*xsink)
            return -1;
    }

    targs = l.listRefSelf();
    return 0;
}

int QorePgsqlPreparedStatement::exec(ExceptionSink* xsink) {
    if (res)
        QorePgsqlStatement::reset();

    if (do_parse) {
        if (!parsed) {
            if (parse(sql, targs, xsink))
                return -1;
            parsed = true;
        } else if (targs) {
            // rebind new arguments
            ConstListIterator li(targs);
            while (li.next()) {
                if (add(li.getValue(), xsink))
                    return -1;
            }
        }
    }

    //printd(5, "QorePgsqlPreparedStatement::exec() this: %p do_parse: %d nParams: %d args: %p (len: %d) sql: %s\n",
    //    this, do_parse, nParams, targs, targs ? targs->size() : 0, sql->c_str());

    return execIntern(sql->c_str(), xsink);
}

QoreHashNode* QorePgsqlPreparedStatement::fetchRow(ExceptionSink* xsink) {
    if (crow == -1) {
        xsink->raiseException("DBI:PGSQL-FETCH-ROW-ERROR", "call SQLStatement::next() before calling "
            "SQLStatement::fetchRow()");
        return nullptr;
    }
    return getSingleRowIntern(xsink, crow);
}

QoreListNode* QorePgsqlPreparedStatement::fetchRows(int rows, ExceptionSink *xsink) {
    if (crow == -1)
        crow = 0;
    return getOutputList(xsink, &crow, rows);
}

QoreHashNode* QorePgsqlPreparedStatement::fetchColumns(int rows, ExceptionSink *xsink) {
    if (crow == -1)
        crow = 0;
    return getOutputHash(xsink, false, &crow, rows);
}

#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
QoreColumnarResult* QorePgsqlPreparedStatement::fetchColumnar(int rows, ExceptionSink *xsink) {
    if (crow == -1)
        crow = 0;
    return getOutputColumnar(xsink, false, &crow, rows);
}
#endif

QoreHashNode* QorePgsqlStatement::describe(ExceptionSink *xsink) {
    // set up hash for row
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);
    QoreString namestr("name");
    QoreString maxsizestr("maxsize");
    QoreString typestr("type");
    QoreString dbtypestr("native_type");
    QoreString internalstr("internal_id");

    int columnCount = PQnfields(res);
    for (int i = 0; i < columnCount; ++i) {
        char* columnName = PQfname(res, i);
        Oid columnType = PQftype(res, i);
        int maxsize = PQfsize(res, i);
        int fmod = PQfmod(res, i);

        ReferenceHolder<QoreHashNode> col(new QoreHashNode(autoTypeInfo), xsink);
        col->setKeyValue(namestr, new QoreStringNode(columnName), xsink);
        col->setKeyValue(internalstr, columnType, xsink);

        switch (columnType) {
        case BOOLOID:
            col->setKeyValue(typestr, NT_BOOLEAN, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("boolean"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case INT8OID:
            col->setKeyValue(typestr, NT_INT, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("bigint"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case INT2OID:
            col->setKeyValue(typestr, NT_INT, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("smallint"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case INT4OID:
            col->setKeyValue(typestr, NT_INT, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("integer"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case OIDOID:
            col->setKeyValue(typestr, NT_INT, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("oid"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case REGPROCOID:
        case XIDOID:
        case CIDOID:
            col->setKeyValue(typestr, NT_INT, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("n/a"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case NUMERICOID:
            col->setKeyValue(typestr, NT_NUMBER, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("numeric"), xsink);
            col->setKeyValue(maxsizestr, ((maxsize << 16) | fmod) - /*VARHDRSZ*/4, xsink);
            break;
        case FLOAT4OID:
        case FLOAT8OID:
            col->setKeyValue(typestr, NT_NUMBER, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("float"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case ABSTIMEOID:
        case RELTIMEOID:
        case DATEOID:
        case TIMEOID:
        case TIMETZOID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
            col->setKeyValue(typestr, NT_DATE, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("todo/fixme"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case BYTEAOID:
            col->setKeyValue(typestr, NT_BINARY, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("bytea"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case TEXTOID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("text"), xsink);
            col->setKeyValue(maxsizestr, maxsize, xsink);
            break;
        case CHAROID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("char"), xsink);
            col->setKeyValue(maxsizestr, fmod - 4, xsink);
            break;
        case VARCHAROID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("varchar"), xsink);
            col->setKeyValue(maxsizestr, fmod - 4, xsink);
            break;
        case XMLOID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("xml"), xsink);
            col->setKeyValue(maxsizestr, fmod - 4, xsink);
            break;
        case JSONOID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("json"), xsink);
            col->setKeyValue(maxsizestr, fmod - 4, xsink);
            break;
        case JSONBOID:
            col->setKeyValue(typestr, NT_STRING, xsink);
            col->setKeyValue(dbtypestr, new QoreStringNode("jsonb"), xsink);
            col->setKeyValue(maxsizestr, fmod - 4, xsink);
            break;
        default:
            // check for extension types (pgvector, etc.)
            if (conn->getVectorOid() && columnType == conn->getVectorOid()) {
                col->setKeyValue(typestr, NT_LIST, xsink);
                col->setKeyValue(dbtypestr, new QoreStringNode("vector"), xsink);
                col->setKeyValue(maxsizestr, maxsize, xsink);
            } else if (conn->getHalfvecOid() && columnType == conn->getHalfvecOid()) {
                col->setKeyValue(typestr, NT_LIST, xsink);
                col->setKeyValue(dbtypestr, new QoreStringNode("halfvec"), xsink);
                col->setKeyValue(maxsizestr, maxsize, xsink);
            } else if (conn->getSparsevecOid() && columnType == conn->getSparsevecOid()) {
                col->setKeyValue(typestr, NT_HASH, xsink);
                col->setKeyValue(dbtypestr, new QoreStringNode("sparsevec"), xsink);
                col->setKeyValue(maxsizestr, maxsize, xsink);
            } else {
                col->setKeyValue(typestr, -1, xsink);
                col->setKeyValue(dbtypestr, new QoreStringNode("n/a"), xsink);
                col->setKeyValue(maxsizestr, maxsize, xsink);
            }
            break;
        }  // switch

        HashAssignmentHelper hah(**h, columnName);
        if (!hah.get().isNothing()) {
            // find a unique column name
            unsigned num = 1;
            while (true) {
                QoreStringMaker tmp("%s_%d", columnName, num);
                hah.reassign(tmp.c_str());
                if (!hah.get().isNothing()) {
                    ++num;
                    continue;
                }
                break;
            }
        }

        hah.assign(col.release(), xsink);
        if (*xsink)
            return nullptr;
    }

    return h.release();
}

bool QorePgsqlPreparedStatement::next() {
    assert(res);
    ++crow;
    if (crow >= PQntuples(res)) {
        crow = -1;
        return false;
    }
    return true;
}

void QorePgsqlPreparedStatement::reset(ExceptionSink* xsink) {
    if (sql) {
        delete sql;
        sql = nullptr;
    }

    if (do_parse)
        do_parse = false;
    if (parsed)
        parsed = false;

    if (targs) {
        targs->deref(xsink);
        targs = nullptr;
    }

    if (crow != -1)
        crow = -1;

    // call parent reset function
    QorePgsqlStatement::reset();
}
