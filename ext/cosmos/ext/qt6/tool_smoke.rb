# Starts one COSMOS tool the way its launcher does (Tool.run), headless,
# against the demo project, and reports every dialog the tool raised on its
# own while it ran: message boxes (ExceptionDialog, "Error During Startup")
# and the "Unexpected STDERR/STDOUT output" dialogs QtTool.redirect_io opens
# for stray output -- which is where the binding's slot error reports land.
# Each is printed as a DIALOG line and dismissed. After SECONDS it prints a
# WINDOW line per visible main window and exits 0 if no dialog showed, else 1.
#
# test_cosmos_tools.rb runs this in a child process: a startup error there
# ends in ExceptionDialog's exit_afterwards (Qt exit + Kernel#exit), which
# must not take the suite down with it.
#
# Run: ruby -Ilib ext/cosmos/ext/qt6/tool_smoke.rb REQUIRE_PATH CLASS SECONDS [TOOL ARGS...]
#   e.g. ruby -Ilib ext/cosmos/ext/qt6/tool_smoke.rb \
#          cosmos/tools/tlm_grapher/tlm_grapher Cosmos::TlmGrapher 5
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
ENV['COSMOS_USERPATH'] ||= File.expand_path('../../../../demo', __dir__)
ENV['QT_QPA_PLATFORM'] ||= 'offscreen'
require 'tmpdir'
require 'fileutils'
# See test_cosmos_tools.rb: Cosmos.data_path resolves a Gemfile through
# Bundler, and the repo Gemfile's DART half needs rails.
unless ENV['BUNDLE_GEMFILE']
  gemfile_dir = Dir.mktmpdir('qt6_tool_smoke')
  File.write(File.join(gemfile_dir, 'Gemfile'), "source 'https://rubygems.org'\n")
  ENV['BUNDLE_GEMFILE'] = File.join(gemfile_dir, 'Gemfile')
end
require_path, class_name, seconds, *tool_args = ARGV
abort "usage: #{$0} REQUIRE_PATH CLASS SECONDS [TOOL ARGS...]" unless seconds
ARGV.replace(tool_args)
STDOUT.sync = true

require 'cosmos'
require 'cosmos/gui/qt'
require require_path
Cosmos::System.targets
Cosmos::System.instance.instance_variable_set(:@sound, false)
$logs_dir = Dir.mktmpdir('qt6_tool_smoke_logs')
$shown_logs = []
Cosmos::System.paths['LOGS'] = $logs_dir

$dialogs = []
def report_and_exit
  windows = Qt::Application.topLevelWidgets.select { |w| w.is_a?(Qt::MainWindow) && w.isVisible }
  windows.each { |w| STDOUT.puts "WINDOW: #{w.windowTitle}" }
  STDOUT.puts "DIALOGS: #{$dialogs.size}"
  exit!($dialogs.empty? && !windows.empty? ? 0 : 1)
end

# The QApplication only exists once Tool.run has created it, so the watcher
# is installed from the hook QtTool.run calls right after that.
tool = class_name.split('::').inject(Object) { |mod, name| mod.const_get(name) }
hook = tool.method(:pre_window_new_hook)
tool.define_singleton_method(:pre_window_new_hook) do |options|
  watch = Qt::Timer.new
  watch.connect(SIGNAL('timeout()')) do
    Qt::Application.topLevelWidgets.each do |w|
      next unless w.is_a?(Qt::Dialog) && w.isVisible
      title = w.windowTitle
      if w.is_a?(Qt::MessageBox)
        # Qt::MessageBox#text is not bound; the text is in its child labels.
        text = w.findChildren.select { |c| c.is_a?(Qt::Label) }.map(&:text).join("\n")
        text = text.gsub(/<br\s*\/?>/i, "\n").gsub(/<[^>]+>/, '')
      elsif title =~ /Unexpected (STDERR|STDOUT) output/
        text = w.findChildren.select { |c| c.is_a?(Qt::TextEdit) }.map(&:toPlainText).join   # ScrollTextDialog
      else
        next   # the tool's own dialogs (splash, progress) are not errors
      end
      $dialogs << title
      STDOUT.puts "DIALOG: #{title}"
      text.each_line.map(&:strip).reject(&:empty?).first(3).each { |l| STDOUT.puts "  #{l[0, 200]}" }
      # ExceptionDialog logged the exception first; show its backtrace.
      log = Dir.glob(File.join($logs_dir, '*_exception.txt')).max_by { |f| File.mtime(f) }
      if log && !$shown_logs.include?(log)
        $shown_logs << log
        File.readlines(log).first(10).each { |l| STDOUT.puts "  | #{l.rstrip}" }
      end
      w.reject
    end
  end
  watch.start(100)
  Qt.single_shot((seconds.to_f * 1000).round) { report_and_exit }
  hook.call(options)
end

tool.run
report_and_exit   # run returned early (the tool quit on its own)
