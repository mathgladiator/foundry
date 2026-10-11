#include "core/util.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>

extern char **environ;

namespace core {

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool read_text(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const std::string &path, const void *data, size_t size) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool write_text(const std::string &path, const std::string &text) {
  return write_file(path, text.data(), text.size());
}

nlohmann::json read_json(const std::string &path) {
  std::string text;
  if (!read_text(path, text)) return nullptr;
  try {
    return nlohmann::json::parse(text);
  } catch (const std::exception &) {
    return nullptr;
  }
}

bool write_json(const std::string &path, const nlohmann::json &j) {
  return write_text(path, j.dump(2) + "\n");
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t *data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < size) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < size) v |= data[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(i + 1 < size ? kB64[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? kB64[v & 63] : '=');
  }
  return out;
}

std::vector<uint8_t> base64_decode(const std::string &text) {
  std::vector<uint8_t> out;
  uint32_t v = 0;
  int bits = 0;
  for (char c : text) {
    const char *p = c ? std::strchr(kB64, c) : nullptr;
    if (!p) continue;  // skips padding and whitespace
    v = (v << 6) | static_cast<uint32_t>(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((v >> bits) & 255));
    }
  }
  return out;
}

std::string now_iso() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}

bool valid_name(const std::string &name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return name[0] >= 'a' && name[0] <= 'z';
}

int run_process(const std::vector<std::string> &argv, std::string &output,
                const std::vector<uint8_t> *input, std::vector<uint8_t> *stdout_bytes) {
  int in_pipe[2], out_pipe[2], err_pipe[2];
  if (pipe2(in_pipe, O_CLOEXEC) || pipe2(out_pipe, O_CLOEXEC) || pipe2(err_pipe, O_CLOEXEC)) return -1;

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
  // Without a separate stdout buffer, stderr goes to the same pipe as stdout.
  posix_spawn_file_actions_adddup2(&actions, stdout_bytes ? err_pipe[1] : out_pipe[1], 2);

  std::vector<char *> args;
  for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (rc != 0) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);
    output = "failed to start " + argv[0] + ": " + std::strerror(rc);
    return -1;
  }

  // Feed stdin and drain stdout/stderr together, so neither side blocks.
  size_t written = 0;
  const size_t to_write = input ? input->size() : 0;
  if (to_write == 0) {
    close(in_pipe[1]);
    in_pipe[1] = -1;
  } else {
    fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
  }
  std::vector<uint8_t> out_bytes;
  bool out_open = true, err_open = true;
  char buf[65536];
  while (out_open || err_open || in_pipe[1] >= 0) {
    pollfd fds[3];
    int n = 0, out_i = -1, err_i = -1, in_i = -1;
    if (out_open) { fds[n] = {out_pipe[0], POLLIN, 0}; out_i = n++; }
    if (err_open) { fds[n] = {err_pipe[0], POLLIN, 0}; err_i = n++; }
    if (in_pipe[1] >= 0) { fds[n] = {in_pipe[1], POLLOUT, 0}; in_i = n++; }
    if (poll(fds, static_cast<nfds_t>(n), -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (out_i >= 0 && fds[out_i].revents) {
      ssize_t r = read(out_pipe[0], buf, sizeof buf);
      if (r > 0) out_bytes.insert(out_bytes.end(), buf, buf + r);
      else out_open = false;
    }
    if (err_i >= 0 && fds[err_i].revents) {
      ssize_t r = read(err_pipe[0], buf, sizeof buf);
      if (r > 0) output.append(buf, static_cast<size_t>(r));
      else err_open = false;
    }
    if (in_i >= 0 && fds[in_i].revents) {
      ssize_t w = (fds[in_i].revents & POLLOUT)
                      ? write(in_pipe[1], input->data() + written, to_write - written)
                      : -1;
      if (w > 0) written += static_cast<size_t>(w);
      if (w <= 0 || written >= to_write) {
        close(in_pipe[1]);
        in_pipe[1] = -1;
      }
    }
  }
  close(out_pipe[0]);
  close(err_pipe[0]);
  if (in_pipe[1] >= 0) close(in_pipe[1]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (stdout_bytes) {
    *stdout_bytes = std::move(out_bytes);
  } else {
    output.insert(output.begin(), out_bytes.begin(), out_bytes.end());
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace core
