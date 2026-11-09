#pragma once

#include "sftp.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/file.h>
#include <utility>

class SftpTransfer
{
public:
  enum class Choice
  {
    Resume,
    Restart,
    Skip,
    Cancel
  };
  enum class Result
  {
    Completed,
    Skipped
  };

  struct Progress
  {
    uint64_t done, total;
    bool verifying;
    std::string path;
  };

  std::function<bool(const Progress &)> progress;
  std::function<Choice(const std::string &, bool)> choose;
  std::function<bool(const std::string &, const Sftp::Attr &)> confirmOverwrite;

  SftpTransfer(Sftp &sftp, std::string endpoint) : sftp(sftp), endpoint(std::move(endpoint)) {}

  static Sftp::Attr getLocalAttr(const std::string &path)
  {
    struct stat st{};

    if (lstat(path.c_str(), &st))
    {
      fail(path);
    }

    return localAttr(st);
  }

  static void applyLocalAttrs(const std::string &path, const Sftp::Attr &a)
  {
    requireMetadata(a);
    timespec times[2]{{static_cast<time_t>(a.atime), 0}, {static_cast<time_t>(a.mtime), 0}};

    if (chmod(path.c_str(), a.mode & 0777) || utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW))
    {
      fail(path);
    }
  }

  void applyRemoteAttrs(const std::string &path, const Sftp::Attr &a)
  {
    requireMetadata(a);
    sftp.setAttrs(path, a);
  }

  Result upload(const std::string &local, const std::string &remote)
  {
    return transfer(local, remote, true, nullptr);
  }

  Result download(const std::string &remote, const std::string &local)
  {
    return transfer(local, remote, false, nullptr);
  }

  Result uploadEdited(const std::string &local, const std::string &remote, const Sftp::Attr &baseline)
  {
    return transfer(local, remote, true, &baseline);
  }

private:
  Sftp &sftp;
  std::string endpoint;
  Progress current{0, 0, false, {}};

  struct ProgressGuard
  {
    Sftp &sftp;
    std::function<bool(uint64_t)> previous;

    ~ProgressGuard()
    {
      sftp.progress = std::move(previous);
    }
  };

  struct LocalFile
  {
    int fd = -1;

    ~LocalFile()
    {
      if (fd >= 0)
      {
        close(fd);
      }
    }
  };

  struct RemoteFile
  {
    Sftp &sftp;
    std::string handle;

    ~RemoteFile()
    {
      if (!handle.empty())
      {
        sftp.quietlyClose(handle);
      }
    }

    void finish()
    {
      sftp.closeHandle(handle);
      handle.clear();
    }
  };

  static void fail(const std::string &path)
  {
    throw Sftp::Error(0, path + ": " + strerror(errno));
  }

  static Sftp::Attr localAttr(const struct stat &st)
  {
    if (st.st_mtime < 0 || st.st_atime < 0 ||
        static_cast<uint64_t>(st.st_mtime) > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(st.st_atime) > std::numeric_limits<uint32_t>::max())
    {
      throw Sftp::Error(0, "Timestamp is outside the SFTP v3 range");
    }

    Sftp::Attr a;
    a.flags = 15;
    a.size = st.st_size;
    a.mode = st.st_mode;
    a.uid = st.st_uid;
    a.gid = st.st_gid;
    a.atime = st.st_atime;
    a.mtime = st.st_mtime;
    return a;
  }

  static void requireMetadata(const Sftp::Attr &a)
  {
    if ((a.flags & 12) != 12)
    {
      throw Sftp::Error(0, "Missing source permissions or timestamps");
    }
  }

  static void requireFile(const Sftp::Attr &a)
  {
    requireMetadata(a);

    if (!(a.flags & 1) || !S_ISREG(a.mode))
    {
      throw Sftp::Error(0, "Only regular files with size and timestamps can be transferred");
    }
  }

  static bool same(const Sftp::Attr &a, const Sftp::Attr &b)
  {
    return (a.flags & 13) == (b.flags & 13) && a.size == b.size && a.mtime == b.mtime && a.mode == b.mode;
  }

  static std::string hash(const std::string &s)
  {
    uint64_t h = 14695981039346656037ull;

    for (unsigned char c : s)
    {
      h = (h ^ c) * 1099511628211ull;
    }

    std::ostringstream out;
    out << std::hex << h;
    return out.str();
  }

  static std::string client()
  {
    std::ifstream in("/etc/machine-id");
    std::string id;
    std::getline(in, id);

    if (id.empty())
    {
      char host[256]{};

      if (gethostname(host, sizeof(host) - 1))
      {
        fail("gethostname");
      }

      id = host;
    }

    return id + ":" + std::to_string(getuid());
  }

  bool exists(const std::string &path, bool remote, Sftp::Attr &a)
  {
    if (remote)
    {
      try
      {
        a = sftp.stat(path, false);
        return true;
      }

      catch (const Sftp::Error &e)
      {
        if (e.code == 2)
        {
          return false;
        }

        throw;
      }
    }

    struct stat st{};

    if (!lstat(path.c_str(), &st))
    {
      a = localAttr(st);
      return true;
    }

    if (errno == ENOENT)
    {
      return false;
    }

    fail(path);
    return false;
  }

  void tick(uint64_t done, uint64_t total, bool verifying, const std::string &path)
  {
    current = {done, total, verifying, path};

    if (progress && !progress(current))
    {
      throw Sftp::Error(0, "Cancelled");
    }
  }

  Choice decision(const std::string &path, bool resume)
  {
    auto c = choose ? choose(path, resume) : Choice::Cancel;

    if (c == Choice::Cancel)
    {
      throw Sftp::Error(0, "Cancelled");
    }

    if (c == Choice::Resume && !resume)
    {
      throw Sftp::Error(0, "Partial copy cannot be resumed");
    }

    return c;
  }

  static std::string readLocal(int fd, uint64_t offset, size_t size)
  {
    std::string data(size, '\0');
    size_t done = 0;

    while (done < size)
    {
      auto n = pread(fd, &data[done], size - done, offset + done);

      if (n < 0 && errno == EINTR)
      {
        continue;
      }

      if (n < 0)
      {
        fail("read");
      }

      if (!n)
      {
        break;
      }

      done += n;
    }

    data.resize(done);
    return data;
  }

  static void writeLocal(int fd, uint64_t offset, const std::string &data)
  {
    size_t done = 0;

    while (done < data.size())
    {
      auto n = pwrite(fd, data.data() + done, data.size() - done, offset + done);

      if (n < 0 && errno == EINTR)
      {
        continue;
      }

      if (n <= 0)
      {
        fail("write");
      }

      done += n;
    }
  }

  std::string readRemote(const std::string &handle, uint64_t offset, uint32_t length)
  {
    std::string data;

    while (data.size() < length)
    {
      auto chunk = sftp.readHandle(handle, offset + data.size(), length - data.size());

      if (chunk.empty())
      {
        break;
      }

      data += chunk;
    }

    return data;
  }

  std::string readSmall(const std::string &path, bool remote)
  {
    Sftp::Attr a;

    if (!exists(path, remote, a) || !S_ISREG(a.mode) || a.size > 65536)
    {
      throw Sftp::Error(0, "Unrecognized transfer state: " + path);
    }

    if (remote)
    {
      RemoteFile f{sftp, sftp.openFile(path, 1)};
      std::string data;

      while (data.size() < a.size)
      {
        auto chunk = sftp.readHandle(f.handle, data.size(), std::min<uint64_t>(32768, a.size - data.size()));

        if (chunk.empty())
        {
          throw Sftp::Error(0, "Truncated transfer state");
        }

        data += chunk;
      }

      return data;
    }

    LocalFile f;
    f.fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);

    if (f.fd < 0)
    {
      fail(path);
    }

    return readLocal(f.fd, 0, a.size);
  }

  void writeSmall(const std::string &path, bool remote, const std::string &data)
  {
    if (remote)
    {
      RemoteFile f{sftp, sftp.openFile(path, 2 | 8 | 32)};
      sftp.writeHandle(f.handle, 0, data);
      f.finish();
    }

    else
    {
      LocalFile f;
      f.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);

      if (f.fd < 0)
      {
        fail(path);
      }

      writeLocal(f.fd, 0, data);
    }
  }

  void erase(const std::string &path, bool remote)
  {
    if (remote)
    {
      sftp.remove(path);
    }

    else if (unlink(path.c_str()))
    {
      fail(path);
    }
  }

  Result transfer(std::string local, std::string remote, bool up, const Sftp::Attr *baseline)
  {
    ProgressGuard guard{sftp, sftp.progress};
    current = {0, 0, false, up ? remote : local};
    sftp.progress = [&](uint64_t)
    {
      return !progress || progress(current);
    };
    // определяем РЕАЛЬНЫЙ путь до директории
    // файлы проверяем отдельно чтобы файл не был симлинком
    auto canonicalLocal = [](const std::string &path)
    {
      auto slash = path.find_last_of('/');
      std::string parent = slash == std::string::npos ? "." : path.substr(0, slash + 1);
      char *resolved = ::realpath(parent.c_str(), nullptr);

      if (!resolved)
      {
        fail(parent);
      }

      std::string out(resolved);
      free(resolved);
      return out + (out == "/" ? "" : "/") + path.substr(slash == std::string::npos ? 0 : slash + 1);
    };
    local = canonicalLocal(local);
    auto slash = remote.find_last_of('/');
    auto remoteParent = sftp.realpath(slash == std::string::npos ? "." : remote.substr(0, slash + 1));
    remote =
        remoteParent + (remoteParent == "/" ? "" : "/") + remote.substr(slash == std::string::npos ? 0 : slash + 1);
    std::string dest = up ? remote : local;
    std::string identity = "scpanel-transfer-v1\n" + std::to_string(endpoint.size()) + ":" + endpoint +
                           std::to_string(local.size()) + ":" + local + std::to_string(remote.size()) + ":" + remote +
                           (up ? "upload" : "download");
    std::string key = hash(endpoint + "\n" + dest + (up ? client() : "local"));
    std::string dir = dest.substr(0, dest.find_last_of('/') + 1) + ".scpanel-transfer-" + key;
    LocalFile lock;
    std::string lockPath = "/tmp/scpanel-transfer-" + std::to_string(getuid()) + "-" + key + ".lock";
    lock.fd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    struct stat lockStat{};

    if (lock.fd < 0 || fstat(lock.fd, &lockStat) || !S_ISREG(lockStat.st_mode) || lockStat.st_uid != getuid())
    {
      throw Sftp::Error(0, "Unsafe transfer lock");
    }

    if (flock(lock.fd, LOCK_EX | LOCK_NB))
    {
      throw Sftp::Error(0, "Transfer already active");
    }

    Sftp::Attr source = up ? getLocalAttr(local) : sftp.stat(remote, false);
    requireFile(source);
    current.total = source.size;
    Sftp::Attr before;
    bool hadDest = exists(dest, up, before);

    if (hadDest)
    {
      requireFile(before);
    }

    if (baseline && (!hadDest || !same(*baseline, before)))
    {
      throw Sftp::Error(0, "Remote file changed since editing began; local edits retained");
    }

    if (hadDest && !baseline && confirmOverwrite && !confirmOverwrite(dest, before))
    {
      return Result::Skipped;
    }

    std::string manifest = identity + "\n" + std::to_string(source.size) + ":" + std::to_string(source.mtime) + ":" +
                           std::to_string(source.mode) + "\n";
    Sftp::Attr state;
    bool existing = exists(dir, up, state);
    bool restart = false;

    if (existing)
    {
      if (!S_ISDIR(state.mode) || (state.mode & 0777) != 0700 || (!up && state.uid != getuid()))
      {
        throw Sftp::Error(0, "Foreign transfer directory: " + dir);
      }

      if (readSmall(dir + "/owner", up) != identity)
      {
        throw Sftp::Error(0, "Foreign transfer directory: " + dir);
      }

      Sftp::Attr manifestAttr;
      bool hasManifest = exists(dir + "/manifest", up, manifestAttr);
      auto old = hasManifest ? readSmall(dir + "/manifest", up) : std::string{};
      bool hasPart = exists(dir + "/data", up, state);

      if (hasPart)
      {
        requireFile(state);
      }

      auto c = decision(dest, old == manifest && hasPart && state.size <= source.size);

      if (c == Choice::Skip)
      {
        return Result::Skipped;
      }

      restart = c == Choice::Restart;

      if (restart)
      {
        if (hasPart)
        {
          erase(dir + "/data", up);
        }

        if (hasManifest)
        {
          erase(dir + "/manifest", up);
        }

        writeSmall(dir + "/manifest", up, manifest);
      }
    }

    else
    {
      if (up)
      {
        sftp.mkdir(dir);
      }

      else if (::mkdir(dir.c_str(), 0700))
      {
        fail(dir);
      }

      writeSmall(dir + "/owner", up, identity);
      writeSmall(dir + "/manifest", up, manifest);
    }

    // временный файл уже мог получить права исходного файла если возникла ошибка передачи
    if (existing && !restart)
    {
      Sftp::Attr partialAttr;

      if (!exists(dir + "/data", up, partialAttr))
      {
        throw Sftp::Error(0, "Partial copy disappeared");
      }

      requireFile(partialAttr);
      partialAttr.mode = 0600;

      if (up)
      {
        applyRemoteAttrs(dir + "/data", partialAttr);
      }

      else
      {
        applyLocalAttrs(dir + "/data", partialAttr);
      }
    }

    LocalFile lf;
    RemoteFile rf{sftp, {}};
    std::string part = dir + "/data";
    lf.fd = open((up ? local : part).c_str(),
                 up ? O_RDONLY | O_NOFOLLOW | O_NONBLOCK
                    : O_RDWR | O_NOFOLLOW | O_NONBLOCK | ((!existing || restart) ? O_CREAT | O_EXCL : 0),
                 0600);

    if (lf.fd < 0)
    {
      fail(up ? local : part);
    }

    struct stat st{};

    if (fstat(lf.fd, &st) || !S_ISREG(st.st_mode))
    {
      throw Sftp::Error(0, "Unsafe local transfer file");
    }

    rf.handle = sftp.openFile(up ? part : remote, up ? 3 | ((!existing || restart) ? 8 | 32 : 0) : 1);
    auto remoteOpened = sftp.statHandle(rf.handle);
    requireFile(remoteOpened);

    if (!same(source, up ? localAttr(st) : remoteOpened))
    {
      throw Sftp::Error(0, "Source changed before transfer");
    }

    uint64_t offset = up ? remoteOpened.size : static_cast<uint64_t>(st.st_size);
    auto sourceRead = [&](uint64_t pos, uint32_t len)
    {
      return up ? readLocal(lf.fd, pos, len) : readRemote(rf.handle, pos, len);
    };

    if (offset > source.size)
    {
      throw Sftp::Error(0, "Partial copy exceeds source size");
    }

    bool mismatch = false;

    for (uint64_t pos = 0; pos < offset;)
    {
      tick(pos, source.size, true, dest);
      uint32_t length = std::min<uint64_t>(32768, offset - pos);
      auto a = sourceRead(pos, length);
      auto b = up ? readRemote(rf.handle, pos, length) : readLocal(lf.fd, pos, length);

      if (a.empty() || a != b)
      {
        mismatch = true;
        break;
      }

      pos += a.size();
    }

    if (mismatch)
    {
      auto c = decision(dest, false);

      if (c == Choice::Skip)
      {
        return Result::Skipped;
      }

      rf.finish();

      if (up)
      {
        erase(part, true);
        rf.handle = sftp.openFile(part, 3 | 8 | 32);
      }

      else
      {
        if (ftruncate(lf.fd, 0))
        {
          fail(part);
        }

        rf.handle = sftp.openFile(remote, 1);
      }

      offset = 0;
    }

    while (offset < source.size)
    {
      tick(offset, source.size, false, dest);
      auto data = sourceRead(offset, std::min<uint64_t>(32768, source.size - offset));

      if (data.empty())
      {
        throw Sftp::Error(0, "Source shortened during transfer");
      }

      if (up)
      {
        sftp.writeHandle(rf.handle, offset, data);
      }

      else
      {
        writeLocal(lf.fd, offset, data);
      }

      offset += data.size();
    }

    tick(offset, source.size, false, dest);

    if (fstat(lf.fd, &st))
    {
      fail(local);
    }

    auto sourceAfter = up ? getLocalAttr(local) : sftp.stat(remote, false);

    if (!same(source, sourceAfter) || !same(source, up ? localAttr(st) : sftp.statHandle(rf.handle)))
    {
      throw Sftp::Error(0, "Source changed during transfer; prepared copy retained");
    }

    rf.finish();
    int fd = lf.fd;
    lf.fd = -1;

    if (close(fd))
    {
      fail(local);
    }

    auto attrs = source;

    if (baseline)
    {
      attrs.mode = baseline->mode;
    }

    if (up)
    {
      applyRemoteAttrs(part, attrs);
    }

    else
    {
      applyLocalAttrs(part, attrs);
    }

    Sftp::Attr current;
    bool hasDest = exists(dest, up, current);

    if (hasDest != hadDest || (hasDest && !same(before, current)))
    {
      throw Sftp::Error(0, "Destination changed during transfer; prepared copy retained");
    }

    if (up)
    {
      if (hadDest)
      {
        sftp.atomicRename(part, dest);
      }

      else
      {
        sftp.rename(part, dest);
      }
    }

    else if (hadDest)
    {
      if (::rename(part.c_str(), dest.c_str()))
      {
        fail(dest);
      }
    }

    else
    {
      // link не заменяет внезапно появившееся назначение
      if (::link(part.c_str(), dest.c_str()))
      {
        fail(dest);
      }

      erase(part, false);
    }

    // ошибка удаления != ошибка передачи
    try
    {
      erase(dir + "/manifest", up);
      erase(dir + "/owner", up);

      if (up)
      {
        sftp.rmdir(dir);
      }

      else
      {
        ::rmdir(dir.c_str());
      }
    }

    catch (...)
    {
    }

    return Result::Completed;
  }
};
