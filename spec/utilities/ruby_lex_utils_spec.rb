# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

require "spec_helper"
require "cosmos/utilities/ruby_lex_utils"

module Cosmos
  describe RubyLexUtils do
    before(:each) do
      @lex = RubyLexUtils.new
    end

    describe "contains_begin?" do
      it "detects the begin keyword" do
        expect(@lex.contains_begin?("  begin  ")).to be true
        expect(@lex.contains_begin?("  begin # asdf  ")).to be true
      end
    end

    describe "contains_keyword?" do
      it "detects the ruby keywords" do
        expect(@lex.contains_keyword?("if something")).to be true
        expect(@lex.contains_keyword?("obj.method = something")).to be false
      end
    end

    describe "contains_block_beginning?" do
      it "detects block beginning keywords" do
        expect(@lex.contains_block_beginning?("do")).to be true
        expect(@lex.contains_block_beginning?("[].each {")).to be true
        expect(@lex.contains_block_beginning?("begin")).to be true
      end
    end

    describe "remove_comments" do
      it "removes comments" do
text = <<DOC
# This is a comment
blah = 5 # Inline comment
# Another
DOC
        expect(@lex.remove_comments(text)).to eql "\nblah = 5 \n\n"
      end

      it "keeps the newlines of a =begin block so line numbers stay the same" do
        text = "a = 1\n=begin\nnotes\n=end\nb = 2\n"
        expect(@lex.remove_comments(text)).to eql "a = 1\n\n\n\nb = 2\n"
      end

      it "leaves a # that does not start a comment" do
        text = "x = %w(\#{var} B) # tail\ny = \"#\" + '#'\n"
        expect(@lex.remove_comments(text)).to eql "x = %w(\#{var} B) \ny = \"#\" + '#'\n"
      end
    end

    describe "each_lexed_segment" do
      it "yields each segment" do
text = <<DOC
begin
  x = 0
end
DOC
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["begin\n",false,0,1],  # can't instrument begin
          ["  x = 0\n",true,0,2],
          ["end\n",false,nil,3])  # can't instrument end
      end

      it "yields each segment" do
text = <<DOC

if x
y
else
z
end
DOC
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["\n",true,nil,1],
          ["if x\n",false,nil,2], # can't instrument if
          ["y\n",true,nil,3],
          ["else\n",false,nil,4], # can't instrument else
          ["z\n",true,nil,5],
          ["end\n",false,nil,6])  # can't instrument end
      end

      it "yields a statement with a heredoc as one segment, not instrumented" do
        # ScriptRunner would put its instrumentation on the terminator line
        text = "x = <<~EOS\n  hello\nEOS\ny = 1\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["x = <<~EOS\n  hello\nEOS\n",false,nil,1],
          ["y = 1\n",true,nil,4])
      end

      it "keeps a heredoc begun on a block's first line with that line" do
        text = "wait(<<~MSG) do |line|\n  waiting\nMSG\n  puts line\nend\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["wait(<<~MSG) do |line|\n  waiting\nMSG\n",false,nil,1],
          ["  puts line\n",true,nil,4],
          ["end\n",false,nil,5])
      end

      it "yields a method chain continued with leading dots as one segment" do
        text = "list = items\n  .map(&:upcase)\n  .sort\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          [text,true,nil,1])
      end

      it "ends the segment after a method's parameters" do
        text = "def run(x)\n  x + 1\nend\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["def run(x)\n",false,nil,1],
          ["  x + 1\n",true,nil,2],
          ["end\n",false,nil,3])
      end

      it "stays inside the outer begin after an inner begin ends" do
        text = "begin\n  begin\n    a\n  rescue\n    b\n  end\n  c\nrescue\n  d\nend\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["begin\n",false,0,1],
          ["  begin\n",false,1,2],
          ["    a\n",true,1,3],
          ["  rescue\n",false,1,4],
          ["    b\n",true,1,5],
          ["  end\n",false,0,6],
          ["  c\n",true,0,7],  # still inside the outer begin
          ["rescue\n",false,0,8],
          ["  d\n",true,0,9],
          ["end\n",false,nil,10])
      end

      it "ignores keywords and blocks inside string interpolation" do
        text = "puts \"\#{x if y} \#{[1].map { |i| i }}\"\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          [text,true,nil,1])
      end

      it "doesn't instrument a rightward pattern match, which has no value" do
        text = "config => {name:}\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          [text,false,nil,1])
      end

      it "stops at __END__" do
        text = "a = 1\n__END__\ndata\n"
        expect { |b| @lex.each_lexed_segment(text, &b) }.to yield_successive_args(
          ["a = 1\n",true,nil,1])
      end
    end
  end
end

