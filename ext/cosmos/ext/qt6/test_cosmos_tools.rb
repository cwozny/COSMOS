# Drives real COSMOS tool code on the Qt6 binding, headless, against the demo
# project in this repo. One section per fixed defect whose failure shows
# through a tool's own code path, so a reintroduction reports what the
# operator would see rather than just the binding call that broke.
#
# Output goes to STDOUT explicitly: ScriptRunnerFrame#redirect_io replaces
# $stdout and $stderr with multiplexers that no longer write to the terminal.
#
# Run: ruby -Ilib ext/cosmos/ext/qt6/test_cosmos_tools.rb
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
ENV['COSMOS_USERPATH'] ||= File.expand_path('../../../../demo', __dir__)
require 'tmpdir'
require 'fileutils'
# Cosmos.data_path (top_level.rb:157), which get_icon uses, asks Bundler for
# cosmos-* extension gems. Outside `bundle exec` Bundler resolves the repo
# Gemfile, whose DART half needs rails, and raises GemNotFound. An empty
# Gemfile means "no extension gems"; under `bundle exec` the caller's stays.
$cleanups = []   # run before exit!, which skips at_exit
unless ENV['BUNDLE_GEMFILE']
  gemfile_dir = Dir.mktmpdir('qt6_cosmos_tools')
  File.write(File.join(gemfile_dir, 'Gemfile'), "source 'https://rubygems.org'\n")
  ENV['BUNDLE_GEMFILE'] = File.join(gemfile_dir, 'Gemfile')
  $cleanups << -> { FileUtils.rm_rf(gemfile_dir) }
end
require 'cosmos'
require 'cosmos/gui/qt'
require 'cosmos/tools/script_runner/script_runner'

STDOUT.sync = true
$failures = []
def say(msg = '')
  STDOUT.puts msg
end
def chk(name)
  $current_check = name
  ok = begin
    yield
  rescue Exception => e
    $failures << "#{name}: #{e.class}: #{e.message}"
    say format('  %-52s FAIL (%s: %s)', name, e.class, e.message.to_s[0, 60])
    return
  end
  $failures << name unless ok
  say format('  %-52s %s', name, ok ? 'ok' : 'FAIL')
end

# ScriptRunnerFrame redirects $stderr away from the terminal, so an error
# escaping a check's setup would end the suite silently -- and a worker thread
# still waiting on the GUI (a Splash thread) would then hang Ruby's exit.
# Report it and leave at once.
at_exit do
  if $! && !$!.is_a?(SystemExit)
    STDOUT.puts "UNCAUGHT #{$!.class}: #{$!.message} (in #{$current_check.inspect})"
    ($!.backtrace || []).first(12).each { |line| STDOUT.puts "  #{line}" }
    $cleanups.each(&:call)
    STDOUT.flush
    exit!(1)
  end
end

# A check that leaves a modal loop open would hang the suite (and CI); name
# it and stop instead.
Thread.new do
  sleep 300
  STDOUT.puts "WATCHDOG: still running after 300 s, in #{$current_check.inspect}"
  exit!(2)
end

APP = Qt::Application.new([])
Cosmos::System.targets
# Message logs (ScriptRunner writes one per run) and exception logs go to a
# temporary directory instead of demo/outputs/logs.
logs_dir = Dir.mktmpdir('qt6_cosmos_tools_logs')
Cosmos::System.paths['LOGS'] = logs_dir
$cleanups << -> { FileUtils.rm_rf(logs_dir) }
# The demo enables sound; the reopened Qt::Dialog#exec (script_module_gui.rb:19)
# would play a wav on every dialog the suite opens.
Cosmos::System.instance.instance_variable_set(:@sound, false)

# Pumps events until the block returns truthy or the time runs out.
def wait_for(seconds = 5)
  deadline = Time.now + seconds
  until (result = yield) || Time.now > deadline
    APP.processEvents
    sleep 0.01
  end
  result
end

# Runs the block on the first visible top-level dialog titled +title+, from
# inside the modal loop the code under test starts. If the block raises, or
# the dialog never shows, every visible dialog is rejected so the modal loop
# ends and the check fails instead of hanging the suite.
def on_dialog(title, deadline = Time.now + 5, &blk)
  Qt.single_shot(20) do
    dialog = Qt::Application.topLevelWidgets.find do |w|
      w.is_a?(Qt::Dialog) && w.isVisible && w.windowTitle == title
    end
    if dialog
      begin
        blk.call(dialog)
      rescue Exception => e
        say "  on_dialog(#{title.inspect}) block raised #{e.class}: #{e.message}"
        dialog.reject
      end
    elsif Time.now < deadline
      on_dialog(title, deadline, &blk)
    else
      say "  on_dialog(#{title.inspect}): dialog never appeared"
      Qt::Application.topLevelWidgets.each { |w| w.reject if w.is_a?(Qt::Dialog) && w.isVisible }
    end
  end
end

def button(parent, text)
  parent.findChildren.find { |c| c.is_a?(Qt::PushButton) && c.text == text }
end

# Records and dismisses every message box that shows (ExceptionDialog is
# one), so an error dialog fails the check that caused it instead of
# leaving the suite stuck in its modal loop.
$message_boxes = []
$message_texts = []   # the same boxes' label text
BOX_WATCH = Qt::Timer.new
BOX_WATCH.connect(SIGNAL('timeout()')) do
  Qt::Application.topLevelWidgets.each do |w|
    next unless w.is_a?(Qt::MessageBox) && w.isVisible
    $message_boxes << w.windowTitle
    $message_texts << w.findChildren.select { |c| c.is_a?(Qt::Label) }.map(&:text).join("\n")
    w.reject
  end
end
BOX_WATCH.start(50)
def no_message_boxes
  $message_boxes.empty? || raise("message box: #{$message_boxes.join(', ')}")
end

# Runs dialog.exec with +action+ fired from inside the modal loop, and
# rejects the dialog after +limit_ms+ if it is still open, so a dialog that
# fails to close fails its check instead of hanging the suite. Returns
# [exec's result, whether the fallback had to close it].
def exec_with(dialog, limit_ms = 3000, &action)
  closed = false
  fallback = false
  Qt.single_shot(20) { action.call }
  Qt.single_shot(limit_ms) { (fallback = true; dialog.reject) unless closed }
  result = dialog.exec
  closed = true
  [result, fallback]
end

say "\n0. Harness"
say "   (every dialog check below needs a modal loop that really runs; in"
say "    test_regressions.rb, from section 9 on, exec returns at once)"
chk('a modal dialog loop runs until the dialog closes') do
  d = Qt::Dialog.new
  t0 = Time.now
  _, fallback = exec_with(d, 200) {}
  ms = ((Time.now - t0) * 1000).round
  d.dispose
  (fallback && ms >= 150) || raise("exec returned after #{ms} ms")
end

say "\n1. ScriptRunner Toggle Disconnect left the real targets connected"
say "   (Qt::Palette.new(RED_PALETTE) raised TypeError at"
say "    script_runner_frame.rb:1198, after the status bar already said"
say "    'Targets disconnected' and before set_disconnected_targets ran."
say "    Clearing hit the same raise on DEFAULT_PALETTE at :1194.)"
win = Qt::MainWindow.new
frame = Cosmos::ScriptRunnerFrame.new(win)
win.setCentralWidget(frame)
recorded = nil
# Record the call instead of starting a disconnected CmdTlmServer.
frame.define_singleton_method(:set_disconnected_targets) { |*args| recorded = args }
toggle = lambda do
  frame.toggle_disconnect(Cosmos::CmdTlmServer::DEFAULT_CONFIG_FILE)
  nil
rescue Exception => e
  e
end

on_dialog('Disconnect Settings') { |d| button(d, 'Ok').click }
error = toggle.call
wait_for { recorded }   # Splash.execute runs the block on its own thread
chk('disconnecting every target completes') { error.nil? || raise(error) }
chk('status bar names the disconnected targets') do
  win.statusBar.currentMessage == "Targets disconnected: #{Cosmos::System.targets.keys.join(' ')}"
end
chk('set_disconnected_targets ran for every target') do
  recorded && recorded[0] == Cosmos::System.targets.keys && recorded[1] == true
end

on_dialog('Disconnect Settings') { |d| button(d, 'Clear All').click; button(d, 'Ok').click }
error = toggle.call
chk('reconnecting (no targets checked) completes') { error.nil? || raise(error) }
chk('status bar is cleared') { win.statusBar.currentMessage == '' }
chk('disconnected targets are cleared') { get_disconnected_targets.nil? }

say "\n2. CmdSender sent the displayed state instead of a typed raw value"
say "   (a raw value typed for a state parameter only reaches the command"
say "    once the state column reads MANUALLY -- params_text,"
say "    cmd_params.rb:72-73. The itemChanged handler that writes MANUALLY"
say "    died on TableWidgetItem#row at cmd_params.rb:337.)"
require 'cosmos/tools/cmd_sender/cmd_params'
def param_row(table, name)
  (0...table.rowCount).find { |r| table.item(r, 0).text == "#{name}:" }
end
cmd_params = Cosmos::CmdParams.new
table = cmd_params.update_cmd_params(Cosmos::System.commands.packet('INST', 'ASCIICMD'))
row = param_row(table, 'STRING')
chk('ASCIICMD STRING starts at its default state, NOOP') do
  [table.item(row, 1).text, table.item(row, 2).text] == ['NOOP', 'NOOP']
end
# The same model change a committed cell edit makes (the delegate's
# setModelData).
table.item(row, 2).setText('HELLO')
chk('typing a raw value switches the state to MANUALLY') do
  table.item(row, 1).text == Cosmos::CmdParams::MANUALLY
end
chk('params_text sends the typed value, not NOOP') do
  (sent = cmd_params.params_text['STRING']) == 'HELLO' || raise("sent #{sent.inspect}")
end
# COLLECT TYPE is REQUIRED, so both of its columns start empty.
table = cmd_params.update_cmd_params(Cosmos::System.commands.packet('INST', 'COLLECT'))
table.item(param_row(table, 'TYPE'), 2).setText('5')
chk('a raw value typed for a REQUIRED state is sent') do
  (sent = cmd_params.params_text['TYPE']) == 5 || raise("sent #{sent.inspect}")
end

say "\n3. ScriptRunner could not start a script"
say "   (run_callback marks the running tab with tabBar.setTabIcon at"
say "    script_runner.rb:754, and tabBar was a plain Qt::Widget, so every"
say "    run died there. stop_callback died the same way at :766.)"
_, sr_options = Cosmos::ScriptRunner.create_default_options
sr_options.title = 'Script Runner : Untitled'
sr_options.auto_size = false
sr_options.remember_geometry = false
# QtTool#initialize's redirect_io swaps $stdout for a StringIO. With several
# tools in this one process, the last one built would take $stdout away from
# ScriptRunnerFrame's own redirect, and a script run died on
# $stdout.add_stream. Each real tool is its own process.
sr_options.redirect_io = false
sr_options.server_config_file = Cosmos::CmdTlmServer::DEFAULT_CONFIG_FILE
sr_options.run_procedure = nil
sr_options.disconnect_mode = false
script_runner = Cosmos::ScriptRunner.new(sr_options)
tab_book = script_runner.instance_variable_get(:@tab_book)
script_runner.create_tab
running = tab_book.tabs[0]
chk('run_callback completes') { script_runner.run_callback(running); true }
chk('the running tab shows the running icon') { !tab_book.tabBar.tabIcon(0).isNull }
chk('the other tab is disabled while it runs') { !tab_book.tabBar.isTabEnabled(1) }
chk('stop_callback completes') { script_runner.stop_callback(running); true }
chk('the icon is cleared and the other tab re-enabled') do
  tab_book.tabBar.tabIcon(0).isNull && tab_book.tabBar.isTabEnabled(1)
end

say "\n4. ProgressDialog could not close itself"
say "   (close_done calls Qt::Dialog#done, which was unbound. ScriptRunner"
say "    instruments every script in a ProgressDialog and closes it with"
say "    close_done (script_runner_frame.rb:551), so each Start showed 'Error"
say "    During Progress'. The Done button (TlmViewer, TestRunner,"
say "    TlmExtractor, TlmGrapher) did nothing.)"
$message_boxes.clear
instrumented = Cosmos::ScriptRunnerFrame.instrument_script("x = 1\n", 'progress_probe.rb')
chk('instrumenting a script shows no error dialog') { no_message_boxes }
chk('instrumenting returns the instrumented script') { instrumented.to_s.include?('x = 1') }
progress = Cosmos::ProgressDialog.new(nil, 'Done button', 300, 100, true, false, false, true, false)
progress.complete   # what a finished worker does: enables Done
_, fallback = exec_with(progress) { button(progress, 'Done').click }
chk('the Done button closes the dialog') { !fallback && progress.complete? }
progress.dispose
chk('Dialog#done(r) ends a modal exec with r') do
  d = Qt::Dialog.new
  result, fallback = exec_with(d) { d.done(7) }
  d.dispose
  (!fallback && result == 7) || raise("exec returned #{result.inspect}, fallback=#{fallback}")
end

say "\n5. ScriptRunner and TestRunner stopped every script at line 1"
say "   (the current-line highlight runs before each line"
say "    (script_runner_frame.rb:658-660) and TextCharFormat#setBackground"
say "    (completion_text_edit.rb:174) was unbound, so line 1 raised before"
say "    its code ran. Starting a script also goes through sections 3 and 4.)"
$qt6_suite_lines = []
$message_boxes.clear
running.set_text("$qt6_suite_lines << 1\n$qt6_suite_lines << 2\n")
running.run
wait_for(20) { !Cosmos::ScriptRunnerFrame.running? }
output = running.instance_variable_get(:@output).toPlainText
chk('the script runs every line') { $qt6_suite_lines == [1, 2] || raise("ran #{$qt6_suite_lines.inspect}") }
chk('the output log says the script completed') do
  output.include?('Script completed') || raise("log: #{output.lines.last(3).join.strip.inspect}")
end
chk('no error dialog') { no_message_boxes }

say "\n6. Qt::ColorListWidget#addItemColor raised (TlmGrapher's left frame)"
say "   (overview_tabbed_plots.rb:978 adds 'No Plot Selected' with a colour"
say "    swatch: Pixmap#fill was unbound (qt.rb:671), and the next call,"
say "    ListWidget#addItem(item) (qt.rb:677), only took a String)"
color_list = Qt::ColorListWidget.new(nil)
chk('addItemColor adds the entry') { color_list.addItemColor('No Plot Selected'); color_list.count == 1 }
chk('the entry carries its text') { color_list.item(0) && color_list.item(0).text == 'No Plot Selected' }
chk('getItemColor returns the colour it was given') { color_list.getItemColor(0) == Cosmos::BLACK }

say "\n7. TlmGrapher could not add plots, so it could not even start"
say "   (every plot goes through Qt::AdaptiveGridLayout#addWidget, whose"
say "    super(widget) hit the 3-5 argument GridLayout#addWidget, and whose"
say "    reorder at 3 and 7 plots needs Layout#addItem. The demo's default"
say "    configuration has plots, so startup failed there first.)"
adaptive = Qt::AdaptiveGridLayout.new
plots = Array.new(4) { |i| Qt::Label.new("plot #{i}") }
chk('AdaptiveGridLayout takes 4 plots') { plots.each { |w| adaptive.addWidget(w) }; adaptive.count == 4 }
chk('they are arranged 2 x 2') do
  host = Qt::Widget.new
  host.setLayout(adaptive)
  host.resize(400, 300)
  host.show
  APP.processEvents
  xs = plots.map { |w| w.pos.x }
  ys = plots.map { |w| w.pos.y }
  host.hide
  (xs[0] == xs[2] && xs[1] == xs[3] && xs[1] > xs[0] && ys[0] == ys[1] && ys[2] > ys[0]) ||
    raise("positions #{xs.zip(ys).inspect}")
end

# Starts a tool in a child process (tool_smoke.rb) and returns
# [passed, output]. A startup error ends in ExceptionDialog's exit, which
# must not end this suite.
def tool_starts(require_path, class_name, seconds = 6)
  smoke = File.join(__dir__, 'tool_smoke.rb')
  out = IO.popen([RbConfig.ruby, '-I', File.expand_path('../../../../lib', __dir__), smoke,
                  require_path, class_name, seconds.to_s, err: [:child, :out]], &:read)
  [$?.success?, out]
end
chk('TlmGrapher starts without an error dialog') do
  ok, out = tool_starts('cosmos/tools/tlm_grapher/tlm_grapher', 'Cosmos::TlmGrapher')
  ok || raise(out.lines.grep(/DIALOG|\|/).first(6).join.strip)
end

say "\n8. LineGraph froze after the first mouse-over; Help > About failed"
say "   (both read Widget#cursor / #x / #y, which were unbound -- see"
say "    test_regressions.rb section 37)"
require 'cosmos/gui/line_graph/line_graph'
require 'cosmos/gui/dialogs/about_dialog'
graph = Cosmos::LineGraph.new
graph.add_line('TEMP1', [1.0, 2.0, 3.0, 2.0], [0, 1, 2, 3])
graph.resize(400, 200)
graph.show
APP.processEvents
draws = 0
graph.singleton_class.prepend(Module.new do
  define_method(:draw_graph_to_screen) { |*args| draws += 1; super(*args) }
end)
graph.mouseMoveEvent(nil)   # the real handler: flags the mouse in, redraws
2.times { graph.repaint; APP.processEvents }
chk('a paint with the mouse over the graph completes') { graph.instance_variable_get(:@painter).nil? }
chk('the graph keeps repainting after it') { draws >= 2 || raise("drew #{draws} time(s)") }
graph.hide
about_parent = Qt::MainWindow.new
about_parent.move(10, 20)
about_text = nil
about_fallback = false
Qt.single_shot(100) do
  about = Qt::Application.topLevelWidgets.find { |w| w.is_a?(Cosmos::AboutDialog) && w.isVisible }
  if about
    about_text = about.findChildren.select { |c| c.is_a?(Qt::Label) }.map(&:text).join("\n")
    about.reject
  end
end
Qt.single_shot(3000) do
  Qt::Application.topLevelWidgets.each do |w|
    (about_fallback = true; w.reject) if w.is_a?(Cosmos::AboutDialog) && w.isVisible
  end
end
about_error = begin
  Cosmos::AboutDialog.new(about_parent, 'Qt6 test tool')
  nil
rescue Exception => e
  e
end
chk('Help > About opens') { about_error.nil? || raise(about_error) }
chk('it reports the main window position') do
  (about_text.to_s.include?('Main Application x:10 y:20') && !about_fallback) ||
    raise("about text #{about_text.to_s[/Main Application[^\n]*/].inspect}")
end

say "\n9. TlmViewer: LEDs ended their screen's updates; SPACER screens did not open"
say "   (the demo's own INST screens: params has 5 LEDs, spacing_box uses"
say "    SPACER; see test_regressions.rb section 38)"
require 'cosmos/tools/tlm_viewer/tlm_viewer'
# Opens a demo INST screen the way TlmViewer does. No CmdTlmServer runs, so
# the value thread just retries (DRbConnError) and never feeds the widgets.
def open_screen(name)
  $message_boxes.clear
  file = File.join(Cosmos::USERPATH, 'config', 'targets', 'INST', 'screens', "#{name}.txt")
  screen = Cosmos::Screen.new("INST #{name.upcase}", file)
  3.times { APP.processEvents; sleep 0.02 }
  screen
end
def close_screen(screen)
  screen.window.close if screen && screen.window
  APP.processEvents
end
spacing_box = open_screen('spacing_box')
chk('INST spacing_box (SPACER) opens') do
  (spacing_box.window && $message_boxes.empty?) || raise("did not open: #{$message_boxes.inspect}")
end
close_screen(spacing_box)
params = open_screen('params')
leds = params.widgets.select { |w| w.is_a?(Cosmos::LedWidget) }
chk('INST params opens with its 5 LEDs') { params.window && leds.size == 5 && no_message_boxes }
chk('every LED takes a value, as the update thread sets it') do
  leds.each { |led| led.value = 0 }
  leds.all? { |led| led.instance_variable_get(:@brush).is_a?(Qt::Brush) }
end
close_screen(params)

say "\n10. TlmViewer: MATRIXBYCOLUMNS screens did not open"
say "   (the widget sets its grid spacing in its constructor; see"
say "    test_regressions.rb section 39. INST graphs is a MATRIXBYCOLUMNS of"
say "    four LINEGRAPHs.)"
graphs = open_screen('graphs')
chk('INST graphs opens') do
  (graphs.window && $message_boxes.empty?) || raise("did not open: #{$message_boxes.inspect}")
end
chk('with its 4 LINEGRAPHs') { graphs.widgets.count { |w| w.is_a?(Cosmos::LinegraphWidget) } == 4 }
close_screen(graphs)
spacing_grid = open_screen('spacing_grid')   # SPACERs in a MATRIXBYCOLUMNS
chk('INST spacing_grid opens') do
  (spacing_grid.window && $message_boxes.empty?) || raise("did not open: #{$message_boxes.inspect}")
end
close_screen(spacing_grid)

say "\n11. CmdSender could not send hazardous commands"
say "   (the prompt runs through execute_in_main_thread(true, 0.05), which"
say "    deferred it on the GUI thread, so prompt_for_hazardous returned nil"
say "    and _cmd gave up; see test_regressions.rb section 40)"
# Stands in for the CmdTlmServer: 'cmd' of a hazardous command raises
# HazardousError, as the server does; cmd_no_hazardous_check goes through.
class HazardousServer
  attr_reader :calls
  def initialize; @calls = []; end
  def method_missing(name, *args)   # _cmd calls this directly
    @calls << name.to_s
    if name.to_s == 'cmd'
      error = HazardousError.new
      error.target_name = 'INST'
      error.cmd_name = 'CLEAR'
      error.hazardous_description = 'Clearing counters may lose valuable information.'
      raise error
    end
    ['INST', 'CLEAR', {}]
  end
end
saved_server = $cmd_tlm_server
server = HazardousServer.new
$cmd_tlm_server = server
BOX_WATCH.stop
answered = false
Qt.single_shot(100) do
  box = Qt::Application.topLevelWidgets.find do |w|
    w.is_a?(Qt::MessageBox) && w.isVisible && w.windowTitle == 'Hazardous Command'
  end
  yes = box && box.findChildren.find { |c| c.is_a?(Qt::PushButton) && c.text.delete('&') == 'Yes' }
  (answered = true; yes.click) if yes
end
# CmdSender's own override (cmd_sender.rb:29-32): report and don't retry.
# ScriptRunnerFrame's, loaded above, needs a running script.
aborted = false
define_singleton_method(:prompt_for_script_abort) { aborted = true; true }
cmd_error = begin
  cmd('INST', 'CLEAR', {})   # CmdSender#send_button's call, on the GUI thread
  nil
rescue Exception => e
  e
ensure
  wait_for(1) { false }       # let a deferred prompt (the bug) show and be answered
  $cmd_tlm_server = saved_server
  singleton_class.send(:remove_method, :prompt_for_script_abort)
  BOX_WATCH.start(50)
end
chk('cmd completes') { cmd_error.nil? || raise(cmd_error) }
chk('the hazardous prompt was answered') { answered }
chk('Yes sends the command without the hazardous check') do
  (server.calls == %w[cmd cmd_no_hazardous_check] && !aborted) ||
    raise("server saw #{server.calls.inspect}, aborted=#{aborted}")
end

say "\n12. No context menu opened"
say "   (menu.exec resolved to the private Kernel#exec; see test_regressions.rb"
say "    section 41. Two of the 14 sites: ScriptRunner's tab menu and"
say "    CmdSender's parameter menu.)"
# Runs the block, which should open a context menu, and returns the texts of
# the menu's actions as seen while it was open (nil if none opened).
def menu_shown_by
  seen = nil
  Qt.single_shot(100) do
    menu = Qt::Application.topLevelWidgets.find { |w| w.is_a?(Qt::Menu) && w.isVisible }
    if menu
      seen = menu.actions.map(&:text)
      menu.close
    end
  end
  error = begin
    yield
    nil
  rescue Exception => e
    e
  end
  wait_for(1) { !seen.nil? }
  raise error if error
  seen
end
chk('menu.exec(point) shows the menu, and returns nil when dismissed') do
  menu = Qt::Menu.new
  menu.addAction(Qt::Action.new('First', menu))
  shown = nil
  Qt.single_shot(50) { shown = menu.isVisible; menu.close }
  result = menu.exec(Qt::Point.new(10, 10))
  menu.dispose
  (shown == true && result.nil?) || raise("shown=#{shown.inspect}, returned #{result.inspect}")
end
chk('ScriptRunner tab context menu opens') do
  r = tab_book.tabBar.tabRect(0)
  texts = menu_shown_by { script_runner.context_menu(Qt::Point.new(r.x + r.width / 2, r.y + r.height / 2)) }
  (texts && texts.include?('&New') && texts.include?('&Close')) || raise("menu #{texts.inspect}")
end
chk('CmdSender parameter context menu opens') do
  table = cmd_params.update_cmd_params(Cosmos::System.commands.packet('INST', 'ASCIICMD'))
  table.resize(600, 200)
  table.show
  APP.processEvents
  # The viewport point over the STRING row's name cell, found with itemAt.
  y = (0..150).step(3).find { |yy| (it = table.itemAt(Qt::Point.new(5, yy))) && it.text == 'STRING:' }
  raise 'no STRING row under the pointer' unless y
  texts = menu_shown_by { cmd_params.context_menu(Qt::Point.new(5, y)) }
  table.hide
  (texts && texts.include?('Details INST ASCIICMD STRING')) || raise("menu #{texts.inspect}")
end

say "\n13. The Ruby editors and CmdSender's history lost their key handling"
say "   (keyPressEvent asks the completer's popup first, and Completer#popup"
say "    was unbound; see test_regressions.rb section 47. The key event is a"
say "    stand-in: KeyEvent.new(type, key, modifiers) is unbound too, F14d.)"
require 'cosmos/tools/cmd_sender/cmd_sender'
StandInKey = Struct.new(:key, :modifiers) do
  def ignore; end
  def accept; end
  def text; ''; end
end
editor = Cosmos::RubyEditor.new(nil)
editor.setPlainText("puts 1\nputs 2")
cursor = editor.textCursor
cursor.movePosition(Qt::TextCursor::Start)
editor.setTextCursor(cursor)
key_error = begin
  editor.keyPressEvent(StandInKey.new(Qt::Key_Tab, Qt::NoModifier))
  nil
rescue Exception => e
  e
end
chk('Tab in a Ruby editor indents the line') do
  (key_error.nil? && editor.toPlainText.lines.first.start_with?('  puts 1')) ||
    raise(key_error || "text #{editor.toPlainText.inspect}")
end
history_bar = Qt::StatusBar.new
history = Cosmos::CmdSenderTextEdit.new(history_bar)
Cosmos::CmdSender.send_count = 0   # CmdSender#initialize sets it
$eval_binding = binding
$qt6_history_ran = false
history.setPlainText('$qt6_history_ran = true')
cursor = history.textCursor
cursor.movePosition(Qt::TextCursor::End)
history.setTextCursor(cursor)
key_error = begin
  history.keyPressEvent(StandInKey.new(Qt::Key_Return, Qt::NoModifier))
  nil
rescue Exception => e
  e
end
chk('Enter in CmdSender history executes the line') do
  (key_error.nil? && $qt6_history_ran && history_bar.currentMessage.include?('sent.')) ||
    raise(key_error || "ran=#{$qt6_history_ran}, status #{history_bar.currentMessage.inspect}")
end

say "\n14. Cancel in a text prompt was not detected"
say "   (Qt::Boolean#nil? returned @value.nil?; see test_regressions.rb"
say "    section 48)"
# Answers the static text prompt titled +title+ each time it opens: Cancel the
# first time, then types +text+ and presses OK, so a prompt that wrongly
# reopens after Cancel ends instead of looping. Returns [timer, openings].
def answer_text_prompt(title, text)
  opened = 0
  timer = Qt::Timer.new
  timer.connect(SIGNAL('timeout()')) do
    dialog = Qt::Application.topLevelWidgets.find do |w|
      w.is_a?(Qt::Dialog) && w.isVisible && w.windowTitle == title
    end
    if dialog
      opened += 1
      if opened == 1
        dialog.reject
      else
        edit = dialog.findChildren.find { |c| c.is_a?(Qt::LineEdit) }
        edit.setText(text) if edit
        dialog.accept
      end
    end
  end
  timer.start(50)
  [timer, -> { opened }]
end
# ScriptRunner pauses the script on Cancel (prompt_for_script_abort) so the
# operator can stop it; stopping raises StopScript.
define_singleton_method(:prompt_for_script_abort) { raise Cosmos::StopScript }
timer, opened = answer_text_prompt('Ask', 'typed after reopening')
ask_result = begin
  ask_string('Name?')   # a script's `ask`, on the GUI thread
rescue Cosmos::StopScript
  :stopped
end
timer.stop
singleton_class.send(:remove_method, :prompt_for_script_abort)
chk('Cancel in ask lets the operator stop the script') do
  ask_result == :stopped ||
    raise("ask returned #{ask_result.inspect}; the prompt opened #{opened.call} time(s)")
end
require 'cosmos/tools/config_editor/config_editor'
_, ce_options = Cosmos::ConfigEditor.create_default_options
ce_options.title = 'Config Editor'
ce_options.auto_size = false
ce_options.remember_geometry = false
ce_options.redirect_io = false   # see sr_options
config_editor = Cosmos::ConfigEditor.new(ce_options)
timer, opened = answer_text_prompt('Target Name', 'NEVER_TYPED')   # only cancelled
create_error = begin
  config_editor.send(:create_target)
  nil
rescue Exception => e
  e
end
timer.stop
chk('Cancel in ConfigEditor Create Target just returns') do
  (create_error.nil? && opened.call == 1) || raise(create_error || "prompt opened #{opened.call} time(s)")
end

say "\n15. Script vertical_message_box always raised"
say "   (it found its buttons at layout index 2, where Qt 4 kept them; see"
say "    test_regressions.rb section 49)"
BOX_WATCH.stop
vmb_done = false
stacked = nil
Qt.single_shot(100) do
  box = Qt::Application.topLevelWidgets.find { |w| w.is_a?(Qt::MessageBox) && w.isVisible }
  if box
    first, second = %w[First Second].map do |t|
      box.findChildren.find { |c| c.is_a?(Qt::PushButton) && c.text == t }
    end
    stacked = !!(first && second && first.pos.x == second.pos.x && first.pos.y != second.pos.y)
    second ? second.click : box.reject
  end
end
Qt.single_shot(3000) do
  next if vmb_done
  Qt::Application.topLevelWidgets.each { |w| w.reject if w.is_a?(Qt::MessageBox) && w.isVisible }
end
vmb_error = begin
  picked = vertical_message_box('Pick one', 'First', 'Second')   # script API, GUI thread
  nil
rescue Exception => e
  e
end
vmb_done = true
BOX_WATCH.start(50)
chk('vertical_message_box shows its buttons stacked') do
  (vmb_error.nil? && stacked) || raise(vmb_error || "stacked=#{stacked.inspect}")
end
chk('and returns the button pressed') { picked == 'Second' || raise("returned #{picked.inspect}") }

say "\n16. Script combo_box always raised"
say "   (it inserts its chooser into the message box's grid layout, which"
say "    came back as a plain Qt::Layout; see test_regressions.rb section 50)"
BOX_WATCH.stop
combo_done = false
Qt.single_shot(100) do
  box = Qt::Application.topLevelWidgets.find { |w| w.is_a?(Qt::MessageBox) && w.isVisible }
  if box
    combo = box.findChildren.find { |c| c.is_a?(Qt::ComboBox) }
    combo.setCurrentIndex(1) if combo   # the operator picks the second entry
    ok = box.findChildren.find { |c| c.is_a?(Qt::PushButton) && c.text.delete('&') == 'OK' }
    ok ? ok.click : box.reject
  end
end
Qt.single_shot(3000) do
  next if combo_done
  Qt::Application.topLevelWidgets.each { |w| w.reject if w.is_a?(Qt::MessageBox) && w.isVisible }
end
combo_error = begin
  chosen = combo_box('Pick a target', 'INST', 'INST2')   # script API, GUI thread
  nil
rescue Exception => e
  e
end
combo_done = true
BOX_WATCH.start(50)
chk('combo_box returns the entry the operator picked') do
  (combo_error.nil? && chosen == 'INST2') || raise(combo_error || "returned #{chosen.inspect}")
end

say "\n17. The Details dialog failed for items with states or limits"
say "   (it adds its States and Limits boxes with FormLayout#addRow(widget);"
say "    see test_regressions.rb section 51)"
require 'cosmos/gui/dialogs/cmd_details_dialog'
require 'cosmos/gui/dialogs/tlm_details_dialog'
def label_texts(widget)
  widget.findChildren.select { |c| c.is_a?(Qt::Label) }.map(&:text)
end
cmd_details = nil
details_error = begin
  cmd_details = Cosmos::CmdDetailsDialog.new(nil, 'INST', 'COLLECT', 'TYPE')   # STATE NORMAL / SPECIAL
  nil
rescue Exception => e
  e
end
chk('Details of a command parameter with states opens') do
  (details_error.nil? && cmd_details.isVisible) || raise(details_error || 'not shown')
end
chk('and lists its states') { label_texts(cmd_details).any? { |t| t.include?('SPECIAL') } }
cmd_details.close if cmd_details
tlm_details = nil
details_error = begin
  # A packet is passed, as PacketViewer does, so no server is needed.
  tlm_details = Cosmos::TlmDetailsDialog.new(nil, 'INST', 'HEALTH_STATUS', 'TEMP1',
                                             Cosmos::System.telemetry.packet('INST', 'HEALTH_STATUS'))
  nil
rescue Exception => e
  e
end
chk('Details of a telemetry item with limits opens') do
  (details_error.nil? && tlm_details.isVisible) || raise(details_error || 'not shown')
end
chk('and lists its limits') { label_texts(tlm_details).any? { |t| t.include?('-80.0') } }
tlm_details.close if tlm_details

say "\n18. CmdSender's Send Raw dialog and MATRIXBYCOLUMNS screens with nested"
say "    layouts failed (GridLayout#addLayout; see test_regressions.rb section 53)"
$message_boxes.clear
_, cs_options = Cosmos::CmdSender.create_default_options
cs_options.title = 'Command Sender'
cs_options.auto_size = false
cs_options.remember_geometry = false
cs_options.redirect_io = false   # see sr_options
cs_options.production = false
cmd_sender = Cosmos::CmdSender.new(cs_options)
# Stands in for the server's interface list.
cmd_sender.define_singleton_method(:get_interface_names) { ['INST_INT'] }
raw_shown = false
Qt.single_shot(100) do
  dialog = Qt::Application.topLevelWidgets.find do |w|
    w.is_a?(Qt::Dialog) && w.isVisible && w.windowTitle == 'Send Raw Data From File'
  end
  (raw_shown = true; dialog.reject) if dialog
end
cmd_sender.file_send_raw
chk('File > Send Raw opens its dialog') do
  (raw_shown && !cmd_sender.statusBar.currentMessage.start_with?('Error')) ||
    raise("status #{cmd_sender.statusBar.currentMessage.inspect}")
end
# HORIZONTAL is a layout (HorizontalWidget < Qt::HBoxLayout), so nesting it
# in MATRIXBYCOLUMNS goes through MatrixbycolumnsWidget#addLayout's super.
matrix_of_layouts = Cosmos::Screen.new('INST MATRIX_OF_LAYOUTS', <<~SCREEN)
  SCREEN AUTO AUTO 1.0
  MATRIXBYCOLUMNS 2
    HORIZONTAL
      LABEL "left"
    END
    HORIZONTAL
      LABEL "right"
    END
  END
SCREEN
3.times { APP.processEvents; sleep 0.02 }
chk('a MATRIXBYCOLUMNS of HORIZONTAL layouts opens') do
  (matrix_of_layouts.window && $message_boxes.empty?) || raise("did not open: #{$message_boxes.inspect}")
end
close_screen(matrix_of_layouts)

say "\n19. PacketViewer: Cancel in Options > Polling Rate stored nil"
say "   (QInputDialog::getDouble returns the value it was given when the user"
say "    cancels; the binding returned nil, packet_viewer.rb:269 stored it, and"
say "    the telemetry thread's `< @polling_rate` (:544-547) raised into an"
say "    ExceptionDialog that ends the tool)"
cancel_prompt = Qt::Timer.new
cancelled = false
cancel_prompt.connect(SIGNAL('timeout()')) do
  dialog = Qt::Application.topLevelWidgets.find { |w| w.is_a?(Qt::Dialog) && w.isVisible && w.windowTitle == 'Options' }
  (cancelled = true; dialog.reject) if dialog
end
cancel_prompt.start(50)
# packet_viewer.rb:269, with the tool's default polling rate
rate = Qt::InputDialog.getDouble(nil, 'Options', 'Polling Rate (sec):', 1.0, 0, 1000, 1, nil)
cancel_prompt.stop
chk('Cancel keeps the polling rate it was given') do
  (cancelled && rate == 1.0) || raise("returned #{rate.inspect}, cancelled=#{cancelled}")
end

say "\n20. Replay's Realtime speed became No Delay"
say "   (ComboBox item data was kept as toString; see test_regressions.rb"
say "    section 54)"
require 'cosmos/tools/cmd_tlm_server/replay_backend'
backend = Cosmos::ReplayBackend.new(nil)
speed = Qt::ComboBox.new
speed_variants = []   # replay_tab.rb:166-188
[['No Delay', 0.0], ['1ms Delay', 0.001], ['Realtime', nil]].each do |label, delay|
  speed_variants << [Qt::Variant.new(delay), delay]
  speed.addItem(label, speed_variants[-1][0])
end
speed.connect(SIGNAL('currentIndexChanged(int)')) do   # replay_tab.rb:190-192
  backend.set_playback_delay(speed.itemData(speed.currentIndex).value)
end
speed.setCurrentIndex(2)
chk('Realtime paces by packet times (no fixed delay)') do
  (delay = backend.instance_variable_get(:@playback_delay)).nil? || raise("playback delay #{delay.inspect}")
end
speed.setCurrentIndex(1)
chk('1ms Delay is 0.001 s') { backend.instance_variable_get(:@playback_delay) == 0.001 }

say "\n21. ScriptRunner's Execute Selected Lines got U+2029 for newlines"
say "   (see test_regressions.rb section 55)"
$qt6_selected = []
running.set_text("$qt6_selected << 1\n$qt6_selected << 2\n$qt6_selected << 3\n")
script_editor = running.instance_variable_get(:@script)
cursor = script_editor.textCursor
cursor.movePosition(Qt::TextCursor::Start)
cursor.movePosition(Qt::TextCursor::Down, Qt::TextCursor::KeepAnchor)
cursor.movePosition(Qt::TextCursor::EndOfLine, Qt::TextCursor::KeepAnchor)
script_editor.setTextCursor(cursor)
chk('selected_lines keeps the newline between lines') do
  (text = script_editor.selected_lines) == "$qt6_selected << 1\n$qt6_selected << 2" || raise("got #{text.inspect}")
end
running.run_selection
wait_for(20) { !Cosmos::ScriptRunnerFrame.running? }
chk('Execute Selected Lines runs exactly the two selected lines') do
  log = running.instance_variable_get(:@output).toPlainText.lines.last(4).join.strip
  $qt6_selected == [1, 2] || raise("ran #{$qt6_selected.inspect}; log: #{log.inspect}")
end

say "\n22. Find/Replace ignored direction, Match Case and Whole Words"
say "   (see test_regressions.rb section 56)"
require 'cosmos/gui/dialogs/find_replace_dialog'
finder = Cosmos::FindReplaceDialog.instance   # include Singleton
find_target = Qt::PlainTextEdit.new
find_host = Object.new   # a tool's #search_text, as ScriptRunner provides
find_host.define_singleton_method(:search_text) { find_target }
finder.instance_variable_set(:@parent, find_host)
def finder_set(finder, text: 'tlm', match_case: false, whole_word: false, up: false)
  finder.instance_variable_get(:@find_box).setText(text)
  finder.instance_variable_get(:@match_case).setChecked(match_case)
  finder.instance_variable_get(:@match_whole_word).setChecked(whole_word)
  finder.instance_variable_get(:@wrap_around).setChecked(false)
  finder.instance_variable_get(:@up).setChecked(up)
end
def target_text(editor, text, at_end: false)
  editor.setPlainText(text)
  cursor = editor.textCursor
  cursor.movePosition(at_end ? Qt::TextCursor::End : Qt::TextCursor::Start)
  editor.setTextCursor(cursor)
end
finder_set(finder, match_case: true, whole_word: true)
target_text(find_target, 'TLM tlmx tlm')   # Qt's words are letters and digits
finder.find_next(find_host)
chk('Find Next with Match Case + Whole Word finds only the lowercase word') do
  (at = find_target.textCursor.selectionStart) == 9 || raise("selected at #{at}")
end
finder_set(finder)
target_text(find_target, 'tlm tlm', at_end: true)
finder.find_previous(find_host)
chk('Find Previous searches backward') do
  (find_target.textCursor.hasSelection && find_target.textCursor.selectionStart == 4) ||
    raise("selection at #{find_target.textCursor.selectionStart}")
end
finder_set(finder, match_case: true)
finder.instance_variable_get(:@replace_box).setText('X')
target_text(find_target, 'TLM tlm Tlm tlm')
finder.send(:handle_replace_all)   # the Replace All button's slot
chk('Replace All with Match Case leaves the other cases alone') do
  (t = find_target.toPlainText) == 'TLM X Tlm X' || raise("text #{t.inspect}")
end

say "\n23. Standard shortcuts became raw key codes"
say "   (see test_regressions.rb section 57; ScriptRunner builds its zoom"
say "    shortcuts from Qt::KeySequence::ZoomIn/ZoomOut at script_runner.rb:175/180)"
chk("ScriptRunner's Zoom In is Ctrl++") do
  (got = script_runner.instance_variable_get(:@edit_zoom_in).shortcut.toString) == 'Ctrl++' || raise("got #{got.inspect}")
end
chk("ScriptRunner's Zoom Out is Ctrl+-") do
  (got = script_runner.instance_variable_get(:@edit_zoom_out).shortcut.toString) == 'Ctrl+-' || raise("got #{got.inspect}")
end

say "\n24. The close button skipped Ruby reject overrides"
say "   (ScriptRunnerDialog refuses to close while its script runs"
say "    (script_runner_frame.rb:65-69); Qt's close handling called Qt's own"
say "    reject, so the dialog closed and execute_text_and_close_on_complete"
say "    went on to dispose it mid-script. See test_regressions.rb section 58.)"
runner_dialog = Cosmos::ScriptRunnerDialog.new(win, 'Executing Selected Lines While Paused')
$qt6_dialog_script = nil
# execute_text_and_close_on_complete's steps, disposing only once the script
# has ended so that the bug cannot dispose a running frame here.
runner_dialog.script_runner_frame.set_text("sleep 1\n$qt6_dialog_script = :done\n")
runner_dialog.script_runner_frame.run_and_close_on_complete
refused = nil
Qt.single_shot(400) do   # the close button, mid-script
  runner_dialog.close
  refused = runner_dialog.isVisible
end
exec_with(runner_dialog, 15000) {}
wait_for(15) { !Cosmos::ScriptRunnerFrame.running? }
runner_dialog.dispose
chk('closing a ScriptRunnerDialog mid-script is refused') { refused == true || raise("refused=#{refused.inspect}") }
chk('and its script runs to the end') { $qt6_dialog_script == :done || raise("script got to #{$qt6_dialog_script.inspect}") }

say "\n25. CmdSender's parameter delegate reverted to Qt's default after a GC"
say "   (only the table held it; see test_regressions.rb section 59)"
state_table = cmd_params.update_cmd_params(Cosmos::System.commands.packet('INST', 'ASCIICMD'))
state_table.resize(600, 200)
state_table.show
APP.processEvents
4.times { GC.start }
state_table.editItem(state_table.item(param_row(state_table, 'STRING'), 1))   # the state column
APP.processEvents
editor = state_table.viewport.findChildren.find { |c| c.is_a?(Qt::ComboBox) || c.is_a?(Qt::LineEdit) }
chk('editing a state after a GC still opens the state combo box') do
  editor.is_a?(Qt::ComboBox) || raise("editor #{editor.class}")
end
state_table.hide

say "\n26. PacketViewer's formatting options stay mutually exclusive after a GC"
say "   (Qt::ActionGroup.new ignored its parent -- test_regressions.rb section"
say "    62 reproduces that. PacketViewer's own group survives regardless: the"
say "    connect blocks in initialize_actions capture its local, and connected"
say "    blocks are kept for good. So this guards the tool; it does not"
say "    reproduce the defect.)"
require 'cosmos/tools/packet_viewer/packet_viewer'
_, pv_options = Cosmos::PacketViewer.create_default_options
pv_options.title = 'Packet Viewer'
pv_options.auto_size = false
pv_options.remember_geometry = false
pv_options.redirect_io = false   # see sr_options
packet_viewer = Cosmos::PacketViewer.new(pv_options)
# Overwrites stale VALUEs left on the machine stack, which Ruby's
# conservative scan would otherwise treat as live references.
def scrub_stack(n = 60); n.zero? ? 0 : scrub_stack(n - 1) + 1; end
scrub_stack
4.times { GC.start }
formatted = packet_viewer.instance_variable_get(:@formatted_tlm_units_action)
raw = packet_viewer.instance_variable_get(:@raw_tlm_action)
formatted.setChecked(true)
raw.setChecked(true)
chk('choosing Raw unchecks Formatted with Units') do
  (!formatted.isChecked && raw.isChecked) || raise("formatted=#{formatted.isChecked} raw=#{raw.isChecked}")
end

say "\n27. Calls the corrected scan_unbound.rb found unbound (F14e)"
say "   (a CLASSIFICATION banner in system.txt stopped every tool from starting"
say "    (classification_banner.rb:53); --maximized and --minimized died in"
say "    complete_initialize (qt_tool.rb:238/240, F14c); and typing cmd(\" in a"
say "    script editor raised in the completer (completion.rb:399-400).)"
system_config = Cosmos::System.instance
previous_banner = system_config.classificiation_banner
banner_tool = nil
banner_error = begin
  system_config.instance_variable_set(:@classificiation_banner,
                                      'display_text' => 'QT6 TEST BANNER', 'color' => Cosmos.getColor('green'))
  _, banner_options = Cosmos::QtTool.create_default_options
  banner_options.redirect_io = false
  banner_options.remember_geometry = false
  banner_tool = Cosmos::QtTool.new(banner_options)   # qt_tool.rb:67 adds the banner
  nil
rescue Exception => e
  e
ensure
  system_config.instance_variable_set(:@classificiation_banner, previous_banner)
end
chk('a tool starts with a classification banner and shows it') do
  (banner_error.nil? &&
   banner_tool.findChildren.any? { |c| c.is_a?(Qt::ToolBar) && label_texts(c).include?('QT6 TEST BANNER') }) ||
    raise(banner_error || 'no toolbar with the banner text')
end
%i[MAXIMIZED MINIMIZED].each do |state|
  word = state.to_s.downcase
  state_tool = nil
  state_error = begin
    _, state_options = Cosmos::QtTool.create_default_options
    state_options.redirect_io = false
    state_options.remember_geometry = false
    state_options.startup_state = state   # what --maximized / --minimized set
    state_tool = Cosmos::QtTool.new(state_options)
    state_tool.complete_initialize
    nil
  rescue Exception => e
    e
  end
  chk("a tool started --#{word} is #{word}") do
    (state_error.nil? && (state == :MAXIMIZED ? state_tool.isMaximized : state_tool.isMinimized)) ||
      raise(state_error || "maximized=#{state_tool.isMaximized} minimized=#{state_tool.isMinimized}")
  end
  state_tool.hide if state_tool
end
completion_edit = Cosmos::CompletionTextEdit.new(nil)
completion_edit.show
completion_edit.setPlainText('cmd("')
cursor = completion_edit.textCursor
cursor.movePosition(Qt::TextCursor::End)
completion_edit.setTextCursor(cursor)
completion = completion_edit.instance_variable_get(:@code_completion)
completion_error = begin
  raise 'the editor has no completer (Completion.new raised)' unless completion
  # What CompletionTextEdit#keyPressEvent calls once the editor has taken the
  # keystroke (completion_text_edit.rb:112).
  completion.handle_keypress(StandInKey.new(Qt::Key_F, Qt::NoModifier))
  nil
rescue Exception => e
  e
end
chk('typing cmd(" lists the targets with the first one selected') do
  (completion_error.nil? && completion.popup.isVisible && completion.popup.currentIndex.row == 0) ||
    raise(completion_error || "visible=#{completion.popup.isVisible} row=#{completion.popup.currentIndex.row}")
end
completion.popup.hide if completion
completion_edit.hide
banner_tool.hide if banner_tool

say "\n28. A closed TlmViewer screen stayed in memory for good (F21)"
say "   (every block a screen's widgets connected was anchored for the life of"
say "    the process, and the blocks capture the widgets, so each screen opened"
say "    and closed stayed reachable: 5 cycles of INST params left 5 Screens.)"
require 'weakref'
def f21_open_and_close(name, times)
  Array.new(times) do
    screen = open_screen(name)
    close_screen(screen)
    WeakRef.new(screen)
  end
end
f21_screens = f21_open_and_close('params', 5)
10.times { APP.processEvents; sleep 0.05 }
scrub_stack
4.times { GC.start }
chk('INST params opened and closed 5 times leaves at most 1 alive') do
  alive = f21_screens.count(&:weakref_alive?)
  alive <= 1 || raise("#{alive} of 5 closed screens still alive")
end

say "\n29. Every event on every TlmViewer widget took the GVL (F22)"
say "   (a busy Ruby thread made each widget's every paint wait out that"
say "    thread's time slice; test_regressions.rb section 68 times it. INST"
say "    params has plain labels and value fields, and 5 LEDs that override"
say "    paintEvent.)"
f22_params = open_screen('params')
f22_window = f22_params.window
f22_window.show
5.times { APP.processEvents }
f22_passthrough = 0
f22_overrides = 0
f22_trace = TracePoint.new(:c_call, :call) do |t|
  next unless t.method_id == :paintEvent
  if t.event == :call
    f22_overrides += 1                       # a Ruby-defined paintEvent
  else
    owner = (t.self.method(:paintEvent).owner rescue nil)
    f22_passthrough += 1 if owner && owner.name.to_s.end_with?('::Impl')
  end
end
f22_trace.enable
f22_window.repaint
APP.processEvents
f22_trace.disable
chk('repainting INST params calls Ruby only for the widgets that override paintEvent') do
  (f22_passthrough.zero? && f22_overrides >= 5) ||
    raise("#{f22_passthrough} pass-through calls, #{f22_overrides} override calls")
end
close_screen(f22_params)

say "\n30. A screen BUTTON's code touched the GUI from its own thread (P1)"
say "   (BackgroundbuttonWidget runs the code on a new thread. qtbindings"
say "    refused Qt calls off the main thread, and backgroundbutton_widget.rb:43"
say "    turns that refusal into 'You must wrap calls to the GUI in"
say "    Qt.execute_in_main_thread'. The call ran on the wrong thread instead.)"
require 'cosmos/tools/tlm_viewer/widgets/backgroundbutton_widget'
$p1_label = Qt::Label.new('untouched')
p1_outer = Qt::Widget.new                 # the error box's parent: parent.parentWidget
p1_inner = Qt::Widget.new(p1_outer)
p1_code = "$p1_label.setText('set by the button thread')"
p1_button = Cosmos::BackgroundbuttonWidget.new(Qt::VBoxLayout.new(p1_inner), 'Go', p1_code)
p1_button.screen = Object.new
$message_boxes.clear
$message_texts.clear
p1_button.execute(p1_code)
wait_for(5) { p1_button.isEnabled }
chk('BUTTON code touching the GUI gets the execute_in_main_thread advice') do
  ($p1_label.text == 'untouched' && $message_texts.any? { |t| t.include?('Qt.execute_in_main_thread') }) ||
    raise("label #{$p1_label.text.inspect}; boxes #{$message_boxes.inspect}")
end

say "\n31. TlmViewer --screen lost output written before its QApplication (P2)"
say "   (screen mode starts QtTool.redirect_io's thread, then loads"
say "    System.telemetry, then makes the QApplication (tlm_viewer.rb:589-591)."
say "    Output during the load made that thread post its 'Unexpected STDERR"
say "    output' dialog before the application existed: never shown, and the"
say "    thread stuck for good. Child process: the real TlmViewer.run, with a"
say "    load that warns and takes 1.5 s.)"
P2_SCREEN = <<~'RUBY'
  STDOUT.sync = true
  $LOAD_PATH.unshift ENV['P2_LIB']
  require 'cosmos'
  require 'cosmos/gui/qt'
  require 'cosmos/tools/tlm_viewer/tlm_viewer'
  Cosmos::System.instance.instance_variable_set(:@sound, false)
  Cosmos::System.paths['LOGS'] = Dir.mktmpdir
  Thread.new { sleep 20; STDOUT.puts 'HUNG'; exit!(3) }
  Cosmos::System.singleton_class.prepend(Module.new do
    def telemetry
      if $stderr.is_a?(StringIO) && !$p2_warned     # after redirect_io took stderr
        $p2_warned = true
        $stderr.puts 'QT6 TEST: warning while telemetry loads'
        sleep 1.5                                   # the redirect thread polls each second
      end
      super
    end
  end)
  Qt::Application.singleton_class.prepend(Module.new do
    def new(*args)
      super.tap do
        watch = Qt::Timer.new
        watch.connect(SIGNAL('timeout()')) do
          Qt::Application.topLevelWidgets.each do |w|
            next unless w.is_a?(Qt::Dialog) && w.isVisible && w.windowTitle =~ /Unexpected STDERR output/
            text = w.findChildren.select { |c| c.is_a?(Qt::TextEdit) }.map(&:toPlainText).join   # ScrollTextDialog
            STDOUT.puts "DIALOG: #{w.windowTitle}: #{text.strip}"
            exit!(0)
          end
        end
        watch.start(100)
        Qt.single_shot(5000) { STDOUT.puts 'NO DIALOG'; exit!(1) }
      end
    end
  end)
  ARGV.replace(['--screen', 'INST HS'])
  Cosmos::TlmViewer.run
  STDOUT.puts 'RUN RETURNED'
  exit!(2)
RUBY
chk("output written before TlmViewer's QApplication still gets its dialog") do
  env = { 'P2_LIB' => File.expand_path('../../../../lib', __dir__), 'QT_QPA_PLATFORM' => 'offscreen' }
  out = IO.popen(env, [RbConfig.ruby, '-rtmpdir', '-rstringio', '-e', P2_SCREEN, err: [:child, :out]], &:read)
  out.include?('DIALOG: Unexpected STDERR output: QT6 TEST: warning while telemetry loads') ||
    raise(out.lines.grep(/DIALOG|NO DIALOG|HUNG|RUN RETURNED|Error/).first(3).join.strip)
end

say "\n32. LineGraph's grid lines were solid, not dashed (F17)"
say "   (they are drawn with Cosmos::DASHLINE_PEN = Qt::Pen.new(Qt::DashLine),"
say "    qt.rb:237, which came out a solid black pen; see test_regressions.rb"
say "    section 71)"
# Records where LineGraph draws with DASHLINE_PEN (line_graph_drawing.rb:97).
$f17_grid = nil
Qt::Painter.prepend(Module.new do
  def addLineColor(x, y, w, h, color = Cosmos::BLACK)
    $f17_grid << [x, y, w, h] if $f17_grid && color.equal?(Cosmos::DASHLINE_PEN)
    super
  end
end)
f17_graph = Cosmos::LineGraph.new
f17_graph.show_y_grid_lines = true   # without these only the top and bottom
f17_graph.show_x_grid_lines = true   # lines are drawn, over the solid border
f17_graph.add_line('TEMP1', [1.0, 5.0, 3.0, 8.0], [0, 1, 2, 3])
f17_graph.resize(400, 200)
f17_graph.show
5.times { APP.processEvents }
$f17_grid = []
f17_image = f17_graph.grab.toImage
f17_rows = $f17_grid.select { |_x, y, _w, h| y == h }
f17_rows = f17_rows.sort_by { |r| r[1] }[1..-2] || []   # not the border rows
$f17_grid = nil
f17_graph.hide
chk('a LineGraph grid line is dashed') do
  raise 'no interior grid line drawn with DASHLINE_PEN' if f17_rows.empty?
  x1, y, x2, = f17_rows.first
  xs = ((x1 + 1)...x2).to_a
  dark = xs.count { |x| f17_image.pixelColor(x, y).red < 128 }
  (dark < xs.size * 0.9 && dark > xs.size * 0.3) || raise("#{dark} of #{xs.size} pixels dark along y=#{y}")
end

say "\n33. Picking a completion stopped short of the next list (F14d)"
say "   (Completion#insertCompletion inserts the pick, then makes an Enter key"
say "    event with Qt::KeyEvent.new(type, key, modifiers) to go on with the"
say "    line (completion.rb:52). That raised, so picking a target never"
say "    brought up its commands.)"
f14d_edit = Cosmos::CompletionTextEdit.new(nil)
f14d_edit.show
f14d_edit.setPlainText('cmd("')
f14d_cursor = f14d_edit.textCursor
f14d_cursor.movePosition(Qt::TextCursor::End)
f14d_edit.setTextCursor(f14d_cursor)
f14d = f14d_edit.instance_variable_get(:@code_completion)
f14d_error = begin
  f14d.handle_keypress(StandInKey.new(Qt::Key_F, Qt::NoModifier))   # the target list
  f14d.insertCompletion('"INST ')                                     # picking INST calls this
  nil
rescue Exception => e
  e
end
chk("picking a target inserts it and lists that target's commands") do
  first = f14d.model && f14d.model.index(0, 0).data.toString
  (f14d_error.nil? && f14d_edit.toPlainText == 'cmd("INST ' && first.to_s =~ /(\"\)|with )\z/ && f14d.popup.isVisible) ||
    raise(f14d_error || "text #{f14d_edit.toPlainText.inspect}, first entry #{first.inspect}")
end
f14d.popup.hide
f14d_edit.hide

say "\n34. The Legal dialog reported modified COSMOS core files (F23)"
say "   (data/crc.txt still held v4.5.2's CRCs for the files the Qt6 port"
say "    changed, so the Launcher's Legal Agreement dialog (launcher.rb:176)"
say "    opened with 'Warning: N Core CRC checks failed!' --"
say "    legal_dialog.rb:106-136)"
require 'cosmos/gui/dialogs/legal_dialog'
f23 = Cosmos::LegalDialog.allocate   # initialize ends in a modal exec
f23.instance_variable_set(:@text_crc, Qt::TextEdit.new)
f23.check_all_crcs
f23_text = f23.instance_variable_get(:@text_crc).toPlainText
chk('the Legal dialog verifies every core CRC') do
  !f23_text.include?('Core CRC checks failed') || raise(f23_text.lines.first(2).join.strip)
end

say "\n35. CmdSender's parameter table reported an error for cells it painted"
say "    and for every state picked"
say "   (CmdParamTableItemDelegate draws the state and description columns"
say "    itself and calls super for the rest, which raised NoMethodError, as"
say "    did the description column's Qt::Style.CE_ItemViewItem. Its"
say "    setModelData writes the picked state with model.setData, which was"
say "    unbound. See test_regressions.rb sections 77 and 78.)"
require 'stringio'
# What the binding reports goes to $stderr, which ScriptRunnerFrame has
# redirected; capture it directly.
def reported
  saved = $stderr
  $stderr = StringIO.new
  yield
  APP.processEvents
  $stderr.string
ensure
  $stderr = saved
end
collect = cmd_params.update_cmd_params(Cosmos::System.commands.packet('INST', 'COLLECT'))
collect.resize(900, 300)
collect.show
APP.processEvents
collect_painted = reported { collect.grab }
chk("painting INST COLLECT's parameters reports no error") do
  collect_painted.empty? || raise(collect_painted.lines.first.to_s.strip)
end
collect_row = param_row(collect, 'TYPE')
collect.editItem(collect.item(collect_row, 1))
APP.processEvents
collect_combo = collect.viewport.findChildren.find { |c| c.is_a?(Qt::ComboBox) }
collect_delegate = collect.findChildren.find { |c| c.is_a?(Cosmos::CmdParamTableItemDelegate) }
chk('picking a state commits it without an error') do
  collect_combo || raise('no state editor opened')
  collect_combo.setCurrentIndex(collect_combo.findText('SPECIAL'))
  committed = reported { collect_delegate.commitData(collect_combo) }
  committed.empty? || raise(committed.lines.first.to_s.strip)
  (state = collect.item(collect_row, 1).text) == 'SPECIAL' || raise("state cell #{state.inspect}")
end
collect.hide

say "\n36. ExceptionListDialog listed none of its exceptions"
say "   (it adds each with Qt::ListWidgetItem.new(string, @list)"
say "    (exception_list_dialog.rb:39), which ignored the list -- see"
say "    test_regressions.rb section 80)"
require 'cosmos/gui/dialogs/exception_list_dialog'
exceptions_listed = nil
on_dialog('COSMOS Exception List') do |d|
  list = d.findChildren.find { |c| c.is_a?(Qt::ListWidget) }
  exceptions_listed = list && (0...list.count).map { |i| list.item(i).text }
  d.accept
end
Cosmos::ExceptionListDialog.new('Errors', [RuntimeError.new('first'), ArgumentError.new('second')])
chk('the dialog lists both exceptions') do
  exceptions_listed == ['1. RuntimeError : first', '2. ArgumentError : second'] ||
    raise("listed #{exceptions_listed.inspect}")
end

say "\n37. PacketViewer's View menu was retitled 'Formatting'"
say "   (view_menu.addSeparator.setText('Formatting'), packet_viewer.rb:191:"
say "    addSeparator returned the menu -- test_regressions.rb section 81)"
chk("PacketViewer's menu bar still has its View menu") do
  titles = packet_viewer.menuBar.actions.map(&:text)
  (titles.include?('&View') && !titles.include?('Formatting')) || raise("menus #{titles.inspect}")
end

say "\n38. TestRunner's Test Selections dialog did not open"
say "   (it reads each node's font while it builds the tree"
say "    (test_runner.rb:764), and COSMOS's tree helpers read checkState,"
say "    parent, childCount and child (qt.rb:336-349, 388-391), all unbound"
say "    on items. Its Ok|Cancel button box had no buttons (test_runner.rb:"
say "    856), and item lookups never compared equal, so a click unchecked"
say "    its own suite (753). See test_regressions.rb sections 82 and 83.)"
require 'cosmos/tools/test_runner/test_runner'
_, tr_options = Cosmos::TestRunner.create_default_options
tr_options.title = 'Test Runner'
tr_options.auto_size = false
tr_options.remember_geometry = false
tr_options.redirect_io = false   # see sr_options
tr_options.server_config_file = Cosmos::CmdTlmServer::DEFAULT_CONFIG_FILE
tr_options.config_file = true    # the demo's, which loads example_test.rb
test_runner = Cosmos::TestRunner.new(tr_options)
test_runner.instance_variable_get(:@timer).stop
# Splash.execute loads the config on its own thread (splash.rb:106).
wait_for(30) { Cosmos::TestRunner.class_variable_get(:@@test_suites).any? }
wait_for(10) do
  Qt::Application.topLevelWidgets.none? { |w| w.is_a?(Cosmos::Splash::SplashDialogBox) && w.isVisible }
end
selections = {}
on_dialog('Test Selections') do |d|
  tree = d.findChildren.find { |c| c.is_a?(Qt::TreeWidget) }
  suites = []
  tree.topLevelItems { |node| suites << node }
  selections[:suites] = suites.map(&:text)
  selections[:buttons] = d.findChildren.select { |c| c.is_a?(Qt::PushButton) }.map { |b| b.text.delete('&') }
  suite = suites.find { |node| node.childCount > 0 }
  if suite
    suite.setCheckStateAll(Qt::Checked)
    tests = []
    suite.children { |node| tests << node }
    selections[:checked] = tests.all? { |node| node.checkState == Qt::Checked }
    selections[:top_is_suite] = tests.all? { |node| node.topLevel == suite }
  end
  d.reject
end
begin
  test_runner.show_select
rescue => e
  selections[:error] = "#{e.class}: #{e.message}"
end
chk('the Test Selections dialog opens and lists the demo suites') do
  selections[:error] && raise(selections[:error])
  selections[:suites].to_a.include?('ExampleTestSuite') || raise("suites #{selections[:suites].inspect}")
end
chk('its button box has OK and Cancel') do
  (selections[:buttons].to_a & %w[OK Cancel]).size == 2 || raise("buttons #{selections[:buttons].inspect}")
end
chk("checking a suite checks its tests, whose top level is that suite") do
  (selections[:checked] && selections[:top_is_suite]) ||
    raise("checked #{selections[:checked].inspect}, top level #{selections[:top_is_suite].inspect}")
end
test_runner.hide

say "\n39. LimitsMonitor's Ignored Telemetry Items dialog removed nothing"
say "   (Remove Selected reads each selected item's data (limits_monitor.rb:"
say "    786), then removes the selection with qt.rb's remove_selected_items,"
say "    which calls ListWidget#row (qt.rb:614). Both were unbound -- see"
say "    test_regressions.rb section 84.)"
require 'cosmos/tools/limits_monitor/limits_monitor'
class Qt6SuiteLimitsMonitor < Cosmos::LimitsMonitor
  # The server threads initialize starts (limits_monitor.rb:560-561).
  def limits_thread; end
  def value_thread; end
end
_, lm_options = Cosmos::LimitsMonitor.create_default_options
lm_options.title = 'Limits Monitor'
lm_options.auto_size = false
lm_options.remember_geometry = false
lm_options.redirect_io = false   # see sr_options
limits_monitor = Qt6SuiteLimitsMonitor.new(lm_options)
lm_items = limits_monitor.instance_variable_get(:@limits_items)
lm_items.ignored << %w[INST HEALTH_STATUS TEMP1]
lm_items.ignored << %w[INST HEALTH_STATUS TEMP2]
lm_removal = nil
on_dialog('Ignored Telemetry Items') do |d|
  list = d.findChildren.find { |c| c.is_a?(Qt::ListWidget) }
  list.item(0).setSelected(true)   # ITEM: INST HEALTH_STATUS TEMP1
  err = reported { button(d, 'Remove Selected').click }
  lm_removal = [err, list.count]
  d.done(0)
end
limits_monitor.edit_ignored_items
chk('Remove Selected takes the item off the ignore list and the dialog') do
  err, left = lm_removal
  err.to_s.empty? || raise(err.lines.first.to_s.strip)
  (lm_items.ignored == [%w[INST HEALTH_STATUS TEMP2]] && left == 1) ||
    raise("ignored #{lm_items.ignored.inspect}, #{left.inspect} left in the list")
end
limits_monitor.hide

say
if $failures.empty?
  say 'ALL COSMOS TOOL CHECKS PASSED'
else
  say "#{$failures.size} FAILURES: #{$failures.inspect}"
end
# Leave without Ruby's thread teardown: a tool's worker thread still waiting
# on the GUI thread (ConfigEditor's splash) has been seen to block it forever.
$cleanups.each(&:call)
STDOUT.flush
exit!($failures.empty? ? 0 : 1)

