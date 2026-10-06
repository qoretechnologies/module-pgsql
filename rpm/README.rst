RPM packaging
=============

The multi-distribution recipe packages the native PostgreSQL driver, compiler
metadata and a separate HTML reference. It uses the installed Qore 3 SDK and
the distribution's shared PostgreSQL client library. No database server is
required by the runtime package.

Prepare pinned source bundles with ``qore-packaging/tools/packaging.py`` and
build them unprivileged with ``qore-packaging/tools/build-local.py``. The default
build runs strict documentation checks, uninstall tests and the complete
PostgreSQL suite, including native bulk load, mutation observers and cancellation.

For build tests, PostgreSQL server and Python 3 are required. Fedora also installs pgvector
and requires successful extension creation before testing, so vector coverage
cannot silently skip. The private
cluster fixture opens only a Unix socket below a unique temporary directory;
it never changes a system cluster or listens on TCP. Inherited PostgreSQL
connection variables are cleared. A connection preflight makes missing or
unreachable databases fail the build. Setup and command failures still stop
and remove the cluster. Failed shutdown preserves it and fails the test.

To test an installed RPM, install PostgreSQL server and Python 3 in a minimal
image without the Qore SDK or compiler. On Fedora, also install ``pgvector``
and export ``QORE_TEST_REQUIRE_PGVECTOR=1``. From the extracted source tree, run as
an unprivileged user::

    QORE_RPM_TEST_TMP=/tmp/pgsql-installed rpm/tests-installed-runtime

The fixture copies only tests outside the checkout and clears module overrides.
``python3 -B -W error rpm/test_postgres_fixture.py -v`` covers fixture failure
paths. ``python3 -B -W error test/test_uninstall.py -v`` covers manifest-based
uninstall. ``python3 -B -W error test/test_docs.py build -v`` checks public
binding API pages and links. Tests and documentation remain enabled for release
qualification; older PostgreSQL servers and external extensions require their
own compatibility runs.

Release 2 disables optional Java bindings because the RPM does not ship Java
artifacts. Native driver, metadata, documentation and database tests are unchanged.

After installing the SDK, repeat the runtime suite and compile the same binding
and transaction example used by the Debian compiler check::

    qcc -o /tmp/pgsql-compiled rpm/compiler.qr
    python3 -B -W error rpm/with-postgres.py -- /tmp/pgsql-compiled

Run these commands as an unprivileged user. The example checks the typed integer
bind, text value and committed query result; database failures return a nonzero
status. This fixture addition does not change the release 2 RPM payload.
