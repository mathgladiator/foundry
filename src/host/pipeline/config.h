// Config tables (design/flow.md, "Config"): named records of one channel type
// that a pipeline picks from by name, such as tree species. A table lives in
// the workspace as config/<table>/: table.json names its channel and how the
// editor shows it, and each other <entry>.json holds one record's value.
//
// Nothing here knows any table: the channel's schema gives the fields and
// ranges, and table.json the editor's groups and colours.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge/forge.h"

namespace pipeline::config {

std::string dir(const forge::Paths &paths, const std::string &table);
std::string entry_path(const forge::Paths &paths, const std::string &table, const std::string &entry);
// The table's description (table.json); null if there's no such table.
nlohmann::json table(const forge::Paths &paths, const std::string &table);
// The table's entries, sorted.
std::vector<std::string> entries(const forge::Paths &paths, const std::string &table);
// An entry's file (state, doc, notes, value); null if missing.
nlohmann::json entry(const forge::Paths &paths, const std::string &table, const std::string &entry);
// The table and entry an entry file belongs to (config/<table>/<entry>.json).
bool split(const std::string &path, std::string &table, std::string &entry);

// The table channel's schema fields, each with name, type, and any min, max, enum, and doc.
nlohmann::json fields(const nlohmann::json &table);
// The same for any channel by name (a nested struct's fields); empty if there is no such channel.
nlohmann::json channel_fields(const std::string &channel);
// A fresh value of a channel (each field at its minimum, its first enum value, or empty).
nlohmann::json blank(const std::string &channel);
// Checks values ({field: value}) against the table's channel and merges them into value.
bool apply(const nlohmann::json &table, nlohmann::json &value, const nlohmann::json &values, std::string &error);

}  // namespace pipeline::config
