#pragma once

// SFTP v3 через OpenSSH
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

class Sftp
{
public:
  struct Error : std::runtime_error
  {
    uint32_t code;

    Error(uint32_t c, const std::string &s) : std::runtime_error(s), code(c) {}
  };

  struct Attr
  {
    std::string name;
    uint64_t size = 0;
    uint32_t mode = 0, uid = 0, gid = 0, mtime = 0, atime = 0, flags = 0;
  };

  // вызываем если ждем ввод-вывод и передачу данных
  // false = отменяет операции + закрытие канала
  std::function<bool(uint64_t)> progress;
  Sftp() = default;
  Sftp(const Sftp &) = delete;
  Sftp &operator=(const Sftp &) = delete;

  ~Sftp()
  {
    stop();
  }

  bool connected() const
  {
    return fd >= 0;
  }

  void stop()
  {
    if (fd >= 0)
    {
      close(fd);
      fd = -1;
    }

    if (pid > 0)
    {
      kill(pid, SIGTERM);
      int status;

      for (int i = 0; i < 20; ++i)
      {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid || (r < 0 && errno == ECHILD))
        {
          pid = -1;
          return;
        }

        poll(nullptr, 0, 10);
      }

      kill(pid, SIGKILL);

      while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      {
      }

      pid = -1;
    }
  }

  void start(const std::vector<std::string> &args)
  {
    stop();
    done = 0;
    atomicRenameAvailable = false;

    if (args.empty())
    {
      throw Error(0, "Empty SFTP command");
    }

    int pair[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
    {
      throw Error(0, strerror(errno));
    }

    pid = fork();

    if (pid < 0)
    {
      close(pair[0]);
      close(pair[1]);
      throw Error(0, strerror(errno));
    }

    if (pid == 0)
    {
      close(pair[0]);
      dup2(pair[1], 0);
      dup2(pair[1], 1);
      close(pair[1]);
      int null = open("/dev/null", O_WRONLY);

      if (null >= 0)
      {
        dup2(null, 2);
        close(null);
      }

      std::vector<char *> av;

      for (auto &a : args)
      {
        av.push_back(const_cast<char *>(a.c_str()));
      }

      av.push_back(nullptr);
      execvp(av[0], av.data());
      _exit(127);
    }

    close(pair[1]);
    fd = pair[0];
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    try
    {
      sendPacket(std::string(1, 1) + u32(3));
      auto p = receive();

      if (p.byte() != 2 || p.number() != 3)
      {
        throw Error(0, "Server does not support SFTP v3");
      }

      while (p.pos < p.data.size())
      {
        auto name = p.string();
        auto version = p.string();

        if (name == "posix-rename@openssh.com" && version == "1")
        {
          atomicRenameAvailable = true;
        }
      }
    }

    catch (...)
    {
      stop();
      throw;
    }
  }

  std::string realpath(const std::string &path)
  {
    auto p = request(16, str(path.empty() ? "." : path), 104);

    if (p.number() != 1)
    {
      throw Error(0, "Invalid SFTP realpath response");
    }

    return p.string();
  }

  Attr stat(const std::string &path, bool follow = true)
  {
    auto p = request(follow ? 17 : 7, str(path), 105);
    return attributes(p);
  }

  std::vector<Attr> list(const std::string &path)
  {
    auto p = request(11, str(path), 102);
    auto handle = p.string();
    std::vector<Attr> out;

    try
    {
      for (;;)
      {
        try
        {
          p = request(12, str(handle), 104);
        }

        catch (const Error &e)
        {
          if (e.code == 1)
          {
            break;
          }

          throw;
        }

        uint32_t n = p.number();

        if (n > 100000)
        {
          throw Error(0, "Invalid SFTP directory count");
        }

        for (uint32_t i = 0; i < n; ++i)
        {
          auto name = p.string();
          p.string();
          auto a = attributes(p);

          if (name == "." || name == "..")
          {
            continue;
          }

          if (name.empty() || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
          {
            throw Error(0, "Unsafe SFTP entry name");
          }

          a.name = name;
          out.push_back(a);
        }
      }

      closeHandle(handle);
    }

    catch (...)
    {
      quietlyClose(handle);
      throw;
    }

    return out;
  }

  void mkdir(const std::string &path, uint32_t mode = 0700)
  {
    request(14, str(path) + u32(4) + u32(mode), 101);
  }

  void rename(const std::string &from, const std::string &to)
  {
    request(18, str(from) + str(to), 101);
  }

  void create(const std::string &path)
  {
    closeHandle(openFile(path, 2 | 8 | 32));
  }

  void removeTree(const std::string &path, unsigned depth = 0)
  {
    if (depth > 128)
    {
      throw Error(0, "Directory nesting exceeds limit");
    }

    auto a = stat(path, false);

    if (S_ISDIR(a.mode))
    {
      for (auto &e : list(path))
      {
        removeTree(path + "/" + e.name, depth + 1);
      }

      request(15, str(path), 101);
    }

    else
    {
      request(13, str(path), 101);
    }
  }

  void resetProgress()
  {
    done = 0;
  }

  void upload(const std::string &local, const std::string &remote)
  {
    copy(local, remote, true);
  }

  void download(const std::string &remote, const std::string &local)
  {
    copy(local, remote, false);
  }

  bool supportsAtomicRename() const
  {
    return atomicRenameAvailable;
  }

  void atomicRename(const std::string &from, const std::string &to)
  {
    if (!atomicRenameAvailable)
    {
      throw Error(0, "Server does not support atomic replacement; prepared copy retained");
    }

    request(200, str("posix-rename@openssh.com") + str(from) + str(to), 101);
  }

  void remove(const std::string &path)
  {
    request(13, str(path), 101);
  }

  void rmdir(const std::string &path)
  {
    request(15, str(path), 101);
  }

  Attr statHandle(const std::string &handle)
  {
    auto p = request(8, str(handle), 105);
    return attributes(p);
  }

  void setAttrs(const std::string &path, const Attr &a)
  {
    if ((a.flags & 12) != 12)
    {
      throw Error(0, "Server omitted permissions or timestamps");
    }

    request(9, str(path) + u32(12) + u32(a.mode & 0777) + u32(a.atime) + u32(a.mtime), 101);
  }

  std::string readHandle(const std::string &h, uint64_t offset, uint32_t length = 32768)
  {
    try
    {
      auto p = request(5, str(h) + u64(offset) + u32(length), 103);
      auto data = p.string();

      if (data.empty() || data.size() > length)
      {
        throw Error(0, "Invalid SFTP read size");
      }

      return data;
    }

    catch (const Error &e)
    {
      if (e.code == 1)
      {
        return {};
      }

      throw;
    }
  }

  void writeHandle(const std::string &h, uint64_t offset, const std::string &data)
  {
    request(6, str(h) + u64(offset) + str(data), 101);
  }

private:
  struct Packet
  {
    std::string data;
    size_t pos = 0;

    unsigned byte()
    {
      require(1);
      return (unsigned char)data[pos++];
    }

    void require(size_t n)
    {
      if (n > data.size() - pos)
      {
        throw Error(0, "Truncated SFTP packet");
      }
    }

    uint32_t number()
    {
      uint32_t n = 0;

      for (int i = 0; i < 4; ++i)
      {
        n = (n << 8) | byte();
      }

      return n;
    }

    uint64_t wide()
    {
      uint64_t hi = number();
      return (hi << 32) | number();
    }

    std::string string()
    {
      auto n = number();
      require(n);
      auto s = data.substr(pos, n);
      pos += n;
      return s;
    }
  };

  int fd = -1;
  pid_t pid = -1;
  uint32_t id = 0;
  uint64_t done = 0;
  bool atomicRenameAvailable = false;

  static std::string u32(uint32_t n)
  {
    std::string s;

    for (int i = 24; i >= 0; i -= 8)
    {
      s += char(n >> i);
    }

    return s;
  }

  static std::string u64(uint64_t n)
  {
    return u32(n >> 32) + u32(n);
  }

  static std::string str(const std::string &s)
  {
    return u32(s.size()) + s;
  }

  static Attr attributes(Packet &p)
  {
    Attr a;
    auto flags = p.number();
    a.flags = flags;

    if (flags & ~(1u | 2u | 4u | 8u | 0x80000000u))
    {
      throw Error(0, "Unsupported SFTP attributes");
    }

    if (flags & 1)
    {
      a.size = p.wide();
    }

    if (flags & 2)
    {
      a.uid = p.number();
      a.gid = p.number();
    }

    if (flags & 4)
    {
      a.mode = p.number();
    }

    if (flags & 8)
    {
      a.atime = p.number();
      a.mtime = p.number();
    }

    if (flags & 0x80000000u)
    {
      auto n = p.number();

      if (n > 100000)
      {
        throw Error(0, "Invalid SFTP extensions");
      }

      while (n--)
      {
        p.string();
        p.string();
      }
    }

    return a;
  }

  void io(char *p, size_t n, bool writing)
  {
    int idle = 0;

    while (n)
    {
      if (progress && !progress(done))
      {
        stop();
        throw Error(0, "Cancelled");
      }

      if (fd < 0)
      {
        throw Error(0, "SFTP disconnected");
      }

      pollfd f{fd, short(writing ? POLLOUT : POLLIN), 0};
      int r = poll(&f, 1, 100);

      if (r < 0 && errno == EINTR)
      {
        continue;
      }

      if (r < 0 || ++idle > 300)
      {
        stop();
        throw Error(0, "SFTP I/O failed or timed out");
      }

      if (!r)
      {
        continue;
      }

      ssize_t k = writing ? send(fd, p, n, MSG_NOSIGNAL) : read(fd, p, n);

      if (k < 0 && (errno == EINTR || errno == EAGAIN))
      {
        continue;
      }

      if (k <= 0)
      {
        stop();
        throw Error(0, "SFTP connection closed");
      }

      p += k;
      n -= k;
      idle = 0;
    }
  }

  void sendPacket(const std::string &body)
  {
    std::string data = u32(body.size()) + body;
    io(&data[0], data.size(), true);
  }

  Packet receive()
  {
    Packet h;
    h.data.resize(4);
    io(&h.data[0], 4, false);
    auto n = h.number();

    if (n < 1 || n > 4 * 1024 * 1024)
    {
      stop();
      throw Error(0, "Invalid SFTP packet size");
    }

    Packet p;
    p.data.resize(n);
    io(&p.data[0], n, false);
    return p;
  }

  Packet request(unsigned type, const std::string &body, unsigned expected)
  {
    uint32_t seq = ++id;
    sendPacket(std::string(1, char(type)) + u32(seq) + body);
    auto p = receive();
    auto t = p.byte();

    if (p.number() != seq)
    {
      stop();
      throw Error(0, "SFTP response ID mismatch");
    }

    if (t == 101)
    {
      auto code = p.number();
      auto message = p.string();

      if (code)
      {
        throw Error(code, message.empty() ? "SFTP operation failed" : message);
      }
    }

    if (t != expected)
    {
      throw Error(0, "Unexpected SFTP response");
    }

    return p;
  }

public:
  std::string openFile(const std::string &path, uint32_t flags)
  {
    auto p = request(3, str(path) + u32(flags) + u32(0), 102);
    return p.string();
  }

  void closeHandle(const std::string &h)
  {
    request(4, str(h), 101);
  }

  void quietlyClose(const std::string &h)
  {
    if (fd >= 0)
    {
      try
      {
        closeHandle(h);
      }

      catch (...)
      {
      }
    }
  }

private:
  void copy(const std::string &local, const std::string &remote, bool upload)
  {
    // cначала открываем источник
    // локальный файл не должен обнуляться при ошибке удаленного OPEN
    int f = -1;
    std::string h;

    try
    {
      if (!upload)
      {
        h = openFile(remote, 1);
      }

      f = open(local.c_str(),
               upload ? O_RDONLY | O_NOFOLLOW | O_NONBLOCK : O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);

      if (f < 0)
      {
        throw Error(0, local + ": " + strerror(errno));
      }

      struct stat st{};

      if (fstat(f, &st) != 0 || !S_ISREG(st.st_mode))
      {
        throw Error(0, "Only regular files can be transferred");
      }

      if (upload)
      {
        h = openFile(remote, 2 | 8 | 16);
      }

      else if (ftruncate(f, 0) != 0)
      {
        throw Error(0, strerror(errno));
      }

      uint64_t offset = 0;

      for (;;)
      {
        std::string data;

        if (upload)
        {
          char b[32768];
          ssize_t n;
          do
          {
            n = read(f, b, sizeof b);
          } while (n < 0 && errno == EINTR);

          if (n < 0)
          {
            throw Error(0, strerror(errno));
          }

          if (!n)
          {
            break;
          }

          data.assign(b, n);
          request(6, str(h) + u64(offset) + str(data), 101);
        }

        else
        {
          try
          {
            auto p = request(5, str(h) + u64(offset) + u32(32768), 103);
            data = p.string();
          }

          catch (const Error &e)
          {
            if (e.code == 1)
            {
              break;
            }

            throw;
          }

          if (data.empty() || data.size() > 32768)
          {
            throw Error(0, "Invalid SFTP read size");
          }

          size_t pos = 0;

          while (pos < data.size())
          {
            ssize_t n = write(f, data.data() + pos, data.size() - pos);

            if (n < 0 && errno == EINTR)
            {
              continue;
            }

            if (n <= 0)
            {
              throw Error(0, strerror(errno));
            }

            pos += n;
          }
        }

        offset += data.size();
        done += data.size();
      }

      closeHandle(h);
      h.clear();

      if (close(f) != 0)
      {
        f = -1;
        throw Error(0, strerror(errno));
      }
    }

    catch (...)
    {
      if (f >= 0)
      {
        close(f);
      }

      if (!h.empty())
      {
        quietlyClose(h);
      }

      throw;
    }
  }
};
