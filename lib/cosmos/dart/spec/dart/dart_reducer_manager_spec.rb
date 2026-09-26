# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

require 'rails_helper'
require 'dart_reducer_manager'
require 'dart_packet_log_writer'
require 'dart_decommutator'
require 'dart_common'

describe DartReducerManager do
  let(:common) { Object.new.extend(DartCommon) }

  before(:each) do
    DatabaseCleaner.strategy = :truncation
    DatabaseCleaner.clean
    Rails.application.load_seed
  end

  after(:each) { stop_dart_master }

  def setup_ples(entries, delta_time)
    time = Time.utc(2018, 1, 1, 0, 0, 0)
    meta = Cosmos::System.telemetry.packet("SYSTEM", "META")
    meta.received_time = time
    hs_packet = Cosmos::System.telemetry.packet("INST", "HEALTH_STATUS")
    # 128 byte file header, SYSTEM META has 15 byte header + length of SYSTEM & META
    # INST HEALTH_STATUS has 15 byte header + length of INST & HEALTH_STATUS
    length = 128 + 25 + meta.length + entries * (32 + hs_packet.length)

    writer = DartPacketLogWriter.new(
      :TLM,    # Log telemetry
      'test_decom_', # File name suffix
      true,    # Enable logging
      nil,     # Don't cycle on time
      length, # Cycle the log after a single INST HEALTH_STATUS packet
      Cosmos::System.paths['DART_DATA']) # Log into the DART_DATA dir

    entries.times do |x|
      hs_packet.received_time = time
      hs_packet.write("COLLECTS", x)
      writer.write(hs_packet)
      time += delta_time
    end
    ples = 0
    count = 0
    writer.shutdown
    while ples != (entries + 1) # SYSTEM META is the plus 1
      ples = PacketLogEntry.count
      sleep 0.1 # Allow the log writer to work
      count += 1
      break if count == 100 # 10s
    end
    sleep 0.1
    expect(count).to be < 100

    start_dart_master
    thread = Thread.new do
      decom = DartDecommutator.new
      decom.run
    end
    complete = 0
    count = 0
    while complete != (entries + 1) # SYSTEM META is the plus 1
      complete = PacketLogEntry.where("decom_state = #{PacketLogEntry::COMPLETE}").length
      sleep 0.1 # Allow the decommutator to work
      count += 1
      break if count == 1200 # 120s
    end
    expect(count).to be < 1200
    thread.kill
  end

  def worker_threads
    count = 0
    Thread.list.each do |t|
      count += 1 if t.inspect.include?("worker_thread")
    end
    count
  end

  def get_mappings(tgt, pkt, item)
    target_model = Target.where("name = ?", tgt).first
    packet_model = Packet.where("target_id = ? and name = ? and is_tlm = ?", target_model.id, pkt, true).first
    item_model = Item.where("packet_id = ? and name = ?", packet_model.id, item).first
    mappings = ItemToDecomTableMapping.where("item_id = ? and value_type != ?", item_model.id, ItemToDecomTableMapping::RAW)
  end

  # Run one reducer pass. The reducer reduces what it can, then waits 60s before
  # its next pass, so wait until the pass has made `count` rows in the INST
  # HEALTH_STATUS reduction table with the given suffix ("_m", "_h" or "_d").
  def run_reducer_pass(table, count, timeout = 60)
    mapping = get_mappings("INST", "HEALTH_STATUS", "COLLECTS").first
    model = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, table)
    drm = DartReducerManager.new(1)
    thread = Thread.new { drm.run }
    deadline = Time.now + timeout
    sleep 0.1 while model.count < count && Time.now < deadline
    drm.shutdown # Lets the job in progress finish
    thread.kill
  end

  describe "run" do
    it "starts the specified number of worker threads" do
      drm = DartReducerManager.new(1)
      thread = Thread.new { drm.run }
      sleep 0.1
      expect(worker_threads()).to eq 1
      drm.shutdown
      thread.kill

      drm = DartReducerManager.new(5)
      thread = Thread.new { drm.run }
      sleep 0.1
      expect(worker_threads()).to eq 5
      drm.shutdown
      thread.kill
    end

    # The reducer only reduces a table once the rows ready to reduce span more
    # than DartReducerWorkerThread::HOLD_OFF_TIME (2 minutes), and it leaves the
    # rows in the last minute of that span for a later pass.

    it "reduces per minute" do
      # 31 entries 6s apart span 00:00:00 to 00:03:00. Minutes 00:00 and 00:01
      # are reduced and the 11 entries from 00:02:00 on are held back.
      setup_ples(31, 6)
      run_reducer_pass("_m", 2)

      get_mappings("INST", "HEALTH_STATUS", "COLLECTS").each do |mapping|
        # Grab the base reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index)
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 11
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 20
        val = 0
        rows.find_each do |row|
          expect(row.read_attribute("i#{mapping.item_index}")).to eq val
          val += 1
        end

        # Grab the minute reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, "_m")
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        # Two minutes don't span enough to be reduced to an hour
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 2
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 0
        val = 0
        rows.find_each do |row|
          expect(row.num_samples).to eq 10
          expect(row.read_attribute("i#{mapping.item_index}min")).to eq val
          expect(row.read_attribute("i#{mapping.item_index}max")).to eq val + 9
          expect(row.read_attribute("i#{mapping.item_index}avg")).to eq ((val..(val + 9)).to_a.sum / 10.0)
          expect(row.read_attribute("i#{mapping.item_index}stddev")).to be_within(0.0000001).of(Math.stddev_population((val..(val + 9)).to_a)[1])
          val += 10
        end
      end
    end

    it "reduces per hour" do
      # 721 entries 10.001s apart span 00:00:00 to 02:00:00.7. Minutes 00:00 to
      # 01:58 are reduced (the 7 entries from 01:59 on are held back), and of
      # those minutes, hour 00 is reduced (01:58 is held back).
      setup_ples(721, 10.001)
      run_reducer_pass("_h", 1)

      get_mappings("INST", "HEALTH_STATUS", "COLLECTS").each do |mapping|
        # Grab the base reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index)
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 7
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 714
        val = 0
        rows.find_each do |row|
          expect(row.read_attribute("i#{mapping.item_index}")).to eq val
          val += 1
        end

        # Grab the minute reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, "_m")
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        # Minutes 01:00 to 01:58 wait for the next hour
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 59
        # We reduced 60 minutes
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 60
        val = 0
        rows.find_each do |row|
          expect(row.num_samples).to eq 6
          expect(row.read_attribute("i#{mapping.item_index}min")).to eq val
          expect(row.read_attribute("i#{mapping.item_index}max")).to eq val + 5
          expect(row.read_attribute("i#{mapping.item_index}avg")).to eq ((val..(val + 5)).to_a.sum / 6.0)
          expect(row.read_attribute("i#{mapping.item_index}stddev")).to be_within(0.0000001).of(Math.stddev_population((val..(val + 5)).to_a)[1])
          val += 6
        end

        # Grab the hour reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, "_h")
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        # A single hour doesn't span enough to be reduced to a day
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 1
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 0
        row = rows.first
        # num_samples counts the rows reduced: 60 minutes of 6 entries each
        expect(row.num_samples).to eq 60
        expect(row.read_attribute("i#{mapping.item_index}min")).to eq 0
        expect(row.read_attribute("i#{mapping.item_index}max")).to eq 359
        expect(row.read_attribute("i#{mapping.item_index}avg")).to eq ((0..359).to_a.sum / 360.0)
        expect(row.read_attribute("i#{mapping.item_index}stddev")).to be_within(0.05).of(Math.stddev_population((0..359).to_a)[1])
      end
    end

    it "reduces per day" do
      # 78 entries 3600.001s apart span three days and 5 hours. Each minute and
      # hour holds one entry, and days 1 to 3 are reduced. The last two rows of
      # each table are held back or wait for the next minute, hour or day.
      setup_ples(78, 3600.001)
      run_reducer_pass("_d", 3)

      get_mappings("INST", "HEALTH_STATUS", "COLLECTS").each do |mapping|
        # Grab the base reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index)
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 2
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 76
        val = 0
        rows.find_each do |row|
          expect(row.read_attribute("i#{mapping.item_index}")).to eq val
          val += 1
        end

        # Grab the minute and hour reduction tables
        [["_m", 74], ["_h", 72]].each do |table, reduced|
          rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, table)
          expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
          expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 2
          expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq reduced
          val = 0
          rows.find_each do |row|
            # Since our samples were more than 1 hour apart there is only 1 sample per row
            expect(row.num_samples).to eq 1
            # Min, max, and avg are all the same since we only have 1 sample
            expect(row.read_attribute("i#{mapping.item_index}min")).to eq val
            expect(row.read_attribute("i#{mapping.item_index}max")).to eq val
            expect(row.read_attribute("i#{mapping.item_index}avg")).to eq val
            expect(row.read_attribute("i#{mapping.item_index}stddev")).to eq 0
            val += 1
          end
        end

        # Grab the day reduction table
        rows = common.get_decom_table_model(mapping.packet_config_id, mapping.table_index, "_d")
        expect(rows.where("reduced_state" => DartCommon::INITIALIZING).count).to eq 0
        # Day values are always "READY" since they don't get further reduced
        expect(rows.where("reduced_state" => DartCommon::READY_TO_REDUCE).count).to eq 3
        # Reduced is always 0
        expect(rows.where("reduced_state" => DartCommon::REDUCED).count).to eq 0
        val = 0
        rows.find_each do |row|
          expect(row.num_samples).to eq 24
          expect(row.read_attribute("i#{mapping.item_index}min")).to eq val
          expect(row.read_attribute("i#{mapping.item_index}max")).to eq val + 23
          expect(row.read_attribute("i#{mapping.item_index}avg")).to eq ((val..(val+23)).to_a.sum / 24.0)
          expect(row.read_attribute("i#{mapping.item_index}stddev")).to be_within(0.0000001).of(Math.stddev_population((val..(val+23)).to_a)[1])
          val += 24
        end
      end
    end
  end
end
