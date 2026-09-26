require_relative 'boot'

require 'rails'
# DART only uses Active Record. It has no controllers, views, jobs, mailers,
# channels or assets.
require 'active_model/railtie'
require 'active_record/railtie'

# Require the gems listed in Gemfile, including any gems
# you've limited to :test, :development, or :production.
Bundler.require(*Rails.groups)

module CosmosDart
  class Application < Rails::Application
    config.load_defaults 8.1

    # Settings in config/environments/* take precedence over those specified here.
    # Application configuration should go into files in config/initializers
    # -- all .rb files in that directory are automatically loaded.

    # DART doesn't use Rails.cache
    config.cache_store = :null_store

    # Active Record builds a message verifier when it loads, and that needs a
    # secret_key_base. Outside development and test, Rails only reads it from
    # ENV['SECRET_KEY_BASE'] or the encrypted credentials. DART signs nothing
    # (no cookies, sessions, signed ids or tokens), so a random key per process
    # is enough and nothing has to be written under Rails.root.
    config.secret_key_base = ENV['SECRET_KEY_BASE'] || SecureRandom.hex(64)
  end
end
