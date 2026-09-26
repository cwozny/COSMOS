# encoding: ascii-8bit

# End-to-end check of DART, run by the Cosmos 4 Tests workflow. It uses a
# COSMOS project the way a user would:
# 1. Writes a telemetry log of INST HEALTH_STATUS packets into DART_DATA.
# 2. Imports it with dart_import.
# 3. Starts DART with the project's Dart tool, which starts every DART process
#    in production mode.
# 4. Asks the decom server for the decommutated and the reduced values and the
#    stream server for the packets, and compares them with what it wrote.
# 5. Stops DART.
#
# Run it from the demo folder with the repository's bundle, after
# rake db:create db:migrate, with DART_USERNAME, DART_PASSWORD and DART_DB set:
#   cd demo && BUNDLE_GEMFILE=../Gemfile bundle exec ruby ../.github/scripts/dart_end_to_end.rb
# It exits 1 if a check fails.

require 'cosmos'
require 'cosmos/io/json_drb_object'
require 'cosmos/interfaces/tcpip_client_interface'
require 'json'

Cosmos::Logger.level = Cosmos::Logger::WARN

NUM_PACKETS = 240 # One a second for four minutes
START_TIME = Time.utc(2020, 1, 1, 0, 0, 0)
END_TIME = START_TIME + NUM_PACKETS
DART_TOOL = File.join(Cosmos::USERPATH, 'tools', 'Dart')
DART_IMPORT = File.expand_path('../../bin/dart_import', __dir__)
DECOM_TIMEOUT = 120 # seconds
# The reducer makes a pass when it starts and then once a minute, and the
# rows have to be decommutated before a pass can reduce them
REDUCE_TIMEOUT = 180 # seconds

$failures = 0
def check(description, ok)
  puts "#{ok ? 'PASS' : 'FAIL'}: #{description}"
  $failures += 1 unless ok
end

# Waits until the block returns something other than nil or false, and returns it
def wait_for(description, timeout)
  deadline = Time.now + timeout
  last_error = nil
  loop do
    result = begin
      yield
    rescue DRb::DRbConnError, Errno::ECONNREFUSED => error
      last_error = error # The server isn't up yet
      nil
    rescue RuntimeError => error
      # The decommutator hasn't made INST HEALTH_STATUS's tables yet
      raise unless error.message.include?('not found')
      last_error = error
      nil
    end
    return result if result
    if Time.now > deadline
      raise "Timed out after #{timeout}s waiting for #{description}#{last_error ? " (last error: #{last_error.message})" : ''}"
    end
    sleep 1
  end
end

def query(server, value_type, reduction)
  server.query({ 'start_time_sec' => START_TIME.tv_sec, 'start_time_usec' => 0,
    'end_time_sec' => END_TIME.tv_sec, 'end_time_usec' => 0,
    'item' => ['INST', 'HEALTH_STATUS', 'TEMP1'], 'reduction' => reduction,
    'value_type' => value_type, 'cmd_tlm' => 'TLM' })
end

# 1. The packet log, with a different TEMP1 each second
packet = Cosmos::System.telemetry.packet('INST', 'HEALTH_STATUS')
writer = Cosmos::PacketLogWriter.new(:TLM, 'dart_end_to_end_', true, nil, 2_000_000_000, Cosmos::System.paths['DART_DATA'])
expected = []
NUM_PACKETS.times do |i|
  packet.received_time = START_TIME + i
  packet.write('TEMP1', 1000 + i, :RAW)
  writer.write(packet)
  expected << { raw: packet.read('TEMP1', :RAW), converted: packet.read('TEMP1', :CONVERTED),
                time: packet.received_time, buffer: packet.buffer }
end
filename = writer.filename
writer.shutdown
puts "Wrote #{NUM_PACKETS} INST HEALTH_STATUS packets to #{filename}"

# 2. Import it
check("dart_import #{File.basename(filename)} exits 0", system(RbConfig.ruby, DART_IMPORT, filename))

FileUtils.mkdir_p(Cosmos::System.paths['DART_LOGS'])
dart_log = File.join(Cosmos::System.paths['DART_LOGS'], 'dart_end_to_end_tool.txt')
dart_pid = nil
begin
  # 3. Start DART
  dart_pid = Process.spawn(RbConfig.ruby, DART_TOOL, [:out, :err] => [dart_log, 'w'])
  puts "Started the Dart tool (pid #{dart_pid})"
  server = Cosmos::JsonDRbObject.new(Cosmos::System.connect_hosts['DART_DECOM'],
    Cosmos::System.ports['DART_DECOM'], 5.0, Cosmos::System.x_csrf_token)

  # 4. Decommutated values
  raw = wait_for('the decommutated values', DECOM_TIMEOUT) do
    rows = query(server, 'RAW', 'NONE')
    rows if rows.length >= NUM_PACKETS
  end
  check("the decom server returns #{NUM_PACKETS} raw TEMP1 values (got #{raw.length})", raw.length == NUM_PACKETS)
  check("the raw values match the log",
    raw.map { |row| row[0] } == expected.map { |e| e[:raw] })
  check("their times match the log",
    raw.map { |row| Time.at(row[1], row[2]).utc } == expected.map { |e| e[:time] })
  converted = query(server, 'CONVERTED', 'NONE')
  check("the converted values match the log",
    converted.length == NUM_PACKETS &&
    converted.zip(expected).all? { |row, e| (row[0] - e[:converted]).abs < 1e-9 })

  # Reduced values. The entries span four minutes and the reducer holds back
  # the last minute of what it has, so minutes 0 and 1 are reduced: minute 2
  # would end at the first entry of minute 3.
  minutes = wait_for('the minute reductions', REDUCE_TIMEOUT) do
    rows = query(server, 'RAW_AVG', 'MINUTE')
    rows if rows.length >= 2
  end
  check("the decom server returns 2 minute averages (got #{minutes.length})", minutes.length == 2)
  2.times do |minute|
    values = expected[minute * 60, 60].map { |e| e[:raw] }
    row = minutes[minute] || []
    check("minute #{minute}: average #{row[0].inspect} of #{row[3].inspect} samples, expected #{values.sum / 60.0} of 60",
      row[0] == values.sum / 60.0 && row[3] == 60 && Time.at(row[1], row[2]).utc == START_TIME + minute * 60)
  end
  mins = query(server, 'RAW_MIN', 'MINUTE').map { |row| row[0] }
  maxs = query(server, 'RAW_MAX', 'MINUTE').map { |row| row[0] }
  check("the minute minimums are #{mins.inspect}", mins == [1000, 1060])
  check("the minute maximums are #{maxs.inspect}", maxs == [1059, 1119])

  # Packets from the stream server
  request_packet = Cosmos::Packet.new('DART', 'DART')
  request_packet.define_item('REQUEST', 0, 0, :BLOCK)
  request_packet.write('REQUEST', JSON.dump({ 'start_time_sec' => START_TIME.tv_sec, 'start_time_usec' => 0,
    'end_time_sec' => END_TIME.tv_sec, 'end_time_usec' => 0,
    'cmd_tlm' => 'TLM', 'packets' => [['INST', 'HEALTH_STATUS']] }))
  interface = Cosmos::TcpipClientInterface.new(Cosmos::System.connect_hosts['DART_STREAM'],
    Cosmos::System.ports['DART_STREAM'], Cosmos::System.ports['DART_STREAM'], 10, 30, 'PREIDENTIFIED')
  interface.connect
  interface.write(request_packet)
  streamed = []
  while (streamed_packet = interface.read)
    streamed << streamed_packet
  end
  interface.disconnect
  health_status = streamed.select { |p| p.target_name == 'INST' && p.packet_name == 'HEALTH_STATUS' }
  check("the stream server sends SYSTEM META and #{NUM_PACKETS} INST HEALTH_STATUS packets (got #{streamed.map { |p| "#{p.target_name} #{p.packet_name}" }.uniq.inspect}, #{health_status.length} INST HEALTH_STATUS)",
    streamed.first && streamed.first.packet_name == 'META' && health_status.length == NUM_PACKETS)
  check("the streamed packets match the log",
    health_status.map { |p| [p.received_time, p.buffer] } == expected.map { |e| [e[:time], e[:buffer]] })
  server.disconnect
rescue => error
  check("no errors (#{error.class}: #{error.message})", false)
ensure
  # 5. Stop DART
  if dart_pid
    Process.kill('INT', dart_pid)
    deadline = Time.now + 30
    stopped = nil
    until (stopped = Process.waitpid(dart_pid, Process::WNOHANG)) || Time.now > deadline
      sleep 0.5
    end
    unless stopped
      Process.kill('KILL', dart_pid)
      Process.waitpid(dart_pid)
    end
    check("DART stops on SIGINT", !stopped.nil?)
  end
end

if $failures > 0
  # Show what DART logged
  Dir[File.join(Cosmos::System.paths['DART_LOGS'], '*')].sort.each do |log|
    puts "\n----- #{log}"
    puts File.read(log).lines.last(40).join
  end
end
puts "\n#{$failures} failed checks"
exit($failures > 0 ? 1 : 0)
