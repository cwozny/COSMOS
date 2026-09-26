# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

require 'prism'
require 'set'

# Splits Ruby source into the segments ScriptRunner instruments.
#
# This used to extend irb's old RubyLex, which Ruby 4 no longer ships. It now
# uses Prism, the parser Ruby itself uses since 3.4. The segments are the ones
# RubyLex produced: a line, or the lines of one statement that continues past
# a newline (an open bracket, a trailing operator or comma, a multi-line
# string or heredoc).
class RubyLexUtils
  # Regular expression to detect blank lines
  BLANK_LINE_REGEX  = /^\s*$/
  # Regular expression to detect lines containing only 'else'
  LONELY_ELSE_REGEX = /^\s*else\s*$/

  # Prism token types of the Ruby keywords. A segment containing one of them
  # is not instrumented. A block's '{' counts as a keyword too; see Lexed.
  KEYWORD_TYPES = [:KEYWORD_CLASS,
                   :KEYWORD_MODULE,
                   :KEYWORD_DEF,
                   :KEYWORD_UNDEF,
                   :KEYWORD_BEGIN,
                   :KEYWORD_RESCUE,
                   :KEYWORD_RESCUE_MODIFIER,
                   :KEYWORD_ENSURE,
                   :KEYWORD_END,
                   :KEYWORD_IF,
                   :KEYWORD_UNLESS,
                   :KEYWORD_THEN,
                   :KEYWORD_ELSIF,
                   :KEYWORD_ELSE,
                   :KEYWORD_CASE,
                   :KEYWORD_WHEN,
                   :KEYWORD_WHILE,
                   :KEYWORD_UNTIL,
                   :KEYWORD_FOR,
                   :KEYWORD_BREAK,
                   :KEYWORD_NEXT,
                   :KEYWORD_REDO,
                   :KEYWORD_RETRY,
                   :KEYWORD_IN,
                   :KEYWORD_DO,
                   :KEYWORD_DO_LOOP,
                   :KEYWORD_RETURN,
                   :KEYWORD_IF_MODIFIER,
                   :KEYWORD_UNLESS_MODIFIER,
                   :KEYWORD_WHILE_MODIFIER,
                   :KEYWORD_UNTIL_MODIFIER,
                   :KEYWORD_ALIAS,
                   :KEYWORD_BEGIN_UPCASE,
                   :KEYWORD_END_UPCASE].freeze

  # Prism token types which begin a block: do and begin. A block's '{' does
  # as well; see Lexed.
  BLOCK_BEGINNING_TYPES = [:KEYWORD_DO,
                           :KEYWORD_DO_LOOP,
                           :KEYWORD_BEGIN].freeze

  # Ruby reads on past a newline that follows one of these, but RubyLex ended
  # the segment there, so the statements after them are instrumented alone.
  SEGMENT_ENDING_TYPES = [:SEMICOLON,
                          :KEYWORD_BEGIN,
                          :KEYWORD_ELSE].freeze

  # Tokens that are not code, which never decide where a segment ends
  NON_CODE_TYPES = [:COMMENT,
                    :EMBDOC_BEGIN,
                    :EMBDOC_LINE,
                    :EMBDOC_END].freeze

  # Nodes whose bodies RubyLex counted as a deeper level of indentation
  NESTING_NODES = [Prism::BeginNode,
                   Prism::BlockNode,
                   Prism::CaseMatchNode,
                   Prism::CaseNode,
                   Prism::ClassNode,
                   Prism::DefNode,
                   Prism::ForNode,
                   Prism::IfNode,
                   Prism::LambdaNode,
                   Prism::ModuleNode,
                   Prism::SingletonClassNode,
                   Prism::UnlessNode,
                   Prism::UntilNode,
                   Prism::WhileNode].freeze

  # The tokens and syntax tree of one piece of text, indexed by line
  class Lexed
    # @return [Array<Array(Integer, Integer)>] The first and last line of
    #   each segment of code, in order
    attr_reader :segments

    # @param text [String] Ruby source
    def initialize(text)
      program, tokens = Prism.parse_lex(text).value
      @num_lines = text.lines.length
      @keyword_lines = Set.new
      @block_beginning_lines = Set.new
      @heredoc_lines = Set.new
      @begin_ranges = []
      @def_rparens = Set.new
      @heredoc_ends = {}
      heredoc_starts = []
      block_braces = Set.new
      find_blocks_and_begins(program, 0, block_braces)
      interpolation_depth = 0
      tokens.each do |token, _state|
        type = token.type
        line = token.location.start_line
        # RubyLex read a string as one token, so the code inside #{} never
        # counted, e.g. "#{x rescue 'none'}" or "#{list.map { |i| i }}"
        case type
        when :EMBEXPR_BEGIN
          interpolation_depth += 1
          next
        when :EMBEXPR_END
          interpolation_depth -= 1 if interpolation_depth > 0
          next
        end
        next if interpolation_depth > 0
        if [:BRACE_LEFT, :LAMBDA_BEGIN].include?(type) and block_braces.include?(token.location.start_offset)
          @keyword_lines << line
          @block_beginning_lines << line
        elsif type == :HEREDOC_START
          @heredoc_lines << line
          heredoc_starts << line
        elsif type == :HEREDOC_END
          # Prism emits a heredoc's body and terminator right after its opening
          start = heredoc_starts.pop
          @heredoc_ends[start] = [@heredoc_ends[start] || 0, line].max if start
        else
          @keyword_lines << line if KEYWORD_TYPES.include?(type)
          @block_beginning_lines << line if BLOCK_BEGINNING_TYPES.include?(type)
        end
      end
      @segments = find_segments(tokens)
    end

    # @param lines [Range<Integer>] Line numbers
    # @return [Boolean] Whether a Ruby keyword begins on any of the lines
    def keyword?(lines)
      lines.any? { |line| @keyword_lines.include?(line) }
    end

    # @param lines [Range<Integer>] Line numbers
    # @return [Integer, nil] The first of the lines on which a block begins
    def first_block_beginning(lines)
      lines.find { |line| @block_beginning_lines.include?(line) }
    end

    # @param lines [Range<Integer>] Line numbers
    # @return [Boolean] Whether a heredoc begins on any of the lines
    def heredoc?(lines)
      lines.any? { |line| @heredoc_lines.include?(line) }
    end

    # @param lines [Range<Integer>] Line numbers
    # @return [Integer, nil] The line of the last terminator of the heredocs
    #   that begin on the lines
    def heredoc_end(lines)
      lines.map { |line| @heredoc_ends[line] }.compact.max
    end

    # @param line [Integer] Line number
    # @return [Integer, nil] The indentation level of the innermost begin
    #   block the line is in, from its 'begin' line up to the line before its
    #   'end', or nil if it isn't in one
    def begin_level(line)
      # Work out every line's level once. Ranges are applied in order of their
      # first line, so the innermost block containing a line (the one that
      # starts last, and of those the last one found) sets its level.
      @begin_levels ||= begin
        levels = []
        @begin_ranges.each_with_index.sort_by { |(first, _last, _level), index| [first, index] }.each do |(first, last, level), _index|
          (first..last).each { |range_line| levels[range_line] = level }
        end
        levels
      end
      @begin_levels[line]
    end

    private

    # Records the offsets of block braces, which Prism lexes as the same
    # BRACE_LEFT as a Hash's, and of the ')' that closes each method's
    # parameters. Also records the lines each explicit begin block spans.
    def find_blocks_and_begins(node, level, block_braces)
      return unless node
      if node.is_a?(Prism::BlockNode) and node.opening_loc.slice == '{'
        block_braces << node.opening_loc.start_offset
      end
      # RubyLex lexed a lambda's '{' as a block's only after parameters:
      # '->(x) {' and '->x {', but not '-> {'
      if node.is_a?(Prism::LambdaNode) and node.parameters and node.opening_loc.slice == '{'
        block_braces << node.opening_loc.start_offset
      end
      if node.is_a?(Prism::DefNode) and node.rparen_loc
        @def_rparens << node.rparen_loc.start_offset
      end
      # 'value => pattern' has no value, so it can't be the last expression
      # of ScriptRunner's '__return_val = begin; ... end'. Like 'return', it
      # is only prefixed.
      if node.is_a?(Prism::MatchRequiredNode)
        @keyword_lines << node.location.start_line
      end
      if node.is_a?(Prism::BeginNode) and node.begin_keyword_loc
        last = node.end_keyword_loc ? node.end_keyword_loc.start_line - 1 : @num_lines
        @begin_ranges << [node.begin_keyword_loc.start_line, last, level]
      end
      level += 1 if NESTING_NODES.include?(node.class)
      node.compact_child_nodes.each do |child|
        find_blocks_and_begins(child, level, block_braces)
      end
    end

    # Splits the text into segments. A segment ends at a newline where the
    # statement ends, or where RubyLex ended one: after SEGMENT_ENDING_TYPES
    # and after a method's parameters, e.g. 'def run(x)'. It never ends
    # before the last line of a multi-line string or heredoc in it. Prism
    # emits a heredoc's body right after its opening, so it is seen before
    # the newline that ends the heredoc's first line.
    def find_segments(tokens)
      segments = []
      first = 1
      last = 0
      last_type = nil
      last_offset = nil
      end_line = @num_lines
      interpolation_depth = 0
      tokens.each do |token, _state|
        type = token.type
        location = token.location
        # A newline inside a multi-line #{} is inside a string
        if type == :EMBEXPR_BEGIN
          interpolation_depth += 1
        elsif type == :EMBEXPR_END
          interpolation_depth -= 1 if interpolation_depth > 0
        elsif interpolation_depth > 0 and [:NEWLINE, :IGNORED_NEWLINE].include?(type)
          next
        end
        case type
        when :EOF
          break
        when :__END__
          # RubyLex stopped here, so the data after __END__ was never instrumented
          end_line = location.start_line - 1
          break
        when :NEWLINE, :IGNORED_NEWLINE
          if last_type and (type == :NEWLINE or
                            SEGMENT_ENDING_TYPES.include?(last_type) or
                            @def_rparens.include?(last_offset))
            segment_end = [location.start_line, last].max
            segments << [first, segment_end]
            first = segment_end + 1
          end
          last_type = nil
        else
          next if NON_CODE_TYPES.include?(type)
          last_type = type
          last_offset = location.start_offset
          token_end = location.end_line
          # A token ending in a newline ends at the start of the next line
          token_end -= 1 if token_end > location.start_line and token.value.end_with?("\n")
          last = token_end if token_end > last
        end
      end
      segments << [first, end_line] if first <= end_line
      segments
    end
  end

  # @param text [String]
  # @return [Boolean] Whether the text contains the 'begin' keyword
  def contains_begin?(text)
    Prism.lex(text).value.any? { |token, _state| token.type == :KEYWORD_BEGIN }
  end

  # @param text [String]
  # @return [Boolean] Whether the text contains a Ruby keyword
  def contains_keyword?(text)
    lexed = Lexed.new(text)
    lexed.keyword?(1..text.lines.length)
  end

  # @param text [String]
  # @return [Boolean] Whether the text contains a keyword which starts a block.
  #   i.e. 'do', '{', or 'begin'
  def contains_block_beginning?(text)
    lexed = Lexed.new(text)
    !lexed.first_block_beginning(1..text.lines.length).nil?
  end

  # @param text [String]
  # @param progress_dialog [Cosmos::ProgressDialog] Not used. Prism finds the
  #   comments in one fast pass, so there is no progress to report.
  # @return [String] The text with all comments removed. A =begin/=end block
  #   leaves its newlines behind, so the line numbers stay the same.
  def remove_comments(text, progress_dialog = nil)
    comments_removed = text.b
    Prism.parse_comments(text).reverse_each do |comment|
      location = comment.location
      replacement = ''
      if comment.is_a?(Prism::EmbDocComment)
        replacement = "\n" * comments_removed.byteslice(location.start_offset, location.length).count("\n")
      end
      comments_removed.bytesplice(location.start_offset, location.length, replacement)
    end
    comments_removed.force_encoding(text.encoding)
  end

  # Yields each lexed segment and if the segment is instrumentable
  #
  # @param text [String]
  # @yieldparam line [String] The entire line
  # @yieldparam instrumentable [Boolean] Whether the line is instrumentable
  # @yieldparam inside_begin [Integer] The level of indentation
  # @yieldparam line_no [Integer] The current line number
  def each_lexed_segment(text)
    lexed = Lexed.new(text)
    lines = text.lines
    lexed.segments.each do |first, last|
      line_no = first
      loop do # loop to allow restarting for nested conditions
        inside_begin = lexed.begin_level(line_no)

        # Yield blank lines and lonely else lines before the actual line
        while line_no <= last
          line = lines[line_no - 1]
          if line =~ BLANK_LINE_REGEX
            yield line, true, inside_begin, line_no
          elsif line =~ LONELY_ELSE_REGEX
            yield line, false, inside_begin, line_no
          else
            break
          end
          line_no += 1
          inside_begin = lexed.begin_level(line_no)
        end
        break if line_no > last

        segment_lines = line_no..last
        lexed_text = lines[(line_no - 1)..(last - 1)].join
        if lexed.keyword?(segment_lines)
          block_line = lexed.first_block_beginning(segment_lines)
          if block_line
            # The body of a heredoc begun on these lines comes right after them
            section_end = block_line
            while (heredoc_end = lexed.heredoc_end(line_no..section_end)) and heredoc_end > section_end
              section_end = heredoc_end
            end
            section_end = last if section_end > last
            yield lines[(line_no - 1)..(section_end - 1)].join, false, inside_begin, line_no
            line_no = section_end + 1
            next if line_no <= last
          else
            yield lexed_text, false, inside_begin, line_no
          end
        elsif lexed.heredoc?(segment_lines)
          # ScriptRunner adds its instrumentation after the last line, which
          # would put it on the heredoc's terminator line
          yield lexed_text, false, inside_begin, line_no
        else
          num_left_brackets  = lexed_text.count('{')
          num_right_brackets = lexed_text.count('}')
          if num_left_brackets != num_right_brackets
            # Don't instrument lines with unequal numbers of { and } brackets
            yield lexed_text, false, inside_begin, line_no
          else
            yield lexed_text, true, inside_begin, line_no
          end
        end
        break
      end # loop do
    end # lexed.segments.each
  end # def each_lexed_segment
end
