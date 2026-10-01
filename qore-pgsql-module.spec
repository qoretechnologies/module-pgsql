# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
# Use the pinned source epoch for RPM headers and installed file timestamps.
%global source_date_epoch_from_changelog 1
%global use_source_date_epoch_as_buildtime 1
%if v"%{rpmversion}" >= v"4.20"
%global build_mtime_policy clamp_to_source_date_epoch
%else
%global clamp_mtime_to_source_date_epoch 1
%endif
%bcond_without tests
%bcond_without docs
Name: qore-pgsql-module
Version: 3.5.0
Release: 1%{?dist}
Summary: PostgreSQL database driver for Qore
License: LGPL-2.1-or-later OR MIT
URL: https://github.com/qoretechnologies/module-pgsql
Source0: %{name}-%{version}.tar.xz
BuildRequires: cmake >= 3.5
BuildRequires: make
BuildRequires: gcc-c++
BuildRequires: pkgconfig(libpq)
BuildRequires: qore-devel >= 3.0.0~
BuildRequires: qore-rpm-macros >= 3.0.0~
%if %{with tests}
BuildRequires: python3
BuildRequires: postgresql-server
%if 0%{?fedora}
BuildRequires: pgvector
%endif
%endif
%if %{with docs}
BuildRequires: doxygen
%if 0%{?suse_version}
BuildRequires: util-linux
%else
BuildRequires: util-linux-core
%endif
%endif

%description
Native PostgreSQL database access with prepared statements, array binding,
transactions, bulk loading, asynchronous cancellation and explicit parameter
types. Includes compiler metadata and separate reference documentation.

%if %{with docs}
%package doc
Summary: PostgreSQL module reference documentation and examples
BuildArch: noarch
%description doc
API reference and database test examples for Qore's PostgreSQL driver.
%endif

%prep
%autosetup
%build
%{?set_build_flags}
. %{_rpmconfigdir}/qore/module-env.sh
qore_set_source_prefix_maps "%{qore_debug_source_dir}"
cmake -S . -B build -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_SKIP_RPATH=ON -DCMAKE_IGNORE_PREFIX_PATH=/usr/local \
  -DQore_DIR=%{_libdir}/cmake/Qore -DQORE_EXECUTABLE=/usr/bin/qore \
  -DQORE_QPP_EXECUTABLE=/usr/bin/qpp \
  -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=%{!?with_docs:ON}%{?with_docs:OFF}
cmake --build build -- %{?_smp_mflags}
%if %{with docs}
printf "\nWARN_AS_ERROR = FAIL_ON_WARNINGS\n" >> build/Doxyfile
cmake --build build --target docs -- %{?_smp_mflags}
%endif
%install
DESTDIR=%{buildroot} cmake --install build
chmod 755 %{buildroot}%{_libdir}/qore-modules/pgsql-api-*.qmod
%if %{with docs}
install -d %{buildroot}%{_docdir}/%{name}-doc
cp -a build/docs/pgsql/html %{buildroot}%{_docdir}/%{name}-doc/
install -d %{buildroot}%{_docdir}/%{name}-doc/examples/test
install -m644 test/*.qtest test/*.q %{buildroot}%{_docdir}/%{name}-doc/examples/test/
hardlink -t -O %{buildroot}%{_docdir}/%{name}-doc
%endif
%check
%if %{with tests}
. %{_rpmconfigdir}/qore/module-env.sh
python3 -B -W error rpm/test_postgres_fixture.py -v
python3 -B -W error test/test_uninstall.py -v
%if %{with docs}
python3 -B -W error test/test_docs.py build -v
%endif
export QORE_PGSQL_BINARY_MODULE="$PWD/build/pgsql-api-$(/usr/bin/qore --latest-module-api).qmod"
%if 0%{?fedora}
export QORE_TEST_REQUIRE_PGVECTOR=1
%endif
python3 -B rpm/with-postgres.py -- rpm/run-suites test
%endif
%files
%license COPYING.MIT COPYING.LGPL
%doc README
%{_libdir}/qore-modules/pgsql-api-*.qmod
%dir %{_datadir}/qore/metadata/pgsql
%{_datadir}/qore/metadata/pgsql/*.meta.json
%if %{with docs}
%files doc
%license COPYING.MIT COPYING.LGPL
%doc %{_docdir}/%{name}-doc/
%endif
%changelog
* Thu Oct 01 2026 David Nichols <david@qore.org> - 3.5.0-1
- Package the PostgreSQL driver, metadata and complete public API documentation.
- Run real PostgreSQL database tests in a private offline cluster.
