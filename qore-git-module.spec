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
Name: qore-git-module
Version: 1.0.0
Release: 1%{?dist}
Summary: Git repositories and data providers for Qore
License: MIT
URL: https://github.com/qoretechnologies/module-git
Source0: %{name}-%{version}.tar.xz
%global _find_debuginfo_dwz_opts %{nil}
BuildRequires: cmake >= 3.21
BuildRequires: make
BuildRequires: gcc-c++
BuildRequires: pkgconfig(libgit2)
%if %{with tests}
BuildRequires: qore-misc-tools >= 3.0.0~
%endif
BuildRequires: qore-devel >= 3.0.0~
BuildRequires: qore-rpm-macros >= 3.0.0~
%if %{with docs}
BuildRequires: doxygen
BuildRequires: /usr/bin/hardlink
%endif
%{?qore_enable_aot_post}

%description
Git repository operations, virtual and disk repositories, configuration
management and forge helpers using the distribution libgit2 library.

%if %{with docs}
%package doc
Summary: Git module reference documentation
BuildArch: noarch
%description doc
API reference and examples for Qore's Git module.
%endif

%prep
%autosetup
%build
%{?set_build_flags}
. %{_rpmconfigdir}/qore/module-env.sh
qore_set_source_prefix_maps "%{qore_debug_source_dir}"
cmake -S . -B build -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_INSTALL_LIBDIR=%{_lib} \
  -DCMAKE_SKIP_RPATH=ON -DCMAKE_IGNORE_PREFIX_PATH=/usr/local \
  -DQore_DIR=%{_libdir}/cmake/Qore -DQORE_EXECUTABLE=/usr/bin/qore \
  -DQORE_QPP_EXECUTABLE=/usr/bin/qpp -DQORE_QCC_EXECUTABLE=/usr/bin/qcc \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DQORE_BUILD_AOT_MODULES=ON -DQORE_AOT_LINK_SOURCE_MODULES=OFF \
  -DQORE_QM_METADATA_ENV:STRING="QORE_MODULE_DIR=$QORE_MODULE_DIR:$PWD/qlib;QORE_MODULE_DIR_ONLY=1;QORE_INCLUDE_DIR=;LD_LIBRARY_PATH=" \
  -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=%{!?with_docs:ON}%{?with_docs:OFF}
cmake --build build -- %{?_smp_mflags}
%if %{with docs}
cmake --build build --target docs -- %{?_smp_mflags}
%endif
%install
DESTDIR=%{buildroot} cmake --install build
%qore_install_aot_sources qlib
find %{buildroot}%{_libdir}/qore-modules -type f -name '*.qmod' -exec chmod 755 {} +
%if %{with docs}
install -d %{buildroot}%{_docdir}/%{name}-doc
cp -a build/docs %{buildroot}%{_docdir}/%{name}-doc/
hardlink -t -O %{buildroot}%{_docdir}/%{name}-doc
%endif
%check
%if %{with tests}
. %{_rpmconfigdir}/qore/module-env.sh
for test in test/*.qtest; do
  timeout 180 /usr/bin/qore -b --enable-debug \
    -l "$PWD/build/git-api-$(/usr/bin/qore --latest-module-api).qmod" \
    -l "$PWD/build/qlib-qmod/GitDataProvider/GitDataProvider.qmod" \
    -l "$PWD/build/qlib-qmod/GitConfigManager/GitConfigManager.qmod" \
    -l "$PWD/build/qlib-qmod/GitForgeHelper/GitForgeHelper.qmod" \
    -l "$PWD/build/qlib-qmod/GitConnections.qmod" "$test" -v
done
qore-data-provider-i18n --no-color --check-source-tree --require-standard-locales \
  --require-complete-locales --output "$PWD/qlib"
%endif
%files
%license COPYING.MIT
%doc README.md
%{_libdir}/qore-modules/git-api-*.qmod
%{_libdir}/qore-modules/GitDataProvider/
%{_libdir}/qore-modules/GitConfigManager/
%{_libdir}/qore-modules/GitForgeHelper/
%{_libdir}/qore-modules/GitConnections.qmod
%{_datadir}/qore-modules/GitDataProvider/
%{_datadir}/qore-modules/GitConfigManager/
%{_datadir}/qore-modules/GitForgeHelper/
%{_datadir}/qore-modules/GitConnections.qm
%dir %{_datadir}/qore/metadata/git
%{_datadir}/qore/metadata/git/*.meta.json
%{_datadir}/qore/i18n/
%if %{with docs}
%files doc
%license COPYING.MIT
%doc %{_docdir}/%{name}-doc/
%endif
%changelog
* Thu Oct 01 2026 David Nichols <david@qore.org> - 1.0.0-1
- Package native and AOT modules, resources, documentation and offline tests.
