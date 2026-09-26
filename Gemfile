# encoding: ascii-8bit

source 'https://rubygems.org'

gem 'ruby-termios', '>= 0.9' if RbConfig::CONFIG['target_os'] !~ /mswin|mingw|cygwin/i and RUBY_ENGINE == 'ruby'
# This is commented out because wdm does not currently support Ruby 2.2
#group :development do
#  gem 'wdm', '>= 0.1.0', :platforms => [:mswin, :mingw]
#end
gemspec
instance_eval File.read(File.join(__dir__, 'install/config/dart/Gemfile'))
# DART's specs (lib/cosmos/dart/spec)
group :test do
  gem 'rspec-rails', '~> 8.0'
  gem 'database_cleaner-active_record', '~> 2.2'
end
