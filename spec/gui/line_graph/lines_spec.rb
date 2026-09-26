# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

require 'spec_helper'
require 'cosmos/gui/line_graph/lines'

module Cosmos

  describe Lines do

    describe "add_line" do
      it "accepts states given as a hash" do
        # TlmGrapher passes an item's states, a Hash
        lines = Lines.new
        lines.add_line('ITEM', [0, 1], [0.0, 1.0], nil, nil, { 'OFF' => 0, 'ON' => 1 }, { 'LOW' => 0.0, 'HIGH' => 1.0 })
        expect(lines.num_lines).to eql 1
      end

      it "rejects states that are not hash-like" do
        expect { Lines.new.add_line('ITEM', [0, 1], nil, nil, nil, [0, 1]) }.to raise_error(ArgumentError, /y_states/)
        expect { Lines.new.add_line('ITEM', [0, 1], nil, nil, nil, nil, [0, 1]) }.to raise_error(ArgumentError, /x_states/)
      end
    end
  end
end
