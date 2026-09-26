# A migration used to insert the Status row that DART's processes update. For
# a new database, Rails 8's db:migrate loads db/schema.rb instead of running
# the migrations, so load db/seeds.rb (which adds the row if it's missing)
# after db:migrate as well.
Rake::Task['db:migrate'].enhance do
  Rails.application.load_seed
end
