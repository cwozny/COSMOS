require 'mkmf'

# `gem install cosmos` runs every extconf.rb in the gemspec, so aborting here
# would fail the entire install on any machine without Qt6. When Qt6 is absent
# we write a do-nothing Makefile instead and let lib/Qt.rb report a useful
# message if something actually tries to use the GUI. Where the GUI is the
# point -- the CI qt6 job -- COSMOS_QT6_REQUIRED=1 makes this a failure: the
# stub let that job pass with nothing built.
def write_stub_makefile(reason)
  abort "cosmos/ext/qt6: #{reason} (COSMOS_QT6_REQUIRED is set)" if ENV['COSMOS_QT6_REQUIRED']
  warn "cosmos/ext/qt6: #{reason}; skipping the Qt6 extension"
  File.write('Makefile', <<~MAKE)
    all:
    \t@echo "qt6 extension skipped (#{reason})"
    install:
    \t@true
    clean:
    \t@true
    static:
    \t@true
    install-so:
    \t@true
    install-rb:
    \t@true
  MAKE
  exit 0
end

# pkg-config's output, or '' when it or the module is missing. No shell:
# cmd.exe cannot redirect to /dev/null, and with `2>/dev/null` every query on
# Windows came back empty, so Qt6 was never found there.
def pkg_config(*args)
  IO.popen(['pkg-config', *args], err: File::NULL, &:read).to_s.chomp
rescue SystemCallError
  ''
end

# Returns [include_flags, link_flags, moc_path] or nil.
def locate_qt6
  if RUBY_PLATFORM =~ /darwin/
    prefix = `brew --prefix qt 2>/dev/null`.chomp
    return nil if prefix.empty? || !File.directory?(prefix)
    moc = File.join(prefix, 'share', 'qt', 'libexec', 'moc')
    return nil unless File.executable?(moc)
    # Homebrew ships Qt6 as macOS frameworks.
    inc = %w[QtCore QtGui QtWidgets QtOpenGL QtOpenGLWidgets]
          .map { |fw| " -I#{prefix}/lib/#{fw}.framework/Headers" }.join
    inc << " -F#{prefix}/lib"
    link = " -F#{prefix}/lib" \
           " -framework QtCore -framework QtGui -framework QtWidgets" \
           " -framework QtOpenGL -framework QtOpenGLWidgets"
    [inc, link, moc]
  else
    # Linux, and Windows with Qt 6 from MSYS2 (pacman mingw-w64-x86_64-qt6-base).
    mods = %w[Qt6Core Qt6Gui Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets]
    inc  = pkg_config('--cflags', *mods)
    link = pkg_config('--libs', *mods)
    return nil if inc.empty? || link.empty?
    # Qt 6's .pc files have no host_bins (Qt 5's did): moc is in libexecdir,
    # e.g. /usr/lib/qt6/libexec/moc on Ubuntu 24.04, and not on PATH. Looking
    # only at host_bins fell back to a bare `moc`, which failed, so the build
    # wrote the stub Makefile and "succeeded" with no GUI.
    # On Windows it is moc.exe (MSYS2: mingw64/share/qt6/bin).
    moc_name = "moc#{RbConfig::CONFIG['EXEEXT']}"
    moc = %w[libexecdir host_libexecs host_bins bindir].map do |var|
      dir = pkg_config("--variable=#{var}", 'Qt6Core')
      File.join(dir, moc_name) unless dir.empty?
    end.compact.find { |path| File.executable?(path) }
    [" #{inc}", " #{link}", moc || moc_name]
  end
end

found = locate_qt6
write_stub_makefile('Qt6 not found (brew --prefix qt / pkg-config Qt6Core)') unless found
inc, link, moc = found

$INCFLAGS << inc
# gnu++17 on MinGW: strict c++17 defines __STRICT_ANSI__, which hides C
# library functions Ruby's win32.h relies on.
$CXXFLAGS << (RUBY_PLATFORM =~ /mingw/ ? ' -std=gnu++17' : ' -std=c++17')
$CXXFLAGS << ' -Wno-register -Wno-unused-parameter'
$LDFLAGS  << link

# moc must run before create_makefile writes the file list. Regenerating on
# every configure is what keeps the meta-object in step with the headers.
{ 'rubycallback.h' => 'moc_rubycallback.cpp',
  'rubywidget.h'   => 'moc_rubywidget.cpp' }.each do |hdr, out|
  write_stub_makefile("moc failed on #{hdr}") unless system(moc, hdr, '-o', out)
end

# No $objs override: setting it makes mkmf reverse-derive the source list as
# C, which selects LDSHARED ($(CC)) instead of LDSHAREDXX ($(CXX)) and breaks
# linking on Linux with undefined operator new / __gxx_personality_v0.
create_makefile('cosmos/ext/qt6')
