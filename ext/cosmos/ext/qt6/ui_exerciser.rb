# Systematically drives a COSMOS tool's UI and reports every error.
#
# Loaded via RUBYOPT so the real tool entry point is untouched:
#   COSMOS_QT6_ERRLOG=/tmp/err.txt UI_OUT=/tmp/out.txt \
#     RUBYOPT=-r.../ui_exerciser.rb bundle exec ruby tools/PacketViewer
#
# Walks the whole widget tree and exercises every control: clicks buttons,
# steps combo boxes through every index, switches every tab, triggers every
# menu action, types into line edits, toggles checkboxes. Slot errors are
# contained by the binding and reported, so this surfaces the click-time
# NoMethodErrors that otherwise only appear when a human happens to use that
# control.
#
# Destructive controls are skipped by name so a sweep does not quit the tool
# or fire real commands.
# Destructive controls, and ones that block on something outside the process.
# "Open ... in Text Editor" shells out to an external editor and never
# returns, which is what stalled the TlmGrapher sweep. Start/Stop/Pause were
# briefly on this list on a wrong hunch -- measured, they return in <1s.
# NOTE: /x strips literal spaces, so multi-word patterns need \s.
SKIP = /quit|exit|close|shutdown|delete|remove|clear\sall|reset|send|
        start\slogging|stop\slogging|text\seditor|open\sin|launch/xi

Thread.new do
  out = File.open(ENV['UI_OUT'] || '/tmp/ui_exercise.txt', 'w'); out.sync = true
  600.times { break if defined?(::Qt) && defined?(::Cosmos); sleep 0.2 }
  sleep (ENV['UI_WAIT'] || 25).to_i

  log = ->(m) { out.puts m }
  acted = Hash.new(0)
  errors = []

  # Record what is being touched BEFORE touching it, so a control that blocks
  # (a modal, a long operation) is named in the report instead of the run
  # just ending as DNF.
  safe = lambda do |what, &blk|
    out.puts "    > #{what}"
    begin
      blk.call
      acted[what] += 1
    rescue Exception => e
      errors << "#{what}: #{e.class}: #{e.message}"
    end
  end

  # A clicked button can open a MODAL dialog, whose exec() blocks the main
  # thread -- and with it the whole sweep. Keep a timer running that closes
  # any dialog that appears, so the sweep can continue.
  dismissed = 0
  ::Qt.execute_in_main_thread(true) do
    killer = ::Qt::Timer.new
    killer.connect(SIGNAL('timeout()')) do
      ::Qt::Application.topLevelWidgets.each do |w|
        next unless w.is_a?(::Qt::Dialog)
        next unless (w.isVisible rescue false)
        dismissed += 1
        (w.reject rescue (w.close rescue nil))
      end
    end
    killer.start(300)
  end

  # Each phase gets its own execute_in_main_thread so one blocking control
  # cannot swallow the entire sweep, and progress is written as it happens.
  phase = lambda do |name, &blk|
    begin
      ::Qt.execute_in_main_thread(true) { blk.call }
      log.call "  phase #{name}: done"
    rescue Exception => e
      log.call "  phase #{name}: ABORTED #{e.class}: #{e.message}"
    end
  end

  begin
    ::Qt.execute_in_main_thread(true) do
      tops = ::Qt::Application.topLevelWidgets.reject { |w| (w.width rescue 0) <= 1 }
      log.call "top-level widgets: #{tops.map { |w| w.class.name }.uniq.join(', ')}"

      $ui_widgets = tops.flat_map { |w| [w] + (w.findChildren rescue []) }.uniq
      log.call "widgets discovered: #{$ui_widgets.size}"
    end

    widgets = $ui_widgets

    phase.call('tabs') do
      # --- tabs: visit every page so lazily-built content is constructed ---
      widgets.select { |w| w.class.name =~ /TabWidget/ }.each do |tw|
        (0...(tw.count rescue 0)).each do |i|
          safe.call("tab:#{tw.tabText(i) rescue i}") { tw.setCurrentIndex(i) }
          ::Qt::Application.processEvents
        end
      end
    end

    phase.call('rewalk') do
      tops2 = ::Qt::Application.topLevelWidgets.reject { |w| (w.width rescue 0) <= 1 }
      $ui_widgets = tops2.flat_map { |w| [w] + (w.findChildren rescue []) }.uniq
    end
    widgets = $ui_widgets

    phase.call('combos') do
      # --- combo boxes: step through every index ---
      widgets.select { |w| w.is_a?(::Qt::ComboBox) }.each do |cb|
        n = (cb.count rescue 0)
        next if n.zero?
        (0...n).each do |i|
          safe.call('combo:setCurrentIndex') { cb.setCurrentIndex(i) }
          ::Qt::Application.processEvents
        end
      end
    end

    # Clicks go in small chunks: batching them all into one
    # execute_in_main_thread means one slow handler stalls every remaining
    # button (TlmGrapher), but one call per button is ~10x slower and makes
    # other tools time out. Five at a time is the compromise.
    widgets.select { |w| w.is_a?(::Qt::PushButton) }.each_slice(5).with_index do |chunk, i|
      phase.call("buttons[#{i}]") do
        chunk.each do |b|
          t = (b.text rescue '').to_s
          next if t =~ SKIP
          safe.call("button:#{t}") { b.click }
          ::Qt::Application.processEvents
        end
      end
    end

    phase.call('lineEdits') do
      # --- line edits: type something ---
      widgets.select { |w| w.is_a?(::Qt::LineEdit) }.each do |le|
        label = (le.objectName rescue '')
        label = (le.text rescue '') if label.to_s.empty?
        safe.call("lineEdit:#{label.to_s[0, 20]}") { le.setText('TEMP1') }
        ::Qt::Application.processEvents
      end
    end

    phase.call('actions') do
      # --- menu actions: trigger the non-destructive ones ---
      widgets.flat_map { |w| (w.findChildren rescue []) }
             .select { |a| a.is_a?(::Qt::Action) }.uniq.each do |a|
        t = (a.text rescue '').to_s
        next if t.empty? || t =~ SKIP
        safe.call("action:#{t}") { a.trigger }
        ::Qt::Application.processEvents
      end
    end
  rescue Exception => e
    log.call "EXERCISER FAILED #{e.class}: #{e.message}"
  end

  log.call ''
  log.call "modal dialogs dismissed: #{dismissed}"
  log.call "actions performed: #{acted.values.sum}"
  acted.keys.group_by { |k| k.split(':').first }.each { |kind, ks| log.call "  #{kind}: #{ks.size}" }
  log.call ''
  if errors.empty?
    log.call 'NO ERRORS RAISED DIRECTLY'
  else
    log.call "#{errors.size} DIRECT ERRORS:"
    errors.uniq.each { |e| log.call "  #{e}" }
  end
  out.close
  exit!(0)
end
