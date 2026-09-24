# Drop-in replacement for the qtbindings 'Qt' gem, backed by Qt 6.
#
# COSMOS does `require 'Qt'` in lib/cosmos/gui/qt.rb. Because lib/ is on the
# load path, that resolves to this file, so switching COSMOS from Qt 4.8 to
# Qt 6 needs no change to any existing COSMOS source file.
begin
  require 'cosmos/ext/qt6'
rescue LoadError => e
  raise LoadError, <<~MSG
    The COSMOS Qt6 extension is not built.

    COSMOS GUI tools need Qt 6. Install it and rebuild:
      macOS:  brew install qt
      Linux:  install the Qt6 base/widgets/opengl development packages
    then:     rake build

    (original error: #{e.message})
  MSG
end

module Qt
  # qtbindings exposes every Qt method under three spellings:
  #
  #   widget.setWindowTitle("x")   # Qt's own camelCase
  #   widget.set_window_title("x") # snake_case
  #   widget.window_title = "x"    # property assignment
  #
  # COSMOS uses all three. Rather than register three names per method in C,
  # resolve the alternate spellings here and forward to the bound camelCase
  # method. method_defined? is used (not respond_to?) so this cannot recurse.
  module MethodAliases
    # Finds a method defined by the binding itself (each Qt class's methods
    # live in a nested Impl module) rather than one a reopen put on the class.
    def self.binding_method(klass, name)
      sym = name.to_sym
      klass.ancestors.each do |mod|
        next if mod.is_a?(Class)
        next unless mod.name.to_s.end_with?('::Impl')
        return mod.instance_method(sym) if mod.instance_methods(false).include?(sym)
      end
      nil
    end

    def self.setter_for(name)
      'set' + name.split('_').map { |p| p.empty? ? p : p[0].upcase + p[1..-1].to_s }.join
    end

    def self.camel_for(name)
      parts = name.split('_')
      parts[0].to_s + parts[1..-1].to_a.map { |p| p.empty? ? p : p[0].upcase + p[1..-1].to_s }.join
    end

    # qtbindings exposed Qt's boolean getters as Ruby predicates, so COSMOS
    # writes `action.checked?` for QAction::isChecked. Returns the candidate
    # Qt spellings for a `foo?` call, most specific first.
    def self.predicate_candidates(name)
      return [] unless name.end_with?('?')
      base = camel_for(name[0..-2])
      return [] if base.empty?
      ['is' + base[0].upcase + base[1..-1].to_s,
       'has' + base[0].upcase + base[1..-1].to_s,
       base]
    end

    def method_missing(name, *args, &block)
      n = name.to_s
      # COSMOS calls method_missing directly to reach the BINDING's
      # implementation, deliberately bypassing its own override:
      #   @timer.method_missing(:start, 100)                (data_viewer.rb:96)
      #   class Qt::Dialog; def exec(*a); method_missing(:exec, *a); end; end
      #                                                     (script_module_gui.rb:19)
      # Dispatching back through __send__ would re-enter the override and
      # recurse, so resolve against the per-class Impl modules instead.
      if !n.end_with?('=')
        m = MethodAliases.binding_method(self.class, n)
        return m.bind(self).call(*args, &block) if m
      end
      if n.end_with?('=') && !n.end_with?('==')
        setter = MethodAliases.setter_for(n[0..-2])
        return __send__(setter, *args) if self.class.method_defined?(setter)
      end
      camel = MethodAliases.camel_for(n)
      return __send__(camel, *args, &block) if camel != n && self.class.method_defined?(camel)
      MethodAliases.predicate_candidates(n).each do |cand|
        return __send__(cand, *args, &block) if self.class.method_defined?(cand)
      end
      super
    end

    def respond_to_missing?(name, include_private = false)
      n = name.to_s
      if n.end_with?('=') && !n.end_with?('==')
        return true if self.class.method_defined?(MethodAliases.setter_for(n[0..-2]))
      end
      camel = MethodAliases.camel_for(n)
      return true if camel != n && self.class.method_defined?(camel)
      return true if MethodAliases.predicate_candidates(n).any? { |c| self.class.method_defined?(c) }
      super
    end
  end

  # Signals declared in Ruby (`signals 'modified()'`) have no Qt meta-object
  # entry, so they are emulated. qtbindings made `signals 'x()'` define a
  # method `x` whose return value is handed to `emit`, which is why COSMOS
  # writes `emit modified()`.
  class SignalEmission
    def initialize(owner, name, args); @owner, @name, @args = owner, name, args; end
    def fire
      @owner.__ruby_signal_handlers(@name).each { |h| h.call(*@args) }
      nil
    end
  end

  module RubySignals
    # `slots 'about()'` only needs to be accepted; the method itself is an
    # ordinary Ruby method that connect() resolves by name.
    def slots(*names)
      (@__qt_slots ||= []).concat(names.flatten)
    end
    def qt_slots; @__qt_slots || []; end

    def signals(*names)
      (@__qt_signals ||= []).concat(names.flatten)
      names.flatten.each do |sig|
        base = sig.to_s.sub(/\(.*\)\z/, '')
        next if base.empty?
        define_method(base) { |*args| Qt::SignalEmission.new(self, sig.to_s, args) }
      end
    end
    def qt_signals; @__qt_signals || []; end
  end

  class Base
    extend RubySignals

    # The C++ side caches, per object, which forwarded virtuals (paintEvent,
    # closeEvent, ...) Ruby overrides, so an event nothing overrides skips the
    # GVL (RubyOverrides, rubywidget.h). Anything that can add or remove an
    # override clears every cached answer; each runs after the change is made.
    class << self
      def method_added(name)
        super
        Qt.__overrides_changed
      end

      def method_removed(name)
        super
        Qt.__overrides_changed
      end

      def method_undefined(name)
        super
        Qt.__overrides_changed
      end

      def include(*mods)
        result = super
        Qt.__overrides_changed
        result
      end

      def prepend(*mods)
        result = super
        Qt.__overrides_changed
        result
      end

      # qtbindings also answered an enum value called like a class method:
      # Qt::Style.CE_ItemViewItem (cmd_param_table_item_delegate.rb:65).
      def method_missing(name, *args, &block)
        owner = args.empty? && !block && __qt_const_owner(name)
        return owner.const_get(name, false) if owner
        super
      end

      def respond_to_missing?(name, include_private = false)
        !!__qt_const_owner(name) || super
      end

      # The Qt class or module in the ancestry that defines constant +name+.
      # Not Object's constants: Qt::Style.String is not a Qt enum.
      def __qt_const_owner(name)
        return nil unless name.to_s =~ /\A[A-Z]\w*\z/
        ancestors.find { |a| a.name.to_s.start_with?('Qt::') && a.const_defined?(name, false) }
      end
    end

    def singleton_method_added(name)
      super
      Qt.__overrides_changed
    end

    def singleton_method_removed(name)
      super
      Qt.__overrides_changed
    end

    def singleton_method_undefined(name)
      super
      Qt.__overrides_changed
    end

    def extend(*mods)
      result = super
      Qt.__overrides_changed
      result
    end

    def __ruby_signal_handlers(name)
      @__ruby_signal_handlers ||= Hash.new { |h, k| h[k] = [] }
      @__ruby_signal_handlers[Qt.__sig(name)]
    end

    def emit(emission)
      emission.fire if emission.respond_to?(:fire)
    end
  end

  # qtbindings' helper for running GUI work from a background thread, with its
  # signature (qtbindings 4.8.6.5, lib/Qt4.rb:101):
  #   execute_in_main_thread(blocking = true, sleep_period = 0.001, delay_execution = false)
  # sleep_period was qtbindings' poll interval while blocking; the condition
  # variable below waits instead, so it is accepted and ignored. Reading it as
  # a delay deferred script_module_gui.rb:74's (true, 0.05) on the GUI thread,
  # so every prompt called there returned nil. Touching Qt widgets off the GUI
  # thread is undefined behaviour, so anything not already on the main thread
  # is posted to the main event loop.
  def self.execute_in_main_thread(blocking = true, _sleep_period = 0.001, delay_execution = false, &block)
    if on_main_thread?
      # delay_execution is COSMOS's "run this after the current call frame
      # unwinds" idiom, always paired with blocking=false. packet_viewer.rb:366
      # sets a shutdown flag and re-invokes itself through here; running the
      # block inline re-enters before the flag can take effect and recurses
      # until SystemStackError. Defer through the event loop instead.
      return Qt.single_shot(0) { block.call } if delay_execution
      return block.call
    end

    mutex  = Mutex.new
    cond   = ConditionVariable.new
    done   = false
    result = nil
    error  = nil
    post_to_main_thread do
      begin
        result = block.call
      rescue Exception => e
        error = e
        if ENV['COSMOS_QT6_DEBUG']
          File.open(ENV['COSMOS_QT6_DEBUG'], 'a') { |f| f.puts "EIMT ERROR #{e.class}: #{e.message}"; f.puts e.backtrace.reject { |b| b.include?('Qt.rb') }.first(4) }
        end
        # exit, Interrupt (Ctrl-C landing in this block) and SignalException
        # must end the app, not only the waiting thread -- or nobody, for a
        # non-blocking post. Re-raised here on the GUI thread, the binding
        # ends every event loop and re-raises it from exec. A non-blocking
        # post has no caller to hand any other error to, so re-raise that too
        # and the binding reports it.
        raise if e.is_a?(SystemExit) || e.is_a?(SignalException) || !blocking
      ensure
        mutex.synchronize { done = true; cond.signal }
      end
    end

    if blocking
      # Wait on a condition variable rather than spinning on sleep: the spin
      # was fragile under load and could crash inside sleep. Waiting releases
      # the GVL so the main thread can run the posted block.
      mutex.synchronize { cond.wait(mutex, 0.05) until done }
      raise error if error
    end
    result
  end

  # qtbindings set $qApp when a QApplication was constructed. COSMOS gates its
  # GUI exception dialogs on it (top_level.rb:333, 576, 599); left unset, every
  # error quietly falls back to stderr instead of showing a dialog.
  class Application
    def self.new(*args)
      super.tap { |app| $qApp = app }
    end
  end

  # qtbindings exposed the blocks waiting to run on the GUI thread as
  # Qt::RubyThreadFix.queue. COSMOS drains it before disposing a dialog so no
  # pending block can touch a destroyed widget (splash.rb:130,
  # progress_dialog.rb:308, cmd_tlm_server_gui.rb:355). Our posts go through
  # Qt's event loop, so the queue mirrors them and each entry runs exactly
  # once -- whether the event loop or a drain loop reaches it first.
  module RubyThreadFix
    class Pending
      attr_reader :seq

      def initialize(block, queue, seq)
        @block = block
        @queue = queue
        @seq   = seq
        @mutex = Mutex.new
        @ran   = false
      end

      def call
        run = @mutex.synchronize { first = !@ran; @ran = true; first }
        @queue.forget(self)
        @block.call if run
      end
    end

    # COSMOS drains with `queue.pop.call until queue.empty?` to flush work that
    # is ALREADY pending before it destroys a widget. Background threads keep
    # posting while that loop runs (cmd_tlm_server_gui.rb:355 drains while the
    # tab thread is still calling execute_in_main_thread), so an unbounded
    # drain never terminates: the main thread spins at 100% and starves the
    # event loop. Each drain is therefore bounded to a snapshot -- the items
    # queued at the moment it started.
    class PendingQueue
      def initialize
        @mutex = Mutex.new
        @items = []
        @seq   = 0
        @limit = nil
      end

      def add(block)
        @mutex.synchronize do
          @seq += 1
          pending = Pending.new(block, self, @seq)
          @items << pending
          pending
        end
      end

      def forget(pending)
        @mutex.synchronize { @items.delete(pending) }
      end

      # Always returns something callable: the drain loop may race the event
      # loop and find the item it was about to take already gone.
      def pop
        taken = @mutex.synchronize do
          @limit ||= @seq
          idx = @items.index { |p| p.seq <= @limit }
          idx ? @items.delete_at(idx) : nil
        end
        taken || lambda {}
      end

      def empty?
        @mutex.synchronize do
          @limit ||= @seq
          done = @items.none? { |p| p.seq <= @limit }
          @limit = nil if done   # the next drain takes a fresh snapshot
          done
        end
      end

      def length
        @mutex.synchronize { @items.length }
      end
    end

    def self.queue
      @queue ||= PendingQueue.new
    end
  end

  class << self
    alias_method :__post_to_main_thread, :post_to_main_thread

    # Register the block with RubyThreadFix.queue before posting, so a drain
    # loop can run anything the event loop has not reached yet.
    def post_to_main_thread(&block)
      pending = RubyThreadFix.queue.add(block)
      __post_to_main_thread { pending.call }
    end
  end

  # qtbindings shadowed Kernel#raise on widgets with QWidget::raise(). Nothing
  # shadows it here: guessing the caller's intent from $! meant that a no-arg
  # widget.raise inside a rescue re-raised the exception being handled instead
  # of raising the window. exception_dialog.rb:104 does exactly that, and
  # because its @@mutex is unlocked after the call and not in an ensure, the
  # first error inside a rescue permanently suppressed every later error
  # dialog in the process. The 25 call sites that mean the Qt sense now say
  # raise_, which is the plain binding.

  # QTextCursor#selection returns a QTextDocumentFragment. COSMOS only ever
  # calls toPlainText on it (qt.rb:530) and reads .format/.cursor off a
  # details object, so a thin wrapper is enough rather than binding a whole
  # fragment class. Its text is the fragment's toPlainText (newlines), not
  # selectedText (U+2029 between blocks).
  class SelectionFragment
    attr_accessor :format, :cursor
    def initialize(text); @text = text; end
    def toPlainText; @text; end
    def to_s; @text; end
  end

  class TextCursor
    def selection
      SelectionFragment.new(__selection_plain_text)
    end
  end

  # Qt::Base covers every QObject-derived class; the value types (Color, Font,
  # KeySequence, ...) descend from ::Object, so include there too.
  # connect() must fall back to the Ruby-side registry for signals that exist
  # only as `signals` declarations, and must forward signal arguments to
  # handlers that accept them.
  class Base
    def connect(*args, &block)
      if block
        sender = args.size > 1 ? args[0] : self
        name   = Qt.__sig(args.size > 1 ? args[1] : args[0])
        __qt_connect(sender, name, block)
      else
        sender, sig, receiver, slot = args
        meth = Qt.__sig(slot).sub(/\(.*\)\z/, '')
        handler = lambda do |*a|
          m = receiver.method(meth)
          n = m.arity < 0 ? a.size : m.arity
          receiver.__send__(meth, *a.first(n))
        end
        __qt_connect(sender, Qt.__sig(sig), handler)
      end
    end

    private

    def __qt_connect(sender, name, handler)
      Qt.connect_raw(sender, name) { |*a| handler.call(*a) }
    rescue ArgumentError => e
      raise unless e.message.include?('no such signal')
      # Only a signal the sender's class (or an ancestor) declared with
      # `signals` lives in the Ruby registry. Filing any other name there
      # connected a misspelt signal silently, never to fire; Qt warns and
      # returns false, which qtbindings passed through.
      key = Qt.__sig(name)
      declared = sender.class.ancestors.any? do |k|
        k.respond_to?(:qt_signals) && k.qt_signals.any? { |s| Qt.__sig(s) == key }
      end
      unless declared
        warn "QObject::connect: No such signal #{sender.class.name}::#{name}"
        return false
      end
      sender.__ruby_signal_handlers(name) << handler   # Ruby-declared signal
      true
    end
  end

  # qtbindings gave every Qt object -- including value types like Point, Size
  # and Font -- a `dispose` that freed the underlying C++ object, and COSMOS
  # calls it routinely (full_text_search_line_edit.rb:145 disposes a Point).
  # Ours are GC-managed values with nothing to free, so this is a no-op. It is
  # defined ONLY on classes that lack it, so Qt::Base's real dispose (which
  # does delete the QObject) is never shadowed.
  module ValueDispose
    def dispose; nil; end
    def disposed?; false; end
  end

  Base.send(:include, MethodAliases)
  constants.each do |c|
    k = const_get(c)
    next unless k.is_a?(Class)
    next if k <= Base
    k.send(:include, MethodAliases)
    k.send(:include, ValueDispose) unless k.method_defined?(:dispose)
  end
end
