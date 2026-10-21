#!/usr/bin/env python3
# Copyright 2026 Qore Technologies, s.r.o.
"""Exercise real libpq connections with owned, event-driven loopback servers.

QORE_MODULE_DIR selects the module under test. QORE_TEST_COMMAND can prefix qore
with Valgrind. QORE_DB_CONNSTR_PGSQL enables the real PostgreSQL integration case.
No production configuration, firewall rules or credentials are changed.
"""
import ctypes
import ctypes.util
import errno
import json
import os
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import tempfile
import unittest
from pathlib import Path


QORE_HEADER = r'''%modern
%try-module json
%define MissingJson
%endtry
%ifdef MissingJson
printf("SKIP_JSON\n");
%else
'''

QORE = QORE_HEADER + r'''%requires pgsql
hash<auto> config = parse_datasource(ENV.QORE_DB_CONNSTR_PGSQL ?? "pgsql:nobody@nothing");
config -= "host";
config -= "port";
config.options = {"connect-timeout": int(ENV.PGSQL_TEST_TIMEOUT ?? "1")};
if (ENV.PGSQL_TEST_HOST) {
    config.host = ENV.PGSQL_TEST_HOST;
}
try {
    Datasource ds(config);
    ds.open();
    on_exit ds.rollback();
    hash<auto> result = ds.selectRow("select 42 as answer");
    ds.rollback();
    if (ENV.PGSQL_TEST_REOPEN) {
        ds.close();
        ds.open();
        result = ds.selectRow("select 42 as answer");
    }
    if (ENV.PGSQL_TEST_APPNAME) {
        result = ds.selectRow("select current_setting('application_name') as application_name");
    }
    printf("CONNECT_RESULT %s\n", make_json(result));
} catch (hash<ExceptionInfo> ex) {
    printf("CONNECT_RESULT %s\n", make_json({"err": ex.err, "desc": ex.desc}));
}
%endif
'''


class Listener:
    """Accept silently, or send a PostgreSQL authentication rejection."""

    def __init__(self, reject=False, host='127.0.0.1', port=0):
        self.reject = reject
        self.host = host
        self.socket = socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET)
        try:
            self.socket.bind((host, port))
            self.socket.listen()
        except BaseException:
            self.socket.close()
            raise
        self.port = self.socket.getsockname()[1]
        self.stopping = threading.Event()
        self.connections = []
        self.errors = []
        self.thread = threading.Thread(target=self.serve)

    def __enter__(self):
        try:
            self.thread.start()
        except BaseException:
            self.socket.close()
            raise
        return self

    @staticmethod
    def receive(conn, size):
        data = b''
        while len(data) < size:
            part = conn.recv(size - len(data))
            if not part:
                raise EOFError('client closed before its startup message')
            data += part
        return data

    def serve(self):
        try:
            while True:
                conn, _ = self.socket.accept()
                if self.stopping.is_set():
                    conn.close()
                    return
                self.connections.append(conn)
                if self.reject:
                    conn.settimeout(10)
                    while True:
                        size, = struct.unpack('!I', self.receive(conn, 4))
                        if not 8 <= size <= 65536:
                            raise ValueError('invalid startup packet size')
                        packet = self.receive(conn, size - 4)
                        # Reject SSL/GSS negotiation, then read the actual startup packet.
                        if size == 8 and packet in (struct.pack('!I', 80877103),
                                                   struct.pack('!I', 80877104)):
                            conn.sendall(b'N')
                        else:
                            break
                    fields = b'SFATAL\0VFATAL\0C28P01\0Mfixture authentication refused\0\0'
                    conn.sendall(b'E' + struct.pack('!I', len(fields) + 4) + fields)
                    conn.close()
        except Exception as exc:
            self.errors.append(exc)

    def __exit__(self, *exc):
        self.stopping.set()
        try:
            # A sentinel connection ends blocking accept without sleeps or polling.
            with socket.create_connection((self.host, self.port), timeout=5):
                pass
            self.thread.join(15)
            if self.thread.is_alive():
                raise AssertionError('fixture thread did not stop')
            if self.errors:
                raise AssertionError('fixture failed') from self.errors[0]
        finally:
            self.socket.close()
            for conn in self.connections:
                conn.close()


class ConnectFailoverTest(unittest.TestCase):
    def connect(self, ports, hosts=None, **overrides):
        env = os.environ.copy()
        for key in ('PGHOST', 'PGHOSTADDR', 'PGPORT', 'PGCONNECT_TIMEOUT', 'PGSERVICE',
                    'PGSERVICEFILE', 'PGTARGETSESSIONATTRS', 'PGLOADBALANCEHOSTS',
                    'PGAPPNAME', 'PGSQL_TEST_HOST', 'PGSQL_TEST_TIMEOUT', 'PGSQL_TEST_REOPEN', 'PGSQL_TEST_APPNAME'):
            env.pop(key, None)
        env.update(PGPORT=','.join(map(str, ports)), PGSSLMODE='prefer', PGGSSENCMODE='disable',
                   PGSQL_TEST_HOST=hosts or ','.join('127.0.0.1' for _ in ports))
        env.update(overrides)
        command = shlex.split(env.get('QORE_TEST_COMMAND', 'qore'))
        completed = subprocess.run(command + ['-b', '--enable-debug', '-l', 'pgsql', '-'],
                                   input=QORE, text=True, env=env, capture_output=True, timeout=90)
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual('', completed.stderr)
        if completed.stdout.strip() == 'SKIP_JSON':
            self.skipTest('requires the external Qore json module')
        results = [line.removeprefix('CONNECT_RESULT ') for line in completed.stdout.splitlines()
                   if line.startswith('CONNECT_RESULT ')]
        self.assertEqual(1, len(results), completed.stdout)
        return json.loads(results[0])

    def assertRejected(self, result):
        self.assertEqual('DBI:PGSQL:ERROR', result.get('err'), result)
        self.assertIn('fixture authentication refused', result.get('desc', ''))

    def test_timeout_then_reachable_host(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, target.port]))
            self.assertEqual(1, len(silent.connections))
            self.assertEqual(1, len(target.connections))

    @unittest.skipUnless(sys.platform.startswith('linux'), 'requires Linux listen backlog semantics')
    def test_tcp_connect_timeout_then_reachable_host(self):
        with socket.socket() as full, Listener(reject=True) as target:
            full.bind(('127.0.0.1', 0))
            full.listen(0)
            # Fill the single pending slot. Linux drops further SYNs until accept() drains it.
            # This reproduces a dropped NodePort without privileged firewall changes or sleeps.
            with socket.create_connection(full.getsockname(), timeout=5):
                start = time.monotonic()
                self.assertRejected(self.connect([full.getsockname()[1], target.port]))
                self.assertGreaterEqual(time.monotonic() - start, 1)
                self.assertEqual(1, len(target.connections))

    def test_each_host_gets_a_timeout(self):
        with Listener() as first, Listener() as second:
            start = time.monotonic()
            result = self.connect([first.port, second.port])
            self.assertEqual('DBI:PGSQL:ERROR', result.get('err'))
            self.assertIn('timeout expired', result.get('desc', ''))
            self.assertGreaterEqual(time.monotonic() - start, 2)
            self.assertEqual(1, len(first.connections))
            self.assertEqual(1, len(second.connections))

    def test_two_timeouts_then_reachable_host(self):
        with Listener() as first, Listener() as second, Listener(reject=True) as target:
            self.assertRejected(self.connect([first.port, second.port, target.port]))
            self.assertEqual(1, len(first.connections))
            self.assertEqual(1, len(second.connections))
            self.assertEqual(1, len(target.connections))

    def test_refused_then_timeout_then_reachable_host(self):
        with socket.socket() as refused, Listener() as silent, Listener(reject=True) as target:
            # Reserve a port without listening; connection refusal is deterministic.
            refused.bind(('127.0.0.1', 0))
            self.assertRejected(self.connect([refused.getsockname()[1], silent.port, target.port]))
            self.assertEqual(1, len(silent.connections))
            self.assertEqual(1, len(target.connections))

    def test_duplicate_endpoints_are_bounded(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, silent.port, target.port]))
            self.assertEqual(2, len(silent.connections))

    def test_authentication_failure_does_not_fall_through(self):
        with Listener(reject=True) as first, Listener() as second:
            self.assertRejected(self.connect([first.port, second.port]))
            self.assertEqual(0, len(second.connections))

    def test_paired_hostaddr_and_port_lists(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, target.port], 'first.invalid,second.invalid',
                                             PGHOSTADDR='127.0.0.1,127.0.0.1'))
            self.assertEqual(1, len(target.connections))

    def test_hostaddr_list_without_hostnames(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, target.port], PGSQL_TEST_HOST='',
                                             PGHOSTADDR='127.0.0.1,127.0.0.1'))
            self.assertEqual(1, len(target.connections))

    def test_hosts_from_environment(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, target.port], PGSQL_TEST_HOST='',
                                             PGHOST='127.0.0.1,127.0.0.1'))

    def test_hosts_from_service_definition(self):
        with Listener() as silent, Listener(reject=True) as target, tempfile.TemporaryDirectory() as directory:
            service = Path(directory) / 'pg_service.conf'
            service.write_text('[failover]\nhost=127.0.0.1,127.0.0.1\nport=%d,%d\n' %
                               (silent.port, target.port))
            self.assertRejected(self.connect([], PGSQL_TEST_HOST='', PGPORT='', PGSERVICE='failover',
                                             PGSERVICEFILE=str(service)))

    def test_normalized_ipv6_hostaddr(self):
        try:
            silent = Listener(host='::1')
        except OSError as exc:
            if exc.errno in (errno.EAFNOSUPPORT, errno.EADDRNOTAVAIL):
                self.skipTest('IPv6 loopback is disabled on this host')
            raise
        with silent, Listener(reject=True, host='::1') as target:
            self.assertRejected(self.connect([silent.port, target.port], 'first.invalid,second.invalid',
                                             PGHOSTADDR='0:0:0:0:0:0:0:1,::1'))

    def test_single_port_for_all_hosts(self):
        with socket.socket() as refused, Listener(reject=True) as target:
            # A refused address is followed by a reachable address on the same port.
            refused.bind(('127.0.0.2', target.port))
            self.assertRejected(self.connect([target.port], '127.0.0.2,127.0.0.1'))
            self.assertEqual(1, len(target.connections))

    def test_empty_port_uses_libpq_default(self):
        # Read the compiled-in default through libpq's public metadata, including custom builds.
        class Option(ctypes.Structure):
            _fields_ = [(key, ctypes.c_char_p) for key in
                        ('keyword', 'envvar', 'compiled', 'val', 'label', 'dispchar')] + \
                       [('dispsize', ctypes.c_int)]
        libpq = ctypes.CDLL(ctypes.util.find_library('pq'))
        libpq.PQconndefaults.restype = ctypes.POINTER(Option)
        libpq.PQconndefaults.argtypes = []
        libpq.PQconninfoFree.argtypes = [ctypes.POINTER(Option)]
        libpq.PQconninfoFree.restype = None
        options = libpq.PQconndefaults()
        self.assertTrue(options)
        try:
            index = 0
            while options[index].keyword != b'port':
                self.assertIsNotNone(options[index].keyword)
                index += 1
            port = int(options[index].compiled)
        finally:
            libpq.PQconninfoFree(options)
        try:
            silent = Listener(host='127.0.0.2', port=port)
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE:
                self.skipTest('libpq default port is already occupied on 127.0.0.2')
            raise
        with silent, Listener(reject=True) as target:
            self.assertRejected(self.connect(['', target.port], '127.0.0.2,127.0.0.1'))
            self.assertEqual(1, len(silent.connections))
            self.assertEqual(1, len(target.connections))

    def test_empty_host_before_timeout_keeps_the_active_entry(self):
        with Listener() as silent, Listener(reject=True) as target:
            self.assertRejected(self.connect([silent.port, silent.port, target.port], ',127.0.0.1,127.0.0.1'))
            self.assertEqual(1, len(silent.connections))

    def test_invalid_list_lengths_are_rejected(self):
        with Listener() as silent:
            result = self.connect([silent.port], '127.0.0.1,127.0.0.1', PGHOSTADDR='127.0.0.1')
            self.assertEqual('DBI:PGSQL:ERROR', result.get('err'))
            self.assertIn('could not match', result.get('desc', ''))
            self.assertEqual(0, len(silent.connections))

    def databaseEndpoint(self):
        # Extract only the endpoint, keeping the credential-bearing connection string in the child environment.
        code = QORE_HEADER + 'hash<auto> c=parse_datasource(ENV.QORE_DB_CONNSTR_PGSQL);\n' \
               'printf("%s\\n",make_json({"host":c.host,"port":c.port ?? 5432}));\n%endif\n'
        result = subprocess.run(['qore', '-b', '--enable-debug', '-'], input=code, text=True,
                                capture_output=True, check=True, timeout=15)
        self.assertEqual('', result.stderr)
        if result.stdout.strip() == 'SKIP_JSON':
            self.skipTest('requires the external Qore json module')
        return json.loads(result.stdout)

    @unittest.skipUnless(os.environ.get('QORE_DB_CONNSTR_PGSQL'), 'requires a disposable PostgreSQL database')
    def test_real_database_query_and_reopen_after_timeout(self):
        endpoint = self.databaseEndpoint()
        with Listener() as silent:
            result = self.connect([silent.port, endpoint['port']], '127.0.0.1,' + endpoint['host'],
                                  PGSQL_TEST_REOPEN='1')
            self.assertEqual({'answer': 42}, result)
            self.assertEqual(2, len(silent.connections))

    @unittest.skipUnless(os.environ.get('QORE_DB_CONNSTR_PGSQL'), 'requires a disposable PostgreSQL database')
    def test_quoted_connection_options_survive_failover(self):
        endpoint = self.databaseEndpoint()
        with Listener() as silent:
            name = "failover's \\ application"
            result = self.connect([silent.port, endpoint['port']], '127.0.0.1,' + endpoint['host'],
                                  PGAPPNAME=name, PGSQL_TEST_APPNAME='1')
            self.assertEqual({'application_name': name}, result)

    @unittest.skipUnless(os.environ.get('QORE_DB_CONNSTR_PGSQL'), 'requires a disposable PostgreSQL database')
    def test_prefer_standby_falls_back_to_primary_after_timeout(self):
        endpoint = self.databaseEndpoint()
        with Listener() as silent:
            result = self.connect([silent.port, endpoint['port']], '127.0.0.1,' + endpoint['host'],
                                  PGTARGETSESSIONATTRS='prefer-standby')
            self.assertEqual({'answer': 42}, result)

    @unittest.skipUnless(os.environ.get('QORE_DB_CONNSTR_PGSQL'), 'requires a disposable PostgreSQL database')
    def test_target_session_attributes_survive_failover(self):
        endpoint = self.databaseEndpoint()
        with Listener() as silent:
            result = self.connect([silent.port, endpoint['port']], '127.0.0.1,' + endpoint['host'],
                                  PGTARGETSESSIONATTRS='read-only')
            self.assertEqual('DBI:PGSQL:ERROR', result.get('err'), result)
            self.assertIn('session is not read-only', result.get('desc', ''))


if __name__ == '__main__':
    unittest.main()
