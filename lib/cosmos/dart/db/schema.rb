# This file is auto-generated from the current state of the database. Instead
# of editing this file, please use the migrations feature of Active Record to
# incrementally modify your database, and then regenerate this schema definition.
#
# This file is the source Rails uses to define your schema when running `bin/rails
# db:schema:load`. When creating a new database, `bin/rails db:schema:load` tends to
# be faster and is potentially less error prone than running all of your
# migrations from scratch. Old migrations may fail to apply correctly if those
# migrations use external dependencies or application code.
#
# It's strongly recommended that you check this file into your version control system.

ActiveRecord::Schema[8.1].define(version: 2018_05_11_194944) do
  # These are extensions that must be enabled in order to support this database
  enable_extension "pg_catalog.plpgsql"

  create_table "item_to_decom_table_mappings", id: :serial, force: :cascade do |t|
    t.integer "item_id", null: false
    t.integer "packet_config_id", null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.integer "value_type"
    t.integer "table_index"
    t.integer "item_index"
    t.boolean "reduced"
    t.index ["item_id", "packet_config_id", "value_type"], name: "mapping_unique", unique: true
  end

  create_table "items", id: :serial, force: :cascade do |t|
    t.string "name", null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.integer "packet_id"
    t.index ["packet_id", "name"], name: "index_items_on_packet_id_and_name", unique: true
  end

  create_table "packet_configs", id: :serial, force: :cascade do |t|
    t.integer "packet_id", null: false
    t.string "name", null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.boolean "ready", default: false
    t.datetime "start_time", precision: nil
    t.datetime "end_time", precision: nil
    t.integer "first_system_config_id", null: false
    t.integer "max_table_index", default: -1
    t.index ["packet_id", "name"], name: "index_packet_configs_on_packet_id_and_name", unique: true
  end

  create_table "packet_log_entries", force: :cascade do |t|
    t.integer "target_id", null: false
    t.integer "packet_id", null: false
    t.datetime "time", precision: nil, null: false
    t.integer "packet_log_id", null: false
    t.bigint "data_offset", null: false
    t.bigint "meta_id"
    t.boolean "is_tlm", null: false
    t.integer "decom_state", default: 0
    t.boolean "ready", default: false
    t.index ["decom_state"], name: "index_packet_log_entries_on_decom_state"
    t.index ["time"], name: "index_packet_log_entries_on_time"
  end

  create_table "packet_logs", id: :serial, force: :cascade do |t|
    t.text "filename", null: false
    t.boolean "is_tlm", default: true, null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.index ["filename"], name: "index_packet_logs_on_filename", unique: true
  end

  create_table "packets", id: :serial, force: :cascade do |t|
    t.integer "target_id", null: false
    t.string "name", null: false
    t.boolean "is_tlm", default: true, null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.index ["target_id", "name", "is_tlm"], name: "index_packets_on_target_id_and_name_and_is_tlm", unique: true
  end

  create_table "statuses", force: :cascade do |t|
    t.bigint "decom_count", default: 0
    t.bigint "decom_error_count", default: 0
    t.text "decom_message", default: ""
    t.datetime "decom_message_time", precision: nil
    t.bigint "reduction_count", default: 0
    t.bigint "reduction_error_count", default: 0
    t.text "reduction_message", default: ""
    t.datetime "reduction_message_time", precision: nil
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
  end

  create_table "system_configs", id: :serial, force: :cascade do |t|
    t.string "name", null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.index ["name"], name: "index_system_configs_on_name", unique: true
  end

  create_table "targets", id: :serial, force: :cascade do |t|
    t.string "name", null: false
    t.datetime "created_at", precision: nil, null: false
    t.datetime "updated_at", precision: nil, null: false
    t.index ["name"], name: "index_targets_on_name", unique: true
  end
end
