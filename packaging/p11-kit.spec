Name:           p11-kit
Version:        0.12
Release:        0
License:        BSD-3-Clause
Summary:        Library to work with PKCS#11 modules
Url:            http://p11-glue.freedesktop.org/p11-kit.html
Group:          Development/Libraries/C and C++
Source0:        http://p11-glue.freedesktop.org/releases/%{name}-%{version}.tar.gz
Source99:       baselibs.conf
BuildRequires:  pkg-config
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package -n libp11-kit
Summary:        Library to work with PKCS#11 modules
Group:          System/Libraries

%description -n libp11-kit
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package tools
Summary:        Library to work with PKCS#11 modules -- Tools
Group:          Development/Libraries/C and C++

%description tools
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package devel
Summary:        Library to work with PKCS#11 modules -- Development Files
Group:          Development/Libraries/C and C++
Requires:       libp11-kit = %{version}

%description devel
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%prep
%setup -q

%build
%configure
make %{?_smp_mflags}

%install
%make_install
# Create pkcs11 config directory
test ! -e %{buildroot}%{_sysconfdir}/pkcs11/modules
install -d %{buildroot}%{_sysconfdir}/pkcs11/modules
# Remove sample config away to doc folder. Having the sample there would conflict
# with future versions of the library on file level. As replacement, we package
# the file as documentation file.
rm %{buildroot}%{_sysconfdir}/pkcs11/pkcs11.conf.example
find %{buildroot}%{_libdir} -name '*.la' -type f -delete -print

%post -n libp11-kit -p /sbin/ldconfig

%postun -n libp11-kit -p /sbin/ldconfig

%files -n libp11-kit
%defattr(-,root,root)
# Package the example conf file as documentation. Like this we're sure that we will
# not introduce conflicts with this version of the library and future ones.
%doc p11-kit/pkcs11.conf.example
%doc COPYING
%dir %{_sysconfdir}/pkcs11
%dir %{_sysconfdir}/pkcs11/modules/
%{_libdir}/libp11-kit.so.*
%{_libdir}/p11-kit-proxy.so

%files tools
%defattr(-,root,root)
%{_bindir}/p11-kit

%files devel
%defattr(-,root,root)
%{_includedir}/p11-kit-1/
%{_libdir}/libp11-kit.so
%{_libdir}/pkgconfig/p11-kit-1.pc
%doc %dir %{_datadir}/gtk-doc
%doc %dir %{_datadir}/gtk-doc/html
%doc %{_datadir}/gtk-doc/html/p11-kit/

%changelog
