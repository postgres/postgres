%define version 3.0
%define release pre20000310
%define name PyGreSQL
%define pythonversion 1.5
Source: %{name}-%{version}-%{release}.tgz
Summary: A Python interface for PostgreSQL database.
Name: %{name}
Version: %{version}
Release: %{release}
#Patch: 
Group: Applications/Databases
BuildRoot: /tmp/rpmbuild_%{name}
Copyright: GPL-like
Requires: python >= %{pythonversion}, postgresql
Packager: Hartmut Goebel <hartmut@goebel.noris.de>
Vendor: D'Arcy J.M. Cain <darcy@druid.net>
URL: http://www.druid.net/pygresql/

%changelog
#* Tue Oct 06 1998 Fabio Coatti <cova@felix.unife.it>
#- fixed installation directory files list

%description
PyGreSQL is a python module that interfaces to a PostgreSQL database. It
embeds the PostgreSQL query library to allow easy use of the powerful
PostgreSQL features from a Python script.

Version 3.0 includes DB-API 2.0 support.

%prep
rm -rf $RPM_BUILD_ROOT

%setup -n %{name}-%{version}-%{release}
#%patch

%build
mkdir -p $RPM_BUILD_ROOT/usr/lib/python%{pythonversion}/lib-dynload
cc -fpic -shared -o $RPM_BUILD_ROOT/usr/lib/python%{pythonversion}/lib-dynload/_pg.so -I/usr/include/pgsql/ -I/usr/include/python1.5 pgmodule.c -lpq
## import fails, since _pg is not yet installed
python -c 'import pg' || true
python -c 'import pgdb' || true

%install
cp *.py *.pyc $RPM_BUILD_ROOT/usr/lib/python%{pythonversion}/

cd $RPM_BUILD_ROOT
find . -type f | sed 's,^\.,\%attr(-\,root\,root) ,' > $RPM_BUILD_DIR/file.list.%{name}
find . -type l | sed 's,^\.,\%attr(-\,root\,root) ,' >> $RPM_BUILD_DIR/file.list.%{name}

%files -f ../file.list.%{name}
%doc %attr(-,root,root) Announce ChangeLog README tutorial


%clean
rm -rf $RPM_BUILD_ROOT
cd $RPM_BUILD_DIR
rm -rf %{name}-%{version}-%{release} file.list.%{name}
