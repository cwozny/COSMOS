# One test per fixed defect, named for the defect. Each test states the failure
# it guards against, so a reintroduction reports what actually broke rather
# than just "something failed".
#
# Run: bundle exec ruby -Ilib ext/cosmos/ext/qt6/test_regressions.rb
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
require 'cosmos/ext/qt6'
require 'Qt'
require 'tmpdir'
require 'fileutils'

STDOUT.sync = true
$failures = []
def chk(name)
  ok = begin
    yield
  rescue Exception => e
    $failures << "#{name}: #{e.class}: #{e.message}"
    puts format('  %-52s FAIL (%s: %s)', name, e.class, e.message.to_s[0, 60])
    return
  end
  $failures << name unless ok
  puts format('  %-52s %s', name, ok ? 'ok' : 'FAIL')
end

WINDOWS = RUBY_PLATFORM =~ /mingw|mswin/
# A check this platform cannot run: reported, and not a failure.
def skip_chk(name, why)
  puts format('  %-52s skipped: %s', name, why)
end

APP = Qt::Application.new([])

puts "\n1. Qt::MessageBox had no instance side"
puts "   (no ctor, no methods -> ArgumentError at all 6 call sites, including"
puts "    exception_dialog.rb:44, so the error reporter itself crashed)"
chk('MessageBox.new(parent) constructs')   { Qt::MessageBox.new(nil).is_a?(Qt::MessageBox) }
chk('MessageBox descends from Qt::Dialog') { Qt::MessageBox.new(nil).is_a?(Qt::Dialog) }
chk('setIcon/setText/setWindowTitle') do
  m = Qt::MessageBox.new(nil)
  m.setIcon(Qt::MessageBox::Question); m.setText('b'); m.setWindowTitle('t')
  m.setInformativeText('i'); m.setDetailedText('d')
  m.setStandardButtons(Qt::MessageBox::Yes)
  m.setDefaultButton(Qt::MessageBox::Yes)
  true
end
chk('addButton(String, role) -> PushButton') do
  Qt::MessageBox.new(nil).addButton('Custom', Qt::MessageBox::AcceptRole).is_a?(Qt::PushButton)
end
chk('addButton(Button, role) accepts a widget') do
  m = Qt::MessageBox.new(nil); b = Qt::PushButton.new('Open')
  m.addButton(b, Qt::MessageBox::ResetRole).equal?(b)
end
chk('addButton(Button) hands ownership to Qt (no double free)') do
  # QMessageBox reparents and deletes the button. If the Ruby wrapper still
  # claims ownership, both sides free it and the process dies at teardown --
  # after every test has already printed "ok".
  m = Qt::MessageBox.new(nil)
  b = Qt::PushButton.new('Open')
  m.addButton(b, Qt::MessageBox::ResetRole)
  GC.start
  !b.destroyed? || true   # the assertion is that GC.start did not crash us
end
chk('clickedButton bound (nil before exec)') { Qt::MessageBox.new(nil).clickedButton.nil? }
chk('exec/dispose inherited from Dialog') do
  m = Qt::MessageBox.new(nil)
  Qt.single_shot(2) { m.accept }
  r = m.exec
  m.dispose
  r == Qt::Dialog::Accepted
end
chk('reopened exec -> method_missing(:exec) resolves') do
  # script_module_gui.rb:19 reopens exec and re-dispatches; must reach the C
  # binding, not recurse into the reopen.
  klass = Class.new(Qt::MessageBox) do
    def exec(*args); method_missing(:exec, *args); end
  end
  m = klass.new(nil)
  Qt.single_shot(2) { m.accept }
  m.exec == Qt::Dialog::Accepted
end

puts "\n2. Qt::Widget#raise shadowed Kernel#raise"
puts "   (Impl modules sit ahead of Kernel, so `raise Foo, 'm'` in any widget"
puts "    subclass became ArgumentError and a bare re-raise was swallowed."
puts "    Dispatching on $! replaced that with a worse bug: a no-arg"
puts "    widget.raise inside a rescue re-raised the exception being handled,"
puts "    and exception_dialog.rb:104 does exactly that -- so the first error"
puts "    inside a rescue killed every later error dialog in the process."
puts "    Nothing shadows Kernel#raise now; the Qt sense is spelled raise_.)"
class RaiseProbe < Qt::Widget
  def with_args;  raise ArgumentError, 'boom'; end
  def bare;       begin; raise 'inner'; rescue; raise; end; end
end
chk('raise with args reaches Kernel#raise') do
  begin; RaiseProbe.new.with_args; false
  rescue ArgumentError => e; e.message == 'boom'; end
end
chk('bare raise re-raises instead of swallowing') do
  begin; RaiseProbe.new.bare; false
  rescue RuntimeError => e; e.message == 'inner'; end
end
chk('widget.raise_ means QWidget::raise()') do
  Qt::Dialog.new.raise_
  true
end
chk('raise_ inside a rescue does not re-raise the handled exception') do
  begin
    raise 'original'
  rescue
    begin
      Qt::Widget.new.raise_
      true
    rescue
      false          # the $!-dispatching shim failed exactly here
    end
  end
end
chk('no widget class shadows Kernel#raise') do
  Qt::Widget.new.method(:raise).owner == Kernel
end

puts "\n3. closeEvent never reached Ruby"
puts "   (only Qt::Widget had a forwarding C++ subclass, and closeEvent was not"
puts "    among the virtuals it forwarded; 19 COSMOS files override it)"
def close_probe(base)
  Class.new(base) do
    attr_reader :closed
    def initialize(*a); super; @closed = false; end
    def closeEvent(_e); @closed = true; end
  end
end
chk('Qt::MainWindow#closeEvent fires (13 files)') do
  w = close_probe(Qt::MainWindow).new; w.show; w.close; w.closed
end
chk('Qt::Dialog#closeEvent fires (3 files)') do
  w = close_probe(Qt::Dialog).new; w.show; w.close; w.closed
end
chk('Qt::Widget#closeEvent fires') do
  w = close_probe(Qt::Widget).new; w.show; w.close; w.closed
end
chk('close() is bound and returns a boolean') do
  [true, false].include?(Qt::Widget.new.close)
end
chk('paintEvent still dispatches (no regression)') do
  k = Class.new(Qt::Widget) do
    attr_reader :painted
    def initialize(*a); super; @painted = false; end
    def paintEvent(_e); @painted = true; end
  end
  w = k.new; w.show; w.update; Qt::Application.processEvents; w.painted
end

puts "\n4. Ruby signal keys were not normalized"
puts "   (ruby_editor.rb declares 'font_changed(const QFont &)', "
puts "    script_runner_frame.rb connects 'font_changed(QFont)' -> different"
puts "    hash keys -> every emission silently dropped)"
chk("__sig('const QFont &') == __sig('QFont')") do
  Qt.__sig('font_changed(const QFont &)') == Qt.__sig('font_changed(QFont)')
end
chk('__sig normalizes pointer spacing') do
  Qt.__sig('key_pressed(QKeyEvent*)') == Qt.__sig('key_pressed(QKeyEvent *)')
end
chk('__sig still strips the SIGNAL() prefix') { Qt.__sig(SIGNAL('clicked()')) == 'clicked()' }
chk('emission crosses the two spellings') do
  k = Class.new(Qt::Widget) { signals 'font_changed(const QFont &)' }
  e = k.new
  got = 0
  e.connect(SIGNAL('font_changed(QFont)')) { got += 1 }
  e.emit e.font_changed(Qt::Font.new('Arial', 11))
  got == 1
end

puts "\n5. Exceptions in Ruby event handlers were discarded silently"
puts "   (ev_invoke_protected cleared errinfo with no report and still claimed"
puts "    the event was handled, so a broken paintEvent drew a blank widget"
puts "    with no diagnostic anywhere)"
chk('a raising paintEvent does not crash the process') do
  k = Class.new(Qt::Widget) { def paintEvent(_e); raise ArgumentError, 'probe explosion'; end }
  w = k.new; w.show; w.update
  Qt::Application.processEvents
  true   # reaching here at all is the assertion
end
chk('the exception is reported on stderr, naming the virtual') do
  # Capture in-process: forking a Qt+threads process to read its stderr is
  # itself unsafe. rb_stderr tracks the $stderr global, so swapping it works.
  require 'stringio'
  saved = $stderr
  $stderr = StringIO.new
  begin
    k = Class.new(Qt::Widget) { def paintEvent(_e); raise ArgumentError, 'probe explosion'; end }
    w = k.new; w.show; w.update
    Qt::Application.processEvents
    out = $stderr.string
  ensure
    $stderr = saved
  end
  out.include?('paintEvent') && out.include?('probe explosion')
end

puts "\n6. Constructor blocks were ignored"
puts "   (top_level.rb:335 builds an entire dialog in a block; ignoring it"
puts "    produced an empty untitled modal that then blocked exec forever)"
chk('block taking an argument is yielded the object') do
  Qt::Dialog.new { |box| box.setWindowTitle('Unexpected text output') }
    .windowTitle == 'Unexpected text output'
end
chk('block taking no argument is instance_eval-ed (7 sites)') do
  ok = Qt::PushButton.new('Ok')
  cancel = Qt::PushButton.new('Cancel')
  Qt::HBoxLayout.new { addWidget(ok); addWidget(cancel) }.count == 2
end
chk('constructing without a block is unaffected') { Qt::VBoxLayout.new.count == 0 }
chk('an exception inside the block propagates') do
  begin
    Qt::Dialog.new { |_b| raise ArgumentError, 'in block' }
    false
  rescue ArgumentError => e
    e.message == 'in block'
  end
end

puts "\n7. Signal arguments were dropped"
puts "   (only int/bool/QString/QModelIndex first-arguments were forwarded and"
puts "    everything after the first was discarded, so QPoint context menus got"
puts "    nil and cellEntered(int,int) lost the column)"
chk('int argument still delivered') do
  cb = Qt::ComboBox.new; cb.addItem('a'); cb.addItem('b')
  got = nil
  cb.connect(SIGNAL('currentIndexChanged(int)')) { |i| got = i }
  cb.setCurrentIndex(1)
  got == 1
end
chk('QString argument still delivered') do
  le = Qt::LineEdit.new
  got = nil
  le.connect(SIGNAL('textChanged(QString)')) { |v| got = v }
  le.setText('hello')
  got == 'hello'
end
chk('zero-argument signal still delivered') do
  pb = Qt::PushButton.new('go'); n = 0
  pb.connect(SIGNAL('clicked()')) { n += 1 }
  pb.click
  n == 1
end
chk('BOTH arguments of a two-arg signal arrive') do
  t = Qt::TableWidget.new; t.setRowCount(2); t.setColumnCount(2)
  r = c = nil
  t.connect(SIGNAL('cellChanged(int,int)')) { |a, b| r = a; c = b }
  t.setItem(1, 1, Qt::TableWidgetItem.new('x'))
  r == 1 && c == 1
end
chk('non-QObject item pointer converts (QTableWidgetItem*)') do
  t = Qt::TableWidget.new; t.setRowCount(1); t.setColumnCount(1)
  item = nil
  t.connect(SIGNAL('itemChanged(QTableWidgetItem*)')) { |i| item = i }
  t.setItem(0, 0, Qt::TableWidgetItem.new('cell'))
  item.is_a?(Qt::TableWidgetItem) && item.text == 'cell'
end
chk('QObject pointer converts (QAction*)') do
  m = Qt::Menu.new('File')
  act = nil
  m.connect(SIGNAL('triggered(QAction*)')) { |a| act = a }
  a1 = Qt::Action.new('Save'); m.addAction(a1); a1.trigger
  act.is_a?(Qt::Action) && act.text == 'Save'
end
chk('a QPoint-carrying signal connects without error') do
  # customContextMenuRequested(QPoint) cannot be emitted headlessly, so this
  # only proves the connection is accepted; the QPoint branch of the converter
  # shares its code path with the types exercised above.
  Qt::TableWidget.new.connect(SIGNAL('customContextMenuRequested(QPoint)')) { |_p| }
  true
end

puts "\n8. Background thread could not reach the GUI thread"
puts "   (CmdTlmServer sat at ~175% CPU and execute_in_main_thread called from"
puts "    a worker never returned, because the main thread never got idle"
puts "    enough to drain posted work)"
chk('execute_in_main_thread returns while exec() runs') do
  # The bug was that this never returned at all. Let exec_for expire on its
  # own rather than quitting from the worker, which races the timer.
  result = nil
  Thread.new { sleep 0.2; result = Qt.execute_in_main_thread(true) { 6 * 7 } }
  APP.exec_for(3_000)
  result == 42
end
chk('a delayed post is deferred, not run inline') do
  # packet_viewer.rb:366 passes delay_execution (the 3rd argument) to escape
  # the current call frame; running it inline recursed instead of unwinding.
  n = 0
  Qt.execute_in_main_thread(false, 0.001, true) { n += 1 }
  deferred = (n == 0)
  Qt::Application.processEvents
  t0 = Time.now
  Qt::Application.processEvents while n.zero? && (Time.now - t0) < 2
  deferred && n == 1
end

puts "\n9. The Qt6 binding was invisible to the build system"
puts "   (absent from gemspec extensions, the Rakefile list and Manifest.txt,"
puts "    so `gem install cosmos` shipped neither the shim nor the sources and"
puts "    never compiled them)"
ROOT = File.expand_path('../../../..', __dir__)
chk('gemspec builds the qt6 extension') do
  File.read(File.join(ROOT, 'cosmos.gemspec')).include?("ext/cosmos/ext/qt6/extconf.rb")
end
chk('Rakefile compiles qt6 with the others') do
  File.read(File.join(ROOT, 'Rakefile')) =~ /^\s*'qt6'\]/
end
chk('Manifest.txt ships every qt6 source') do
  m = File.read(File.join(ROOT, 'Manifest.txt')).split("\n")
  %w[cosmos_qt6.cpp extconf.rb rubycallback.cpp rubycallback.h
     rubywidget.cpp rubywidget.h].all? { |f| m.include?("ext/cosmos/ext/qt6/#{f}") }
end
chk('Manifest.txt ships lib/Qt.rb') do
  File.read(File.join(ROOT, 'Manifest.txt')).split("\n").include?('lib/Qt.rb')
end
chk('the replaced qtbindings dependency is gone') do
  !File.read(File.join(ROOT, 'cosmos.gemspec')).include?("add_runtime_dependency 'qtbindings'")
end
chk('build output is git-ignored, not shippable') do
  ig = File.read(File.join(__dir__, '.gitignore'))
  %w[*.o *.bundle Makefile moc_*.cpp].all? { |pat| ig.include?(pat) }
end
chk('extconf does not hardcode a macOS-only path as fatal') do
  # It must degrade to a stub Makefile rather than abort, or one missing Qt6
  # fails the whole `gem install cosmos`.
  e = File.read(File.join(__dir__, 'extconf.rb'))
  e.include?('write_stub_makefile') && !e.include?('abort "Qt6 not found')
end
chk('extconf lets mkmf pick the C++ linker') do
  # An explicit $objs makes mkmf derive the sources as C and link with $(CC),
  # which fails on Linux with undefined operator new.
  !File.read(File.join(__dir__, 'extconf.rb')).include?('$objs =')
end

puts "\n10. Qt::FontMetrics#size was unbound"
puts "   (scroll_text_dialog.rb:35 raised NoMethodError AFTER parenting its"
puts "    TextEdit but BEFORE setLayout, leaving a dialog whose child had no"
puts "    geometry -- a blank pane. That dialog is what COSMOS shows for any"
puts "    unexpected stderr output, so it appeared whenever anything failed.)"
chk('FontMetrics#size(flags, text) returns a Qt::Size') do
  te = Qt::TextEdit.new('hello world')
  sz = Qt::FontMetrics.new(te.document.defaultFont).size(0, "hello world\nsecond line")
  sz.is_a?(Qt::Size) && sz.width > 0 && sz.height > 0
end
chk('a dialog built that way ends up with a layout') do
  # The scroll_text_dialog.rb shape: parent the child first, size it with
  # FontMetrics, then install the layout.
  d = Qt::Dialog.new
  lay = Qt::VBoxLayout.new
  te = Qt::TextEdit.new('body text', d)
  lay.addWidget(te)
  sz = Qt::FontMetrics.new(te.document.defaultFont).size(0, 'body text')
  te.setMinimumSize(sz.width + 30, sz.height + 30)
  d.layout = lay
  !d.layout.nil? && d.findChildren.any? { |c| c.is_a?(Qt::TextEdit) }
end

puts "\n11. Constructor arguments were discarded by ctor_plain"
puts "   (Qt::Splitter.new(Qt::Vertical, parent) came out HORIZONTAL, which put"
puts "    CmdTlmServer's console pane on the right instead of along the bottom;"
puts "    6 splitter sites and Qt::Slider.new(Qt::Horizontal) were affected)"
chk('Splitter honours Qt::Vertical') do
  Qt::Splitter.new(Qt::Vertical, Qt::Widget.new).orientation == Qt::Vertical
end
chk('Splitter honours Qt::Horizontal') do
  Qt::Splitter.new(Qt::Horizontal, Qt::Widget.new).orientation == Qt::Horizontal
end
chk('Splitter.new(parent) keeps Qt default (horizontal)') do
  Qt::Splitter.new(Qt::Widget.new).orientation == Qt::Horizontal
end
chk('Slider honours Qt::Horizontal') { Qt::Slider.new(Qt::Horizontal).orientation == Qt::Horizontal }
chk('Slider.new keeps Qt default (vertical)')  { Qt::Slider.new.orientation == Qt::Vertical }

puts "\n12. QFormLayout::itemAt(row, role) was unbound"
puts "   (only the 1-arg QLayout::itemAt existed, so logging_tab.rb:53 raised"
puts "    ArgumentError every time the Logging tab refreshed)"
chk('itemAt(row, role) returns the field item') do
  fl = Qt::FormLayout.new
  fl.addRow('Logging Enabled:', Qt::Label.new('true'))
  fl.addRow('Queue Size:', Qt::Label.new('0'))
  Qt::Widget.new.layout = fl
  fl.itemAt(1, Qt::FormLayout::FieldRole).widget.text == '0'
end
chk('1-arg itemAt still works') { !Qt::FormLayout.new.tap { |f| f.addRow('a', Qt::Label.new('b')) }.itemAt(0).nil? }
chk('wrong arity reports clearly') do
  begin
    Qt::FormLayout.new.itemAt(1, 2, 3); false
  rescue ArgumentError => e
    e.message.include?('1 or 2')
  end
end

puts "\n13. Qt::CoreApplication#exit was unbound"
puts "   (it fell through to private Kernel#exit -> NoMethodError, inside"
puts "    exception_dialog.rb:111, i.e. only once something had already failed)"
chk('Application#exit(code) is a bound public method') do
  APP.respond_to?(:exit) && APP.method(:exit).owner != Kernel
end

puts "\n14. Dark mode made light-assuming stylesheets invisible"
puts "   (legal_dialog.rb:53 sets background-color white with no text colour;"
puts "    Qt4 had no dark-mode support, Qt6 follows the system and painted"
puts "    white on white -- the Legal Agreement pane looked empty)"
chk('the app defaults to the light colour scheme') do
  # Render white-on-default text over the stylesheet COSMOS actually uses and
  # confirm the result is not a single flat colour.
  l = Qt::Label.new('Copyright 2017 Ball Aerospace & Technologies Corp.')
  l.setStyleSheet('QLabel { background-color : white; padding: 5px; }')
  l.resize(420, 40)
  l.show
  Qt::Application.processEvents
  png = File.join(Dir.tmpdir, "_regr_label_#{Process.pid}.png")   # no /tmp on Windows
  l.grab.save(png)
  data = File.binread(png)
  # A blank (all-white) label compresses to a much smaller PNG than one with
  # glyphs on it; 1.5 KB is far above an empty 420x40 fill and far below text.
  ok = data.bytesize > 1500
  File.delete(png) rescue nil
  ok
end

puts "\n15. Widgets fetched back out of Qt came back as bare Qt::Widget"
puts "   (best_ruby_class existed but was never called, so anything reached by"
puts "    introspection lost its real class -- logging_tab.rb:53 pulls a label"
puts "    out of a form layout and calls setText on it -> NoMethodError)"
chk('layout item resolves to the concrete class') do
  fl = Qt::FormLayout.new
  fl.addRow('Logging Enabled:', Qt::Label.new('true'))
  Qt::Widget.new.layout = fl
  w = fl.itemAt(0, Qt::FormLayout::FieldRole).widget
  w.is_a?(Qt::Label) && w.text == 'true'
end
chk('setText works on the retrieved widget') do
  fl = Qt::FormLayout.new
  fl.addRow('Queue:', Qt::Label.new('0'))
  Qt::Widget.new.layout = fl
  w = fl.itemAt(0, Qt::FormLayout::FieldRole).widget
  w.setText('7')
  w.text == '7'
end
chk('table cellWidget resolves to the concrete class') do
  # status_tab.rb:250 does cellWidget(row, 3).setText(text)
  t = Qt::TableWidget.new
  t.setRowCount(1); t.setColumnCount(4)
  t.setCellWidget(0, 3, Qt::PushButton.new('START'))
  b = t.cellWidget(0, 3)
  b.is_a?(Qt::PushButton) && b.text == 'START'
end
chk('findChildren returns concrete classes') do
  w = Qt::Widget.new
  lay = Qt::VBoxLayout.new(w)
  lay.addWidget(Qt::PushButton.new('Go'))
  lay.addWidget(Qt::Label.new('Hi'))
  kinds = w.findChildren.map { |c| c.class.name }
  kinds.include?('Qt::PushButton') && kinds.include?('Qt::Label')
end

puts "\n16. QAbstractItemView#indexAt was unbound"
puts "   (status_tab.rb:262 maps a cell widget's position back to its row to"
puts "    decide which background task a START/STOP button belongs to)"
chk('indexAt(point) maps a cell widget back to its row') do
  t = Qt::TableWidget.new
  t.setRowCount(3); t.setColumnCount(4)
  3.times { |r| t.setCellWidget(r, 3, Qt::PushButton.new('START')) }
  t.resize(500, 200); t.show
  Qt::Application.processEvents
  idx = t.indexAt(t.cellWidget(1, 3).pos)
  idx.isValid && idx.row == 1 && idx.column == 3
end
chk('rowAt / columnAt are bound') do
  t = Qt::TableWidget.new
  t.setRowCount(2); t.setColumnCount(2)
  t.resize(200, 100); t.show
  Qt::Application.processEvents
  t.rowAt(5).is_a?(Integer) && t.columnAt(5).is_a?(Integer)
end
chk('contained slot errors reach COSMOS_QT6_ERRLOG') do
  # COSMOS redirects $stderr into its GUI pane, so the stderr report alone is
  # invisible to logs -- which is how the indexAt failure escaped my own
  # click-through test. The bypass must work independently of $stderr.
  require 'stringio'
  path = File.join(Dir.tmpdir, "cosmos_errlog_#{Process.pid}.txt")
  File.delete(path) rescue nil
  ENV['COSMOS_QT6_ERRLOG'] = path
  saved = $stderr
  $stderr = StringIO.new
  begin
    b = Qt::PushButton.new('probe')
    b.connect(SIGNAL('clicked()')) { raise ArgumentError, 'errlog probe' }
    b.click
  ensure
    $stderr = saved
    ENV.delete('COSMOS_QT6_ERRLOG')
  end
  ok = File.exist?(path) && File.read(path).include?('errlog probe')
  File.delete(path) rescue nil
  ok
end

puts "\n17. QFormLayout#addRow silently dropped a layout field"
puts "   (addRow has widget and layout overloads; casting a QLayout to"
puts "    QWidget* gives NULL and Qt adds an EMPTY field with no error, so the"
puts "    four buttons in logging_tab.rb:174 never appeared. Caught by diffing"
puts "    widget counts against the Qt 4.8.7 reference: 10 buttons vs 6.)"
chk('addRow(label, layout) keeps the buttons') do
  f = Qt::FormLayout.new
  row = Qt::HBoxLayout.new
  %w[Start\ Cmd Start\ Tlm Stop\ Cmd Stop\ Tlm].each { |t| row.addWidget(Qt::PushButton.new(t)) }
  f.addRow('Actions:', row)
  host = Qt::Widget.new
  host.layout = f
  host.findChildren.count { |c| c.is_a?(Qt::PushButton) } == 4
end
chk('addRow(label, widget) still works') do
  f = Qt::FormLayout.new
  f.addRow('Interfaces:', Qt::Label.new('INST_INT'))
  host = Qt::Widget.new
  host.layout = f
  host.findChildren.any? { |c| c.is_a?(Qt::Label) && c.text == 'INST_INT' }
end
chk('addRow rejects a non-widget, non-layout clearly') do
  begin
    Qt::FormLayout.new.addRow('x', Qt::Font.new('Arial', 10)); false
  rescue StandardError => e
    true   # any clear failure beats silently dropping the field
  end
end

puts "\n18. currentCharFormat / setCurrentCharFormat were unbound"
puts "   (interface_raw_dialog.rb:75 reads the char format, sets a monospaced"
puts "    font on it and writes it back -- how every raw-data pane is styled)"
chk('PlainTextEdit char format round-trips a font') do
  te = Qt::PlainTextEdit.new
  fmt = te.currentCharFormat
  fmt.setFont(Qt::Font.new('Courier', 14))
  te.setCurrentCharFormat(fmt)
  f = te.currentCharFormat.font
  fmt.is_a?(Qt::TextCharFormat) && f.family == 'Courier' && f.pointSize == 14
end
chk('TextEdit has the same accessors') do
  te = Qt::TextEdit.new
  fmt = te.currentCharFormat
  fmt.setFont(Qt::Font.new('Courier', 10))
  te.setCurrentCharFormat(fmt)
  te.currentCharFormat.font.pointSize == 10
end

puts "\n19. Unbound Qt methods (the whole class of click-time NoMethodError)"
puts "   (every error reported from real use lately -- FontMetrics#size,"
puts "    itemAt(row,role), CoreApplication#exit, indexAt, currentCharFormat --"
puts "    was a Qt method COSMOS calls that was never bound. scan_unbound.rb"
puts "    finds them statically instead of waiting for a click.)"
UNBOUND_BUDGET = 0    # ratchet: lower this as they get bound, never raise it
chk("no more than #{UNBOUND_BUDGET} unbound Qt calls remain") do
  out = `ruby -rset #{File.join(__dir__, 'scan_unbound.rb').inspect} 2>&1`
  total = out[/TOTAL (\d+)/, 1]
  n = total.to_i
  puts "    (scanner reports #{n} unbound; budget #{UNBOUND_BUDGET})"
  # total must be present: the scanner used to die on Array#to_set with no
  # require 'set', and a scanner that cannot run must not pass silently.
  !total.nil? && n <= UNBOUND_BUDGET
end
chk('the 6 that needed infrastructure are now bound') do
  # These needed more than a QDEF: real event objects instead of Qnil
  # (hasUrls / acceptProposedAction / setAccepted), the QStyleOption structs
  # (closeEditor / drawControl) and a QSyntaxHighlighter subclass (setFormat).
  Qt::MimeData.method_defined?(:hasUrls) &&
    Qt::DropEvent.method_defined?(:acceptProposedAction) &&
    Qt::Event.method_defined?(:setAccepted) &&
    Qt::StyledItemDelegate.method_defined?(:closeEditor) &&
    Qt::Application.style.respond_to?(:drawControl) &&
    Qt::SyntaxHighlighter.method_defined?(:setFormat)
end
chk('methods bound from the last sweep still resolve') do
  t = Qt::TableWidget.new; t.setRowCount(2); t.setColumnCount(2)
  t.setItem(0, 0, Qt::TableWidgetItem.new('x'))
  w = Qt::Widget.new
  [-> { w.adjustSize }, -> { w.hasFocus }, -> { w.minimumHeight },
   -> { w.setGeometry(0, 0, 10, 10) }, -> { w.contentsRect },
   -> { Qt::Font.new('Arial', 10).setFamily('Courier') },
   -> { t.horizontalHeaderItem(0) }, -> { t.selectRow(0) },
   -> { t.item(0, 0).textColor }, -> { Qt::ComboBox.new.maxCount },
   -> { Qt::Slider.new.sliderPosition }].all? { |f| f.call; true }
end

puts "\n21. `super` inside an event handler had nothing to call"
puts "   (25 COSMOS event overrides call super; the forwarding C++ subclasses"
puts "    dispatched INTO Ruby but no bound method existed to dispatch back"
puts "    OUT, so interface_raw_dialog.rb:132 raised on closing View Raw)"
def super_probe(base, event_name)
  Class.new(base) do
    attr_reader :ran, :supered
    define_method(:initialize) { |*a| super(*a); @ran = false; @supered = false }
    define_method(event_name) do |event|
      super(event)
      @supered = true
      @ran = true
    end
  end
end
chk('Dialog#closeEvent can call super(event)') do
  d = super_probe(Qt::Dialog, :closeEvent).new
  d.show; d.close
  d.ran && d.supered
end
chk('MainWindow#closeEvent can call super(event)') do
  m = super_probe(Qt::MainWindow, :closeEvent).new
  m.show; m.close
  m.ran && m.supered
end
chk('Widget#paintEvent can call super(event)') do
  w = super_probe(Qt::Widget, :paintEvent).new
  w.show; w.update; Qt::Application.processEvents
  w.ran && w.supered
end
chk('an override that does NOT call super still runs') do
  k = Class.new(Qt::Widget) do
    attr_reader :hit
    def initialize(*a); super; @hit = false; end
    def closeEvent(_e); @hit = true; end
  end
  w = k.new; w.show; w.close
  w.hit
end

puts "\n22. `super` from a reopened CLASS method, and Variant geometry"
puts "   (script_module_gui.rb:28 reopens `def self.critical` and calls super;"
puts "    rb_define_singleton_method put the original directly in the singleton"
puts "    class, where the reopen replaced it. qt_tool.rb:255 stores window"
puts "    geometry as Variant.new(pos()), which fell through to a String cast.)"
chk('reopened self.critical can call super (5 args)') do
  ran = false
  klass = Class.new(Qt::MessageBox)
  klass.define_singleton_method(:critical) do |parent, title, text, buttons = Qt::MessageBox::Ok, dflt = Qt::MessageBox::NoButton|
    ran = true
    Qt::MessageBox.critical(parent, title, text, buttons, dflt)
  end
  Qt.single_shot(30) { Qt::Application.topLevelWidgets.each { |w| w.close if w.is_a?(Qt::Dialog) } }
  klass.critical(nil, 'T', 'body')
  ran
end
chk('MessageBox statics accept the 5-arg COSMOS form') do
  Qt.single_shot(30) { Qt::Application.topLevelWidgets.each { |w| w.close if w.is_a?(Qt::Dialog) } }
  Qt::MessageBox.warning(nil, 'T', 'b', Qt::MessageBox::Ok, Qt::MessageBox::NoButton)
  true
end
chk('Variant round-trips a Point') do
  w = Qt::Widget.new; w.move(30, 40)
  p = Qt::Variant.new(w.pos).toPoint
  p.x == 30 && p.y == 40
end
chk('Variant round-trips a Size') do
  w = Qt::Widget.new; w.resize(640, 480)
  sz = Qt::Variant.new(w.size).toSize
  sz.width == 640 && sz.height == 480
end

puts "\n23. Ruby-style predicates did not reach Qt boolean getters"
puts "   (qtbindings exposed QAction::isChecked as `checked?`; COSMOS writes"
puts "    it that way, and the camelCase-only scanner never looked for it --"
puts "    which is how `checked?` reached a user from Command Sender)"
chk('action.checked? maps to isChecked') do
  a = Qt::Action.new('Go'); a.setCheckable(true); a.setChecked(true)
  a.checked? == true
end
chk('predicates work across classes') do
  a = Qt::Action.new('Go'); a.setCheckable(true)
  w = Qt::Widget.new
  a.checkable? && a.enabled? && w.enabled? && w.visible? == false
end
chk('respond_to? agrees with method_missing') do
  Qt::Action.new('Go').respond_to?(:checked?)
end
chk('disposed? is bound (COSMOS spelling of destroyed?)') do
  w = Qt::Widget.new
  before = w.disposed?
  w.dispose
  before == false && w.disposed? == true
end
chk('the scanner now checks predicate spellings too') do
  src = File.read(File.join(__dir__, 'scan_unbound.rb'))
  src.include?('predicate_ok?') && src.include?('RUBY_PREDICATES')
end

puts "\n24. dispose on value types, and event forwarding beyond Qt::Widget"
puts "   (qtbindings gave Point/Size/Font a dispose; ours are GC-managed so it"
puts "    must be a harmless no-op -- full_text_search_line_edit.rb:145."
puts "    And only 5 classes had forwarding subclasses, so event overrides on"
puts "    Qt::LineEdit, ComboBox, Label, ListWidget, TextEdit, PlainTextEdit,"
puts "    Frame and TreeWidget silently never fired.)"
chk('value types accept dispose as a no-op') do
  [Qt::Point.new(0, 20), Qt::Size.new(1, 2),
   Qt::Font.new('Arial', 9), Qt::Color.new(1, 2, 3)].all? { |v| v.dispose.nil? }
end
chk('Qt::Base#dispose is NOT shadowed by the no-op') do
  w = Qt::Widget.new
  w.dispose
  w.disposed?      # must really be destroyed, not a silent no-op
end
chk('paintEvent reaches subclasses of 7 more classes') do
  bad = []
  { 'Label' => Qt::Label, 'Frame' => Qt::Frame, 'LineEdit' => Qt::LineEdit,
    'ListWidget' => Qt::ListWidget, 'TextEdit' => Qt::TextEdit,
    'PlainTextEdit' => Qt::PlainTextEdit, 'ComboBox' => Qt::ComboBox }.each do |name, base|
    k = Class.new(base) do
      attr_reader :painted
      def initialize(*a); super; @painted = false; end
      def paintEvent(_e); @painted = true; end
    end
    w = k.new
    w.resize(50, 20); w.show; w.update
    Qt::Application.processEvents
    bad << name unless w.painted
  end
  puts "    (not fired for: #{bad.join(', ')})" unless bad.empty?
  bad.empty?
end
chk('super() works through the forwarding template') do
  k = Class.new(Qt::Label) do
    attr_reader :ran
    def initialize(*a); super; @ran = false; end
    def paintEvent(e); super(e); @ran = true; end
  end
  l = k.new('hi'); l.resize(40, 20); l.show; l.update
  Qt::Application.processEvents
  l.ran
end

puts "\n25. Found by the UI exerciser sweep"
puts "   (a widget-tree walker that clicks every button, steps every combo box"
puts "    and triggers every action; these surfaced without anyone clicking)"
chk('binary strings with NUL bytes do not raise') do
  # COSMOS telemetry is ascii-8bit; StringValueCStr rejected embedded NULs,
  # crashing PacketViewer on any binary item (packet_viewer.rb:536). This is
  # on the path of EVERY string the binding converts.
  bin = "AB\x00CD\x00".dup.force_encoding('ASCII-8BIT')
  t = Qt::TableWidget.new; t.setRowCount(1); t.setColumnCount(1)
  t.setItem(0, 0, Qt::TableWidgetItem.new('x'))
  t.item(0, 0).setText(bin)
  Qt::Label.new(bin)
  Qt::LineEdit.new.setText(bin)
  Qt::Widget.new.setWindowTitle(bin)
  Qt::Label.new('hello').text == 'hello'   # plain strings still fine
end
chk('Qt::Date is a real class with a 3-arg ctor') do
  # It was declared, and rb_define_class_under was called with the return
  # value DISCARDED -- so Qt::Date existed but cDate stayed 0 and
  # Qt::Date.new(y,m,d) hit Object#initialize (calendar_dialog.rb:44).
  d = Qt::Date.new(2026, 9, 21)
  cal = Qt::CalendarWidget.new
  cal.setSelectedDate(d)
  d.year == 2026 && d.month == 9 && d.day == 21 && cal.selectedDate.year == 2026
end
chk('Variant#value returns typed values') do
  w = Qt::Widget.new; w.move(11, 22)
  Qt::Variant.new(w.pos).value.x == 11 &&
    Qt::Variant.new(42).value == 42 &&
    Qt::Variant.new('hi').value == 'hi'
end
chk('ListWidgetItem#text / setText') do
  lw = Qt::ListWidget.new; lw.addItem('ITEM_A')
  lw.item(0).setText('ITEM_B')
  lw.item(0).text == 'ITEM_B'
end
chk('CheckBox#setCheckState / checkState') do
  cb = Qt::CheckBox.new('opt')
  cb.setCheckState(2)
  cb.checkState == 2
end
chk('ComboBox#removeItem (not Layout#removeItem)') do
  cb = Qt::ComboBox.new
  %w[A B C].each { |t| cb.addItem(t) }
  (0...cb.count).to_a.reverse_each { |i| cb.removeItem(i) }
  cb.count.zero?
end

puts "\n26. Second sweep: ScriptRunner, CmdSequence, ConfigEditor, TestRunner"
chk('Widget#clearFocus / clear_focus / scroll / rect') do
  b = Qt::PushButton.new('b'); b.clearFocus; b.clear_focus
  w = Qt::Widget.new; w.resize(30, 20); w.scroll(1, 2)
  w.rect.width == 30
end
chk('Widget#actions lists a menu\'s actions') do
  m = Qt::Menu.new('F'); m.addAction(Qt::Action.new('A'))
  m.actions.size == 1
end
chk('Application#desktop works as an instance method') do
  Qt::Application.instance.desktop.width > 0
end
chk('paste/copy/cut on all three editors') do
  [Qt::PlainTextEdit.new('x'), Qt::TextEdit.new('x'), Qt::LineEdit.new].all? do |e|
    e.paste; e.copy; e.cut; true
  end
end
chk('TextEdit#moveCursor (was PlainTextEdit-only)') do
  Qt::TextEdit.new('abc').moveCursor(0)
  true
end
chk('cursor.selection.toPlainText') do
  e = Qt::PlainTextEdit.new('hello world'); e.selectAll
  e.textCursor.selection.toPlainText.include?('hello')
end
chk('document() wraps as Qt::TextDocument, not Qt::Object') do
  # isModified was bound all along; the receiver wrapped generically because
  # QTextDocument was missing from the class registry (config_editor_frame.rb:142).
  d = Qt::PlainTextEdit.new('x').document
  d.is_a?(Qt::TextDocument) && [true, false].include?(d.isModified)
end
chk('addRow(labelWidget, field) as well as addRow(String, field)') do
  f = Qt::FormLayout.new
  f.addRow(Qt::Label.new('Find:'), Qt::LineEdit.new)   # find_replace_dialog.rb:111
  f.addRow('Replace:', Qt::LineEdit.new)
  host = Qt::Widget.new; host.layout = f
  host.findChildren.count { |c| c.is_a?(Qt::LineEdit) } == 2
end

puts "\n27. A method bound on one class but called on another"
puts "   (scan_unbound.rb matches names, not receiver types, so setWordWrap"
puts "    being bound on Qt::Label made a Qt::TableWidget call look bound."
puts "    Cmd Sender raised on it at cmd_params.rb:238. Same shape as"
puts "    Qt::Layout#removeItem vs Qt::ComboBox#removeItem earlier.)"
chk('Qt::TableWidget#setWordWrap') do
  Qt::TableWidget.new.setWordWrap(true)
  true
end
chk('Qt::Label#setWordWrap still works') do
  Qt::Label.new('x').setWordWrap(true)
  true
end
chk('cmd_params.rb create_table sequence runs end to end') do
  t = Qt::TableWidget.new
  t.setSizePolicy(Qt::SizePolicy::Expanding, Qt::SizePolicy::Expanding)
  t.setWordWrap(true)
  t.setRowCount(2)
  t.setColumnCount(5)
  t.setHorizontalHeaderLabels(['Name', 'Value', '', 'Units', 'Description'])
  t.horizontalHeader.setStretchLastSection(true)
  true
end

puts "\n28. Found by the class-aware pass of scan_unbound.rb"
puts "   (each of these was bound on some other class, so the name-only"
puts "    sweep reported zero while the call site would raise)"
chk('Qt::ComboBox#maxCount=')          { Qt::ComboBox.new.maxCount = 5; true }
chk('Qt::Image#rect')                  { Qt::Image.method_defined?(:rect) }
chk('Qt::Label#setMargin')             { Qt::Label.new('x').setMargin(11); true }
chk('Qt::ListWidgetItem#setData')      { Qt::ListWidgetItem.new('x').respond_to?(:setData) }
chk('Qt::Point#x= and #y=')            { p = Qt::Point.new(1, 1); p.x = 3; p.y = 4; [p.x, p.y] == [3, 4] }
chk('Qt::Shortcut#activated')          { Qt::Shortcut.new(Qt::KeySequence.new('F5'), Qt::Widget.new).respond_to?(:activated) }
chk('Qt::TabWidget#current == currentIndex') do
  t = Qt::TabWidget.new
  t.current == t.currentIndex
end
chk('Qt::TreeWidgetItem#setCheckState') { Qt::TreeWidgetItem.new('x').setCheckState(0, 0); true }
chk('Qt::VBoxLayout#spacing')          { Qt::VBoxLayout.new.spacing.is_a?(Integer) }
chk('Qt::ActionGroup#addAction still resolves') do
  g = Qt::ActionGroup.new(nil)
  g.addAction(Qt::Action.new('a'))
  true
end

puts "\n29. Qt::Palette.new(palette) raised TypeError"
puts "   (every non-Integer argument went down the QColor path. ScriptRunner's"
puts "    Toggle Disconnect copies RED_PALETTE at script_runner_frame.rb:1198,"
puts "    after the status bar already says 'Targets disconnected' and before"
puts "    set_disconnected_targets runs, so scripts kept commanding the real"
puts "    targets)"
red_palette = Qt::Palette.new(Qt::red)   # qt.rb:236 RED_PALETTE
def window_pixel(palette)
  w = Qt::Widget.new
  w.setAutoFillBackground(true)
  w.setPalette(palette)
  w.resize(8, 8)
  w.grab.toImage.pixelColor(4, 4)
end
chk('Palette.new(palette) returns a Qt::Palette') do
  Qt::Palette.new(red_palette).is_a?(Qt::Palette)
end
chk('the copy keeps the source colours') do
  window_pixel(Qt::Palette.new(red_palette)) == Qt::Color.new(255, 0, 0)
end
chk('the copy is independent of the source') do
  copy = Qt::Palette.new(red_palette)
  copy.setColor(Qt::Palette::Window, Qt::Color.new(0, 255, 0))
  window_pixel(red_palette) == Qt::Color.new(255, 0, 0) &&
    window_pixel(copy) == Qt::Color.new(0, 255, 0)
end

puts "\n30. TableWidgetItem#row/#column/#flags and TableWidget#itemAt were unbound"
puts "   (CmdSender's itemChanged handler died on item.row at cmd_params.rb:337"
puts "    before it set the state column to MANUALLY, so a raw value typed for"
puts "    a state parameter was dropped and the displayed state was sent."
puts "    item.flags is the itemClicked handler (:251), itemAt the context"
puts "    menu (:163).)"
table = Qt::TableWidget.new
table.setRowCount(2)
table.setColumnCount(3)
cells = {}
2.times { |r| 3.times { |c| table.setItem(r, c, cells[[r, c]] = Qt::TableWidgetItem.new("#{r},#{c}")) } }
chk('TableWidgetItem#row and #column') { [cells[[1, 2]].row, cells[[1, 2]].column] == [1, 2] }
chk('row and column are -1 outside a table') do
  item = Qt::TableWidgetItem.new('x')
  [item.row, item.column] == [-1, -1]
end
chk('row and column of the item itemChanged passes') do
  seen = nil
  table.connect(SIGNAL('itemChanged(QTableWidgetItem*)')) { |item| seen = [item.row, item.column] }
  cells[[1, 1]].setText('edited')
  seen == [1, 1]
end
chk('TableWidgetItem#flags returns what setFlags set') do
  item = Qt::TableWidgetItem.new('x')
  item.setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled)
  item.flags == (Qt::ItemIsSelectable | Qt::ItemIsEnabled) &&
    (item.flags & Qt::ItemIsEditable) == 0
end
chk('TableWidget#itemAt(point) returns the cell under it') do
  table.resize(400, 200)
  table.show
  APP.processEvents
  item = table.itemAt(Qt::Point.new(5, 5))
  item && item.text == '0,0' && [item.row, item.column] == [0, 0]
end
chk('TableWidget#itemAt past the last cell is nil') { table.itemAt(Qt::Point.new(390, 150)).nil? }
table.hide

puts "\n31. QTabWidget#tabBar came back as a plain Qt::Widget"
puts "   (there was no Qt::TabBar for best_ruby_class to pick, so ScriptRunner's"
puts "    run_callback died on tabBar.setTabIcon (script_runner.rb:754) before"
puts "    a script could start, and the tab context menus in ScriptRunner,"
puts "    ConfigEditor, DataViewer and TlmGrapher died on tabBar.count/tabRect)"
tabs = Qt::TabWidget.new
tabs.addTab(Qt::Widget.new, 'one')
tabs.addTab(Qt::Widget.new, 'two')
chk('tabBar is a Qt::TabBar') { tabs.tabBar.is_a?(Qt::TabBar) }
chk('tabBar.count') { tabs.tabBar.count == 2 }
chk('tabBar.setTabIcon sets the icon of that tab only') do
  tabs.tabBar.setTabIcon(1, Qt::Icon.new(Qt::Pixmap.new(8, 8)))
  !tabs.tabBar.tabIcon(1).isNull && tabs.tabBar.tabIcon(0).isNull
end
chk('tabBar.setTabIcon(i, Qt::Icon.new) clears it') do
  tabs.tabBar.setTabIcon(1, Qt::Icon.new)   # script_runner.rb:766 (@no_icon)
  tabs.tabBar.tabIcon(1).isNull
end
chk('tabBar.tabRect(i).contains(point) finds the tab') do
  tabs.resize(300, 200)
  tabs.show
  APP.processEvents
  r = tabs.tabBar.tabRect(1)
  point = Qt::Point.new(r.x + r.width / 2, r.y + 2)
  (0...tabs.tabBar.count).find { |i| tabs.tabBar.tabRect(i).contains(point) } == 1
end
chk('tabBar.isTabEnabled follows setTabEnabled') do
  tabs.setTabEnabled(0, false)
  result = !tabs.tabBar.isTabEnabled(0) && tabs.tabBar.isTabEnabled(1)
  tabs.setTabEnabled(0, true)
  result
end
tabs.hide

puts "\n32. Qt::Dialog#done was unbound"
puts "   (ProgressDialog#close_done calls done(0) at progress_dialog.rb:162."
puts "    Worker threads got the NoMethodError back from"
puts "    execute_in_main_thread and showed 'Error During Progress' --"
puts "    ScriptRunner instruments every script that way"
puts "    (script_runner_frame.rb:551) -- and the Done button did nothing."
puts "    Also about_dialog.rb:110/150 and limits_monitor.rb:802. These checks"
puts "    avoid exec: see test_cosmos_tools.rb section 4 for the modal path.)"
chk('Dialog#done hides a shown dialog') do
  d = Qt::Dialog.new
  d.show
  d.done(0)
  result = !d.isVisible
  d.dispose
  result
end
chk('Dialog#done from a Ruby subclass (ProgressDialog shape)') do
  klass = Class.new(Qt::Dialog) { def close_done; done(0) unless disposed?; end }
  d = klass.new
  d.show
  d.close_done
  result = !d.isVisible
  d.dispose
  result
end

puts "\n33. Qt::TextCharFormat#setBackground was unbound"
puts "   (ScriptRunner and TestRunner highlight each line before running it --"
puts "    script_runner_frame.rb:658-660 -> completion_text_edit.rb:174 -- so"
puts "    every script raised NoMethodError before its first line ran)"
palegreen = Qt::Color.new(152, 251, 152)
chk('setBackground(Brush)') { Qt::TextCharFormat.new.setBackground(Qt::Brush.new(palegreen)); true }
chk('setBackground(Color)') { Qt::TextCharFormat.new.setBackground(palegreen); true }
chk('the highlight_line sequence paints the current line') do
  # completion_text_edit.rb:45-47 and :172-178
  editor = Qt::PlainTextEdit.new
  editor.setPlainText("puts 1\nputs 2\n")
  editor.resize(240, 80)
  editor.show
  APP.processEvents
  format = Qt::TextCharFormat.new
  format.setProperty(Qt::TextFormat::FullWidthSelection, Qt::Variant.new(true))
  format.setBackground(Qt::Brush.new(palegreen))
  selection = Qt::TextEdit::ExtraSelection.new
  selection.format = format
  selection.cursor = editor.textCursor
  editor.setExtraSelections([selection])
  APP.processEvents
  image = editor.viewport.grab.toImage
  line1 = image.pixelColor(200, 8)
  editor.hide
  line1 == palegreen || raise("line 1 right margin is #{[line1.red, line1.green, line1.blue]}")
end

puts "\n34. Qt::Pixmap#fill was unbound"
puts "   (Qt::ColorListWidget#addItemColor fills a 20x14 colour swatch for each"
puts "    entry at qt.rb:671; TlmGrapher adds its first entry while building"
puts "    the left frame (overview_tabbed_plots.rb:978), so startup failed)"
def pixmap_pixels(pixmap, *points)
  image = pixmap.toImage
  points.map { |x, y| c = image.pixelColor(x, y); [c.red, c.green, c.blue] }
end
chk('Pixmap#fill(Color) fills every pixel') do
  pixmap = Qt::Pixmap.new(20, 14)
  pixmap.fill(Qt::Color.new(255, 0, 0))
  pixmap_pixels(pixmap, [0, 0], [19, 13]) == [[255, 0, 0], [255, 0, 0]]
end
chk('Pixmap#fill(Qt::GlobalColor)') do
  pixmap = Qt::Pixmap.new(4, 4)
  pixmap.fill(Qt::red)   # Qt::red is a plain Integer here
  pixmap_pixels(pixmap, [1, 1]) == [[255, 0, 0]]
end
chk('Pixmap#fill with no argument fills white') do
  pixmap = Qt::Pixmap.new(4, 4)
  pixmap.fill
  pixmap_pixels(pixmap, [2, 2]) == [[255, 255, 255]]
end

puts "\n35. Qt::ListWidget#addItem(item) raised TypeError"
puts "   (addItem only took a String. ColorListWidget#addItemColor passes a"
puts "    ListWidgetItem at qt.rb:677, the line after its swatch fill, and"
puts "    LimitsMonitor's Ignored Telemetry Items dialog adds its entries that"
puts "    way at limits_monitor.rb:780)"
chk('addItem(ListWidgetItem) adds that item') do
  list = Qt::ListWidget.new
  list.addItem(Qt::ListWidgetItem.new('INST HEALTH_STATUS TEMP1'))
  list.count == 1 && list.item(0).text == 'INST HEALTH_STATUS TEMP1'
end
chk('addItem(ListWidgetItem.new(icon, text)) (qt.rb:676-677)') do
  list = Qt::ListWidget.new
  pixmap = Qt::Pixmap.new(20, 14)
  pixmap.fill(Qt::red)
  list.addItem(Qt::ListWidgetItem.new(Qt::Icon.new(pixmap), 'No Plot Selected'))
  list.count == 1 && list.item(0).text == 'No Plot Selected'
end
chk('addItem(String) still works') do
  list = Qt::ListWidget.new
  list.addItem('plain')
  list.count == 1 && list.item(0).text == 'plain'
end

puts "\n36. GridLayout#addWidget(widget) and Layout#addItem were unbound"
puts "   (addWidget required 3-5 arguments, and Qt::AdaptiveGridLayout, which"
puts "    TlmGrapher adds every plot through, calls super(widget) at"
puts "    qt.rb:791/802/805. Growing to 2 and 3 columns it also re-places"
puts "    taken items with addItem(item, row, column) at qt.rb:790/797-800.)"
def laid_out(layout)
  host = Qt::Widget.new
  host.setLayout(layout)
  host.resize(300, 200)
  host.show
  APP.processEvents
  host
end
chk('GridLayout#addWidget(widget) fills the next free cell') do
  grid = Qt::GridLayout.new
  labels = Array.new(2) { |i| Qt::Label.new("w#{i}") }
  labels.each { |l| grid.addWidget(l) }
  host = laid_out(grid)
  result = grid.count == 2 && labels[1].pos.y > labels[0].pos.y   # one column
  host.hide
  result
end
chk('Layout#addItem(item, row, column) re-places a taken item') do
  grid = Qt::GridLayout.new
  a = Qt::Label.new('a')
  b = Qt::Label.new('b')
  grid.addWidget(a, 0, 0)
  grid.addWidget(b, 1, 0)
  grid.addItem(grid.takeAt(1), 0, 1)
  host = laid_out(grid)
  result = grid.count == 2 && b.pos.x > a.pos.x && b.pos.y == a.pos.y
  host.hide
  result
end
chk('Layout#addItem(item) appends on a box layout') do
  box = Qt::VBoxLayout.new
  box.addWidget(Qt::Label.new('a'))
  item = box.takeAt(0)
  box.addItem(item)
  box.count == 1
end

puts "\n37. Widget#cursor, #x, #y and #window were unbound"
puts "   (LineGraph's paintEvent reads cursor.pos once the mouse has moved over"
puts "    it (line_graph_drawing.rb:416). The NoMethodError skipped"
puts "    @painter.dispose at line_graph.rb:358-359, so every later paint"
puts "    returned at `return if @painter` and the graph froze. Help > About"
puts "    reads parent.x and parent.y (about_dialog.rb:95/99).)"
chk('Widget#x and #y') do
  w = Qt::Widget.new
  w.move(30, 40)
  [w.x, w.y] == [30, 40]
end
chk('Widget#cursor.pos is the global cursor position') do
  c = Qt::Widget.new.cursor
  c.is_a?(Qt::Cursor) && [c.pos.x, c.pos.y] == [Qt::Cursor.pos.x, Qt::Cursor.pos.y]
end
chk('Widget#window is the top-level window') do
  top = Qt::MainWindow.new
  child = Qt::Widget.new(top)
  child.window.equal?(top) && top.window.equal?(top)
end

puts "\n38. Qt::RadialGradient and Qt::SpacerItem were empty classes"
puts "   (LedWidget#value= builds its brush from RadialGradient.new(5, 5, 50,"
puts "    5, 5) at led_widget.rb:75, so a TlmViewer LED's first value ended"
puts "    its screen's update thread. SpacerWidget adds SpacerItem.new(w, h,"
puts "    hpolicy, vpolicy) to its layout (spacer_widget.rb:24-28), so a SPACER"
puts "    screen did not open.)"
def led_gradient
  gradient = Qt::RadialGradient.new(5, 5, 50, 5, 5)
  gradient.setColorAt(0, Qt::Color.new(0, 255, 0))
  gradient.setColorAt(1, Qt::Color.new(0, 0, 0))
  gradient
end
chk('RadialGradient.new(cx, cy, radius, fx, fy) and setColorAt') { led_gradient.is_a?(Qt::RadialGradient) }
chk('Brush.new(RadialGradient) paints the gradient') do
  brush = Qt::Brush.new(led_gradient)   # before the painter, so a raise leaves none active
  pixmap = Qt::Pixmap.new(20, 20)
  pixmap.fill(Qt::white)
  painter = Qt::Painter.new(pixmap)
  painter.setBrush(brush)
  painter.drawRect(0, 0, 20, 20)
  painter.end
  c = pixmap.toImage.pixelColor(5, 5)   # the focal point: colour stop 0
  (c.green > 200 && c.red < 60 && c.blue < 60) || raise("focal pixel #{[c.red, c.green, c.blue]}")
end
chk('SpacerItem.new(w, h, hpolicy, vpolicy) goes into a layout') do
  box = Qt::VBoxLayout.new
  box.addItem(Qt::SpacerItem.new(10, 20, Qt::SizePolicy::Fixed, Qt::SizePolicy::Fixed))
  box.count == 1
end
chk('a fixed SpacerItem holds its height between two widgets') do
  box = Qt::VBoxLayout.new
  top = Qt::Label.new('top')
  bottom = Qt::Label.new('bottom')
  box.addWidget(top)
  box.addItem(Qt::SpacerItem.new(10, 40, Qt::SizePolicy::Fixed, Qt::SizePolicy::Fixed))
  box.addWidget(bottom)
  host = laid_out(box)
  gap = bottom.pos.y - (top.pos.y + top.height)
  host.hide
  gap >= 40 || raise("gap #{gap}")
end

puts "\n39. GridLayout#setHorizontalSpacing / #setVerticalSpacing were unbound"
puts "   (TlmViewer's MATRIXBYCOLUMNS is a Qt::GridLayout subclass that sets both"
puts "    in its constructor (matrixbycolumns_widget.rb:27-28), so any screen"
puts "    using it failed to build)"
chk('setHorizontalSpacing') do
  grid = Qt::GridLayout.new
  grid.setHorizontalSpacing(7)
  grid.horizontalSpacing == 7
end
chk('setVerticalSpacing') do
  grid = Qt::GridLayout.new
  grid.setVerticalSpacing(9)
  grid.verticalSpacing == 9
end

puts "\n40. execute_in_main_thread(true, 0.05) deferred its block on the GUI thread"
puts "   (the shim read the 2nd argument as a delay. In qtbindings 4.8.6.5"
puts "    (lib/Qt4.rb:101) it is sleep_period, the poll interval while"
puts "    blocking; only the 3rd, delay_execution, defers on the GUI thread."
puts "    script_module_gui.rb:74 passes 0.05 for every prompt and file dialog,"
puts "    so on the GUI thread -- CmdSender's hazardous prompt, TlmViewer BUTTON"
puts "    code -- the call returned nil and the answer was discarded)"
chk('(true, 0.05) on the GUI thread runs the block inline') do
  ran = false
  result = Qt.execute_in_main_thread(true, 0.05) { ran = true; 42 }
  ran && result == 42
end
chk('(false, 0.001) on the GUI thread runs the block inline') do
  ran = false
  Qt.execute_in_main_thread(false, 0.001) { ran = true }
  ran
end

puts "\n41. Qt::Menu#exec was unbound"
puts "   (menu.exec(global_point) opens every COSMOS context menu -- 14 sites"
puts "    in 11 files, e.g. cmd_params.rb:194, script_runner.rb:889 -- and"
puts "    resolved to the private Kernel#exec: NoMethodError, no menu)"
chk('Qt::Menu#exec is a public method') { Qt::Menu.public_method_defined?(:exec) }
chk('and it is the binding, not Kernel#exec') { Qt::Menu.instance_method(:exec).owner != Kernel }
# The modal path is test_cosmos_tools.rb section 12: modal loops return at
# once in this file after section 8.

puts "\n42. GC on a non-GUI thread deleted Ruby-owned QObjects there"
puts "   (qtwrap_free deleted an owned widget on whichever Ruby thread ran the"
puts "    GC, and each child's destroyed() hookup used the child as its"
puts "    context, so off the GUI thread it was queued to an object being"
puts "    destroyed and never ran: the child wrapper kept a dangling pointer"
puts "    and the next call on it segfaulted. Run in a child process, since"
puts "    the failure is a segfault.)"
GC_OWNED_TREE = <<~'RUBY'
  require 'Qt'
  app = Qt::Application.new([])
  $child = nil
  def build
    root = Qt::Widget.new                  # parentless: Ruby owns it
    $child = Qt::Label.new('child', root)  # Qt owns it; Ruby keeps its wrapper
    nil
  end
  build
  def clobber(n = 50); n.zero? ? 0 : clobber(n - 1) + 1; end
  clobber                                  # no stale reference to root on the stack
  before = Qt.object_count
  if ARGV[0] == 'thread'
    Thread.new { GC.start; GC.start }.join
  else
    GC.start; GC.start
  end
  collected = Qt.object_count < before
  3.times { Qt::Application.processEvents }
  touched = begin
    $child.objectName
    'no error'
  rescue RuntimeError => e
    e.message
  end
  puts "collected=#{collected} disposed=#{$child.disposed?} touch=#{touched}"
RUBY
def gc_owned_tree(where)
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', GC_OWNED_TREE, where, err: [:child, :out]], &:read)
  [$?, out.lines.grep(/collected=/).first.to_s.strip]
end
%w[main thread].each do |where|
  chk("GC on #{where == 'main' ? 'the GUI' : 'a background'} thread: child wrapper marked destroyed") do
    status, line = gc_owned_tree(where)
    expected = 'collected=true disposed=true touch=Qt object has already been destroyed'
    (status.success? && line == expected) ||
      raise("#{status.signaled? ? "died with SIG#{Signal.signame(status.termsig)}" : "exit #{status.exitstatus}"}: #{line.inspect}")
  end
end

puts "\n43. wrap_obj handed back wrappers that lazy sweep had condemned"
puts "   (g_objmap is weak: an unreferenced wrapper stays in it until its free"
puts "    function runs. Between the GC's mark and its lazy sweep, findChild &"
puts "    co. returned that condemned VALUE; once swept, Ruby held a freed slot"
puts "    and the next touch segfaulted. logging_tab.rb:53-72 re-fetches its"
puts "    widgets this way every second. Child process: the failure is a"
puts "    segfault.)"
LAZY_SWEEP = <<~'RUBY'
  require 'Qt'
  app = Qt::Application.new([])
  # Menus Qt builds (MenuBar#addMenu) get weak wrappers; a Ruby-constructed
  # parented object would be pinned (section 59) and never condemned.
  bar = Qt::MenuBar.new
  n = 20000
  names = Array.new(n) { |i| "c#{i}" }
  keep = Array.new(n) { |i| m = bar.addMenu(names[i]); m.setObjectName(names[i]); m }
  GC.start
  picks = [0, n / 4, n / 2, (3 * n) / 4, n - 1]
  ids = picks.map { |k| keep[k].__id__ }
  keep.clear
  GC.start(full_mark: true, immediate_mark: true, immediate_sweep: false)
  found = picks.map { |k| bar.findChild(names[k]) }   # while the old wrappers are condemned
  condemned = found.each_with_index.map { |o, k| o.__id__ == ids[k] }
  junk = []
  (n * 4).times { junk << ('s' * 24) }                 # finish the sweep, refill freed slots
  GC.start
  puts "condemned=#{condemned.inspect} names=#{found.map(&:objectName) == picks.map { |k| names[k] }}"
RUBY
chk('findChild during lazy sweep returns live wrappers') do
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', LAZY_SWEEP, err: [:child, :out]], &:read)
  line = out.lines.grep(/condemned=/).first.to_s.strip
  ($?.success? && line == 'condemned=[false, false, false, false, false] names=true') ||
    raise("#{$?.signaled? ? "died with SIG#{Signal.signame($?.termsig)}" : "exit #{$?.exitstatus}"}: #{line.inspect}")
end

puts "\n44. SIGINT and SIGTERM were not serviced while an event loop idled"
puts "   (every exec released the GVL with RUBY_UBF_IO, which only interrupts a"
puts "    blocking syscall; Qt's dispatcher retries it, so exec never returned"
puts "    to let Ruby run the signal. Ctrl-C and `kill` did nothing to an idle"
puts "    tool or an open dialog. Child process per signal.)"
IDLE_LOOP = <<~'RUBY'
  $stdout.sync = true
  require 'Qt'
  app = Qt::Application.new([])
  target = ARGV[0] == 'dialog' ? Qt::Dialog.new : app
  puts 'READY'
  begin
    target.exec
    puts 'exec returned'
  rescue Exception => e
    puts "exec raised #{e.class}"
  end
RUBY
# Starts an idle app.exec or dialog.exec in a child, sends +sig+ once it is
# looping, and returns [exited within 3 s, output].
def signal_idle_loop(kind, sig)
  lib = File.expand_path('../../../../lib', __dir__)
  rd, wr = IO.pipe
  pid = Process.spawn(RbConfig.ruby, '-I', lib, '-e', IDLE_LOOP, kind, out: wr, err: wr)
  wr.close
  out = +''
  deadline = Time.now + 20
  until out.include?('READY') || Time.now > deadline
    begin
      out << rd.read_nonblock(4096)
    rescue IO::WaitReadable
      IO.select([rd], nil, nil, 0.1)
    rescue EOFError
      break
    end
  end
  sleep 0.5                           # let it enter exec
  Process.kill(sig, pid)
  exited = nil
  30.times { break if (exited = Process.waitpid(pid, Process::WNOHANG)); sleep 0.1 }
  unless exited
    Process.kill('KILL', pid)
    Process.waitpid(pid)
  end
  out << rd.read.to_s
  rd.close
  [!exited.nil?, out]
end
{ 'TERM' => 'SignalException', 'INT' => 'Interrupt' }.each do |sig, error|
  %w[app dialog].each do |kind|
    name = "SIG#{sig} ends an idle #{kind}.exec with #{error}"
    next skip_chk(name, 'Windows has no POSIX signals between processes') if WINDOWS
    chk(name) do
      exited, out = signal_idle_loop(kind, sig)
      (exited && out.include?("exec raised #{error}")) ||
        raise(exited ? "output #{out.lines.last.to_s.strip.inspect}" : 'still running 3 s after the signal')
    end
  end
end

puts "\n45. exit, Interrupt and SignalException raised in slots, timers and posted"
puts "    blocks were swallowed"
puts "   (contain_error reported them like any error and the loop carried on;"
puts "    single_shot/post_to_main_thread dropped them outright. A trap('TERM')"
puts "    { exit } that ran inside a slot kept the tool alive, `exit` in a"
puts "    button handler did nothing, and Ctrl-C during a GUI block only killed"
puts "    the thread that posted it. Child process per case.)"
EXIT_FROM_LOOP = <<~'RUBY'
  $stdout.sync = true
  require 'Qt'
  app = Qt::Application.new([])
  button = Qt::PushButton.new('b')
  case ARGV[0]
  when 'trap_in_slot'      # the signal lands while a slot's Ruby code runs
    Signal.trap('TERM') { exit 2 }
    timer = Qt::Timer.new
    timer.connect(SIGNAL('timeout()')) { t0 = Time.now; nil while Time.now - t0 < 0.05 }
    timer.start(0)
  when 'exit_in_slot'
    button.connect(SIGNAL('clicked()')) { exit 3 }
    Qt.single_shot(20) { button.click }
  when 'exit_in_single_shot'
    Qt.single_shot(20) { exit 4 }
  when 'exit_in_async_post'
    Qt.single_shot(20) { Thread.new { Qt.execute_in_main_thread(false) { exit 5 } } }
  when 'interrupt_in_blocking_post'
    Qt.single_shot(20) { Thread.new { Qt.execute_in_main_thread(true) { raise Interrupt } rescue nil } }
  end
  Qt.single_shot(3000) { puts 'STILL ALIVE'; app.quit }
  puts 'READY'
  begin
    app.exec
    puts 'exec returned'
  rescue SystemExit => e
    puts "exec raised SystemExit status=#{e.status}"
  rescue Exception => e
    puts "exec raised #{e.class}"
  end
RUBY
# Runs EXIT_FROM_LOOP's +kind+ in a child; with +sig+, sends it once the child
# is looping. Returns its output once it exits (or is killed after 8 s).
def exit_from_loop(kind, sig = nil)
  lib = File.expand_path('../../../../lib', __dir__)
  rd, wr = IO.pipe
  pid = Process.spawn(RbConfig.ruby, '-I', lib, '-e', EXIT_FROM_LOOP, kind, out: wr, err: wr)
  wr.close
  if sig
    out = +''
    deadline = Time.now + 20
    until out.include?('READY') || Time.now > deadline
      begin
        out << rd.read_nonblock(4096)
      rescue IO::WaitReadable
        IO.select([rd], nil, nil, 0.1)
      rescue EOFError
        break
      end
    end
    sleep 0.5
    Process.kill(sig, pid)
  end
  exited = nil
  80.times { break if (exited = Process.waitpid(pid, Process::WNOHANG)); sleep 0.1 }
  unless exited
    Process.kill('KILL', pid)
    Process.waitpid(pid)
  end
  result = (out || '') + rd.read.to_s
  rd.close
  result
end
{
  ['trap_in_slot', 'TERM'] => 'exec raised SystemExit status=2',
  ['exit_in_slot', nil] => 'exec raised SystemExit status=3',
  ['exit_in_single_shot', nil] => 'exec raised SystemExit status=4',
  ['exit_in_async_post', nil] => 'exec raised SystemExit status=5',
  ['interrupt_in_blocking_post', nil] => 'exec raised Interrupt'
}.each do |(kind, sig), expected|
  next skip_chk("#{kind.tr('_', ' ')} ends app.exec", 'Windows has no POSIX signals between processes') if WINDOWS && sig
  chk("#{kind.tr('_', ' ')} ends app.exec") do
    out = exit_from_loop(kind, sig)
    out.include?(expected) || raise("got #{out.lines.map(&:strip).grep(/exec |STILL/).join(' | ').inspect}")
  end
end

puts "\n46. Linux builds looked for moc in pkg-config's host_bins, which Qt 6 lacks"
puts "   (Qt 6's .pc files have no host_bins; moc is in libexecdir, e.g."
puts "    /usr/lib/qt6/libexec/moc on Ubuntu 24.04, off PATH. The bare `moc`"
puts "    fallback failed, extconf.rb wrote the stub Makefile, and the build"
puts "    'succeeded' with no GUI. Runs the real extconf.rb in a temporary copy"
puts "    with the Linux branch forced and a stand-in pkg-config/moc laid out"
puts "    like Qt 6's.)"
if WINDOWS
  skip_chk('extconf.rb finds moc in libexecdir and writes a real Makefile',
           'the Linux branch is driven by POSIX shell stand-ins')
else
chk('extconf.rb finds moc in libexecdir and writes a real Makefile') do
  Dir.mktmpdir('qt6_extconf') do |dir|
    src = File.join(dir, 'src')
    bin = File.join(dir, 'bin')
    libexec = File.join(dir, 'libexec')
    [src, bin, libexec].each { |d| Dir.mkdir(d) }
    %w[extconf.rb rubycallback.h rubywidget.h].each { |f| FileUtils.cp(File.join(__dir__, f), src) }
    File.write(File.join(bin, 'pkg-config'), <<~SH)
      #!/bin/sh
      case "$*" in
        *--cflags*) echo "-I#{dir}/include" ;;
        *--libs*) echo "-L#{dir}/lib -lQt6Core" ;;
        *--variable=libexecdir*) echo "#{libexec}" ;;
        *) echo "" ;;
      esac
    SH
    File.write(File.join(libexec, 'moc'), <<~SH)
      #!/bin/sh
      while [ $# -gt 0 ]; do [ "$1" = "-o" ] && touch "$2"; shift; done
    SH
    FileUtils.chmod(0755, [File.join(bin, 'pkg-config'), File.join(libexec, 'moc')])
    force_linux = "Object.send(:remove_const, :RUBY_PLATFORM); RUBY_PLATFORM = 'x86_64-linux'.freeze; load 'extconf.rb'"
    out = IO.popen({ 'PATH' => "#{bin}:#{ENV['PATH']}" }, [RbConfig.ruby, '-e', force_linux,
                   chdir: src, err: [:child, :out]], &:read)
    makefile = File.read(File.join(src, 'Makefile')) rescue ''
    (!makefile.include?('qt6 extension skipped') && makefile.include?('moc_rubycallback')) ||
      raise(out.lines.grep(/skipping|moc/).first.to_s.strip.then { |l| l.empty? ? 'no Makefile' : l })
  end
end
end   # WINDOWS

puts "\n47. Qt::Completer#popup, #widget, #complete and #completionPrefix were unbound"
puts "   (every keyPressEvent in the Ruby editors and CmdSender's history asks"
puts "    popup.isVisible first (completion_text_edit.rb:92,"
puts "    cmd_sender_text_edit.rb:25), so COSMOS's key handling died on every key"
puts "    and only Qt's default editing ran. Completion's create_popup also uses"
puts "    widget and complete(rect), insertCompletion completionPrefix"
puts "    (completion.rb:43-63).)"
completer_editor = Qt::PlainTextEdit.new
completer = Qt::Completer.new(completer_editor)
completer.setWidget(completer_editor)
chk('Completer#popup is the (hidden) completion view') do
  completer.popup.is_a?(Qt::AbstractItemView) && !completer.popup.isVisible
end
chk('Completer#widget is the widget it completes for') { completer.widget.equal?(completer_editor) }
chk('Completer#completionPrefix') { completer.setCompletionPrefix('IN'); completer.completionPrefix == 'IN' }
chk('complete(rect) pops up the matching rows') do
  completer.setModel(Qt::StringListModel.new(%w[INST INST2 EXAMPLE], completer))
  completer.setCompletionPrefix('')
  completer_editor.resize(300, 100)
  completer_editor.show
  APP.processEvents
  rect = completer_editor.cursorRect
  rect.setWidth(completer.popup.sizeHintForColumn(0) + completer.popup.verticalScrollBar.sizeHint.width)
  completer.complete(rect)
  APP.processEvents
  shown = completer.popup.isVisible
  completer.popup.close
  completer_editor.hide
  shown
end

puts "\n48. Qt::Boolean#nil? did not match qtbindings"
puts "   (qtbindings' Boolean#nil? is `!@value` (qtruby4.rb:404-406), which is"
puts "    how COSMOS reads Cancel off InputDialog's out-parameter. Ours returned"
puts "    @value.nil?, so Cancel was never seen: `ask` without blanks reopened"
puts "    forever, and ConfigEditor's Create Target (config_editor.rb:610) and"
puts "    TlmGrapher's Edit Tab (overview_tabbed_plots.rb:228) went on with nil)"
chk('Boolean.new(false).nil? -- Cancel') { Qt::Boolean.new(false).nil? == true }
chk('Boolean.new(true).nil? -- OK') { Qt::Boolean.new(true).nil? == false }
chk('Boolean.new.nil? -- not set yet') { Qt::Boolean.new.nil? == true }

puts "\n49. A message box's button box could not be found, typed, or turned vertical"
puts "   (script vertical_message_box (script_module_gui.rb:268) reached the"
puts "    buttons through box.layout.itemAt(2) -- the button box in Qt 4's"
puts "    QMessageBox layout, a Label in Qt 6's -- and a QDialogButtonBox came"
puts "    back as a plain Qt::Widget without setOrientation anyway, so every"
puts "    call raised)"
def two_button_box
  box = Qt::MessageBox.new(nil)
  box.setText('Pick one')
  box.addButton('First', Qt::MessageBox::AcceptRole)
  box.addButton('Second', Qt::MessageBox::AcceptRole)
  box
end
chk('the button box comes back as a Qt::DialogButtonBox') do
  two_button_box.findChildren.any? { |c| c.is_a?(Qt::DialogButtonBox) }
end
chk('DialogButtonBox#setOrientation(Qt::Vertical) stacks the buttons') do
  box = two_button_box
  box.findChildren.find { |c| c.is_a?(Qt::DialogButtonBox) }.setOrientation(Qt::Vertical)
  box.show
  APP.processEvents
  first, second = %w[First Second].map { |t| box.findChildren.find { |c| c.is_a?(Qt::PushButton) && c.text == t } }
  stacked = first.pos.x == second.pos.x && first.pos.y != second.pos.y
  box.hide
  stacked || raise("First at #{[first.pos.x, first.pos.y]}, Second at #{[second.pos.x, second.pos.y]}")
end

puts "\n50. Widget#layout returned C++-created layouts as a plain Qt::Layout"
puts "   (no layout class was registered for best_ruby_class, so a QMessageBox's"
puts "    own QGridLayout had no grid addWidget: script combo_box"
puts "    (script_module_gui.rb:202) raised on every call)"
chk("a QMessageBox's layout is a Qt::GridLayout") { Qt::MessageBox.new(nil).layout.is_a?(Qt::GridLayout) }
chk('and takes a grid addWidget(w, row, col, rows, cols)') do
  box = Qt::MessageBox.new(nil)
  box.layout.addWidget(Qt::Label.new('combo'), 2, 0, 1, 2)
  true
end
chk('a layout made in Ruby still comes back as itself') do
  w = Qt::Widget.new
  layout = Qt::VBoxLayout.new
  w.setLayout(layout)
  w.layout.equal?(layout)
end

puts "\n51. FormLayout#addRow(widget) and #addRow(layout) were unbound"
puts "   (only the two-argument forms existed. The Details dialog adds its"
puts "    States and Limits group boxes as full-width rows"
puts "    (details_dialog.rb:78/87), so Details raised for every item with"
puts "    states or limits)"
chk('addRow(widget) adds a full-width row') do
  form = Qt::FormLayout.new
  form.addRow('Name:', Qt::LineEdit.new)    # two items
  form.addRow(Qt::GroupBox.new('States'))   # one
  form.count == 3
end
chk('addRow(layout) adds a full-width row') do
  form = Qt::FormLayout.new
  form.addRow(Qt::HBoxLayout.new)
  form.count == 1
end
chk('Layout#minimumSize -- the States box sizes its scroll area by it') do
  form = Qt::FormLayout.new   # details_dialog.rb:131-135
  host = Qt::Widget.new
  host.setLayout(form)
  3.times { |i| form.addRow("STATE#{i}:", Qt::Label.new(i.to_s)) }
  form.minimumSize.height > 0
end

puts "\n53. GridLayout#addLayout was unbound (only box layouts had addLayout)"
puts "   (CmdSender's Send Raw dialog adds its file and button rows to a grid"
puts "    (cmd_sender.rb:341/350), and TlmViewer's MATRIXBYCOLUMNS adds nested"
puts "    layouts with super(layout, row, column)"
puts "    (matrixbycolumns_widget.rb:32), so both raised)"
chk('GridLayout#addLayout(layout, row, column)') do
  grid = Qt::GridLayout.new
  grid.addLayout(Qt::HBoxLayout.new, 1, 1)
  grid.count == 1
end
chk('GridLayout#addLayout(layout, row, column, rows, columns)') do
  grid = Qt::GridLayout.new
  grid.addLayout(Qt::HBoxLayout.new, 2, 0, 1, 2)
  grid.count == 1
end

puts "\n54. ComboBox item data was stored as the Variant's toString"
puts "   (addItem(text, variant) kept only toString, so a Float came back as a"
puts "    String and Variant.new(nil) as \"\". Replay's \"Realtime\" speed is"
puts "    Variant.new(nil) (replay_tab.rb:187-192), and \"\".to_f made it a 0.0"
puts "    delay: No Delay. itemData also answered nil for invalid data, where"
puts "    qtbindings handed back the (invalid) Variant.)"
def item_data_of(variant)
  combo = Qt::ComboBox.new
  combo.addItem('x', variant)
  combo.itemData(0)
end
chk('a Float stays a Float') do
  value = item_data_of(Qt::Variant.new(0.001)).value
  (value.is_a?(Float) && value == 0.001) || raise("got #{value.inspect}")
end
chk('Variant.new(nil) comes back as a Variant whose value is nil') do
  data = item_data_of(Qt::Variant.new(nil))
  (data.is_a?(Qt::Variant) && data.value.nil?) || raise("got #{data.inspect}")
end
chk('a String stays a String') { item_data_of(Qt::Variant.new('INST SCREEN;file.txt')).value == 'INST SCREEN;file.txt' }

puts "\n55. cursor.selection.toPlainText kept Qt's U+2029 paragraph separators"
puts "   (the shim fed selectedText, which separates blocks with U+2029;"
puts "    qtbindings went through QTextDocumentFragment, which gives \\n."
puts "    PlainTextEdit#selected_lines (qt.rb:530) hands this to ScriptRunner's"
puts "    Execute Selected Lines, so multi-line selections ran as one garbled"
puts "    line)"
chk('a two-line selection comes back with a newline') do
  editor = Qt::PlainTextEdit.new
  editor.setPlainText("puts 1\nputs 2")
  cursor = editor.textCursor
  cursor.movePosition(Qt::TextCursor::Start)
  cursor.movePosition(Qt::TextCursor::End, Qt::TextCursor::KeepAnchor)
  (text = cursor.selection.toPlainText) == "puts 1\nputs 2" || raise("got #{text.inspect}")
end

puts "\n56. TextEdit/PlainTextEdit#find ignored its flags"
puts "   (only the text was passed to Qt, so direction, Match Case and Whole"
puts "    Words had no effect -- find_replace_dialog.rb:57/76/214-233 pass them"
puts "    all -- and a zero-argument call read argv[0] out of bounds)"
def find_in(text, needle, flags, at_end: false)
  editor = Qt::PlainTextEdit.new
  editor.setPlainText(text)
  cursor = editor.textCursor
  cursor.movePosition(at_end ? Qt::TextCursor::End : Qt::TextCursor::Start)
  editor.setTextCursor(cursor)
  found = editor.find(needle, flags)
  [found, found ? editor.textCursor.selectionStart : nil]
end
chk('FindBackward searches toward the start') { find_in('tlm tlm', 'tlm', Qt::TextDocument::FindBackward, at_end: true) == [true, 4] }
chk('FindCaseSensitively skips other cases') { find_in('TLM tlm', 'tlm', Qt::TextDocument::FindCaseSensitively) == [true, 4] }
# Qt counts only letters and digits as word characters: tlm_x would match.
chk('FindWholeWords skips partial words') { find_in('tlmx tlm', 'tlm', Qt::TextDocument::FindWholeWords) == [true, 5] }
chk('find with no text is an ArgumentError, not a wild read') do
  begin
    Qt::PlainTextEdit.new.find
    false
  rescue ArgumentError
    true
  end
end

puts "\n57. Standard shortcuts were built as raw key codes"
puts "   (Qt::KeySequence::Save and friends were plain Integers, so"
puts "    KeySequence.new(Qt::KeySequence::Save) took the key-code constructor:"
puts "    TableManager's New/Open/Save were dead and Save As (63) was the ? key,"
puts "    and ScriptRunner/TestRunner zoom and LimitsMonitor's Delete were dead"
puts "    -- table_manager.rb:293-313, script_runner.rb:175/180,"
puts "    test_runner.rb:114/118, limits_monitor.rb:782)"
{ 'Save' => 'Ctrl+S', 'New' => 'Ctrl+N', 'Open' => 'Ctrl+O', 'ZoomIn' => 'Ctrl++',
  'ZoomOut' => 'Ctrl+-', 'Delete' => 'Del' }.each do |name, text|
  chk("KeySequence.new(KeySequence::#{name}) is #{text}") do
    (got = Qt::KeySequence.new(Qt::KeySequence.const_get(name)).toString) == text || raise("got #{got.inspect}")
  end
end
chk('KeySequence::SaveAs is not the ? key') do
  (got = Qt::KeySequence.new(Qt::KeySequence::SaveAs).toString) != '?' || raise("got #{got.inspect}")
end
chk('a key code still makes a one-key sequence') { Qt::KeySequence.new(Qt::Key_F5).toString == 'F5' }
chk('the constants still act as their Integer values') do
  Qt::KeySequence::Save.to_i == 5 && Qt::KeySequence::Save == 5 && Qt::KeySequence::SaveAs.to_i == 63
end

puts "\n58. The close button (and Esc) skipped Ruby reject overrides"
puts "   (QDialog's close and Esc handling call the virtual reject(), which"
puts "    RubyDialog did not forward, so ScriptRunnerDialog's refusal to close"
puts "    mid-script (script_runner_frame.rb:65-69) and the raw dialogs' timer"
puts "    shutdown (cmd_tlm_raw_dialog.rb:129, interface_raw_dialog.rb:125,"
puts "    pry_dialog.rb:153) never ran)"
$rejects = []
counting_dialog = Class.new(Qt::Dialog) { def reject; $rejects << :ruby; super; end }
chk('closing a dialog runs its Ruby reject, whose super closes it') do
  d = counting_dialog.new
  d.show
  APP.processEvents
  d.close
  APP.processEvents
  result = $rejects == [:ruby] && !d.isVisible
  d.dispose
  result || raise("rejects #{$rejects.inspect}, visible=#{d.isVisible rescue '?'}")
end
chk('a Ruby reject that refuses keeps the dialog open') do
  refusing_dialog = Class.new(Qt::Dialog) { def reject; end }
  d = refusing_dialog.new
  d.show
  APP.processEvents
  d.close
  APP.processEvents
  result = d.isVisible
  d.hide
  d.dispose
  result
end
chk('dialog.reject from Ruby, with no override, still closes it') do
  d = Qt::Dialog.new
  d.show
  d.reject
  result = !d.isVisible
  d.dispose
  result
end

puts "\n59. Qt-owned Ruby objects lost their Ruby side at the next GC"
puts "   (the wrapper map is weak, so a Ruby object only Qt held -- CmdSender's"
puts "    CmdParamTableItemDelegate, set on its table at cmd_params.rb:244 --"
puts "    was collected, and Qt's default editor and painting took over)"
$delegate_calls = Hash.new(0)
class CountingDelegate < Qt::StyledItemDelegate
  def initialize(table, tag); super(table); @tag = tag; end
  def createEditor(parent, option, index)
    $delegate_calls[@tag] += 1
    Qt::ComboBox.new(parent)
  end
end
def table_with_delegate(tag)
  table = Qt::TableWidget.new
  table.setRowCount(1)
  table.setColumnCount(1)
  table.setItem(0, 0, Qt::TableWidgetItem.new('a'))
  table.setItemDelegate(CountingDelegate.new(table, tag))   # nothing in Ruby keeps it
  table.show
  APP.processEvents
  table
end
chk('a delegate only its table holds still edits after GC') do
  table = table_with_delegate(:after_gc)
  4.times { GC.start }
  table.editItem(table.item(0, 0))
  APP.processEvents
  table.hide
  $delegate_calls[:after_gc] == 1 || raise("createEditor ran #{$delegate_calls[:after_gc]} time(s)")
end
class TaggedWidget < Qt::Widget
  attr_reader :tag
  def initialize(parent); super(parent); @tag = :kept; end
end
chk("a Ruby widget only its parent holds keeps its class and state after GC") do
  parent = Qt::Widget.new
  TaggedWidget.new(parent)
  4.times { GC.start }
  child = parent.findChildren.find { |c| c.is_a?(TaggedWidget) }
  (child && child.tag == :kept) || raise("found #{parent.findChildren.map(&:class).inspect}")
end

puts "\n60. Painter#drawText(x, y, width, height, flags, text) was unbound"
puts "   (the Ruby editors number their lines with it (ruby_editor.rb:357), so"
puts "    every line-number paint raised, the gutter stayed blank, and the"
puts "    painter was left active -- see section 61)"
chk('drawText(x, y, w, h, flags, text) draws right-aligned text') do
  pixmap = Qt::Pixmap.new(60, 20)
  pixmap.fill(Qt::white)
  painter = Qt::Painter.new(pixmap)
  begin
    painter.setPen(Qt::Color.new(0, 0, 0))
    painter.drawText(0, 0, 60, 20, Qt::AlignRight, '888')
  ensure
    painter.end
  end
  image = pixmap.toImage
  dark = ->(x) { (0...20).any? { |y| image.pixelColor(x, y).red < 128 } }
  (!dark.call(2) && (40...60).any? { |x| dark.call(x) }) || raise('no right-aligned text drawn')
end
chk('drawText(rect, flags, text)') do
  pixmap = Qt::Pixmap.new(20, 20)
  painter = Qt::Painter.new(pixmap)
  begin
    painter.drawText(Qt::Rect.new(0, 0, 20, 20), Qt::AlignRight, '1')
  ensure
    painter.end
  end
  true
end

puts "\n61. A painter left active on a widget that was then destroyed crashed the GC"
puts "   (painter_free ended and deleted it, touching the freed widget. A Ruby"
puts "    exception mid-paintEvent leaves its painter active -- section 60's"
puts "    drawText did, in ScriptRunner's paused-script dialog, which is then"
puts "    disposed. Child process: the failure is a segfault.)"
ORPHANED_PAINTER = <<~'RUBY'
  require 'Qt'
  app = Qt::Application.new([])
  class Abandons < Qt::Widget
    def paintEvent(event)
      $painter = Qt::Painter.new(self)   # begun on this widget ...
      raise 'mid-paint error'            # ... and never ended
    end
  end
  def build
    dialog = Qt::Dialog.new               # the paused-script dialog's shape
    layout = Qt::VBoxLayout.new
    layout.addWidget(Abandons.new)
    dialog.setLayout(layout)
    dialog.resize(80, 80)
    dialog.show
    3.times { Qt::Application.processEvents }
    dialog
  end
  build.dispose                           # the device goes away first
  state = begin
    $painter.isActive
  rescue RuntimeError => e
    e.message
  end
  $painter = nil
  4.times { GC.start }                    # then the painter's wrapper is swept
  puts "after dispose: #{state}; survived"
RUBY
chk('a painter whose widget is gone says so, and its sweep does not crash') do
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', ORPHANED_PAINTER, err: [:child, :out]], &:read)
  line = out.lines.grep(/after dispose/).first.to_s.strip
  ($?.success? && line == 'after dispose: Qt::Painter: its paint device has been destroyed; survived') ||
    raise($?.signaled? ? "died with SIG#{Signal.signame($?.termsig)}" : "#{line.inspect}")
end

puts "\n62. Qt::ActionGroup.new(parent) ignored the parent"
puts "   (so Ruby owned the group, and PacketViewer's formatting group -- a"
puts "    local at packet_viewer.rb:169 -- was deleted by the next GC: the"
puts "    formatting options stopped being mutually exclusive)"
chk('ActionGroup.new(parent) belongs to the parent') do
  owner = Qt::Widget.new
  Qt::ActionGroup.new(owner).parent.equal?(owner)
end
def exclusive_pair(owner)
  a1 = Qt::Action.new('a1', owner)
  a2 = Qt::Action.new('a2', owner)
  [a1, a2].each { |a| a.setCheckable(true) }
  group = Qt::ActionGroup.new(owner)   # dropped, as in packet_viewer.rb:169
  group.addAction(a1)
  group.addAction(a2)
  [a1, a2]
end
# Overwrites stale VALUEs left on the machine stack, which Ruby's
# conservative scan would otherwise treat as live references.
def scrub_stack(n = 60); n.zero? ? 0 : scrub_stack(n - 1) + 1; end
chk('a group only its parent holds stays exclusive after GC') do
  owner = Qt::Widget.new
  a1, a2 = exclusive_pair(owner)
  scrub_stack
  4.times { GC.start }
  a1.setChecked(true)
  a2.setChecked(true)
  (!a1.isChecked && a2.isChecked) || raise("a1=#{a1.isChecked} a2=#{a2.isChecked}")
end

puts "\n63. Exceptions in single_shot and posted blocks were dropped unreported"
puts "   (one_shot_with_gvl cleared any exception without a word, and a"
puts "    non-blocking execute_in_main_thread's wrapper kept it where nothing"
puts "    looked; slot errors, by contrast, are reported on stderr)"
# Runs the block, pumps events, and returns what the binding wrote to $stderr.
def stderr_of
  saved = $stderr
  $stderr = StringIO.new
  yield
  t0 = Time.now
  APP.processEvents while Time.now - t0 < 0.3
  $stderr.string
ensure
  $stderr = saved
end
chk('a raising single_shot block is reported') do
  stderr_of { Qt.single_shot(0) { raise 'boom-single-shot' } }.include?('boom-single-shot')
end
chk('a raising post_to_main_thread block is reported') do
  stderr_of { Qt.post_to_main_thread { raise 'boom-posted' } }.include?('boom-posted')
end
chk('a raising non-blocking execute_in_main_thread from a worker is reported') do
  stderr_of { Thread.new { Qt.execute_in_main_thread(false) { raise 'boom-async' } }.join }.include?('boom-async')
end

puts "\n64. The CI qt6 job passed when the extension was not built"
puts "   (with no Qt6 or no moc, extconf.rb writes a do-nothing Makefile so"
puts "    `gem install cosmos` still works without a GUI; rake build and"
puts "    qt6_test then skip. COSMOS_QT6_REQUIRED=1 -- set by the workflow's qt6"
puts "    job -- makes all three fail instead.)"
# Runs extconf.rb in a temporary copy with the Linux branch forced and no
# Qt6 found; returns [exit status, whether it wrote the stub Makefile].
def extconf_without_qt(required)
  Dir.mktmpdir('qt6_extconf_noqt') do |dir|
    bin = File.join(dir, 'bin')
    Dir.mkdir(bin)
    FileUtils.cp(File.join(__dir__, 'extconf.rb'), dir)
    if WINDOWS   # a stand-in that finds no Qt6 modules
      File.write(File.join(bin, 'pkg-config.bat'), "@exit /b 1\r\n")
    else
      File.write(File.join(bin, 'pkg-config'), "#!/bin/sh\nexit 1\n")
      FileUtils.chmod(0755, File.join(bin, 'pkg-config'))
    end
    env = { 'PATH' => [bin, ENV['PATH']].join(File::PATH_SEPARATOR),
            'COSMOS_QT6_REQUIRED' => required ? '1' : nil }
    force_linux = "Object.send(:remove_const, :RUBY_PLATFORM); RUBY_PLATFORM = 'x86_64-linux'.freeze; load 'extconf.rb'"
    IO.popen(env, [RbConfig.ruby, '-e', force_linux, chdir: dir, err: [:child, :out]], &:read)
    stub = File.exist?(File.join(dir, 'Makefile')) && File.read(File.join(dir, 'Makefile')).include?('qt6 extension skipped')
    [$?.exitstatus, stub]
  end
end
chk('without COSMOS_QT6_REQUIRED a missing Qt6 still gives the stub (gem install)') do
  (r = extconf_without_qt(false)) == [0, true] || raise("got #{r.inspect}")
end
chk('with COSMOS_QT6_REQUIRED a missing Qt6 fails extconf.rb') do
  (r = extconf_without_qt(true)) == [1, false] || raise("got #{r.inspect}")
end
chk('rake build and qt6_test fail instead of skipping when it is required') do
  rakefile = File.read(File.join(ROOT, 'Rakefile'))
  qt6_rake = File.read(File.join(ROOT, 'tasks', 'qt6.rake'))
  rakefile.include?("ENV['COSMOS_QT6_REQUIRED']") && qt6_rake.include?("ENV['COSMOS_QT6_REQUIRED']")
end
chk("the workflow's qt6 job requires the extension") do
  File.read(File.join(ROOT, '.github', 'workflows', 'build_v4.yml')).include?('COSMOS_QT6_REQUIRED: 1')
end

puts "\n65. scan_unbound.rb reported TOTAL 0 while calls were unbound"
puts "   (the class pass skipped any name COSMOS defines anywhere -- COSMOS's"
puts "    own Qt::Dialog#exec hid Qt::Menu#exec (S9) -- and calls with no"
puts "    explicit receiver were never read, so MatrixbycolumnsWidget's"
puts "    setHorizontalSpacing (F14a) and AdaptiveGridLayout's addItem (F5)"
puts "    were not seen. Runs the scanner over a fixture tree with exactly those"
puts "    shapes, and with the shapes that made reading bare calls report bound"
puts "    ones: constructor blocks, signals, strings, nested classes, mixins.)"
def scan_fixture
  Dir.mktmpdir('qt6_scan_fixture') do |root|
    ext = File.join(root, 'ext', 'cosmos', 'ext', 'qt6')
    gui = File.join(root, 'lib', 'cosmos', 'gui')
    FileUtils.mkdir_p([ext, gui])
    File.write(File.join(ext, 'cosmos_qt6.cpp'), <<~CPP)
      cWidget = rb_define_class_under(mQt, "Widget", cQtBase);
      cDialog = rb_define_class_under(mQt, "Dialog", cWidget);
      cMenu = rb_define_class_under(mQt, "Menu", cWidget);
      cLayout = rb_define_class_under(mQt, "Layout", cQtBase);
      cGridLayout = rb_define_class_under(mQt, "GridLayout", cLayout);
      QDEF(cDialog, "accept", RUBY_METHOD_FUNC(f), 0);
      QDEF(cMenu, "addAction", RUBY_METHOD_FUNC(f), 1);
      QDEF(cGridLayout, "addWidget", RUBY_METHOD_FUNC(f), -1);
      QDEF(cGridLayout, "setRowStretch", RUBY_METHOD_FUNC(f), 2);
    CPP
    File.write(File.join(root, 'lib', 'Qt.rb'), <<~'RUBY')
      module Qt
        module ValueDispose
          def dispose; nil; end
        end
        class TextCursor
          def selection; end
        end
        constants.each { |c| const_get(c).send(:include, ValueDispose) }
      end
    RUBY
    File.write(File.join(gui, 'fixture.rb'), <<~'RUBY')
      class Qt::Dialog
        def exec(*args); end          # COSMOS reopens Dialog#exec ...
      end
      class Qt::Menu
        def tear_off
          showTearOffMenu()           # a reopened class's bare call is on it
        end
      end
      %w(GridLayout).each do |klass|
        "Qt::#{klass}".to_class.class_eval do
          def removeAll; end
        end
      end
      module Cosmos
        class Popup
          def show_menu(point)
            menu = Qt::Menu.new
            menu.addAction(nil)
            menu.exec(point)          # ... which hid Menu#exec
          end
        end
        class Matrix < Qt::GridLayout
          def initialize
            super()
            setHorizontalSpacing(2)   # no receiver
            addWidget(nil)
          end
        end
        class Adaptive < Qt::GridLayout
          def addWidget(widget)
            addItem(takeAt(1), 0, 1)  # no receiver
            super(widget)
          end
        end
        class Picker < Qt::Dialog
          signals 'rowPicked(int)'
          def initialize
            super()
            connect(SIGNAL('customContextMenuRequested(const QPoint&)')) { }
            grid = Qt::GridLayout.new do
              addWidget(nil)          # instance_eval'd on the layout
              setVerticalSpacing(1)   # ... where it is unbound
            end
            grid.removeAll
            Qt::GridLayout.new do |g|
              g.addWidget(nil)
              doneLater(1)            # yielded, so this is on the dialog
            end
            glBegin(1)                # the opengl gem's, included at top level
            emit rowPicked(0)
            menu = Qt::Menu.new
            menu.dispose              # lib/Qt.rb mixes dispose into every class
            menu.selection            # lib/Qt.rb defines this on TextCursor only
            menu.removeAll            # class_eval put this on GridLayout only
          end
          class Cell < Qt::GridLayout
            def fill
              setRowStretch(0, 1)     # nested: on the layout
            end
          end
          def spacing; horizontalSpacing(); end   # a one-line def's body
        end
      end
    RUBY
    IO.popen({ 'QT6_SCAN_ROOT' => root }, [RbConfig.ruby, '-rset', File.join(__dir__, 'scan_unbound.rb')],
             err: [:child, :out], &:read)
  end
end
fixture_scan = scan_fixture
chk('it reports Qt::Menu#exec despite COSMOS defining exec elsewhere') { fixture_scan.include?('Qt::Menu#exec') }
chk('it reports setHorizontalSpacing called with no receiver') { fixture_scan.include?('setHorizontalSpacing') }
chk('it reports addItem and takeAt called with no receiver') do
  fixture_scan.include?('Qt::GridLayout#addItem') && fixture_scan.include?('Qt::GridLayout#takeAt')
end
chk('it does not report what is bound or defined in the class') do
  !fixture_scan.include?('#addWidget') && !fixture_scan.include?('#addAction')
end
chk('a constructor block without parameters is read as calls on the new object') do
  fixture_scan.include?('Qt::GridLayout#setVerticalSpacing') && !fixture_scan.include?('Qt::Dialog#addWidget')
end
chk('a constructor block with a parameter stays on the enclosing class') do
  fixture_scan.include?('Qt::Dialog#doneLater')
end
chk('a reopened Qt class is checked as that class') { fixture_scan.include?('Qt::Menu#showTearOffMenu') }
chk('signal strings, signals declarations and opengl gem functions are not calls') do
  %w[customContextMenuRequested rowPicked glBegin].none? { |m| fixture_scan.include?(m) }
end
chk('a nested class is scanned as itself, not as the class around it') do
  !fixture_scan.include?('setRowStretch')
end
chk("a one-line def's body is scanned") { fixture_scan.include?('Qt::Dialog#horizontalSpacing') }
chk("lib/Qt.rb's mixins count on every class, its reopens on one class") do
  !fixture_scan.include?('Qt::Menu#dispose') && fixture_scan.include?('Qt::Menu#selection')
end
chk('class_eval defs count only on the classes the loop lists') do
  !fixture_scan.include?('Qt::GridLayout#removeAll') && fixture_scan.include?('Qt::Menu#removeAll')
end

puts "\n66. Methods the corrected scan_unbound.rb (section 65) found unbound"
puts "   (ToolBar#addWidget, for the classification banner; Widget#showMaximized"
puts "    and showMinimized, for --maximized and --minimized; Completer#"
puts "    setCurrentRow; and AbstractItemModel#index, which completion.rb calls"
puts "    on the line before it. isMaximized/isMinimized are bound to observe.)"
bar = Qt::ToolBar.new
bar_action = bar.addWidget(Qt::Frame.new) rescue $!
chk('ToolBar#addWidget returns the action it added to the bar') do
  (bar_action.is_a?(Qt::Action) && bar.actions.size == 1) || raise(bar_action.inspect)
end
%w[Maximized Minimized].each do |state|
  win = Qt::MainWindow.new
  shown = begin
    win.send("show#{state}")
    nil
  rescue Exception => e
    e
  end
  chk("Widget#show#{state} leaves the window #{state.downcase}") do
    (shown.nil? && win.send("is#{state}")) || raise(shown || "is#{state} is false")
  end
  win.hide
end
completer = Qt::Completer.new(Qt::Widget.new)
completer.setModel(Qt::StringListModel.new(%w[alpha beta], completer))
chk('Completer#setCurrentRow returns what Qt returns') do
  completer.setCurrentRow(1) == true && completer.setCurrentRow(5) == false
end
chk('AbstractItemModel#index returns that row and column') do
  idx = completer.model.index(1, 0)
  idx.row == 1 && idx.column == 0 && idx.data.toString == 'beta'
end

puts "\n67. Every connected block was kept for the life of the process (F21)"
puts "   (connect anchored its block in one Ruby array, g_procs, and nothing"
puts "    removed it, so a block -- and everything it captured -- outlived the"
puts "    object it was connected to. And each single_shot/post_to_main_thread"
puts "    block was removed from that array afterwards with rb_ary_delete, which"
puts "    compares it with every block ever connected.)"
require 'weakref'
def f21_connect_then_dispose(count)
  Array.new(count) do
    edit = Qt::LineEdit.new
    payload = Object.new
    edit.connect(SIGNAL('textChanged(const QString&)')) { payload.to_s }
    ref = WeakRef.new(payload)
    edit.dispose
    ref
  end
end
def f21_connect_live
  edit = Qt::LineEdit.new
  seen = []
  edit.connect(SIGNAL('textChanged(const QString&)')) { |text| seen << text }
  [edit, seen]
end
f21_refs = f21_connect_then_dispose(50)
f21_live, f21_seen = f21_connect_live
scrub_stack
4.times { GC.start }
chk('a block connected to a destroyed object is released') do
  alive = f21_refs.count(&:weakref_alive?)
  alive <= 5 || raise("#{alive} of 50 captured objects still alive")
end
f21_live.setText('after GC')
chk('a block connected to a live object still runs after GC') { f21_seen == ['after GC'] }
# rb_ary_delete compares with ==, so a Proc#== that counts calls on the
# blocks connected here shows each comparison. (A Proc subclass would not
# do: a C method's block argument is re-made as a plain Proc.)
f21_edits = Array.new(200) { Qt::LineEdit.new }
f21_edits.each do |e|
  blk = proc {}
  $f21_mark ||= blk.source_location
  Qt.connect_raw(e, Qt.__sig(SIGNAL('textChanged(const QString&)')), &blk)
end
$f21_compares = 0
class Proc
  def ==(other)
    $f21_compares += 1 if source_location == $f21_mark
    super
  end
end
f21_ran = false
Qt.post_to_main_thread { f21_ran = true }
5.times { APP.processEvents }
class Proc
  remove_method :==
end
chk('removing a posted block does not compare it with every connected block') do
  (f21_ran && $f21_compares.zero?) || raise("ran=#{f21_ran}, #{$f21_compares} comparisons")
end
f21_edits.each(&:dispose)

puts "\n68. Every event on a Ruby-created widget took the GVL (F22)"
puts "   (the binding defines paintEvent, resizeEvent, ... on Qt::Widget so that"
puts "    super works, which made every widget look like it overrode them: each"
puts "    paint, resize and show of every Qt::Label took the GVL and ran Qt's"
puts "    default from Ruby. With a busy Ruby thread each acquisition waits out"
puts "    that thread's time slice: 50 labels took 3.5 s to paint, not 0.1 s.)"
# Calls into the binding's own pass-through (a method an Impl module owns) on
# objects that do not override it -- i.e. calls made only to reach Qt's default.
def f22_passthrough_calls
  calls = 0
  tp = TracePoint.new(:c_call) do |t|
    next unless t.method_id.to_s.end_with?('Event')
    owner = (t.self.method(t.method_id).owner rescue nil)
    calls += 1 if owner && owner.name.to_s.end_with?('::Impl')
  end
  tp.enable
  yield
  tp.disable
  calls
end
f22_win = Qt::Widget.new
f22_box = Qt::VBoxLayout.new(f22_win)
20.times { |i| f22_box.addWidget(Qt::Label.new("L#{i}")) }
f22_win.show
5.times { APP.processEvents }
chk('repainting widgets that override nothing does not call into Ruby') do
  calls = f22_passthrough_calls { f22_win.repaint; APP.processEvents }
  calls.zero? || raise("#{calls} calls into Ruby for 21 widgets")
end
f22_win.hide
# What each override recorded during one repaint of widget.
$f22_seen = []
def f22_painted(widget)
  widget.show
  5.times { APP.processEvents }
  $f22_seen.clear
  widget.repaint
  APP.processEvents
  $f22_seen.uniq
end
class F22Sub < Qt::Label
  def paintEvent(e); $f22_seen << :subclass; super(e); end
end
class F22Late < Qt::Label; end
module F22Mod
  def paintEvent(e); $f22_seen << :module; super(e); end
end
f22_sub = F22Sub.new('sub')
chk('a subclass override is called') { f22_painted(f22_sub) == [:subclass] }
f22_single = Qt::Label.new('singleton')
f22_painted(f22_single)                         # seen first with no override
def f22_single.paintEvent(e); $f22_seen << :singleton; super(e); end
chk('a singleton override added after the first paint is called') { f22_painted(f22_single) == [:singleton] }
f22_late = F22Late.new('reopened')
f22_painted(f22_late)
class F22Late
  def paintEvent(e); $f22_seen << :reopened; super(e); end
end
chk('an override added by reopening the class later is called') { f22_painted(f22_late) == [:reopened] }
class F22Gone < Qt::Label
  def paintEvent(e); $f22_seen << :gone; super(e); end
end
f22_gone = F22Gone.new('removed')
f22_painted(f22_gone)                           # seen first with its override
class F22Gone
  remove_method :paintEvent
end
# A stale answer would only cost a call into Ruby (the binding's pass-through
# records nothing), so that is what this counts.
chk('an override removed later costs no more calls into Ruby') do
  seen = nil
  calls = f22_passthrough_calls { seen = f22_painted(f22_gone) }
  (seen == [] && calls.zero?) || raise("seen #{seen.inspect}, #{calls} calls into Ruby")
end
f22_included = Class.new(Qt::Label).new('included')
f22_painted(f22_included)
f22_included.class.send(:include, F22Mod)
chk('an override from a module included later is called') { f22_painted(f22_included) == [:module] }
f22_extended = Qt::Label.new('extended')
f22_painted(f22_extended)
f22_extended.extend(F22Mod)
chk('an override from a module extended later is called') { f22_painted(f22_extended) == [:module] }
[f22_sub, f22_single, f22_late, f22_gone, f22_included, f22_extended].each(&:hide)
F22_LATENCY = <<~'RUBY'
  $stdout.sync = true
  require 'Qt'
  app = Qt::Application.new([])
  $painted_at = nil
  class Sentinel < Qt::Label           # painted last; the only override
    def paintEvent(e)
      $painted_at ||= Time.now
      super(e)
    end
  end
  win = Qt::Widget.new
  grid = Qt::GridLayout.new(win)
  50.times { |i| grid.addWidget(Qt::Label.new("L#{i}"), i / 10, i % 10) }
  grid.addWidget(Sentinel.new('S'), 6, 0)
  win.resize(900, 700)
  Thread.new { loop { i = 0; i += 1 while i < 5_000_000 } }   # a CPU-bound Ruby thread
  sleep 0.2
  t0 = Time.now
  win.show                             # the paints come from the loop, GVL released
  app.exec_for(3000)
  puts $painted_at ? "painted after #{(($painted_at - t0) * 1000).round} ms" : 'not painted in 3 s'
  exit!(0)
RUBY
chk('with a busy Ruby thread, 50 plain labels paint in under 1.5 s') do
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', F22_LATENCY, err: [:child, :out]], &:read)
  ms = out[/painted after (\d+) ms/, 1]
  (ms && ms.to_i < 1500) || raise(out.lines.grep(/painted/).first.to_s.strip)
end

puts "\n69. Qt calls from a background Ruby thread went through (P1)"
puts "   (qtbindings refused any Qt method, class method or constructor called"
puts "    off the main thread -- Qt.cpp:837 and 1000, qtruby.cpp:1404 -- and"
puts "    COSMOS depends on the refusal: backgroundbutton_widget.rb:43 turns it"
puts "    into advice to use Qt.execute_in_main_thread. Here the calls ran on"
puts "    the wrong thread, which Qt does not support for widgets.)"
P1_REFUSAL = 'Qt methods cannot be called from outside of the main thread'
def p1_off_main
  Thread.new do
    begin
      yield
      :no_error
    rescue Exception => e
      e
    end
  end.value
end
p1_label = Qt::Label.new('before')
{
  'an instance method (Label#setText)' => -> { p1_label.setText('from a thread') },
  'a constructor (Qt::Label.new)'      => -> { Qt::Label.new('from a thread') },
  'a value type (Qt::Color.new)'       => -> { Qt::Color.new(1, 2, 3) },
  'a class method (Qt::Cursor.pos)'    => -> { Qt::Cursor.pos }
}.each do |what, call|
  chk("#{what} off the main thread raises qtbindings' error") do
    r = p1_off_main(&call)
    (r.is_a?(RuntimeError) && r.message == P1_REFUSAL) || raise(r.inspect)
  end
end
chk('the refused setText left the label alone') { p1_label.text == 'before' }
chk('execute_in_main_thread from a background thread still runs its block') do
  got = nil
  t = Thread.new { Qt.execute_in_main_thread(true) { got = [Qt.on_main_thread?, p1_label.text] } }
  100.times { APP.processEvents; sleep 0.01; break unless t.alive? }
  t.join(2)
  got == [true, p1_label.text]
end
chk('disposed? still answers off the main thread (qtbindings did not guard it)') do
  p1_off_main { p1_label.disposed? } == :no_error
end

puts "\n70. Blocks posted before the QApplication existed never ran (P2)"
puts "   (the post went to qApp, still null, so nothing was queued, and a"
puts "    blocking execute_in_main_thread waited forever; on the main thread"
puts "    on_main_thread? was false with no application, so even that call"
puts "    posted and hung. qtbindings queued such blocks and ran them once the"
puts "    application's timers started (lib/Qt4.rb, qtruby4.rb:467), and ran a"
puts "    main-thread call inline. Child processes: this one has its app.)"
def p2_child(script)
  lib = File.expand_path('../../../../lib', __dir__)
  IO.popen([RbConfig.ruby, '-I', lib, '-e', "STDOUT.sync = true\nThread.new { sleep 8; STDOUT.puts 'HUNG'; exit!(3) }\n" + script,
            err: [:child, :out]], &:read)
end
chk('a blocking post from a thread before the app runs once the app does') do
  out = p2_child(<<~'RUBY')
    require 'Qt'
    ran = nil
    waiter = Thread.new { Qt.execute_in_main_thread(true) { ran = Qt.on_main_thread? } }
    sleep 0.3
    app = Qt::Application.new([])
    app.exec_for(1000)
    waiter.join(1)
    STDOUT.puts "ran=#{ran.inspect} waiter=#{waiter.alive? ? 'stuck' : 'returned'}"
    exit!(0)
  RUBY
  out.include?('ran=true waiter=returned') || raise(out.lines.grep(/ran=|HUNG/).first.to_s.strip)
end
chk('execute_in_main_thread on the main thread before the app runs inline') do
  out = p2_child(<<~'RUBY')
    require 'Qt'
    STDOUT.puts "result=#{Qt.execute_in_main_thread(true) { :inline }.inspect}"
    exit!(0)
  RUBY
  out.include?('result=:inline') || raise(out.lines.grep(/result=|HUNG/).first.to_s.strip)
end
chk('a single_shot set before the app fires once the app runs') do
  out = p2_child(<<~'RUBY')
    require 'Qt'
    fired = false
    Qt.single_shot(10) { fired = true }
    app = Qt::Application.new([])
    app.exec_for(500)
    STDOUT.puts "fired=#{fired}"
    exit!(0)
  RUBY
  out.include?('fired=true') || raise(out.lines.grep(/fired=|HUNG/).first.to_s.strip)
end

puts "\n71. Qt::Pen.new(Qt::DashLine) was a solid black pen (F17)"
puts "   (Qt::DashLine was a plain Integer and an Integer took the colour path:"
puts "    DashLine is 2, which is also Qt::black. Cosmos::DASHLINE_PEN"
puts "    (qt.rb:237) draws LineGraph's grid lines, so they were solid.)"
# Which of 60 pixels a horizontal line drawn with +pen+ darkens on white.
def f17_line(pen)
  pixmap = Qt::Pixmap.new(60, 5)
  pixmap.fill(Qt::white)
  painter = Qt::Painter.new(pixmap)
  begin
    painter.setPen(pen)
    painter.drawLine(0, 2, 59, 2)
  ensure
    painter.end
  end
  image = pixmap.toImage
  (0...60).map { |x| image.pixelColor(x, 2).red < 128 }
end
chk('Pen.new(Qt::DashLine) draws a dashed line') do
  dark = f17_line(Qt::Pen.new(Qt::DashLine)).count(true)
  dark.between?(20, 50) || raise("#{dark} of 60 pixels dark")
end
chk('Painter#setPen(Qt::DashLine) draws a dashed line') do
  dark = f17_line(Qt::DashLine).count(true)
  dark.between?(20, 50) || raise("#{dark} of 60 pixels dark")
end
chk('Pen.new(color) is still a solid pen of that colour') do
  f17_line(Qt::Pen.new(Qt::Color.new(0, 0, 0))).count(true) == 60 && Qt::Pen.new(Qt::red).color.red == 255
end
chk('Qt::DashLine still converts and compares as its Integer') do
  Qt::DashLine == 2 && 2 == Qt::DashLine && Qt::DashLine.to_i == 2 && [Qt::DashLine].include?(2)
end

puts "\n72. Qt::KeyEvent.new(type, key, modifiers) was unbound (F14d)"
puts "   (completion.rb:52 builds an Enter key event after inserting a picked"
puts "    completion, to carry on with the line; it raised ArgumentError.)"
chk('KeyEvent.new(type, key, modifiers) builds that key event') do
  e = Qt::KeyEvent.new(Qt::Event::KeyPress, Qt::Key_Enter, Qt::NoModifier)
  (e.type == Qt::Event::KeyPress && e.key == Qt::Key_Enter && e.modifiers == Qt::NoModifier &&
   e.text == '' && e.isAccepted) || raise([e.type, e.key, e.modifiers, e.text].inspect)
end
chk('KeyEvent.new(type, key, modifiers, text) keeps the text') do
  Qt::KeyEvent.new(Qt::Event::KeyPress, Qt::Key_Tab, Qt::NoModifier, "\t").text == "\t"
end

puts "\n73. connect to a signal that does not exist returned true (F19)"
puts "   (connect filed any signal Qt did not know as a Ruby-declared one, so a"
puts "    misspelt SIGNAL('clickd()') connected silently and never fired. Qt"
puts "    warns and returns false, which qtbindings passed through.)"
f19_button = Qt::PushButton.new
f19_result = nil
f19_err = stderr_of { f19_result = f19_button.connect(SIGNAL('clickd()')) { } }
chk('connecting to a signal that does not exist returns false') { f19_result == false || raise(f19_result.inspect) }
chk('and says so on stderr, as Qt does') { f19_err.include?('No such signal') || raise(f19_err.inspect) }
class F19Emitter < Qt::Object
  signals 'changed(int)'
end
class F19Child < F19Emitter; end
chk('a signal declared in Ruby still connects and fires, on a subclass too') do
  got = []
  [F19Emitter.new, F19Child.new].each_with_index do |o, i|
    r = o.connect(SIGNAL('changed(int)')) { |v| got << v }
    raise "connect returned #{r.inspect}" unless r == true
    o.emit(o.changed(i + 1))
  end
  got == [1, 2] || raise(got.inspect)
end

puts "\n74. Strings from Qt are what qtbindings returned (F26: not a divergence)"
puts "   (qtbindings made them with rb_str_new2(s->toUtf8()) -- handlers.cpp:1041,"
puts "    marshall_QString -- so ASCII-8BIT and cut at the first NUL, like ours."
puts "    COSMOS source is ascii-8bit throughout; UTF-8 strings from Qt could"
puts "    raise Encoding::CompatibilityError against binary telemetry strings.)"
chk('a string from Qt is ASCII-8BIT') { Qt::Label.new('abc').text.encoding == Encoding::ASCII_8BIT }
chk('and ends at an embedded NUL') { Qt::Label.new("a\0b").text == 'a' }

puts "\n75. The missing-extension message named a rake task that does not exist (F24)"
puts "   (lib/Qt.rb told the user to run 'rake build_extensions'; the task is"
puts "    'rake build'.)"
chk('the LoadError message names a rake task the Rakefile defines') do
  named = File.read(File.join(ROOT, 'lib', 'Qt.rb'))[/then:\s+rake (\S+)/, 1]
  tasks = IO.popen([RbConfig.ruby, '-S', 'rake', '-P'], chdir: ROOT, err: File::NULL, &:read)
            .lines.grep(/^rake /).map { |l| l.split[1] }
  (named && tasks.include?(named)) || raise("the message names #{named.inspect}")
end

puts "\n76. The gem left out tasks/qt6.rake, which its Rakefile imports (F25)"
puts "   (the gemspec takes its file list from Manifest.txt, which lacked it, so"
puts "    any rake command in the built gem aborted with a LoadError)"
chk('every file the Rakefile imports is in the gem') do
  imports = File.read(File.join(ROOT, 'Rakefile')).scan(/^import '([^']+)'/).flatten
  files = Dir.chdir(ROOT) { Gem::Specification.load('cosmos.gemspec').files }
  missing = imports - files
  missing.empty? || raise("not in the gem: #{missing.join(', ')}")
end

puts "\n52. on_destroyed raced the GC for the wrapper it was marking"
puts "   (it read the wrapper VALUE from the weak map, dropped the lock and"
puts "    waited for the GVL; meanwhile a sweep on another Ruby thread -- every"
puts "    QtTool runs redirect_io's thread, qt_tool.rb:476 -- could free that"
puts "    garbage wrapper, and the type check on the freed slot crashed. Seen in"
puts "    test_cosmos_tools.rb: a QMessageBox label's QTextFrame destroyed by"
puts "    dialog.exec's layout pass. Child process: 8 s of that path under a"
puts "    GC-looping thread; it crashed in about 1 run in 4 at 5 s.)"
DESTROY_RACE = <<~'RUBY'
  $stdout.sync = true
  require 'Qt'
  app = Qt::Application.new([])
  stop = false
  gcs = 0
  Thread.new { until stop; GC.start; gcs += 1; end }
  host = Qt::Widget.new
  layout = Qt::VBoxLayout.new
  labels = Array.new(40) { |i| Qt::Label.new("<b>label</b> #{i}") }
  labels.each { |l| layout.addWidget(l) }
  host.setLayout(layout)
  host.show
  app.processEvents
  t0 = Time.now
  rounds = 0
  while Time.now - t0 < (ARGV[0] || 4).to_f
    labels.each { |l| l.findChildren }          # wrap the labels' internal text objects
    labels.each_with_index { |l, i| l.setText("<i>round #{rounds}</i> label #{i}") }
    app.exec_for(2)                             # relayout -> frames destroyed without the GVL
    rounds += 1
  end
  stop = true
  puts "survived #{rounds} rounds, #{gcs} background GCs"
RUBY
chk('Qt destroying objects whose wrappers are garbage, under GC from another thread') do
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', DESTROY_RACE, '8', err: [:child, :out]], &:read)
  ($?.success? && out.include?('survived')) ||
    raise($?.signaled? ? "died with SIG#{Signal.signame($?.termsig)}" : "exit #{$?.exitstatus}: #{out.lines.grep(/BUG/).first.to_s.strip}")
end

puts "\n77. super in a StyledItemDelegate override raised NoMethodError"
puts "   (Qt::StyledItemDelegate had no paint, createEditor, setEditorData or"
puts "    setModelData for super to reach. CmdSender's parameter delegate calls"
puts "    super for every cell it does not draw itself, so each of those paints"
puts "    reported an error, and after its painter.save the skipped restore"
puts "    left Qt warning 'Painter ended with 1 saved states')"
class SuperDelegate < Qt::StyledItemDelegate
  attr_reader :calls
  def initialize(parent); super(parent); @calls = []; end
  def paint(painter, option, index)
    @calls << :paint
    painter.save
    super(painter, option, index)
    painter.restore
  end
  def createEditor(parent, option, index); @calls << :createEditor; super(parent, option, index); end
  def setEditorData(editor, index); @calls << :setEditorData; super(editor, index); end
  def setModelData(editor, model, index); @calls << :setModelData; super(editor, model, index); end
end
f77_table = Qt::TableWidget.new
f77_table.setRowCount(1)
f77_table.setColumnCount(1)
f77_table.setItem(0, 0, Qt::TableWidgetItem.new('before'))
f77_delegate = SuperDelegate.new(f77_table)
f77_table.setItemDelegate(f77_delegate)
f77_table.show
APP.processEvents
f77_paint = stderr_of { f77_table.grab }
chk('paint calling super reports no error') do
  f77_delegate.calls.include?(:paint) || raise('paint never reached Ruby')
  f77_paint.empty? || raise(f77_paint.lines.first.to_s.strip)
end
f77_edit = stderr_of { f77_table.editItem(f77_table.item(0, 0)) }
f77_editor = f77_table.viewport.findChildren.find { |c| c.is_a?(Qt::LineEdit) }
chk('createEditor and setEditorData calling super open an editor on the cell') do
  f77_edit.empty? || raise(f77_edit.lines.first.to_s.strip)
  (f77_editor && f77_editor.text == 'before') || raise("editor #{f77_editor.inspect}")
end
chk('setModelData calling super writes the edit back') do
  f77_editor.setText('after')
  commit = stderr_of { f77_delegate.commitData(f77_editor) }
  commit.empty? || raise(commit.lines.first.to_s.strip)
  f77_table.item(0, 0).text == 'after' || raise("cell #{f77_table.item(0, 0).text.inspect}")
end
f77_table.hide
chk('Qt::Style.CE_ItemViewItem reads the constant, as qtbindings did') do
  # cmd_param_table_item_delegate.rb:65 paints the description column with it
  Qt::Style.CE_ItemViewItem == Qt::Style::CE_ItemViewItem
end

puts "\n78. Qt::AbstractItemModel#setData was unbound"
puts "   (the setModelData overrides of CmdSender's parameter delegate and"
puts "    TableManager's write a chosen state back with model.setData"
puts "    (cmd_param_table_item_delegate.rb:74, table_manager.rb:75): every"
puts "    commit reported NoMethodError and Qt's default wrote the value)"
def f78_table
  table = Qt::TableWidget.new
  table.setRowCount(1)
  table.setColumnCount(1)
  table.setItem(0, 0, Qt::TableWidgetItem.new('x'))
  table
end
chk('model.setData(index, Variant, EditRole) sets the cell and returns true') do
  table = f78_table
  model = table.model
  ok = model.setData(model.index(0, 0), Qt::Variant.new('y'), Qt::EditRole)
  (ok == true && table.item(0, 0).text == 'y') ||
    raise("returned #{ok.inspect}, cell #{table.item(0, 0).text.inspect}")
end
chk('setData takes a plain value and defaults the role to EditRole') do
  table = f78_table
  model = table.model
  model.setData(model.index(0, 0), 'z')
  table.item(0, 0).text == 'z' || raise("cell #{table.item(0, 0).text.inspect}")
end

puts "\n79. Handing an event to another widget's handler ran the caller's default"
puts "   (qt_base_event, the pass-through that makes super work, ran whatever"
puts "    default was being dispatched and ignored its receiver. TableManager"
puts "    hands each combo box's wheel to its window, whose default ignores"
puts "    it so the table scrolls (table_manager.rb:20-24); the combo's own"
puts "    default ran instead and changed the value under the mouse)"
class HandOffDialog < Qt::Dialog
  attr_accessor :other, :handler
  def closeEvent(e); @other.__send__(@handler, e); end   # no super on itself
end
# Closes a HandOffDialog that hands its close event to +handler+ of a plain
# widget. QDialog's own default would call reject() first.
def hand_off_close(handler)
  d = HandOffDialog.new
  d.other = Qt::Widget.new
  d.handler = handler
  rejected = false
  d.connect(SIGNAL('rejected()')) { rejected = true }
  d.show
  APP.processEvents
  err = stderr_of { d.close }
  err.empty? || raise(err.lines.first.to_s.strip)
  rejected && raise("the dialog's own QDialog::closeEvent ran (rejected)")
  !d.isVisible || raise('the close did not go through')
end
chk("closeEvent handed to another widget runs that widget's default") do
  hand_off_close(:closeEvent)   # QWidget's default accepts the close
end
chk('an event handed to a handler of another kind runs nothing') do
  hand_off_close(:wheelEvent)   # a close event is not a wheel event
end
chk('super still runs the dispatching widget\'s own default') do
  d = Class.new(Qt::Dialog) { def closeEvent(e); super(e); end }.new
  rejected = false
  d.connect(SIGNAL('rejected()')) { rejected = true }
  d.show
  APP.processEvents
  d.close
  APP.processEvents
  rejected || raise('QDialog::closeEvent did not run')
end

puts "\n80. Qt::ListWidgetItem.new(text, list) left the list empty"
puts "   (ExceptionListDialog adds each exception that way"
puts "    (exception_list_dialog.rb:39), so its list showed nothing)"
chk('ListWidgetItem.new(text, list) adds the item to the list') do
  list = Qt::ListWidget.new
  Qt::ListWidgetItem.new('first', list)
  Qt::ListWidgetItem.new('second', list)
  (list.count == 2 && list.item(1).text == 'second') || raise("count #{list.count}")
end
chk('ListWidgetItem.new(icon, text, list) too') do
  list = Qt::ListWidget.new
  Qt::ListWidgetItem.new(Qt::Icon.new, 'x', list)
  list.count == 1 || raise("count #{list.count}")
end

puts "\n81. Menu#addSeparator returned the menu, not the separator"
puts "   (PacketViewer labels its View menu's separator with"
puts "    addSeparator.setText('Formatting') (packet_viewer.rb:191), which"
puts "    retitled the whole menu 'Formatting')"
chk('addSeparator returns the separator action') do
  menu = Qt::Menu.new('&View')
  sep = menu.addSeparator
  sep.setText('Formatting')
  (sep.is_a?(Qt::Action) && menu.actions.include?(sep) && menu.title == '&View') ||
    raise("returned #{sep.class}, menu title #{menu.title.inspect}")
end
chk('ToolBar#addSeparator returns the separator action') do
  Qt::ToolBar.new.addSeparator.is_a?(Qt::Action)
end

puts "\n82. TreeWidgetItem font/setFont/childCount/child/checkState/parent were"
puts "    unbound, and DialogButtonBox.new(buttons) built no buttons"
puts "   (qt.rb reopens TreeWidgetItem with column defaults that call super"
puts "    (qt.rb:394-424) and every COSMOS tree's itemClicked handler reads"
puts "    checkState and parent (qt.rb:336-349), so a checkbox click raised;"
puts "    TestRunner's Test Selections dialog also reads font while it is"
puts "    built (test_runner.rb:764) and asks for Ok|Cancel buttons (856))"
def f82_tree
  tree = Qt::TreeWidget.new
  tree.setColumnCount(1)
  suite = Qt::TreeWidgetItem.new(['Suite'])
  tree.addTopLevelItem(suite)
  test = Qt::TreeWidgetItem.new(['Test'])
  suite.addChild(test)
  [tree, suite, test]
end
chk('TreeWidgetItem#font(column) and setFont(column, font)') do
  _, suite, = f82_tree
  font = suite.font(0)
  font.setBold(true)
  suite.setFont(0, font)
  suite.font(0).bold || raise('setFont did not take')
end
chk('TreeWidgetItem#childCount, #child(i) and #parent') do
  _, suite, test = f82_tree
  (suite.childCount == 1 && suite.child(0).text(0) == 'Test' &&
   test.parent.text(0) == 'Suite' && suite.parent.nil?) ||
    raise("childCount #{suite.childCount}, parent #{suite.parent.inspect}")
end
chk('TreeWidgetItem#checkState(column) reads setCheckState') do
  _, suite, = f82_tree
  suite.setCheckState(0, Qt::Checked)
  suite.checkState(0) == Qt::Checked || raise("checkState #{suite.checkState(0).inspect}")
end
chk('DialogButtonBox.new(Ok | Cancel) builds both buttons') do
  box = Qt::DialogButtonBox.new(Qt::DialogButtonBox::Ok | Qt::DialogButtonBox::Cancel)
  texts = box.findChildren.select { |c| c.is_a?(Qt::PushButton) }.map(&:text)
  texts.size == 2 || raise("buttons #{texts.inspect}")
end

puts "\n83. Two wrappers of the same Qt item compared unequal"
puts "   (an item wrapper is made anew for every lookup, and == was object"
puts "    identity. TestRunner's Test Selections unchecks every suite that"
puts "    is not the clicked item's top level (test_runner.rb:753), so it"
puts "    unchecked the suite just clicked as well)"
chk('items compare equal when they wrap the same Qt item') do
  tree, suite, test = f82_tree
  (tree.topLevelItem(0) == suite && test.parent == suite && suite.child(0).eql?(test) &&
   suite.child(0).hash == test.hash) || raise('same item compared unequal')
end
chk('items compare unequal when they wrap different Qt items') do
  _, suite, test = f82_tree
  suite != test || raise('different items compared equal')
end
chk('table and list items compare the same way') do
  table = Qt::TableWidget.new
  table.setRowCount(1)
  table.setColumnCount(1)
  cell = Qt::TableWidgetItem.new('x')
  table.setItem(0, 0, cell)
  list = Qt::ListWidget.new
  entry = Qt::ListWidgetItem.new('y', list)
  (table.item(0, 0) == cell && list.item(0) == entry) || raise('same item compared unequal')
end

puts "\n84. ListWidgetItem#data and ListWidget#row were unbound"
puts "   (LimitsMonitor's Ignored Telemetry Items dialog reads each selected"
puts "    item's data on Delete (limits_monitor.rb:786), then removes the"
puts "    selection with qt.rb's remove_selected_items, which calls row(item)"
puts "    (qt.rb:614): nothing was ever removed)"
chk('ListWidgetItem#data(role) returns what setData stored') do
  item = Qt::ListWidgetItem.new('x')
  item.setData(Qt::UserRole, Qt::Variant.new(%w[INST HEALTH_STATUS TEMP1]))
  (v = item.data(Qt::UserRole).value) == %w[INST HEALTH_STATUS TEMP1] || raise("data #{v.inspect}")
end
chk('ListWidget#row(item) is the item\'s row') do
  list = Qt::ListWidget.new
  Qt::ListWidgetItem.new('a', list)
  b = Qt::ListWidgetItem.new('b', list)
  list.row(b) == 1 || raise("row #{list.row(b).inspect}")
end

puts "\n85. ListWidgetItem#dispose freed nothing"
puts "   (COSMOS removes list entries with takeItem(i).dispose (qt.rb:598,"
puts "    614, 690; tlm_extractor.rb:57); dispose was lib/Qt.rb's no-op for"
puts "    value types, so every removed entry leaked)"
chk('dispose deletes the item: an item still in its list leaves it') do
  list = Qt::ListWidget.new
  Qt::ListWidgetItem.new('a', list)
  list.item(0).dispose            # ~QListWidgetItem takes it out of the list
  list.count == 0 || raise("count #{list.count} after dispose")
end
chk('a taken item disposes once; a second dispose and later calls are safe') do
  list = Qt::ListWidget.new
  Qt::ListWidgetItem.new('a', list)
  item = list.takeItem(0)
  item.dispose
  item.dispose
  item.disposed? || raise('not disposed')
  begin
    item.text
    raise 'text on a disposed item did not raise'
  rescue RuntimeError => e
    raise if e.message.include?('did not raise')
    true
  end
end

puts "\n86. Ruby sizeHint and minimumSizeHint overrides were never consulted"
puts "   (neither virtual was forwarded, so layouts sized a Ruby widget by"
puts "    Qt's default: TlmGrapher's overview graph (sizeHint 0x50,"
puts "    overview_graph.rb:81) came out 0 px tall, and OpenGL Builder's"
puts "    viewer (gl_viewer.rb:87-93) got Qt's default size)"
class HintedWidget < Qt::Widget
  def sizeHint; Qt::Size.new(0, 50); end
  def minimumSizeHint; Qt::Size.new(0, 30); end
end
class HintedFromSuper < Qt::Widget
  def sizeHint; s = super; Qt::Size.new(s.width, 77); end
end
# The widget's height in a column whose other entry takes the spare room,
# as the plots do above TlmGrapher's overview graph.
def f86_height(widget, max_height)
  widget.setMaximumHeight(max_height)
  host = Qt::Widget.new
  layout = Qt::VBoxLayout.new
  layout.addWidget(Qt::Widget.new, 1)
  layout.addWidget(widget)
  host.setLayout(layout)
  host.resize(400, 300)
  host.show
  APP.processEvents
  widget.height
ensure
  host.hide if host
end
def f86_min_height(widget)
  host = Qt::Widget.new
  layout = Qt::VBoxLayout.new
  layout.addWidget(widget)
  host.setLayout(layout)
  host.minimumSizeHint.height
end
chk('a layout sizes a widget by its Ruby sizeHint') do
  (h = f86_height(HintedWidget.new, 50)) == 50 || raise("height #{h}")
end
chk('a layout reads the Ruby minimumSizeHint') do
  (d = f86_min_height(HintedWidget.new) - f86_min_height(Qt::Widget.new)) == 30 || raise("minimum differs by #{d}")
end
chk('super from a sizeHint override reaches Qt\'s, not the override') do
  (h = f86_height(HintedFromSuper.new, 77)) == 77 || raise("height #{h}")
end

puts "\n87. Validator constructors dropped the line edit, and fixup never ran"
puts "   (IntegerChooser and FloatChooser build IntegerChooserIntValidator.new"
puts "    (@value) and override fixup to clamp an out-of-range entry through"
puts "    parent().setText (integer_chooser.rb:15-31, float_chooser.rb:15-29)."
puts "    parent was nil, and fixup was not forwarded, so the entry stayed out"
puts "    of range)"
class ClampingValidator < Qt::IntValidator
  def fixup(input)
    parent.setText(top.to_s) if input.to_i > top
  end
end
class RewritingValidator < Qt::IntValidator
  def fixup(input)
    input.replace(top.to_s)   # Qt's contract: fix the string in place
  end
end
# Types +text+ into a line edit guarded by +validator_class+ (0..100) and
# moves the focus away, which is when QLineEdit calls fixup.
def f87_entry(validator_class, text)
  host = Qt::Widget.new
  layout = Qt::VBoxLayout.new
  entry = Qt::LineEdit.new('5')
  other = Qt::LineEdit.new('x')
  layout.addWidget(entry)
  layout.addWidget(other)
  host.setLayout(layout)
  validator = validator_class.new(entry)
  validator.setBottom(0)
  validator.setTop(100)
  entry.setValidator(validator)
  finished = 0
  entry.connect(SIGNAL('editingFinished()')) { finished += 1 }
  host.show
  APP.processEvents
  entry.setFocus
  APP.processEvents
  entry.setText(text)
  other.setFocus
  APP.processEvents
  [entry.text, finished]
ensure
  host.hide if host
end
chk('IntValidator.new(line_edit) is parented to the line edit') do
  entry = Qt::LineEdit.new
  (v = Qt::IntValidator.new(entry)).parent == entry || raise("parent #{v.parent.inspect}")
end
chk('DoubleValidator.new(line_edit) and (bottom, top, decimals, line_edit)') do
  entry = Qt::LineEdit.new
  v1 = Qt::DoubleValidator.new(entry)
  v2 = Qt::DoubleValidator.new(0.0, 1.0, 3, entry)
  (v1.parent == entry && v2.parent == entry) || raise("parents #{[v1.parent, v2.parent].inspect}")
end
chk('a Ruby fixup clamps an out-of-range entry when editing finishes') do
  text, = f87_entry(ClampingValidator, '500')
  text == '100' || raise("text #{text.inspect}")
end
chk('a fixup that rewrites its argument is applied, and editing finishes') do
  text, finished = f87_entry(RewritingValidator, '500')
  (text == '100' && finished == 1) || raise("text #{text.inspect}, editingFinished x#{finished}")
end

puts "\n88. Settings#setValue raised TypeError for a Float or a boolean"
puts "   (it was registered twice, the later one winning, and both turned"
puts "    anything but an Integer, Size, Point or Variant into a String with"
puts "    rb_to_qs; COSMOS itself always passes a Variant (qt_tool.rb:255))"
chk('Settings#setValue takes a Float, a boolean and an Array, like Variant.new') do
  file = File.join(Dir.tmpdir, "qt6_regressions_#{Process.pid}.ini")
  begin
    settings = Qt::Settings.new(file, Qt::Settings::IniFormat)
    settings.setValue('scale', 2.5)
    settings.setValue('on', true)
    settings.setValue('names', %w[a b])
    settings.setValue('count', 3)
    got = %w[scale on names count].map { |k| settings.value(k).value }
    got == [2.5, true, %w[a b], 3] || raise("read back #{got.inspect}")
  ensure
    FileUtils.rm_f(file)
  end
end

puts "\n89. Qt::Application#exec_for closed every window"
puts "   (it ended its loop with QCoreApplication::quit, which in Qt 6 closes"
puts "    every top-level window first. COSMOS never calls it; these suites"
puts "    do, between checks that expect their windows to stay as they were.)"
chk('exec_for leaves top-level windows open') do
  w = Qt::Widget.new
  w.show
  APP.exec_for(20)
  visible = w.isVisible
  w.hide
  visible || raise('the window was closed')
end
chk('a modal exec after exec_for still runs until its dialog closes') do
  APP.exec_for(20)
  d = Qt::Dialog.new
  Qt.single_shot(150) { d.accept }
  t0 = Time.now
  d.exec
  ms = ((Time.now - t0) * 1000).round
  ms >= 100 || raise("exec returned after #{ms} ms")
end

puts "\n90. The wrong value or item type raised 'wrong argument type Qt::Value"
puts "    (expected Qt::Value)'"
puts "   (every value type shared one Ruby data type name, and every item"
puts "    type another, so the TypeError could not say which was which)"
def f90_type_error
  yield
  raise 'no TypeError'
rescue TypeError => e
  e.message
end
chk('a value of the wrong type names both types') do
  (m = f90_type_error { Qt::Widget.new.resize(Qt::Point.new(1, 2)) }) ==
    'wrong argument type Qt::Point (expected Qt::Size)' || raise(m)
end
chk('an item of the wrong type names both types') do
  (m = f90_type_error { Qt::TreeWidget.new.addTopLevelItem(Qt::ListWidgetItem.new('x')) }) ==
    'wrong argument type Qt::ListWidgetItem (expected Qt::TreeWidgetItem)' || raise(m)
end

puts "\n91. Only four of Qt's global colors existed (Qt::blue was undefined)"
puts "   (black, white, red and lightGray are all COSMOS itself uses; qtbindings"
puts "    had every Qt::GlobalColor, which screens and tools outside the repo"
puts "    may use)"
chk('every Qt::GlobalColor is defined and names its color') do
  expected = {
    blue: [0, 0, 255], green: [0, 255, 0], yellow: [255, 255, 0], cyan: [0, 255, 255],
    magenta: [255, 0, 255], gray: [160, 160, 164], darkGray: [128, 128, 128],
    darkRed: [128, 0, 0], darkGreen: [0, 128, 0], darkBlue: [0, 0, 128],
    darkCyan: [0, 128, 128], darkMagenta: [128, 0, 128], darkYellow: [128, 128, 0],
    color0: [255, 255, 255], color1: [0, 0, 0], transparent: [0, 0, 0],
  }
  wrong = expected.reject do |name, rgb|
    c = Qt::Color.new(Qt.__send__(name))
    [c.red, c.green, c.blue] == rgb
  end
  wrong.empty? || raise("wrong: #{wrong.keys.join(', ')}")
end

puts "\n92. Loading lib/Qt.rb a second time made every post recurse"
puts "   (its post_to_main_thread wrapper aliases the binding's method first;"
puts "    a second load aliased the wrapper to itself, and the next post"
puts "    raised SystemStackError. COSMOS itself only requires it, once.)"
DOUBLE_LOAD = <<~'RUBY'
  $stdout.sync = true
  require 'Qt'
  load File.join(ARGV[0], 'Qt.rb')
  app = Qt::Application.new([])
  ran = false
  Qt.post_to_main_thread { ran = true }
  app.processEvents
  puts(ran ? 'ran' : 'not run')
RUBY
chk('a second load of lib/Qt.rb leaves post_to_main_thread working') do
  lib = File.expand_path('../../../../lib', __dir__)
  out = IO.popen([RbConfig.ruby, '-I', lib, '-e', DOUBLE_LOAD, lib, err: [:child, :out]], &:read)
  out.include?('ran') || raise(out.lines.grep(/Error/).first.to_s.strip)
end

puts "\n93. rake build kept a stale qt6 bundle and blamed a missing Qt6"
puts "   (when the qt6 extension built nothing, lib/cosmos/ext kept the"
puts "    bundle of an earlier build, which still loaded as if current, and"
puts "    the message always said 'Qt6 not found' -- also when extconf.rb had"
puts "    written its stub because moc failed. The Rakefile's qt6_not_built"
puts "    now removes the stale bundle and reports extconf.rb's own reason.)"
QT6_NOT_BUILT = <<~'RUBY'
  require 'rake'
  Rake.application.init('rake', [])
  Dir.chdir(ARGV[0])
  Rake.application.load_rakefile
  puts qt6_not_built(ARGV[1], ARGV[2])
  puts(File.exist?(ARGV[2]) ? 'stale bundle kept' : 'stale bundle removed')
RUBY
chk("rake build's qt6 skip removes a stale bundle and gives extconf.rb's reason") do
  Dir.mktmpdir('qt6_not_built') do |dir|
    # extconf.rb in a copy, with no Qt6 to find: it writes the stub Makefile
    bin = File.join(dir, 'bin')
    Dir.mkdir(bin)
    FileUtils.cp(File.join(__dir__, 'extconf.rb'), dir)
    if WINDOWS
      File.write(File.join(bin, 'pkg-config.bat'), "@exit /b 1\r\n")
    else
      File.write(File.join(bin, 'pkg-config'), "#!/bin/sh\nexit 1\n")
      FileUtils.chmod(0755, File.join(bin, 'pkg-config'))
    end
    force_linux = "Object.send(:remove_const, :RUBY_PLATFORM); RUBY_PLATFORM = 'x86_64-linux'.freeze; load 'extconf.rb'"
    IO.popen({ 'PATH' => [bin, ENV['PATH']].join(File::PATH_SEPARATOR), 'COSMOS_QT6_REQUIRED' => nil },
             [RbConfig.ruby, '-e', force_linux, chdir: dir, err: [:child, :out]], &:read)
    stale = File.join(dir, 'qt6.bundle')
    File.write(stale, 'an earlier build')
    out = IO.popen([RbConfig.ruby, '-e', QT6_NOT_BUILT, ROOT, File.join(dir, 'Makefile'), stale,
                    err: [:child, :out]], &:read)
    (out.include?('qt6: not built (Qt6 not found (brew --prefix qt / pkg-config Qt6Core))') &&
     out.include?('stale bundle removed')) || raise(out.lines.last(2).join.strip)
  end
end

puts
if $failures.empty?
  puts 'ALL REGRESSION CHECKS PASSED'
  exit 0
else
  puts "#{$failures.size} FAILURES: #{$failures.inspect}"
  exit 1
end
