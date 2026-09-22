require 'mkmf'

# `gem install cosmos` runs every extconf.rb in the gemspec, so aborting here
# would fail the entire install on any machine without Qt6. When Qt6 is absent
# we write a do-nothing Makefile instead and let lib/Qt.rb report a useful
# message if something actually tries to use the GUI.
def write_stub_makefile(reason)
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
    mods = 'Qt6Core Qt6Gui Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets'
    inc  = `pkg-config --cflags #{mods} 2>/dev/null`.chomp
    link = `pkg-config --libs   #{mods} 2>/dev/null`.chomp
    return nil if inc.empty? || link.empty?
    moc = `pkg-config --variable=host_bins Qt6Core 2>/dev/null`.chomp
    moc = File.join(moc, 'moc') unless moc.empty?
    moc = 'moc' unless !moc.empty? && File.executable?(moc)
    [" #{inc}", " #{link}", moc]
  end
end

found = locate_qt6
write_stub_makefile('Qt6 not found (brew --prefix qt / pkg-config Qt6Core)') unless found
inc, link, moc = found

$INCFLAGS << inc
$CXXFLAGS << ' -std=c++17 -Wno-register -Wno-unused-parameter'
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
