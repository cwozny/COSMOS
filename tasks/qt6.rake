# encoding: ascii-8bit

# The Qt6 binding's own suites. These existed but were wired to nothing, so
# nothing ran them and nothing could fail a build because of them --
# including scan_unbound.rb, the guard against unbound Qt methods, which is
# ratcheted in test_regressions.rb section 19.
QT6_SUITES = %w[
  test_qt6
  test_regressions
  test_gvl_race
  test_nested_gvl
  test_paint_gvl
].freeze

desc 'Run the Qt6 binding test suites (headless)'
task :qt6_test do
  ext = File.join('ext', 'cosmos', 'ext', 'qt6')
  unless File.exist?(File.join('lib', 'cosmos', 'ext', "qt6.#{RbConfig::CONFIG['DLEXT']}"))
    puts 'qt6 extension not built - skipping (run rake build first)'
    next
  end
  ENV['QT_QPA_PLATFORM'] ||= 'offscreen'
  failed = []
  QT6_SUITES.each do |suite|
    file = File.join(ext, "#{suite}.rb")
    puts "\n--- #{suite} ---"
    failed << suite unless system(RbConfig.ruby, '-Ilib', file)
  end
  abort("\nQt6 suites FAILED: #{failed.join(', ')}") unless failed.empty?
  puts "\nAll #{QT6_SUITES.size} Qt6 suites passed."
end

desc 'Report Qt methods COSMOS calls that the binding never bound'
task :qt6_scan_unbound do
  system(RbConfig.ruby, '-rset', File.join('ext', 'cosmos', 'ext', 'qt6', 'scan_unbound.rb')) ||
    abort('scan_unbound.rb failed')
end
