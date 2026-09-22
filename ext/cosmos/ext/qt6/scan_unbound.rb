#!/usr/bin/env ruby
# Static sweep: every camelCase call COSMOS makes on a Qt receiver, minus every
# name the binding defines and every name COSMOS defines itself. What is left
# is a Qt method that will raise NoMethodError the moment that control is used.
#
# This exists because each of these used to be found the slow way -- by a user
# clicking something and getting a stack trace.
ROOT = File.expand_path('../../../..', __dir__)

bound = []
cpp = File.read(File.join(ROOT, 'ext/cosmos/ext/qt6/cosmos_qt6.cpp'))
bound += cpp.scan(/QS?DEF\(\s*\w+\s*,\s*"([^"]+)"/).flatten   # QDEF and QSDEF (class methods)
bound += cpp.scan(/rb_define_(?:singleton_)?method\(\s*\w+\s*,\s*"([^"]+)"/).flatten
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
puts "TOTAL #{missing.size}"
