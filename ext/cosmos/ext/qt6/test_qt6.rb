#!/usr/bin/env ruby
# Regression suite for the COSMOS Ruby<->Qt6 binding.
#
#   QT_QPA_PLATFORM=offscreen ruby -Ilib ext/cosmos/ext/qt6/test_qt6.rb
#
# Runs headless (offscreen platform); no display or window server required.

$stdout.sync = true   # keep output if a check crashes the process
require 'Qt'   # the drop-in entry point (loads cosmos/ext/qt6 + Ruby layer)

$fail = []
def chk(name) yielded = (yield rescue $!); ok = (yielded == true)
  puts format("  %-52s %s", name, ok ? "ok" : "FAIL (#{yielded.inspect})")
  $fail << name unless ok
end
def section(t) puts "\n#{t}" end

app = Qt::Application.new

section "1. load and hierarchy"
chk("Qt 6.x runtime")                { Qt.qVersion.start_with?("6") }
chk("Label < Frame < Widget")        { Qt::Label.superclass == Qt::Frame && Qt::Frame.superclass == Qt::Widget }
chk("Widget < Base (line_graph.c)")  { Qt::Widget.superclass == Qt::Base }
chk("Object < Base")                 { Qt::Object.superclass == Qt::Base }
chk("PushButton < AbstractButton")   { Qt::PushButton.superclass == Qt::AbstractButton }
chk("VBoxLayout < BoxLayout<Layout") { Qt::VBoxLayout.superclass == Qt::BoxLayout && Qt::BoxLayout.superclass == Qt::Layout }

section "2. widgets and properties"
w = Qt::Widget.new
w.setWindowTitle("COSMOS")
chk("windowTitle round-trip")        { w.windowTitle == "COSMOS" }
chk("setEnabled/isEnabled")          { w.setEnabled(false); w.isEnabled == false }
w.setEnabled(true)
chk("setToolTip round-trip")         { w.setToolTip("tip"); w.toolTip == "tip" }
l = Qt::Label.new("hello")
chk("Label text ctor + setText")     { l.setText("world"); l.text == "world" }
chk("Label inherits Widget methods") { l.setEnabled(false); l.isEnabled == false }
b = Qt::PushButton.new("go")
chk("Button text via AbstractButton"){ b.text == "go" }
chk("Button checkable/checked")      { b.setCheckable(true); b.setChecked(true); b.isChecked }
e = Qt::LineEdit.new("abc")
chk("LineEdit text + readOnly")      { e.setReadOnly(true); e.text == "abc" && e.isReadOnly }
chk("LineEdit clear")                { e.setReadOnly(false); e.clear; e.text == "" }
c = Qt::ComboBox.new
chk("ComboBox addItem/count")        { c.addItem("a"); c.addItem("b"); c.count == 2 }
chk("ComboBox currentIndex/text")    { c.setCurrentIndex(1); c.currentText == "b" }
p = Qt::PlainTextEdit.new("body")
chk("PlainTextEdit round-trip")      { p.toPlainText == "body" }
g = Qt::GroupBox.new("grp")
chk("GroupBox title")                { g.title == "grp" }
a = Qt::Action.new("Save")
chk("Action text + trigger")         { a.setCheckable(true); a.trigger; a.text == "Save" }

section "3. layouts and nesting"
v = Qt::VBoxLayout.new
h = Qt::HBoxLayout.new
v.addWidget(Qt::Label.new("x"))
v.addLayout(h)
chk("VBox count after add")          { v.count == 2 }
chk("addLayout accepted")            { h.is_a?(Qt::HBoxLayout) }
mw = Qt::MainWindow.new
inner = Qt::Widget.new
mw.setCentralWidget(inner)
chk("MainWindow setCentralWidget")   { !inner.owned? }

section "4. signals (COSMOS SIGNAL('clicked()') idiom)"
fired = 0
btn = Qt::PushButton.new("click")
btn.connect(SIGNAL("clicked()")) { fired += 1 }       # the COSMOS idiom
btn.click; btn.click
chk("instance connect + SIGNAL() fires") { fired == 2 }
recv_fired = false
target = Qt::Widget.new
def target.on_click; $recv = true; end
btn2 = Qt::PushButton.new("c2")
btn2.connect(btn2, SIGNAL("clicked()"), target, SLOT("on_click()"))
btn2.click
chk("4-arg connect -> receiver slot")    { $recv == true }
chk("slots/signals class macros")        { k = Class.new(Qt::Widget) { slots "about()" }; k.qt_slots == ["about()"] }
chk("unknown signal raises")             { begin; Qt.connect(btn, "nope()") {}; false
                                           rescue ArgumentError; true; end }

section "5. class reopening and subclassing (the hard constraint)"
class Qt::Label
  def shout!; setText(text.upcase + "!"); self; end
end
chk("reopen native class")           { Qt::Label.new("hi").shout!.text == "HI!" }
class StatusLabel < Qt::Label
  def status=(s); setText("status: #{s}"); end
end
sl = StatusLabel.new("init")
sl.status = "CONNECTED"
chk("subclass native class")         { sl.text == "status: CONNECTED" }
chk("subclass keeps is_a? chain")    { sl.is_a?(Qt::Label) && sl.is_a?(Qt::Widget) }
chk("reopen+super reaches binding")  { Qt::Painter.class_eval { def setPen(c); super(c); :reopened; end }
                                       px = Qt::Pixmap.new(10,10); pt = Qt::Painter.new(px)
                                       r = pt.setPen(Qt::black); pt.end; r == :reopened }

section "6. ownership and lifetime"
lab = Qt::Label.new("child"); lay = Qt::VBoxLayout.new; par = Qt::Widget.new
chk("fresh object Ruby-owned")       { lab.owned? }
lay.addWidget(lab)
chk("addWidget releases ownership")  { !lab.owned? }
par.setLayout(lay)
chk("setLayout releases ownership")  { !lay.owned? }
chk("QApplication never Ruby-owned") { !app.owned? }
par.destroy!
chk("parent destroyed")              { par.destroyed? }
chk("child destroyed via signal")    { lab.destroyed? }
chk("dead object raises, no crash")  { begin; lab.text; false
                                       rescue RuntimeError => ex; ex.message =~ /destroyed/ ? true : ex; end }

section "7. GC safety"
base = Qt.object_count
2000.times { Qt::Label.new("churn") }
GC.start; GC.start
chk("GC reclaims orphans")           { Qt.object_count < base + 2000 }
chk("reparented tree survives GC")   { 50.times { ww = Qt::Widget.new; vv = Qt::VBoxLayout.new
                                         3.times { vv.addWidget(Qt::Label.new("n")) }; ww.setLayout(vv) }
                                       GC.start; true }

section "8. event loop vs Ruby threads (GVL)"
ticks = 0; mu = Mutex.new
ths = 3.times.map { Thread.new { loop { mu.synchronize { ticks += 1 }; sleep 0.005 } } }
cbs = []
Qt.single_shot(150) { cbs << :a }
Qt.single_shot(400) { cbs << :b }
before = mu.synchronize { ticks }
app.exec_for(700)
after = mu.synchronize { ticks }
# The 400 ms callback missed the 700 ms window once on windows-latest (PR #7,
# run 36202371689, attempt 1; the rerun passed). Keep running the loop until
# both have run, for up to 5 s, and report the ones that did if not.
deadline = Time.now + 5
app.exec_for(50) while cbs.size < 2 && Time.now < deadline
ths.each(&:kill)
chk("Ruby threads run during exec()"){ (after - before) > 30 }
chk("Qt calls into Ruby during exec"){ cbs == [:a, :b] || "callbacks run: #{cbs.inspect}" }

section "9. value types"
ks = Qt::KeySequence.new("Ctrl+Q")
chk("KeySequence round-trip")        { ks.toString == "Ctrl+Q" }
act = Qt::Action.new("Quit"); act.setShortcut(ks)
chk("Action setShortcut/shortcut")   { act.shortcut.toString == "Ctrl+Q" }
chk("Variant int")                   { Qt::Variant.new(42).toInt == 42 }
chk("Variant string")                { Qt::Variant.new("hi").toString == "hi" }
chk("Variant bool")                  { Qt::Variant.new(true).toBool == true }
fnt = Qt::Font.new("Courier", 12); fnt.setBold(true)
chk("Font family/size/bold")         { fnt.family == "Courier" && fnt.pointSize == 12 && fnt.bold }
chk("Color rgb + name")              { c = Qt::Color.new(255,128,0); c.name == "#ff8000" && c.green == 128 }
chk("Color from GlobalColor")        { Qt::Color.new(Qt::red).name == "#ff0000" }
chk("Size / widget.size")            { ww = Qt::Widget.new; ww.resize(300,200); ww.size.width == 300 }
chk("Point")                         { Qt::Point.new(4,5).y == 5 }
chk("SizePolicy constants exist")    { Qt::SizePolicy::Fixed.is_a?(Integer) && Qt::SizePolicy::Expanding.is_a?(Integer) }
chk("setSizePolicy accepts them")    { ww = Qt::Widget.new
                                       ww.setSizePolicy(Qt::SizePolicy::Fixed, Qt::SizePolicy::Expanding); true }
chk("MessageBox button constants")   { Qt::MessageBox::Yes.is_a?(Integer) && Qt::MessageBox::Ok.is_a?(Integer) }
chk("MessageBox statics defined")    { %i[warning critical information question].all? { |m| Qt::MessageBox.respond_to?(m) } }
chk("FileDialog statics defined")    { %i[getOpenFileName getSaveFileName getExistingDirectory].all? { |m| Qt::FileDialog.respond_to?(m) } }

section "10. containers and item views"
tbl = Qt::TableWidget.new
tbl.setRowCount(2); tbl.setColumnCount(3)
tbl.setItem(0, 0, Qt::TableWidgetItem.new("cell"))
chk("TableWidget dimensions")        { tbl.rowCount == 2 && tbl.columnCount == 3 }
chk("TableWidgetItem round-trip")    { tbl.item(0,0).text == "cell" }
tree = Qt::TreeWidget.new; tree.setColumnCount(1)
root = Qt::TreeWidgetItem.new("root"); root.addChild(Qt::TreeWidgetItem.new("kid"))
tree.addTopLevelItem(root)
chk("TreeWidget top-level item")     { tree.topLevelItemCount == 1 }
chk("TreeWidgetItem text(col)")      { root.text(0) == "root" }
lst = Qt::ListWidget.new; lst.addItem("a"); lst.addItem("b")
chk("ListWidget add/count/itemText") { lst.count == 2 && lst.itemText(1) == "b" }
tabs = Qt::TabWidget.new
tabs.addTab(Qt::Widget.new, "One"); tabs.addTab(Qt::Widget.new, "Two")
chk("TabWidget addTab/tabText")      { tabs.count == 2 && tabs.tabText(1) == "Two" }
sp = Qt::Splitter.new; sp.addWidget(Qt::Label.new("l")); sp.addWidget(Qt::Label.new("r"))
chk("Splitter addWidget")            { sp.count == 2 }
sa = Qt::ScrollArea.new; inner2 = Qt::Widget.new; sa.setWidget(inner2)
chk("ScrollArea takes ownership")    { !inner2.owned? }
fl = Qt::FormLayout.new; fl.addRow("Name:", Qt::LineEdit.new)
chk("FormLayout addRow")             { fl.count >= 1 }

section "11. menus, bars, numeric widgets, timer"
mw2 = Qt::MainWindow.new
menu = mw2.menuBar.addMenu("&File")
menu.addAction(Qt::Action.new("Open")); menu.addSeparator
chk("MenuBar addMenu -> Menu")       { menu.is_a?(Qt::Menu) }
chk("Menu title")                    { menu.title == "&File" }
mw2.statusBar.showMessage("ready")
chk("StatusBar showMessage")         { mw2.statusBar.currentMessage == "ready" }
tb = Qt::ToolBar.new("main"); tb.addAction(Qt::Action.new("Go"))
chk("ToolBar addAction")             { tb.findChildren.any? { |c| c.is_a?(Qt::Action) } }
pb = Qt::ProgressBar.new; pb.setRange(0,10); pb.setValue(7)
chk("ProgressBar value")             { pb.value == 7 }
sld = Qt::Slider.new; sld.setRange(0,100); sld.setValue(42)
chk("Slider (inherited setValue)")   { sld.value == 42 }
sb2 = Qt::SpinBox.new; sb2.setRange(0,10); sb2.setValue(3)
chk("SpinBox value")                 { sb2.value == 3 }
ds = Qt::DoubleSpinBox.new; ds.setRange(0.0, 10.0); ds.setValue(2.5)
chk("DoubleSpinBox float value")     { ds.value == 2.5 }
te = Qt::TextEdit.new; te.setPlainText("body")
chk("TextEdit round-trip")           { te.toPlainText == "body" }
tm = Qt::Timer.new; tm.setInterval(50); tm.setSingleShot(true)
chk("Timer interval/isActive")       { tm.interval == 50 && tm.isActive == false }
chk("RadioButton via AbstractButton"){ rb2 = Qt::RadioButton.new("opt"); rb2.text == "opt" }

section "12. model/view"
m = Qt::StringListModel.new(%w[INST INST2 SYSTEM], nil)
chk("StringListModel(list, parent)")  { m.is_a?(Qt::AbstractItemModel) }
cmp = Qt::Completer.new; cmp.setModel(m)
chk("Completer#setModel")             { cmp.model.equal?(m) }
fs = Qt::FileSystemModel.new
ridx = fs.setRootPath(Dir.pwd)
chk("FileSystemModel#setRootPath")    { ridx.is_a?(Qt::ModelIndex) && ridx.isValid }
chk("FileSystemModel#filePath")       { fs.filePath(ridx).include?(File.basename(Dir.pwd)) }
trv = Qt::TreeView.new; trv.setModel(fs); trv.setRootIndex(ridx); trv.setColumnHidden(1, true)
chk("TreeView setModel/setRootIndex") { trv.model.equal?(fs) && trv.isColumnHidden(1) }
tw = Qt::TableWidget.new; tw.setRowCount(3); tw.setColumnCount(2)
tw.verticalHeader.setResizeMode(Qt::HeaderView::ResizeToContents)
tw.horizontalHeader.setResizeMode(Qt::HeaderView::Stretch)
# Qt4 setResizeMode -> Qt6 setSectionResizeMode; assert the mode actually took.
chk("HeaderView setResizeMode")       { tw.horizontalHeader.sectionResizeMode(0) == Qt::HeaderView::Stretch }

class ProbeDelegate < Qt::StyledItemDelegate
  attr_reader :seen
  def initialize; super(); @seen = []; end
  def createEditor(parent, option, index)
    @seen << [:createEditor, index.row, index.column]; Qt::LineEdit.new("e")
  end
  def setEditorData(editor, index); @seen << [:setEditorData, index.row]; end
  def setModelData(editor, model, index); @seen << [:setModelData, index.row]; end
end
dlg = ProbeDelegate.new
tw.setItemDelegate(dlg)
tw.setItem(1, 0, Qt::TableWidgetItem.new("cell"))
tw.show; 3.times { app.processEvents }
tw.editItem(tw.item(1, 0)); 5.times { app.processEvents }
chk("delegate createEditor dispatched") { dlg.seen.any? { |x| x[0] == :createEditor } }
chk("delegate got correct ModelIndex")  { dlg.seen.any? { |x| x[0] == :createEditor && x[1] == 1 && x[2] == 0 } }
chk("delegate setEditorData dispatched"){ dlg.seen.any? { |x| x[0] == :setEditorData } }

section "13. OpenGL (Qt::GLWidget)"
chk("Qt::GLWidget exists")            { Qt::GLWidget.ancestors.include?(Qt::Widget) }
chk("GL context methods bound")       { %i[makeCurrent doneCurrent updateGL isValid grabFrameBuffer]
                                          .all? { |mm| Qt::GLWidget.method_defined?(mm) } }
if ENV['QT_QPA_PLATFORM'] == 'offscreen'
  puts "  (GL virtual dispatch skipped: offscreen has no GL. Re-run without QT_QPA_PLATFORM=offscreen)"
else
  gw = Class.new(Qt::GLWidget) do
    attr_reader :calls
    def initialize; super(); @calls = []; end
    def initializeGL; @calls << :initializeGL; end
    def resizeGL(w, h); @calls << [:resizeGL, w, h]; end
    def paintGL; @calls << :paintGL; end
  end.new
  gw.resize(64, 48); gw.show
  8.times { app.processEvents }
  chk("GL virtuals dispatch to Ruby") { c = gw.calls.map { |x| x.is_a?(Array) ? x[0] : x }
                                        c.include?(:initializeGL) && c.include?(:paintGL) }
  chk("resizeGL receives w,h")        { gw.calls.any? { |x| x.is_a?(Array) && x[0] == :resizeGL } }
end

section "14. dialog result codes (post-exec paths)"
# Regression: reaching the event loop is not enough -- code AFTER exec()
# returns must work too. COSMOS's LegalDialog does
#   exit if exec() != Qt::Dialog::Accepted      (legal_dialog.rb:93)
chk("Qt::Dialog::Accepted == 1")      { Qt::Dialog::Accepted == 1 }
chk("Qt::Dialog::Rejected == 0")      { Qt::Dialog::Rejected == 0 }
dlg = Qt::Dialog.new
Qt.single_shot(300) { w = Qt::Application.activeModalWidget || Qt::Application.activeWindow
                      w.accept if w }
chk("exec() returns Accepted")        { dlg.exec == Qt::Dialog::Accepted }
dlg2 = Qt::Dialog.new
Qt.single_shot(300) { w = Qt::Application.activeModalWidget || Qt::Application.activeWindow
                      w.reject if w }
chk("exec() returns Rejected")        { dlg2.exec == Qt::Dialog::Rejected }

# Every Qt::X::Y reference in COSMOS should resolve, not just the ones a
# happy-path launch happens to touch.
scoped = `git -C #{File.expand_path('../../../..', __dir__)} grep -ohE "Qt::[A-Z][A-Za-z]*::[A-Za-z_][A-Za-z_0-9]*" -- lib`.split("\n").map(&:strip).uniq
unresolved = scoped.reject do |ref|
  begin
    Object.const_get(ref); true
  rescue NameError
    parts = ref.split("::"); m = parts.pop
    begin; Object.const_get(parts.join("::")).respond_to?(m); rescue NameError; false; end
  end
end
chk("all #{scoped.size} scoped Qt::X::Y refs resolve") { scoped.size > 50 && unresolved.empty? ? true : (unresolved.empty? ? "grep found only #{scoped.size}" : unresolved) }

section "15. background threads vs blocking event loops"
# Regression: COSMOS initialises CmdTlmServer on a background Ruby thread while
# the main thread sits in a blocking Qt loop (splash.rb:104). If exec() holds
# the GVL that thread never runs and the app hangs on the splash forever.
progressed = 0
mu2 = Mutex.new
th = Thread.new { 200.times { mu2.synchronize { progressed += 1 }; sleep 0.002 } }
d3 = Qt::Dialog.new
Qt.single_shot(500) { w = Qt::Application.activeModalWidget || Qt::Application.activeWindow
                      w.accept if w }
d3.exec                      # blocking modal loop
th.kill
chk("thread runs during dialog.exec") { mu2.synchronize { progressed } > 5 }

# Regression: posting GUI work from a background thread must not construct a
# QObject on that thread ("Cannot create children for a parent in a different
# thread" is a crash, not a warning).
got = nil
worker = Thread.new do
  Qt.execute_in_main_thread(true) { got = Qt.on_main_thread? }
end
app.exec_for(1500)
worker.join(2)
chk("execute_in_main_thread runs on main") { got == true }

# Regression: a Ruby slot that triggers another signal must not re-acquire the
# GVL recursively (that corrupts Ruby's heap and crashes unrelated threads).
inner = 0
b3 = Qt::PushButton.new("outer")
b4 = Qt::PushButton.new("inner")
b4.connect(SIGNAL("clicked()")) { inner += 1 }
b3.connect(SIGNAL("clicked()")) { b4.click }
b3.click
chk("nested signal dispatch is safe")  { inner == 1 }

# Same idea for class-level calls written with dot notation -- Qt::Cursor.pos
# is a static, and missing ones only surface when a user clicks the feature.
statics = `git -C #{File.expand_path('../../../..', __dir__)} grep -ohE "Qt::[A-Z][A-Za-z]*\.[a-z][A-Za-z_0-9]*" -- lib`.split("\n").map(&:strip).uniq
unbound_statics = statics.reject do |ref|
  cls, meth = ref.split('.')
  next true if meth.nil? || meth.empty? || meth == 'new'
  begin; Object.const_get(cls).respond_to?(meth); rescue NameError; false; end
end
chk("all #{statics.size} Qt::Class.method calls resolve") do
  statics.size > 50 && unbound_statics.empty? ? true :
    (unbound_statics.empty? ? "grep found only #{statics.size}" : unbound_statics)
end

section "16. GVL stress (signals + threads + GC concurrently)"
# The failure mode this guards against is "pthread_mutex_lock: Invalid
# argument (EINVAL)" / heap corruption: calling into Ruby from a thread that
# does not hold (or cannot acquire) the GVL. It only shows up under
# concurrent signal dispatch, background threads and GC.
stress_errors = []
fired_total = 0
begin
  buttons = 5.times.map { |i| Qt::PushButton.new("b#{i}") }
  buttons.each { |b| b.connect(SIGNAL("clicked()")) { fired_total += 1 } }

  # The labels are made on the main thread, from a timer inside the event
  # loop: the binding refuses Qt calls from any other thread, as qtbindings
  # did (test_regressions.rb section 69). They become garbage that gcer's
  # sweeps free off the main thread.
  churned = 0
  churn = Qt::Timer.new
  churn.connect(SIGNAL("timeout()")) do
    10.times { Qt::Label.new("x") }
    churned += 10
    churn.stop if churned >= 300
  end
  churn.start(1)
  gcer  = Thread.new { 20.times { GC.start; sleep 0.01 } }
  poster = Thread.new do
    30.times { Qt.execute_in_main_thread(false) { fired_total += 0 }; sleep 0.005 }
  end

  40.times { buttons.each(&:click) }
  app.exec_for(1200)
  [gcer, poster].each { |t| t.join(3) }
  churn.stop
  GC.start
rescue Exception => e
  stress_errors << "#{e.class}: #{e.message}"
end
chk("survives concurrent signal/thread/GC load") { stress_errors.empty? ? true : stress_errors }
chk("all signal dispatches landed")              { fired_total >= 200 }
chk("the event loop churned 300 labels")          { churned >= 300 }

puts
if $fail.empty?
  puts "ALL CHECKS PASSED"
  exit 0
else
  puts "#{$fail.size} FAILURES: #{$fail.inspect}"
  exit 1
end
