Name: pfgrep
Version: 0.5
Release: 0
Summary: grep for source physical file members

License: GPL-3.0-or-later
URL: https://github.com/SeidenGroup/pfgrep

Source: pfgrep-%{version}.tar.gz

BuildRequires: gcc
BuildRequires: make-gnu
BuildRequires: json-c-devel
BuildRequires: pcre2-devel
BuildRequires: libzip-devel
BuildRequires: pkg-config

%description
pfgrep is a fast (with lots on the table for future optimization) way to search
with regular expressions (using PCRE2) in physical file members. It's faster
than QShell grep or using a PASE iconv/Rfile with grep in a shell script.

%prep
%setup -q

%build

%make_build

%install

%make_install

%files
%defattr(-, qsys, *none)
%doc README.md
%license COPYING
%{_bindir}/pfgrep
%{_bindir}/pfcat
%{_bindir}/pfstat
%{_bindir}/pfzip
%{_mandir}/man1/pf*.1*
