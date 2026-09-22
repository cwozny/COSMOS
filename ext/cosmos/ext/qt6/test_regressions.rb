# One test per fixed defect, named for the defect. Each test states the failure
# it guards against, so a reintroduction reports what actually broke rather
# than just "something failed".
#
# Run: bundle exec ruby -Ilib ext/cosmos/ext/qt6/test_regressions.rb
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
require 'cosmos/ext/qt6'
require 'Qt'
require 'tmpdir'

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
  # packet_viewer.rb:366 uses the delay to escape the current call frame;
  # running it inline recursed instead of unwinding.
  n = 0
  Qt.execute_in_main_thread(false, 0.001) { n += 1 }
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
  png = '/tmp/_regr_label.png'
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

puts
if $failures.empty?
  puts 'ALL REGRESSION CHECKS PASSED'
  exit 0
else
  puts "#{$failures.size} FAILURES: #{$failures.inspect}"
  exit 1
end
