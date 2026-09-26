# This file is used by Rack-based servers to start the application.
#
# DART runs no web server, but keep this file: Rails finds the application's
# root folder by looking for config.ru, and otherwise uses the current
# directory. DART's processes and rake tasks run from the COSMOS project's
# folder.

require_relative 'config/environment'

run Rails.application
