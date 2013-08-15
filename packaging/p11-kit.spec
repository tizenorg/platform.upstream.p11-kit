Name:           p11-kit
Version:        0.18.4
Release:        0
License:        BSD-3-Clause
Summary:        Library to work with PKCS#11 modules
Url:            http://p11-glue.freedesktop.org/p11-kit.html
Group:          Security/Crypto Libraries
Source0:        http://p11-glue.freedesktop.org/releases/%{name}-%{version}.tar.gz
Source99:       baselibs.conf
Source1001: 	p11-kit.manifest
BuildRequires:  pkg-config
BuildRequires:  pkgconfig(libtasn1)

%description
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package -n libp11-kit
Summary:        Library to work with PKCS#11 modules

%description -n libp11-kit
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package tools
Summary:        Library to work with PKCS#11 modules -- Tools

%description tools
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%package devel
Summary:        Library to work with PKCS#11 modules -- Development Files
Requires:       libp11-kit = %{version}

%description devel
p11-kit provides a way to load and enumerate PKCS#11 modules, as well
as a standard configuration setup for installing PKCS#11 modules in
such a way that they're discoverable.

%prep
%setup -q
cp %{SOURCE1001} .

%build
%configure --with-trust-paths=/etc/ssl/
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

%post -n libp11-kit -p /sbin/ldconfig

%postun -n libp11-kit -p /sbin/ldconfig

%files -n libp11-kit
%manifest %{name}.manifest
%defattr(-,root,root)
# Package the example conf file as documentation. Like this we're sure that we will
# not introduce conflicts with this version of the library and future ones.
%doc p11-kit/pkcs11.conf.example
%license COPYING
%dir %{_sysconfdir}/pkcs11
%dir %{_sysconfdir}/pkcs11/modules/
%{_libdir}/libp11-kit.so.*
%{_libdir}/p11-kit-proxy.so
%dir %{_libdir}/pkcs11
%dir %{_libdir}/p11-kit
%{_libdir}/pkcs11/p11-kit-trust.so
%{_libdir}/p11-kit/p11-kit-extract-trust
%dir %_datadir/p11-kit
%dir %_datadir/p11-kit/modules
%_datadir/p11-kit/modules/p11-kit-trust.module

%files tools
%manifest %{name}.manifest
%defattr(-,root,root)
%{_bindir}/p11-kit

%files devel
%manifest %{name}.manifest
%defattr(-,root,root)
%{_includedir}/p11-kit-1/
%{_libdir}/libp11-kit.so
%{_libdir}/pkgconfig/p11-kit-1.pc
%doc %dir %{_datadir}/gtk-doc
%doc %dir %{_datadir}/gtk-doc/html
%doc %{_datadir}/gtk-doc/html/p11-kit/

%changelog
