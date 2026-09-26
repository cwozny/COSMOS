Rails.application.configure do
  # Settings specified here will take precedence over those in config/application.rb.

  # Make code changes take effect without restarting.
  config.enable_reloading = true

  # Do not eager load code on boot.
  config.eager_load = false

  # Show full error reports.
  config.consider_all_requests_local = true

  # Print deprecation notices to the Rails logger.
  config.active_support.deprecation = :log

  if ENV['DART_DEBUG']
    logger = ActiveSupport::Logger.new(File.join(Cosmos::System.paths['DART_LOGS'], 'dart_development.log'))
    logger.formatter = config.log_formatter
    config.logger = ActiveSupport::TaggedLogging.new(logger)
  else
    config.logger = ActiveSupport::Logger.new(nil)
  end
end
