#!/usr/bin/env ruby
# Static sweep for Qt methods that will raise NoMethodError the moment a
# control is used. Two passes:
#
#   NAME  every camelCase call COSMOS makes, minus every name the binding
#         defines and every name COSMOS defines itself. Catches a method
#         bound on no class at all.
#
#   CLASS for call sites whose receiver class can be resolved, the method is
#         checked against THAT class and its ancestors. A name-only check
#         cannot see that setWordWrap was bound on Qt::Label but called on a
#         Qt::TableWidget (cmd_params.rb:238), or that Qt::Layout#removeItem
#         does not make Qt::ComboBox#removeItem exist. Both of those reached
#         a user while the name pass reported zero.
#
# Both passes exist because they fail differently: the name pass needs no type
# information and so covers every call site; the class pass is precise but
# only covers receivers it can resolve.
require 'set'   # Array#to_set; not autoloaded on Ruby 2.6

# QT6_SCAN_ROOT points the scan at another tree (test_regressions.rb's fixture).
ROOT = ENV['QT6_SCAN_ROOT'] || File.expand_path('../../../..', __dir__)

bound = []
cpp = File.read(File.join(ROOT, 'ext/cosmos/ext/qt6/cosmos_qt6.cpp'))
# The class argument is any expression, not just an identifier: some classes
# are registered through an array (the drag/drop event classes), and missing
# that reported already-bound methods as unbound.
bound += cpp.scan(/Q[SC]?DEF\(\s*[^,]+?\s*,\s*"([^"]+)"/).flatten   # QDEF, and QSDEF/QCDEF (class methods)
bound += cpp.scan(/rb_define_(?:singleton_)?method\(\s*[^,]+?\s*,\s*"([^"]+)"/).flatten
# rb_define_attr defines real readers/writers and was not counted at all.
cpp.scan(/rb_define_attr\(\s*[^,]+?\s*,\s*"([^"]+)"\s*,\s*(\d)\s*,\s*(\d)\s*\)/) do |name, r, w|
  bound << name
  bound << "#{name}=" if w == '1'
end
bound += cpp.scan(/\bdef\s+(?:self\.)?([a-zA-Z_]\w*[?!=]?)/).flatten
bound += File.read(File.join(ROOT, 'lib/Qt.rb')).scan(/\bdef\s+(?:self\.)?([a-zA-Z_]\w*[?!=]?)/).flatten

cosmos_defs = []
Dir.glob(File.join(ROOT, 'lib/**/*.rb')).each do |f|
  t = File.read(f, encoding: 'BINARY')
  cosmos_defs += t.scan(/^\s*def\s+(?:self\.)?([a-zA-Z_]\w*[?!=]?)/).flatten
  t.scan(/attr_(?:accessor|reader|writer)\s+(.+)/).flatten.each do |a|
    cosmos_defs += a.scan(/:([a-zA-Z_]\w*)/).flatten
  end
end
cosmos_defs = cosmos_defs.to_set
known = (bound.to_set + cosmos_defs)

# A `foo?` call resolves if is/hasFoo (or plain foo) is bound -- qtbindings
# exposed Qt's boolean getters as Ruby predicates and COSMOS writes them that
# way (e.g. action.checked? for QAction::isChecked). Missing this spelling is
# how `checked?` reached a user.
def predicate_ok?(name, known)
  base = name[0..-2].split('_').each_with_index
             .map { |p, i| i.zero? ? p : p.sub(/\A./, &:upcase) }.join
  return true if base.empty?
  cap = base.sub(/\A./, &:upcase)
  known.include?("is#{cap}") || known.include?("has#{cap}") || known.include?(base)
end

hits = Hash.new(0); where = {}
Dir.glob(File.join(ROOT, 'lib/cosmos/{gui,tools}/**/*.rb')).each do |f|
  File.readlines(f, encoding: 'BINARY').each_with_index do |line, i|
    next if line.lstrip.start_with?('#')
    # URLs inside string literals look like predicate calls ("ball.com?subject=")
    next if line =~ %r{mailto:|https?://}
    # camelCase calls, plus predicate calls
    (line.scan(/\.([a-z][a-zA-Z0-9]*[A-Z][a-zA-Z0-9]*)\s*[(\s=]/).flatten +
     line.scan(/\.([a-z][a-zA-Z0-9_]*\?)/).flatten).each do |m|
      hits[m] += 1
      where[m] ||= "#{f.sub(ROOT + '/', '')}:#{i + 1}"
    end
  end
end
# Ruby predicates on non-Qt receivers (Time#utc?, File#file?, Integer#even?,
# Hash#key?, Mutex#locked?, ...). The scanner cannot see receiver types, so
# these are excluded by name.
RUBY_PREDICATES = %w[
  utc? file? success? even? odd? key? locked? writable? readable? exist?
  empty? nil? zero? any? all? none? include? start_with? end_with? frozen?
  is_a? kind_of? respond_to? equal? eql? between? finite? nan? infinite?
  method_defined? const_defined? instance_variable_defined? private_method_defined?
  directory? blank? present? closed? eof? tty? alive? stop? casecmp?
].freeze

missing = hits.reject do |m, _|
  RUBY_PREDICATES.include?(m) ||
  known.include?(m) || (m.end_with?('?') && predicate_ok?(m, known))
end.sort_by { |m, c| [-c, m] }
missing.each { |m, c| puts format('%4d  %-28s %s', c, m, where[m]) }

# ---------------------------------------------------------------------------
# CLASS pass
# ---------------------------------------------------------------------------

# Qt class table: C++ variable -> ruby name and parent variable.
class_of_var = {}
var_of_name  = {}
cpp.scan(/(\w+)\s*=\s*rb_define_class_under\(\s*mQt\s*,\s*"(\w+)"\s*,\s*([A-Za-z_]\w*)\s*\)/) do |var, name, parent|
  class_of_var[var] = { name: name, parent: parent }
  var_of_name[name] = var
end
# Qt4 spellings kept as constants pointing at the same class.
cpp.scan(/rb_define_const\(\s*mQt\s*,\s*"(\w+)"\s*,\s*([A-Za-z_]\w*)\s*\)/) do |name, var|
  var_of_name[name] = var if class_of_var.key?(var)
end
# The same Ruby class is sometimes fetched back into a second C variable and
# the methods registered on that one instead -- Qt::ActionGroup is created as
# cActionGroup but bound on cActionGroupK. Treating those as two classes
# reported every one of its methods as missing.
name_of_var = {}
class_of_var.each { |var, info| name_of_var[var] = info[:name] }
cpp.scan(/(\w+)\s*=\s*rb_const_get\(\s*mQt\s*,\s*rb_intern\("(\w+)"\)\s*\)/) do |var, name|
  name_of_var[var] = name
end

# Methods per class variable. A registration whose class argument is not a
# plain identifier (an array element, say) cannot be attributed, so its names
# are exempted everywhere rather than guessed at.
methods_of_var = Hash.new { |h, k| h[k] = Set.new }
UNATTRIBUTABLE = Set.new
add = lambda do |kls, name|
  kls = kls.strip
  if kls =~ /\A[A-Za-z_]\w*\z/ then methods_of_var[kls] << name else UNATTRIBUTABLE << name end
end
cpp.scan(/Q[SC]?DEF\(\s*([^,]+?)\s*,\s*"([^"]+)"/)                            { |k, m| add.call(k, m) }
cpp.scan(/rb_define_(?:singleton_)?method\(\s*([^,]+?)\s*,\s*"([^"]+)"/)      { |k, m| add.call(k, m) }
cpp.scan(/rb_define_attr\(\s*([^,]+?)\s*,\s*"([^"]+)"\s*,\s*(\d)\s*,\s*(\d)\s*\)/) do |k, m, _r, w|
  add.call(k, m)
  add.call(k, "#{m}=") if w == '1'
end

# Methods COSMOS and lib/Qt.rb add by reopening a Qt class in Ruby.
ruby_methods_of_name = Hash.new { |h, k| h[k] = Set.new }
(Dir.glob(File.join(ROOT, 'lib/cosmos/**/*.rb')) + [File.join(ROOT, 'lib/Qt.rb')]).each do |f|
  current = nil
  indent = nil
  in_qt_module = false
  File.readlines(f, encoding: 'BINARY').each do |line|
    in_qt_module = true if line =~ /^\s*module\s+Qt\s*$/
    if (md = line.match(/^(\s*)class\s+Qt::(\w+)/)) then current, indent = md[2], md[1]
    elsif in_qt_module && (md = line.match(/^(\s*)class\s+([A-Z]\w*)/)) then current, indent = md[2], md[1]
    elsif line =~ /^\s*(class|module)\s/ then current = nil
    # The class ends at its `end`: a def after it (qt.rb's removeAll, in a
    # class_eval loop) is not the last-opened class's.
    elsif current && line =~ /^#{indent}end\b/ then current = nil
    end
    if current && (md = line.match(/^\s*def\s+(?:self\.)?([a-zA-Z_]\w*[?!=]?)/))
      ruby_methods_of_name[current] << md[1]
    end
  end
end

# qt.rb adds removeAll to six layout classes in one loop:
#   %w(GridLayout BoxLayout ...).each do |klass|
#     "Qt::#{klass}".to_class.class_eval do
#       def removeAll
# Those defs belong to each class listed.
Dir.glob(File.join(ROOT, 'lib/cosmos/**/*.rb')).each do |f|
  lines = File.readlines(f, encoding: 'BINARY')
  lines.each_with_index do |line, i|
    md = line.match(/^(\s*)%w\(([^)]*)\)\.each\s+do\s+\|(\w+)\|/)
    next unless md && lines[i + 1].to_s.include?("\"Qt::\#{#{md[3]}}\".to_class.class_eval do")
    last = (i + 1...lines.size).find { |j| lines[j] =~ /^#{md[1]}end\b/ } || lines.size - 1
    defs = lines[i..last].map { |l| l[/^\s*def\s+([a-zA-Z_]\w*[?!=]?)/, 1] }.compact
    md[2].split.each { |k| ruby_methods_of_name[k].merge(defs) }
  end
end

# Every method a class responds to, walking the C++ parent chain.
resolved = {}
methods_for = lambda do |name|
  return resolved[name] if resolved.key?(name)
  set = Set.new
  var = var_of_name[name]
  seen = Set.new
  while var && class_of_var.key?(var) && !seen.include?(var)
    seen << var
    cname = class_of_var[var][:name]
    # Every C variable that names this same Ruby class, not just this one.
    name_of_var.each { |v, n| set += methods_of_var[v] if n == cname }
    set += ruby_methods_of_name[cname]
    var = class_of_var[var][:parent]
  end
  set += methods_of_var[var] if var && !seen.include?(var)   # cQtBase / rb_cObject
  resolved[name] = set
end

# lib/Qt.rb's method_missing resolves other spellings onto a bound method.
def spelling_ok?(m, set)
  return true if set.include?(m)
  camel = m.gsub(/_([a-z])/) { Regexp.last_match(1).upcase }
  return true if set.include?(camel)
  if m.end_with?('=') && !m.end_with?('==')
    base = m[0..-2].gsub(/_([a-z])/) { Regexp.last_match(1).upcase }
    return true if set.include?("set#{base.sub(/\A./, &:upcase)}")
  end
  if m.end_with?('?')
    base = m[0..-2].gsub(/_([a-z])/) { Regexp.last_match(1).upcase }
    cap = base.sub(/\A./, &:upcase)
    return true if set.include?("is#{cap}") || set.include?("has#{cap}") || set.include?(base)
  end
  false
end

# Anything Object already answers is never a binding gap.
OBJECT_METHODS = Object.instance_methods.map(&:to_s).to_set
# lib/Qt.rb mixes modules into every Qt class (`k.send(:include,
# ValueDispose)` gives each value type dispose; MethodAliases adds
# method_missing), so the instance methods of those modules count as bound
# on every class.
GENERIC_DEFS = Set.new
qt_rb = File.readlines(File.join(ROOT, 'lib/Qt.rb'), encoding: 'BINARY')
mixed_in = qt_rb.join.scan(/\.send\(:include,\s*(\w+)\)/).flatten.to_set
qt_rb.each_with_index do |line, i|
  md = line.match(/^(\s*)module\s+(\w+)\s*$/)
  next unless md && mixed_in.include?(md[2])
  last = (i + 1...qt_rb.size).find { |j| qt_rb[j] =~ /^#{md[1]}end\b/ } || qt_rb.size - 1
  qt_rb[i..last].each { |l| (m = l[/^\s*def\s+(?!self\.)([a-zA-Z_]\w*[?!=]?)/, 1]) && GENERIC_DEFS << m }
end
# lib/cosmos/gui/opengl/opengl.rb includes the opengl gem's OpenGL and GLU
# modules at top level, which makes their gl*/glu* functions callable with no
# receiver anywhere. They are not Qt methods.
GEM_FUNCTION = /\Aglu?[A-Z]/
# String literals hold signal and slot signatures -- SIGNAL('itemClicked(...)')
# -- which look like calls.
def code_only(line)
  line.gsub(/'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"/, '""')
end

# Resolve receivers from `var = Qt::X.new`. Locals are scoped to the enclosing
# def -- a file-wide map made `box` in one method pick up the class of a `box`
# in another -- while ivars are file-wide by nature. A name assigned from two
# different Qt classes, or from anything else as well, is ambiguous and skipped.
def single_qt_class(kinds, name)
  k = kinds[name]
  return nil unless k && k.size == 1 && k.first.is_a?(String)
  k.first
end

class_hits = []
Dir.glob(File.join(ROOT, 'lib/cosmos/{gui,tools}/**/*.rb')).each do |f|
  lines = File.readlines(f, encoding: 'BINARY')

  ivars = Hash.new { |h, k| h[k] = Set.new }
  lines.each do |line|
    next if line.lstrip.start_with?('#')
    if (md = line.match(/^\s*(@[a-z_]\w*)\s*=\s*Qt::(\w+)\.new\b/))      then ivars[md[1]] << md[2]
    elsif (md = line.match(/^\s*(@[a-z_]\w*)\s*=\s*(?!Qt::\w+\.new)\S/)) then ivars[md[1]] << :other
    end
  end

  locals = Hash.new { |h, k| h[k] = Set.new }
  lines.each_with_index do |line, i|
    locals = Hash.new { |h, k| h[k] = Set.new } if line =~ /^\s*def\s/
    next if line.lstrip.start_with?('#')
    if (md = line.match(/^\s*([a-z_]\w*)\s*=\s*Qt::(\w+)\.new\b/))      then locals[md[1]] << md[2]
    elsif (md = line.match(/^\s*([a-z_]\w*)\s*=\s*(?!Qt::\w+\.new)\S/)) then locals[md[1]] << :other
    end

    # (?<![.\w@:]) keeps the receiver to the FIRST segment of a chain: in
    # `item.text.scan(...)` the receiver of scan is a String, not the widget.
    code_only(line).scan(/(?<![.\w@:])(@?[a-z_]\w*)\.([a-zA-Z_]\w*[?!]?)\s*(=[^=~]|[(\s]|$)/) do |recv, meth, tail|
      klass = recv.start_with?('@') ? single_qt_class(ivars, recv) : single_qt_class(locals, recv)
      next unless klass && var_of_name.key?(klass)
      # `x.foo = v` is a call to foo=, not to foo.
      meth = "#{meth}=" if tail.to_s.start_with?('=')
      next if OBJECT_METHODS.include?(meth) || UNATTRIBUTABLE.include?(meth) || GENERIC_DEFS.include?(meth)
      # No blanket skip for names COSMOS defines somewhere: COSMOS's own
      # Qt::Dialog#exec hid the unbound Qt::Menu#exec that way. COSMOS reopens
      # of this class are already in methods_for.
      next if spelling_ok?(meth, methods_for.call(klass))
      class_hits << ["Qt::#{klass}##{meth}", "#{f.sub(ROOT + '/', '')}:#{i + 1}"]
    end
  end
end
# ---------------------------------------------------------------------------
# IMPLICIT pass: calls with no receiver inside a COSMOS subclass of a Qt
# class -- MatrixbycolumnsWidget < Qt::GridLayout calls setHorizontalSpacing,
# AdaptiveGridLayout calls addItem -- are checked against the Qt class the
# chain ends in, plus what the COSMOS classes in the chain (and the modules
# they include) define. Only camelCase names, which is what Qt's API looks
# like and COSMOS's own helpers mostly do not.
# ---------------------------------------------------------------------------
cosmos_classes = {}   # simple name -> { super:, defs:, includes: }
module_defs = Hash.new { |h, k| h[k] = Set.new }
class_bodies = []     # [file, first line index, last line index, class name]
Dir.glob(File.join(ROOT, 'lib/**/*.rb')).each do |f|
  lines = File.readlines(f, encoding: 'BINARY')
  lines.each_with_index do |line, i|
    md = line.match(/^(\s*)(class|module)\s+([A-Z][\w:]*)(?:\s*<\s*([A-Z][\w:]*))?/)
    next unless md
    indent, kind, full, sup = md[1], md[2], md[3], md[4]
    # A reopened Qt class (`class Qt::MainWindow`, classification_banner.rb)
    # makes bare calls on that Qt class. It is keyed by its full name so it
    # cannot merge with a COSMOS class of the same simple name.
    reopen = kind == 'class' && sup.nil? && full.start_with?('Qt::')
    name = reopen ? full : full.split('::').last
    last = (i + 1...lines.size).find { |j| lines[j] =~ /^#{indent}end\b/ } || lines.size - 1
    body = lines[i..last]
    defs = body.map { |l| l[/^\s*def\s+(?:self\.)?([a-zA-Z_]\w*[?!=]?)/, 1] }.compact.to_set
    body.each { |l| l.scan(/attr_(?:accessor|reader|writer)\s+(.+)/).flatten.each { |a| defs += a.scan(/:([a-zA-Z_]\w*)/).flatten } }
    # `signals 'enterKeyPressed(int)'` defines enterKeyPressed (lib/Qt.rb
    # RubySignals#signals), which COSMOS calls bare: emit enterKeyPressed(row).
    body.each { |l| l.scan(/^\s*signals\s+(.+)/).flatten.each { |a| defs += a.scan(/['"]([a-zA-Z_]\w*)\s*\(/).flatten } }
    if kind == 'module'
      module_defs[name] += defs
    else
      entry = (cosmos_classes[name] ||= { super: nil, defs: Set.new, includes: Set.new })
      entry[:qt] = full.sub('Qt::', '') if reopen
      entry[:super] ||= sup
      entry[:defs] += defs
      body.each { |l| (m = l[/^\s*include\s+([A-Z][\w:]*)/, 1]) && entry[:includes] << m.split('::').last }
      class_bodies << [f, i, last, name] if f.include?('/lib/cosmos/')
    end
  end
end
# Methods a COSMOS class answers from its own chain, and the Qt class it ends in.
chain_of = lambda do |name|
  defs = Set.new
  seen = Set.new
  qt = nil
  while name && !seen.include?(name)
    seen << name
    entry = cosmos_classes[name]
    break unless entry
    defs += entry[:defs]
    entry[:includes].each { |m| defs += module_defs[m] }
    if entry[:qt]
      qt = entry[:qt]
      break
    end
    sup = entry[:super]
    if sup && sup.start_with?('Qt::')
      qt = sup.sub('Qt::', '')
      break
    end
    name = sup && sup.split('::').last
  end
  [qt, defs]
end
bare_calls = lambda do |line|
  return [] if line.lstrip.start_with?('#')
  # Drop a def's own name -- `def setFoo(x)` is not a call -- but not the
  # rest of the line: a one-line def's body is.
  code_only(line).sub(/^\s*def\s+(?:self\.)?[\w?!=]+/, '').scan(/(?<![.\w@:$])([a-z][a-zA-Z0-9]*[A-Z][a-zA-Z0-9]*)\(/).flatten.reject do |meth|
    OBJECT_METHODS.include?(meth) || UNATTRIBUTABLE.include?(meth) || GENERIC_DEFS.include?(meth) || meth =~ GEM_FUNCTION
  end
end
# A constructor block without parameters -- Qt::VBoxLayout.new do
# addWidget(ok) end -- is instance_eval'd on the new object (run_ctor_block in
# cosmos_qt6.cpp), so its bare calls are on that Qt class, not on the class
# the block is written in. The innermost block owns a line.
ctor_blocks = Hash.new { |h, k| h[k] = [] }   # file -> [[line range, Qt class]]
Dir.glob(File.join(ROOT, 'lib/cosmos/**/*.rb')).each do |f|
  lines = File.readlines(f, encoding: 'BINARY')
  lines.each_with_index do |line, i|
    md = code_only(line).match(/^(\s*).*\bQt::(\w+)\.new(?:\((?:[^()]|\([^()]*\))*\))?\s+do\s*$/)
    next unless md && var_of_name.key?(md[2])
    last = (i + 1...lines.size).find { |j| lines[j] =~ /^#{md[1]}end\b/ }
    ctor_blocks[f] << [(i + 1)..(last - 1), md[2]] if last
  end
end
ctor_blocks.each do |f, blocks|
  lines = File.readlines(f, encoding: 'BINARY')
  blocks.each do |range, qt|
    range.each do |n|
      next unless blocks.select { |r, _| r.cover?(n) }.max_by { |r, _| r.first }.first == range
      bare_calls.call(lines[n]).each do |meth|
        next if spelling_ok?(meth, methods_for.call(qt))
        class_hits << ["Qt::#{qt}##{meth} (no receiver, Qt::#{qt}.new block)", "#{f.sub(ROOT + '/', '')}:#{n + 1}"]
      end
    end
  end
end
class_bodies.each do |f, first, last, name|
  qt, defs = chain_of.call(name)
  next unless qt && var_of_name.key?(qt)
  known_here = methods_for.call(qt) + defs
  # A nested class's body is scanned as its own class, not as this one's.
  nested = class_bodies.select { |g, a, b, _| g == f && a > first && b <= last }.map { |_, a, b, _| (a..b) }
  File.readlines(f, encoding: 'BINARY')[first..last].each_with_index do |line, k|
    n = first + k
    next if nested.any? { |r| r.cover?(n) } || ctor_blocks[f].any? { |r, _| r.cover?(n) }
    bare_calls.call(line).each do |meth|
      next if spelling_ok?(meth, known_here)
      class_hits << ["Qt::#{qt}##{meth} (no receiver, #{name})", "#{f.sub(ROOT + '/', '')}:#{n + 1}"]
    end
  end
end

class_hits.uniq!(&:first)
class_hits.sort_by!(&:first)
class_hits.each { |sig, loc| puts format('      %-40s %s', sig, loc) }

puts "TOTAL #{missing.size + class_hits.size}   (name #{missing.size}, class #{class_hits.size})"
