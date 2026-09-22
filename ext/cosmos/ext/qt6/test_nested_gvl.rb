# The most common COSMOS interaction: click a button, a modal dialog opens,
# click OK. That path nests dialog.exec inside a slot that is already running
# under a re-acquired GVL.
#
# ruby_run_with_gvl tracks "this thread re-acquired the GVL" in a thread-local.
# dialog_exec then releases the GVL again on that same thread. If the flag is
# not cleared for the duration, every callback fired from inside the modal
# loop takes the "I already hold the GVL" branch and runs Ruby code with the
# GVL RELEASED, concurrently with COSMOS worker threads -> heap corruption,
# reported later as [BUG] on an unrelated thread.
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
require 'cosmos/ext/qt6'
require 'Qt'

app = Qt::Application.new([])
errors = []
inner  = 0
ROUNDS = 60

churning = true
worker = Thread.new do
  begin
    i = 0
    while churning
      i += 1
      Array.new(80) { |j| "tlm-#{i}-#{j}" }.map(&:upcase).join('|')
      GC.start if (i % 30).zero?
    end
  rescue Exception => e
    errors << "worker: #{e.class}: #{e.message}"
  end
end

driver = Qt::Timer.new
driver.connect(SIGNAL('timeout()')) do
  begin
    ROUNDS.times do
      dlg = Qt::Dialog.new
      Qt.single_shot(1) do
        inner += 1
        ('payload-' * 12) + inner.to_s   # allocate inside the nested loop
        dlg.accept
      end
      dlg.exec                            # nested loop, releases the GVL again
    end
  rescue Exception => e
    errors << "slot: #{e.class}: #{e.message}"
  end
  app.quit
end
driver.start(5)

app.exec_for(45_000)
churning = false
worker.join(5)

ok = errors.empty? && inner >= ROUNDS
puts "  nested dialog callbacks: #{inner}/#{ROUNDS}#{' ' * 14}#{inner >= ROUNDS ? 'ok' : 'FAIL'}"
puts "  no errors                                      #{errors.empty? ? 'ok' : 'FAIL'}"
errors.each { |e| puts "    #{e}" }
puts ok ? 'NESTED GVL OK' : 'NESTED GVL FAILED'
exit(ok ? 0 : 1)
