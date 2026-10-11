// Small helpers shared by host slices.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace core {

bool read_file(const std::string &path, std::vector<uint8_t> &out);
bool read_text(const std::string &path, std::string &out);
bool write_file(const std::string &path, const void *data, size_t size);
bool write_text(const std::string &path, const std::string &text);

// Reads a JSON file; returns a null json on failure.
nlohmann::json read_json(const std::string &path);
// Writes pretty JSON (2-space indent, trailing newline), atomically.
bool write_json(const std::string &path, const nlohmann::json &j);

std::string base64(const uint8_t *data, size_t size);
std::vector<uint8_t> base64_decode(const std::string &text);

// The current time as ISO 8601 UTC, e.g. 2026-10-10T19:30:00Z.
std::string now_iso();

// A name safe to use as a directory: lowercase letters, digits, and underscores.
bool valid_name(const std::string &name);

// Runs a command (argv, no shell), capturing stdout and stderr together.
// Optional stdin bytes are written to the child. Returns the exit status, or -1.
int run_process(const std::vector<std::string> &argv, std::string &output,
                const std::vector<uint8_t> *input = nullptr, std::vector<uint8_t> *stdout_bytes = nullptr);

}  // namespace core
