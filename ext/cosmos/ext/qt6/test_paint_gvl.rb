# Reproduces the crash behind the recurring
# "[BUG] pthread_mutex_lock: Invalid argument (EINVAL)" / segfault reports.
#
# Qt calls paintEvent from its event loop, which app.exec() runs with the GVL
# RELEASED. The binding used to build the callback's Ruby arguments before
# re-acquiring the GVL, so every repaint of a Ruby-painted widget allocated
# Ruby objects without the GVL. That corrupts the Ruby heap; the [BUG] lands
# later, on whichever thread allocates next -- which in COSMOS is a background
# worker like splash.rb:106, making the crash look unrelated to painting.
#
# Cosmos::LineGraph is exactly this shape: a Qt::Widget subclass that paints
# from Ruby, repainting while background threads process telemetry.
$LOAD_PATH.unshift File.expand_path('../../../../lib', __dir__)
require 'cosmos/ext/qt6'
require 'Qt'

class Plot < Qt::Widget
  attr_reader :paints
  def initialize(*args)
    super
    @paints = 0
  end

  # Allocate Ruby objects during paint, the way a real painter builds label
  # strings and point arrays.
  def paintEvent(_event)
    @paints += 1
    pts = []
    150.times { |i| pts << "pt-#{i}-#{@paints}" }
    pts.join(',').length
  end
end

app  = Qt::Application.new([])
plot = Plot.new
plot.resize(320, 240)
plot.show

errors = []
churning = true

# Background Ruby thread allocating and collecting, like COSMOS telemetry work.
worker = Thread.new do
  begin
    i = 0
    while churning
      i += 1
      buf = Array.new(200) { |j| "tlm-#{i}-#{j}" }
      buf.map(&:upcase).join('|')
      GC.start if (i % 25).zero?
    end
  rescue Exception => e
    errors << "worker: #{e.class}: #{e.message}"
  end
end

repaint = Qt::Timer.new
repaint.connect(SIGNAL('timeout()')) { plot.update }
repaint.start(1)

app.exec_for(10_000)
churning = false
worker.join(5)

# Only shows that painting went through the contended path. Each repaint waits
# for the GVL twice (the timer's block, then paintEvent) while the worker holds
# it, so the count depends on the machine: GitHub's runners manage about 50 in
# 10 s and a recent laptop about 70. Requiring more than 50 failed on 49.
enough = plot.paints > 10
ok = errors.empty? && enough
puts "  paintEvent dispatched (#{plot.paints} repaints)#{' ' * [1, 12 - plot.paints.to_s.length].max}#{enough ? 'ok' : 'FAIL'}"
puts "  no errors                                      #{errors.empty? ? 'ok' : 'FAIL'}"
errors.each { |e| puts "    #{e}" }
puts ok ? 'PAINT GVL OK' : 'PAINT GVL FAILED'
exit(ok ? 0 : 1)
