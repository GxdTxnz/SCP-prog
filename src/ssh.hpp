#pragma once

#include "config.hpp"
#include "filesystem.hpp"
#include "process.hpp"

// OpenSSH и удаленные файловые операции
// + SFTP без запуска команд оболочки
class Ssh
{
public:
  explicit Ssh(const Target &target) : tgt(target)
  {
    std::string id = std::to_string(getpid());
    const char *home = getenv("HOME");
    ctlOpt = "ControlPath=~/.ssh/scpanel-" + id + ".sock";
    ctlFile = std::string(home ? home : "") + "/.ssh/scpanel-" + id + ".sock";
  }

  Ssh(const Ssh &) = delete;
  Ssh &operator=(const Ssh &) = delete;

  // статус подключения (жив или нет)
  // если нет - подключаемся заново
  bool ensureConnection()
  {
    if (runCapture({"ssh", "-o", ctlOpt, "-O", "check", "--", tgt.ssh_host}, nullptr) == 0)
    {
      return true;
    }

    unlink(ctlFile.c_str());
    return runInTerminal({"ssh", "-o", ctlOpt, "-o", "ControlMaster=yes", "-o", "ControlPersist=30m", "-N", "-f", "--",
                          tgt.ssh_host},
                         "Connecting to " + tgt.ssh_host + "...") == 0;
  }

  void closeConnection()
  {
    if (closed)
    {
      return;
    }

    closed = true;
    runCapture({"ssh", "-o", ctlOpt, "-O", "exit", "--", tgt.ssh_host}, nullptr);
    unlink(ctlFile.c_str());
  }

  std::vector<std::string> sshCmd(const std::string &script)
  {
    return {"ssh",
            "-o",
            ctlOpt,
            "-o",
            "ControlMaster=no",
            "-o",
            "BatchMode=yes",
            "--",
            tgt.ssh_host,
            "sh -c " + shq(script)};
  }

  int remoteSh(const std::string &script, std::string &out)
  {
    if (!ensureConnection())
    {
      return 255;
    }

    out.clear();
    return runCapture(sshCmd(script), &out);
  }

  // вес директорий; -1 = хз
  long long remoteDu(const std::vector<std::string> &paths)
  {
    std::string script = "du -sbc --";

    for (const auto &p : paths)
    {
      script += " " + shq(p);
    }

    script += " 2>/dev/null | tail -n 1";
    std::string out;

    if (remoteSh(script, out) == 0 && !out.empty() && isdigit((unsigned char)out[0]))
    {
      return atoll(out.c_str());
    }

    return -1;
  }

  bool readRemoteDir(const std::string &path, std::vector<Entry> &out, std::string &real, std::string &err)
  {
    std::string cdTo = (!path.empty() && path[0] == '-') ? "./" + path : path;
    std::string script =
        (path.empty() ? std::string("cd") : "cd " + shq(cdTo)) +
        " 2>&1 || exit 3\n"
        "pwd\n"
        "find . -mindepth 1 -maxdepth 1 -printf '%y\\t%Y\\t%m\\t%u\\t%g\\t%s\\t%T@\\t%f\\0' 2>/dev/null || exit 4\n";
    std::string data;
    int rc = remoteSh(script, data);

    if (rc == 3)
    {
      err = "Server: " + trim(data);
      return false;
    }

    if (rc == 4)
    {
      err = "Cannot read directory on the server (no permission or find without -printf)";
      return false;
    }

    if (rc == 255)
    {
      err = "No connection to the server";
      return false;
    }

    if (rc != 0)
    {
      err = "Server error, code " + std::to_string(rc);
      return false;
    }

    size_t nl = data.find('\n');

    if (nl == std::string::npos)
    {
      err = "Unexpected response from the server";
      return false;
    }

    real = data.substr(0, nl);
    out.clear();
    size_t pos = nl + 1;

    while (pos < data.size())
    {
      size_t end = data.find('\0', pos);

      if (end == std::string::npos)
      {
        end = data.size();
      }

      std::string rec = data.substr(pos, end - pos);
      pos = end + 1;
      std::string f[8];
      size_t s = 0;
      bool ok = true;

      for (int i = 0; i < 7 && ok; ++i)
      {
        size_t t = rec.find('\t', s);

        if (t == std::string::npos)
        {
          ok = false;
        }

        else
        {
          f[i] = rec.substr(s, t - s);
          s = t + 1;
        }
      }

      if (!ok)
      {
        continue;
      }

      f[7] = rec.substr(s);

      if (f[7].empty())
      {
        continue;
      }

      Entry e;
      e.name = f[7];
      e.mtime = atoll(f[6].c_str()); // "1695481234.1234567890" - дробную часть выкидываем
      e.type = f[0].empty() ? '?' : f[0][0];
      e.is_dir = (f[1] == "d");
      e.mode = (unsigned)strtoul(f[2].c_str(), nullptr, 8) & 0777;
      e.owner = f[3];
      e.group = f[4];
      e.size = atoll(f[5].c_str());
      out.push_back(e);
    }

    return true;
  }

  void rename(const std::string &from, const std::string &to, std::string &err)
  {
    std::string out;
    std::string script = "if [ -e " + shq(to) + " ] || [ -L " + shq(to) +
                         " ]; then echo 'already exists'; exit 1; fi\n" + "mv -- " + shq(from) + " " + shq(to) +
                         " 2>&1";

    int rc = remoteSh(script, out);

    if (rc != 0)
    {
      err = out.empty() ? "mv returned " + std::to_string(rc) : firstLine(out);
    }
  }

  void makeDir(const std::string &path, std::string &err)
  {
    std::string out;
    std::string script =
        "if [ -e " + shq(path) + " ]; then echo 'already exists'; exit 1; fi\n" + "mkdir -p -- " + shq(path) + " 2>&1";
    int rc = remoteSh(script, out);

    if (rc != 0)
    {
      err = out.empty() ? "mkdir returned " + std::to_string(rc) : firstLine(out);
    }
  }

  void makeFile(const std::string &path, std::string &err)
  {
    std::string out;
    std::string script = "if [ -e " + shq(path) + " ] || [ -L " + shq(path) +
                         " ]; then echo 'already exists'; exit 1; fi\n" + "( : > " + shq(path) + " ) 2>&1";
    int rc = remoteSh(script, out);

    if (rc != 0)
    {
      err = out.empty() ? "code " + std::to_string(rc) : firstLine(out);
    }
  }

  bool removePaths(const std::vector<std::string> &paths, std::string &err)
  {
    std::string script = "rm -rf --";

    for (const auto &p : paths)
    {
      script += " " + shq(p);
    }

    std::string out;
    int rc = remoteSh(script + " 2>&1", out);

    if (rc != 0)
    {
      err = out.empty() ? "rm returned " + std::to_string(rc) : firstLine(out);
    }

    return rc == 0;
  }

  std::vector<std::string> transferCommand(const std::vector<std::string> &paths, const std::string &destination,
                                           bool upload, bool useRsync) const
  {
    std::string sshOpts = "ssh -o " + ctlOpt + " -o ControlMaster=no -o BatchMode=yes";
    std::vector<std::string> args;

    if (useRsync)
    {
      // -a сохраняет права и даты, --partial-dir позволяет докачать после обрыва и не портит старый файл
      // -s не дает shell на сервере "поделить" имена с пробелами
      args = {"rsync", "-a",    "-s", std::string("--partial-dir=") + PARTIAL_DIR, "--info=progress2",
              "-e",    sshOpts, "--"};
    }

    else
    {
      args = {"scp", "-r", "-o", ctlOpt, "-o", "ControlMaster=no", "-o", "BatchMode=yes", "--"};
    }

    for (const auto &p : paths)
    {
      args.push_back(upload ? p : tgt.scp_host + ":" + p);
    }

    args.push_back(upload ? tgt.scp_host + ":" + dirArg(destination) : dirArg(destination));
    return args;
  }

  std::vector<std::string> sftpCommand() const
  {
    return {"ssh",
            "-o",
            ctlOpt,
            "-o",
            "ControlMaster=no",
            "-o",
            "BatchMode=yes",
            "-o",
            "ServerAliveInterval=15",
            "-o",
            "ServerAliveCountMax=2",
            "-s",
            "--",
            tgt.ssh_host,
            "sftp"};
  }

  void view(const std::string &path)
  {
    std::string script = "if command -v less >/dev/null 2>&1; then exec less -- \"$1\"; else exec more -- \"$1\"; fi";
    runInTerminal({"ssh", "-o", ctlOpt, "-o", "ControlMaster=no", "-t", "--", tgt.ssh_host,
                   "sh -c " + shq(script) + " sh " + shq(path)},
                  "Viewing: " + tgt.ssh_host + ":" + path);
  }

  int download(const std::string &path, int fd, std::string &err)
  {
    return runPiped(sshCmd("cat -- " + shq(path)), -1, fd, &err);
  }

  int upload(const std::string &path, int fd, std::string &err)
  {
    return runPiped(sshCmd("cat > " + shq(path)), fd, -1, &err);
  }

private:
  Target tgt;
  std::string ctlOpt, ctlFile;
  bool closed = false;
};
