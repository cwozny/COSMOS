# Reproduces the concurrency failure behind the recurring
# "[BUG] pthread_mutex_lock: Invalid argument (EINVAL)" crash.
#
# app.exec() releases the GVL, so Qt's event loop and a Ruby background thread
# run in parallel for real. Both mutate g_objmap (one via destroyed(), the
# other via wrap_obj/GC sweep), and g_in_gc_free was a plain global that one
# thread's GC could flip under the other thread's on_destroyed. This is the
# shape of COSMOS's splash screen: Splash.execute runs startup on a
# Thread (splash.rb:106) while the main thread sits in a modal exec().
#
# Against the unfixed binding this aborts or corrupts the heap. It must run to
# completion cleanly.
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
require 'cosmos/ext/qt6'
require 'Qt'

app  = Qt::Application.new([])
done = false
errors = []

# Background Ruby thread: allocate and destroy Qt wrappers + force GC sweeps.
worker = Thread.new do
  begin
    3000.times do |i|
      w = Qt::Widget.new
      Qt::Label.new("l#{i}", w)      # child: destroyed cascades
      Qt::Object.new(w)
      w.dispose                      # -> on_destroyed on THIS thread
      GC.start if (i % 40).zero?     # -> qtwrap_free, g_in_gc_free
    end
  rescue Exception => e
    errors << "worker: #{e.class}: #{e.message}"
  ensure
    done = true
  end
end

# Main thread: churn objects from inside the event loop (GVL released), so
# destroyed() fires there while the worker is mutating the same map.
churn = Qt::Timer.new
churn.connect(SIGNAL('timeout()')) do
  begin
    20.times do
      x = Qt::Widget.new
      Qt::Label.new('m', x)
      x.dispose
    end
    app.quit if done
  rescue Exception => e
    errors << "main: #{e.class}: #{e.message}"
    app.quit
  end
end
churn.start(1)

app.exec_for(30_000)
worker.join(5)

ok = errors.empty? && done
puts "  worker completed                               #{done ? 'ok' : 'FAIL'}"
puts "  no errors                                      #{errors.empty? ? 'ok' : 'FAIL'}"
errors.each { |e| puts "    #{e}" }
puts ok ? "GVL RACE OK" : "GVL RACE FAILED"
exit(ok ? 0 : 1)
