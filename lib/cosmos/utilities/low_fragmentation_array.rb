# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

module Cosmos
  # The Array that TlmGrapher and LineGraph keep their data points in.
  #
  # This used to be a C extension that copied Ruby 1.9's private array code
  # to preallocate memory and prune without reallocating. It set RArray flag
  # bits directly, and Ruby 3.x and 4.0 use different bits, so it would
  # corrupt arrays there. Ruby's own Array now manages its memory well enough.
  class LowFragmentationArray < Array
    # @param capacity [Integer] How many values the caller expects to add.
    #   Kept for the callers; like the C version, the array starts empty.
    def initialize(capacity = 0)
      super()
    end

    # Removes the values before index and shifts the rest down.
    #
    # @param index [Integer] Index of the first value to keep. Negative
    #   values count from the end.
    # @return [LowFragmentationArray] self
    def remove_before!(index)
      index += length if index < 0
      slice!(0, index) if index > 0
      self
    end
  end
end
