Name:           ffmpeg-mpv
Version:        2.5.999
Release:        0
Summary:        AV Codec Stuff

Group:          Multimedia/Libraries
License:        LGPL-2.1+
URL:            https://github.com/FFmpeg
Source0:        %{name}-%{version}.tar.bz2

BuildRequires: yasm
BuildRequires: zlib-devel
BuildRequires: pkgconfig

%description
FFmpeg libraries for multimedia purposes

%package        devel
Summary:        Development files for %{name}
Group:          Development/Libraries
Requires:       %{name} = %{version}-%{release}
Requires:       pkgconfig

%description    devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.

%prep
%setup -q

%build
./configure \
    --prefix=%{_prefix} \
    --libdir=%_libdir \
    --shlibdir=%_libdir \
    --build-suffix="-mpv" \
    --disable-stripping \
    --enable-avresample \
    --disable-programs \
    --disable-static \
    --enable-shared
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%make_install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%{_libdir}/*.so.*

%files devel
%{_includedir}/*
%{_libdir}/*.so
%{_libdir}/pkgconfig/*.pc
