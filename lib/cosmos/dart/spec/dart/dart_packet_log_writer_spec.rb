# encoding: ascii-8bit

# Copyright 2014 Ball Aerospace & Technologies Corp.
# All Rights Reserved.
#
# This program is free software; you can modify and/or redistribute it
# under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 3 with
# attribution addendums as found in the LICENSE.txt

require 'rails_helper'
require 'dart_packet_log_writer'

describe DartPacketLogWriter do
  before(:each) do
    DatabaseCleaner.strategy = :truncation
    DatabaseCleaner.clean
    Rails.application.load_seed
  end

  # Target name, packet name and ready for each entry of a PacketLog, in file order
  def log_entries(packet_log)
    PacketLogEntry.where(packet_log_id: packet_log.id).order(:data_offset).map do |ple|
      [ple.target.name, ple.packet.name, ple.ready]
    end
  end

  describe "write" do
    it "creates PacketLogEntries and flushes the file" do
      DatabaseCleaner.clean
      Rails.application.load_seed

      writer = DartPacketLogWriter.new(
        :TLM,    # Log telemetry
        'test_dart_tlm_', # Put dart_ in the log file name
        true,    # Enable logging
        nil,     # Don't cycle on time
        2_000_000_000, # Cycle the log at 2GB
        Cosmos::System.paths['DART_DATA']) # Log into the DART_DATA dir

      hs_packet = Cosmos::System.telemetry.packet("INST", "HEALTH_STATUS")
      (DartPacketLogWriter::DEFAULT_SYNC_COUNT_LIMIT).times do
        hs_packet.received_time = Time.now
        writer.write(hs_packet)
        sleep 0.01
      end
      sleep 0.1

      # The first Log Entry is always SYSTEM META
      ple = PacketLogEntry.find(1)
      expect(ple.target.name).to eq "SYSTEM"
      expect(ple.packet.name).to eq "META"
      expect(ple.ready).to eq true

      packet = writer.read_packet_from_ple(ple)
      expect(packet.class).to eq Cosmos::Packet
      expect(packet.target_name).to eq "SYSTEM"
      expect(packet.packet_name).to eq "META"

      target = Target.find_by_name("INST")
      packet = Packet.find_by_name("HEALTH_STATUS")
      count = 0
      count = PacketLogEntry.where("target_id = ? and packet_id = ?", target.id, packet.id).count
      expect(count).to eq (0)

      hs_packet.received_time = Time.now
      writer.write(hs_packet) # Write the packet that causes the flush
      sleep 0.1
      count = 0
      previous_time = Time.now
      PacketLogEntry.all.each do |ple|
        if count == 0
          expect(ple.target.name).to eq "SYSTEM"
          expect(ple.packet.name).to eq "META"
          expect(ple.ready).to eq true

          packet = writer.read_packet_from_ple(ple)
          expect(packet.target_name).to eq "SYSTEM"
          expect(packet.packet_name).to eq "META"
          expect(packet.received_time).to_not eq previous_time
        else
          expect(ple.target.name).to eq "INST"
          expect(ple.packet.name).to eq "HEALTH_STATUS"
          expect(ple.ready).to eq true

          packet = writer.read_packet_from_ple(ple)
          expect(packet.target_name).to eq "INST"
          expect(packet.packet_name).to eq "HEALTH_STATUS"
          expect(packet.received_time).to_not eq previous_time
        end
        previous_time = packet.received_time
        count += 1
      end
      # We wrote one SYSTEM META plus (DartPacketLogWriter::DEFAULT_SYNC_COUNT_LIMIT)
      # plus one more to cause the flush
      expect(count).to eq DartPacketLogWriter::DEFAULT_SYNC_COUNT_LIMIT + 1
      writer.shutdown
      sleep 0.1

      files = Dir["#{Cosmos::System.paths['DART_DATA']}/*_test_dart_tlm_*"]
      expect(files.length).to eq 1
    end

    it "creates command logs" do
      DatabaseCleaner.clean
      Rails.application.load_seed

      meta = Cosmos::System.commands.packet("SYSTEM", "META")
      clr_cmd = Cosmos::System.commands.packet("INST", "CLEAR")
      # 128 byte file header, SYSTEM META has 24 byte header,
      # INST CLEAR has 23 byte header
      length = 128 + 24 + meta.length + 23 + clr_cmd.length

      writer = DartPacketLogWriter.new(
        :CMD,    # Log commands
        'test_dart_cmd_', # Put dart_ in the log file name
        true,    # Enable logging
        nil,     # Don't cycle on time
        length, # Cycle the log at 1 Meta plus 1 Cmd
        Cosmos::System.paths['DART_DATA']) # Log into the DART_DATA dir

      clr_cmd.received_time = Time.now
      writer.write(clr_cmd)
      sleep 0.1

      # The first Log Entry is always SYSTEM META
      ple = PacketLogEntry.find(1)
      expect(ple.target.name).to eq "SYSTEM"
      expect(ple.packet.name).to eq "META"
      expect(ple.ready).to eq true

      clr_cmd.received_time = Time.now
      writer.write(clr_cmd) # The second command should create a new log
      sleep 0.1

      # Each log holds SYSTEM META and then the command. The writer saves
      # SYSTEM META entries at once but queues the others for its database
      # thread, so the first command's entry can get a higher id than the
      # second log's SYSTEM META. Check each log's entries in file order.
      first_log, second_log = PacketLog.order(:id).to_a
      expect(log_entries(first_log)).to eq [["SYSTEM", "META", true], ["INST", "CLEAR", true]]

      writer.shutdown
      sleep 0.1

      expect(log_entries(second_log)).to eq [["SYSTEM", "META", true], ["INST", "CLEAR", true]]

      files = Dir["#{Cosmos::System.paths['DART_DATA']}/*_test_dart_cmd_*"]
      expect(files.length).to eq 2
    end
  end
end
