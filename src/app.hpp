#pragma once

#include "filesystem.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ui.hpp"

struct Pane
{
  bool remote = false;
  std::string cwd;
  std::vector<Entry> files;
  std::set<std::string> marked; // отмеченные имена в текущей директории
  int highlight = 0;
  int top = 0; // первая видимая строка
};

class App
{
public:
  std::string fatal;      // вывод ошибки (после закрытия интерфейса)
  bool offerSave = false; // сервера нет в ~/.ssh/config - после подключения предлагаем сохранить

  App(const Target &t, const std::string &localStart, Protocol mode) : tgt(t), localStart(localStart), protocol(mode), ssh(t)
  {
    panes[0].remote = false;
    panes[1].remote = true;
  }

  // "user@host" -> "host"
  std::string hostName() const
  {
    size_t at = tgt.ssh_host.find('@');
    return at == std::string::npos ? tgt.ssh_host : tgt.ssh_host.substr(at + 1);
  }

  int run()
  {
    if (!ssh.ensureConnection())
    {
      fatal = "Could not connect to " + tgt.ssh_host;
      return 1;
    }

    rsyncLocal = !usingSftp() && inPath("rsync");
    std::string out;
    rsyncRemote = !usingSftp() && ssh.remoteSh("command -v rsync >/dev/null 2>&1 && echo yes", out) == 0 &&
                  out.compare(0, 3, "yes") == 0;

    if (!loadPane(panes[0], localStart, ""))
    {
      fatal = status;
      closeConnection();
      return 1;
    }

    status.clear();

    if (!loadPane(panes[1], tgt.path, ""))
    {
      std::string why = status;

      if (tgt.path.empty() || !loadPane(panes[1], "", ""))
      {
        fatal = why;
        closeConnection();
        return 1;
      }

      status = why + " - opened home directory";
    }

    if (!usingSftp() && status.empty() && (!rsyncLocal || !rsyncRemote))
    {
      status = std::string("rsync not found ") + (rsyncLocal ? "on the server" : "on this machine") +
               " - using scp for everything";
    }

    // уведомление о кривом конфиге

    if (!g_cfg.warnings.empty())
    {
      status = g_cfg.warnings[0] +
               (g_cfg.warnings.size() > 1 ? " (and " + std::to_string(g_cfg.warnings.size() - 1) + " more)" : "");
    }

    if (offerSave)
    {
      offerSaveHost();
    }

    while (true)
    {
      draw();
      int ch = getch();

      if (searching)
      {
        searchKey(ch);
        continue;
      }

      Pane &p = panes[active];
      status.clear();
      int last = (int)p.files.size() - 1;
      int listH = std::max(1, getmaxy(stdscr) - 3);

      switch (actionFor(ch))
      {
      case ACT_PANEL:
        active = 1 - active;
        break;

      case ACT_UP:
        if (p.highlight > 0)
        {
          --p.highlight;
        }

        break;

      case ACT_DOWN:
        if (p.highlight < last)
        {
          ++p.highlight;
        }

        break;

      case ACT_PAGEUP:
        p.highlight = std::max(0, p.highlight - listH);
        break;

      case ACT_PAGEDOWN:
        p.highlight = std::max(0, std::min(last, p.highlight + listH));
        break;

      case ACT_HOME:
        p.highlight = 0;
        break;

      case ACT_END:
        p.highlight = std::max(0, last);
        break;

      case ACT_PARENT:
        if (p.cwd != "/") // встаем на папку, из которой вышли
        {
          loadPane(p, parentOf(p.cwd), p.cwd.substr(p.cwd.find_last_of('/') + 1));
        }

        break;

      case ACT_OPEN:
        if (p.files.empty())
        {
          break;
        }

        if (p.files[p.highlight].is_dir)
        {
          loadPane(p, joinPath(p.cwd, p.files[p.highlight].name), "");
        }

        else
        {
          transfer(true);
        }

        break;

      case ACT_MARK:
        if (p.files.empty())
        {
          break;
        }

        {
          const std::string &name = p.files[p.highlight].name;

          if (!p.marked.erase(name))
          {
            p.marked.insert(name);
          }
        }

        if (p.highlight < last)
        {
          ++p.highlight;
        }

        break;

      case ACT_MARKALL:
        if (p.marked.size() == p.files.size())
        {
          p.marked.clear();
        }

        else
        {
          for (const auto &e : p.files)
          {
            p.marked.insert(e.name);
          }
        }

        break;

      case ACT_SEND:
        if (!p.files.empty())
        {
          transfer(false);
        }

        break;

      case ACT_VIEW:
        viewCurrent();
        break;

      case ACT_EDIT:
        editCurrent();
        break;

      case ACT_RENAME:
        renameCurrent();
        break;

      case ACT_MKDIR:
        makeDir();
        break;

      case ACT_NEWFILE:
        makeFile();
        break;

      case ACT_DELETE:
        deleteSelected();
        break;

      case ACT_GOTO:
        gotoPath();
        break;

      case ACT_SORT:
        cycleSort();
        break;

      case ACT_HELP:
        showHelp();
        break;

      case ACT_SEARCH:
        searching = true;
        search.clear();
        searchFrom = p.highlight;
        searchMiss = false;
        break;

      case ACT_NEXT:
        if (lastSearch.empty())
        {
          status = "Search for something with / first";
        }

        else if (!findMatch(p, lastSearch, p.highlight + 1))
        {
          status = "Not found: " + lastSearch;
        }

        break;

      case ACT_PREV:
        if (lastSearch.empty())
        {
          status = "Search for something with / first";
        }

        else if (!findMatch(p, lastSearch, p.highlight - 1, -1))
        {
          status = "Not found: " + lastSearch;
        }

        break;

      case ACT_REFRESH:
        reloadPane(panes[0]);
        reloadPane(panes[1]);
        break;

      case ACT_QUIT:
        closeConnection();
        return 0;

      default:
        break;
      }
    }
  }

private:
  // если адреса нет в ~/.ssh/config то предлагаем сохранить
  void offerSaveHost()
  {
    size_t at = tgt.ssh_host.find('@');
    std::string user = at == std::string::npos ? "" : tgt.ssh_host.substr(0, at);
    std::string host = at == std::string::npos ? tgt.ssh_host : tgt.ssh_host.substr(at + 1);
    std::string keep = status;

    if (ask("Save " + tgt.ssh_host + " to ~/.ssh/config so it shows up in the list?  y yes  n no", "yn") != 'y')
    {
      status = keep;
      return;
    }

    std::string alias = inputLine("Name in the list: ", host), err;

    if (alias.empty())
    {
      status = keep;
    }

    else if (alias.find_first_of(" \t*?!") != std::string::npos)
    {
      status = "Not saved: the name cannot contain spaces or * ? !";
    }

    else
    {
      std::vector<std::string> hosts = sshConfigHosts();

      if (std::find(hosts.begin(), hosts.end(), alias) != hosts.end())
      {
        status = "Not saved: " + alias + " is already in ~/.ssh/config";
      }

      else if (!saveSshHost(alias, user, host, err))
      {
        status = "Not saved: " + err;
      }

      else
      {
        status = "Saved to ~/.ssh/config as " + alias;
      }
    }
  }

  Target tgt;
  std::string localStart;
  Protocol protocol;
  Ssh ssh;
  Sftp sftp;

  bool usingSftp() const
  {
    return protocol == Protocol::Sftp;
  }

  Pane panes[2];
  int active = 0;
  std::string status;
  bool rsyncLocal = false, rsyncRemote = false;
  bool searching = false, searchMiss = false;
  std::string search, lastSearch;
  int searchFrom = 0;
  int sortMode = g_cfg.sort;

  // отрисовка прогресса передачи снизу
  struct Progress
  {
    bool on = false;
    std::string title;
    long long done = 0, total = 0;
    double speed = 0; // байт в секунду
    double eta = -1;  // секунд до конца, -1 = пока хз
  } prog;

  void closeConnection()
  {
    sftp.stop();
    ssh.closeConnection();
  }

  void connectSftp()
  {
    if (sftp.connected())
    {
      return;
    }

    if (!ssh.ensureConnection())
    {
      throw Sftp::Error(0, "Could not connect via SSH transport");
    }

    sftp.start(ssh.sftpCommand());
  }

  bool readSftpDir(const std::string &path, std::vector<Entry> &out, std::string &real, std::string &err)
  {
    try
    {
      connectSftp();
      real = sftp.realpath(path);
      out.clear();

      for (const auto &a : sftp.list(real))
      {
        Entry e;
        e.name = a.name;
        e.type = typeOf(a.mode);
        e.mode = a.mode & 0777;
        e.size = a.size;
        e.mtime = a.mtime;
        e.owner = std::to_string(a.uid);
        e.group = std::to_string(a.gid);
        e.is_dir = S_ISDIR(a.mode);

        if (S_ISLNK(a.mode))
        {
          try
          {
            e.is_dir = S_ISDIR(sftp.stat(joinPath(real, a.name)).mode);
          }

          catch (const Sftp::Error &)
          { /* broken or inaccessible link */
          }
        }

        out.push_back(e);
      }

      return true;
    }

    catch (const std::exception &e)
    {
      err = e.what();
      return false;
    }
  }

  void sftpCopyTree(const std::string &local, const std::string &remote, bool upload, unsigned depth = 0)
  {
    if (depth > 128)
    {
      throw Sftp::Error(0, "Directory nesting exceeds limit");
    }

    unsigned mode;

    if (upload)
    {
      struct stat st{};

      if (lstat(local.c_str(), &st) != 0)
      {
        throw Sftp::Error(0, strerror(errno));
      }

      mode = st.st_mode;
    }

    else
    {
      mode = sftp.stat(remote, false).mode;
    }

    // Avoid cycles and crossing symlinks outside the selected tree.

    if (S_ISLNK(mode))
    {
      throw Sftp::Error(0, "SFTP copying symbolic links is not supported: " + (upload ? local : remote));
    }

    if (S_ISDIR(mode))
    {
      if (upload)
      {
        try
        {
          sftp.mkdir(remote);
        }

        catch (const Sftp::Error &)
        {
          if (!S_ISDIR(sftp.stat(remote, false).mode))
          {
            throw;
          }
        }

        std::vector<Entry> files;
        std::string err;

        if (!readLocalDir(local, files, err))
        {
          throw Sftp::Error(0, err);
        }

        for (const auto &e : files)
        {
          sftpCopyTree(joinPath(local, e.name), joinPath(remote, e.name), true, depth + 1);
        }
      }

      else
      {
        struct stat st{};

        if (mkdir(local.c_str(), 0700) != 0 &&
            (errno != EEXIST || lstat(local.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
        {
          throw Sftp::Error(0, "Cannot create local directory: " + local);
        }

        for (const auto &e : sftp.list(remote))
        {
          sftpCopyTree(joinPath(local, e.name), joinPath(remote, e.name), false, depth + 1);
        }
      }
    }

    else if (S_ISREG(mode))
    {
      if (upload)
      {
        try
        {
          auto a = sftp.stat(remote, false);

          if (!S_ISREG(a.mode))
          {
            throw Sftp::Error(0, "Destination is not a regular file");
          }
        }

        catch (const Sftp::Error &e)
        {
          if (e.code != 2)
          {
            throw;
          }
        }

        sftp.upload(local, remote);
      }

      else
      {
        sftp.download(remote, local);
      }
    }

    else
    {
      throw Sftp::Error(0, "Only regular files and directories can be copied");
    }
  }

  void transferSftp(const std::vector<std::string> &names)
  {
    Pane &src = panes[active], &dst = panes[1 - active];
    bool upload = !src.remote, cancelled = false;
    std::string result;

    try
    {
      connectSftp();
      prog.on = true;
      prog.title = upload ? "Uploading (SFTP)" : "Downloading (SFTP)";
      prog.done = 0;
      prog.total = 0;
      prog.speed = 0;
      prog.eta = -1;
      sftp.resetProgress();
      nodelay(stdscr, TRUE);
      double start = nowSec(), lastDraw = 0;
      sftp.progress = [&](uint64_t bytes)
      {
        int ch;

        while ((ch = getch()) != ERR)
        {
          if (ch == 3 || g_cfg.cancelKeys.count(ch))
          {
            cancelled = true;
          }
        }

        prog.done = bytes;
        double elapsed = nowSec() - start;

        if (elapsed > 0)
        {
          prog.speed = bytes / elapsed;
        }

        if (nowSec() - lastDraw >= 0.1)
        {
          draw();
          lastDraw = nowSec();
        }

        return !cancelled;
      };

      for (const auto &name : names)
      {
        sftpCopyTree(joinPath(upload ? src.cwd : dst.cwd, name), joinPath(upload ? dst.cwd : src.cwd, name), upload);
      }

      src.marked.clear();
      result = "Transferred via SFTP";
    }

    catch (const std::exception &e)
    {
      result = cancelled ? "Cancelled: partial files remain in destination"
                         : std::string("SFTP transfer failed: ") + e.what();
    }

    sftp.progress = {};
    nodelay(stdscr, FALSE);
    prog.on = false;
    reloadPane(dst);
    status = result;
  }

  bool readRemoteDir(const std::string &path, std::vector<Entry> &out, std::string &real, std::string &err)
  {
    return usingSftp() ? readSftpDir(path, out, real, err) : ssh.readRemoteDir(path, out, real, err);
  }

  long long remoteSize(const Pane &src, const std::vector<std::string> &names, const std::vector<std::string> &paths)
  {
    long long du = ssh.remoteDu(paths);

    if (du >= 0)
    {
      return du;
    }

    long long sum = 0;

    for (const auto &e : src.files)
    {
      if (std::find(names.begin(), names.end(), e.name) != names.end())
      {
        sum += e.size;
      }
    }

    return sum;
  }

  bool loadPane(Pane &p, const std::string &target, const std::string &focus)
  {
    std::vector<Entry> tmp;
    std::string err, real = target;
    bool ok = p.remote ? readRemoteDir(target, tmp, real, err) : readLocalDir(target, tmp, err);

    if (!ok)
    {
      status = err;
      return false;
    }

    sortEntries(tmp, sortMode);

    if (real != p.cwd)
    {
      p.marked.clear();
    }

    p.cwd = real;
    p.files.swap(tmp);
    std::set<std::string> alive;

    for (const auto &e : p.files)
    {
      if (p.marked.count(e.name))
      {
        alive.insert(e.name);
      }
    }

    p.marked.swap(alive);
    p.highlight = 0;
    p.top = 0;

    for (size_t i = 0; i < p.files.size(); ++i)
    {
      if (p.files[i].name == focus)
      {
        p.highlight = (int)i;
        break;
      }
    }

    return true;
  }

  void reloadPane(Pane &p)
  {
    std::string focus = p.files.empty() ? "" : p.files[p.highlight].name;
    int oldTop = p.top;

    if (loadPane(p, p.cwd, focus))
    {
      p.top = oldTop;
    }
  }

  // отмеченные
  // а если ничего не отмечено то по позиции курсора
  std::vector<std::string> selection(const Pane &p, bool onlyCurrent)
  {
    std::vector<std::string> names;

    if (!onlyCurrent && !p.marked.empty())
    {
      names.assign(p.marked.begin(), p.marked.end());
    }

    else if (!p.files.empty())
    {
      names.push_back(p.files[p.highlight].name);
    }

    return names;
  }

  static std::string describe(const std::vector<std::string> &names)
  {
    return names.size() == 1 ? names[0] : std::to_string(names.size()) + " items";
  }

  // вопрос в нижней строке; ждем одну из клавиш; Esc вернет 27
  int ask(const std::string &question, const std::string &keys)
  {
    status = question;
    draw();
    int ch;

    while ((ch = getch()) != 27 && ch != 3 && (ch < 0 || ch > 255 || keys.find((char)ch) == std::string::npos))
    {
      if (ch == KEY_RESIZE)
      {
        draw();
      }
    }

    status.clear();
    return ch == 3 ? 27 : ch; // Ctrl+C - то же, что Esc
  }

  // узнаем что уже есть в папке назначения и выкидываем пропущенное
  // false - передавать нечего или Esc
  bool askOverwrite(const Pane &dst, std::vector<std::string> &names)
  {
    std::vector<std::string> keep;
    bool all = false, none = false;

    for (const auto &n : names)
    {
      const Entry *old = nullptr;

      for (const auto &e : dst.files)
      {
        if (e.name == n)
        {
          old = &e;
          break;
        }
      }

      if (old == nullptr || all)
      {
        keep.push_back(n);
        continue;
      }

      if (none)
      {
        continue;
      }

      std::string q = old->is_dir ? "Folder " + n + "/ exists, files inside will be overwritten"
                                  : n + " already exists (" + sizeText(old->size) + ", " + formatDate(old->mtime) + ")";
      int k = ask(q + ":  y overwrite  n skip  a overwrite all  s skip all  Esc cancel", "ynas");

      if (k == 27)
      {
        status = "Cancelled";
        return false;
      }

      if (k == 'y' || k == 'a')
      {
        keep.push_back(n);
      }

      all = all || k == 'a';
      none = none || k == 's';
    }

    if (keep.empty())
    {
      status = "Nothing to transfer: everything was skipped";
      return false;
    }

    names.swap(keep);
    return true;
  }

  // "  1,234,567  45%  1.23MB/s  0:00:12" (строка --info=progress2 от rsync) -> 1234567
  static bool parseRsyncProgress(const std::string &line, long long &bytes)
  {
    std::string num;
    size_t i = 0;

    while (i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == ','))
    {
      if (line[i] != ',')
      {
        num += line[i];
      }

      ++i;
    }

    if (num.empty() || line.find('%', i) == std::string::npos)
    {
      return false;
    }

    bytes = atoll(num.c_str());
    return true;
  }

  // "huge.bin   45% 3686MB 804.1MB/s   00:05 ETA" (строка прогресса scp) -> имя файла и сколько его уже передано
  static bool parseScpProgress(const std::string &line, std::string &name, long long &bytes)
  {
    std::istringstream iss(line);
    std::vector<std::string> tok;

    for (std::string t; iss >> t;)
    {
      tok.push_back(t);
    }

    for (size_t i = 1; i + 1 < tok.size(); ++i)
    {
      const std::string &pct = tok[i];

      if (pct.size() < 2 || pct.back() != '%' || pct.find_first_not_of("0123456789") != pct.size() - 1)
      {
        continue;
      }

      char *end = nullptr;
      double v = strtod(tok[i + 1].c_str(), &end);
      std::string unit = end ? end : "";
      const char *units[] = {"B", "KB", "MB", "GB", "TB"};

      for (int k = 0; k < 5; ++k)
      {
        if (unit == units[k])
        {
          for (int j = 0; j < k; ++j)
          {
            v *= 1024;
          }
        }
      }

      name.clear();

      for (size_t j = 0; j < i; ++j)
      {
        name += (j ? " " : "") + tok[j];
      }

      bytes = (long long)v;
      return true;
    }

    return false;
  }

  // запуск scp/rsync без выхода из интерфейса; прогресс снизу
  // запуск в псевдотерминале
  int runWithProgress(const std::vector<std::string> &args, const std::string &title, long long total, bool rsyncOut,
                      std::string &firstMsg, bool &cancelled)
  {
    int master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname(master) == nullptr)
    {
      if (master >= 0)
      {
        close(master);
      }

      return -1;
    }

    std::string slaveName = ptsname(master);
    pid_t pid = fork();

    if (pid < 0)
    {
      close(master);
      return -1;
    }

    if (pid == 0)
    {
      setsid();
      int slave = open(slaveName.c_str(), O_RDWR);

      if (slave < 0)
      {
        _exit(127);
      }

      struct winsize ws{};
      ws.ws_row = 24;
      ws.ws_col = 512;
      ioctl(slave, TIOCSWINSZ, &ws);
      dup2(slave, 0);
      dup2(slave, 1);
      dup2(slave, 2);
      close(slave);
      close(master);
      signal(SIGINT, SIG_DFL);
      setenv("LC_ALL", "C", 1);
      execArgs(args);
      perror(args[0].c_str());
      _exit(127);
    }

    g_cancel = 0;
    void (*oldInt)(int) = signal(SIGINT, onSigint);
    nodelay(stdscr, TRUE);

    prog = Progress();
    prog.on = true;
    prog.title = title;
    prog.total = total;
    cancelled = false;

    // скорость по последним 5 секундам
    std::vector<std::pair<double, long long>> samples;
    double lastDraw = -1e9;
    std::string buf;
    // у scp прогресс по каждому файлу отдельно поэтому складываем перенесенные файлы и текущий
    std::string curName;
    long long doneFiles = 0, curBytes = 0;

    for (bool eof = false; !eof;)
    {
      pollfd pfd{master, POLLIN, 0};

      if (poll(&pfd, 1, 200) > 0)
      {
        char tmp[4096];
        ssize_t n = read(master, tmp, sizeof(tmp));

        if (n > 0)
        {
          buf.append(tmp, n);
        }

        else if (n == 0 || errno != EINTR)
        {
          eof = true;
        }
      }

      // rsync обновляет строку прогресса через \r поэтому режем и по \r и по \n
      size_t cut;

      while ((cut = buf.find_first_of("\r\n")) != std::string::npos)
      {
        std::string line = trim(buf.substr(0, cut));
        buf.erase(0, cut + 1);
        long long b;
        std::string nm;

        if (rsyncOut && parseRsyncProgress(line, b))
        {
          prog.done = b;
        }

        else if (!rsyncOut && parseScpProgress(line, nm, b))
        {
          if (nm != curName)
          {
            doneFiles += curBytes;
            curName = nm;
          }

          curBytes = b;
          prog.done = doneFiles + curBytes;
        }

        else if (firstMsg.empty() && !line.empty())
        {
          firstMsg = line;
        }
      }

      double t = nowSec();

      if (total > 0 && prog.done > total)
      {
        prog.done = total;
      }

      samples.push_back({t, prog.done});

      while (samples.size() > 2 && t - samples.front().first > 5)
      {
        samples.erase(samples.begin());
      }

      double dt = t - samples.front().first;

      // первые пару секунд надо выждать

      if (dt >= 2)
      {
        prog.speed = (prog.done - samples.front().second) / dt;
        prog.eta = prog.speed > 0 && total > 0 ? (total - prog.done) / prog.speed : -1;
      }

      int ch;

      while ((ch = getch()) != ERR)
      {
        if (g_cfg.cancelKeys.count(ch) || ch == 3) // Ctrl+C отменяет всегда, даже если в конфиге его убрали
        {
          g_cancel = 1;
        }
      }

      if (g_cancel && !cancelled)
      {
        cancelled = true;
        kill(-pid, SIGTERM);
      }

      if (t - lastDraw >= 0.25)
      {
        draw();
        lastDraw = t;
      }
    }

    close(master);
    int st = 0;

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    {
    }

    nodelay(stdscr, FALSE);
    signal(SIGINT, oldInt);
    prog.on = false;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  }

  bool removePaths(bool remote, const std::vector<std::string> &paths, std::string &err)
  {
    if (remote && usingSftp())
    {
      try
      {
        connectSftp();

        for (const auto &p : paths)
        {
          sftp.removeTree(p);
        }

        return true;
      }

      catch (const std::exception &e)
      {
        err = e.what();
        return false;
      }
    }

    if (remote)
    {
      return ssh.removePaths(paths, err);
    }

    for (const auto &p : paths)
    {
      if (!removeLocal(p, err))
      {
        return false;
      }
    }

    return true;
  }

  // при отмене предлагаем убрать то что успели перекинуть
  // удаляем только то чего в директории не было
  // если то что было перезаписали - не трогаем
  std::string afterCancel(const Pane &dst, const std::vector<std::string> &names, const std::set<std::string> &existed,
                          bool useRsync)
  {
    std::vector<std::string> fresh;
    size_t overwritten = 0;

    for (const auto &n : names)
    {
      if (existed.count(n))
      {
        ++overwritten;
      }

      else
      {
        fresh.push_back(joinPath(dst.cwd, n));
      }
    }

    std::string note;

    if (overwritten > 0)
    {
      note = useRsync ? "; files you chose to overwrite are untouched, send again to resume"
                      : "; " + std::to_string(overwritten) + " overwritten item(s) already lost the old version";
    }

    if (fresh.empty())
    {
      return "Cancelled" + note;
    }

    // папку для недокачанного rsync тоже создал сам, если ее не было

    if (useRsync && !existed.count(PARTIAL_DIR))
    {
      fresh.push_back(joinPath(dst.cwd, PARTIAL_DIR));
    }

    if (ask("Cancelled. Remove what was already copied (only new items)?  y remove  n keep", "yn") != 'y')
    {
      return "Cancelled: partly copied files are left in the destination" + note;
    }

    std::string err;

    if (!removePaths(dst.remote, fresh, err))
    {
      return "Cancelled, but cleanup failed: " + err;
    }

    return "Cancelled: removed partly copied files" + note;
  }

  void transfer(bool onlyCurrent)
  {
    Pane &src = panes[active];
    Pane &dst = panes[1 - active];
    bool upload = !src.remote;
    std::vector<std::string> names = selection(src, onlyCurrent);

    if (!ssh.ensureConnection())
    {
      status = "No connection to the server";
      return;
    }

    reloadPane(dst);

    if (!askOverwrite(dst, names))
    {
      return;
    }

    if (usingSftp())
    {
      transferSftp(names);
      return;
    }

    // что уже было в директории назначения до перекидки
    std::set<std::string> existed;

    for (const auto &e : dst.files)
    {
      existed.insert(e.name);
    }

    std::vector<std::string> paths;

    for (const auto &n : names)
    {
      paths.push_back(joinPath(src.cwd, n));
    }

    status = "Calculating size...";
    draw();
    long long size = upload ? localSize(paths, LLONG_MAX) : remoteSize(src, names, paths);
    bool useRsync = g_cfg.rsyncFrom != RSYNC_NEVER && size >= g_cfg.rsyncFrom && rsyncLocal && rsyncRemote;
    std::vector<std::string> args = ssh.transferCommand(paths, dst.cwd, upload, useRsync);
    std::string what = describe(names);
    std::string how = useRsync ? "rsync" : "scp";

    std::string msg;
    bool cancelled = false;
    double start = nowSec();
    int rc = runWithProgress(args, (upload ? "Uploading: " : "Downloading: ") + what + " (" + how + ")", size, useRsync,
                             msg, cancelled);
    double took = nowSec() - start;

    if (cancelled)
    {
      status = afterCancel(dst, names, existed, useRsync);
    }

    else if (rc == 0)
    {
      src.marked.clear();
      status = (upload ? "Uploaded: " : "Downloaded: ") + what + " via " + how + ", " + sizeText(size) + " in " +
               formatDuration(took);

      if (took >= 1)
      {
        status += " (" + sizeText((long long)(size / took)) + "/s)";
      }
    }

    else
    {
      status = "Failed (" + how + " returned " + std::to_string(rc) + "): " + (msg.empty() ? what : msg);
    }

    std::string result = status;
    reloadPane(dst);
    status = result;
  }

  void viewSftp(const std::string &path)
  {
    char temp[] = "/tmp/scpanel-view-XXXXXX";
    int fd = mkstemp(temp);

    if (fd < 0)
    {
      status = strerror(errno);
      return;
    }

    close(fd);

    try
    {
      connectSftp();
      sftp.download(path, temp);
      const char *pager = getenv("PAGER");
      std::string cmd = (pager && *pager) ? pager : (inPath("less") ? "less" : "more");
      runInTerminal({"sh", "-c", cmd + " \"$1\"", "sh", temp}, "Viewing: " + path);
    }

    catch (const std::exception &e)
    {
      status = e.what();
    }

    unlink(temp);
  }

  void viewCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
    {
      return;
    }

    const Entry &e = p.files[p.highlight];

    if (e.is_dir)
    {
      status = "Cannot view a folder";
      return;
    }

    std::string path = joinPath(p.cwd, e.name);

    if (p.remote && usingSftp())
    {
      viewSftp(path);
      return;
    }

    if (p.remote)
    {
      if (!ssh.ensureConnection())
      {
        status = "No connection to the server";
        return;
      }

      ssh.view(path);
      return;
    }

    const char *pager = getenv("PAGER");
    std::string cmd = (pager && *pager) ? pager : (inPath("less") ? "less" : "more");
    runInTerminal({"sh", "-c", cmd + " \"$1\"", "sh", path}, "Viewing: " + path);
  }

  static std::string editorCmd()
  {
    for (const char *var : {"VISUAL", "EDITOR"})
    {
      const char *v = getenv(var);

      if (v && *v)
      {
        return v;
      }
    }

    return inPath("nano") ? "nano" : "vi";
  }

  // редактирование файлов
  // файл с сервера скачиваем во временную папку и открываем в редакторе
  // если что то поменяли - заливаем через "cat >"
  void editCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
    {
      return;
    }

    Entry e = p.files[p.highlight];

    if (e.is_dir)
    {
      status = "Cannot edit a folder";
      return;
    }

    std::string path = joinPath(p.cwd, e.name);
    std::string editor = editorCmd();

    if (!p.remote)
    {
      runInTerminal({"sh", "-c", editor + " \"$1\"", "sh", path}, "Editing: " + path);
      reloadPane(p);
      return;
    }

    if (!ssh.ensureConnection())
    {
      status = "No connection to the server";
      return;
    }

    const char *tmpEnv = getenv("TMPDIR");
    std::string tmpl = std::string(tmpEnv && *tmpEnv ? tmpEnv : "/tmp") + "/scpanel-XXXXXX";
    std::vector<char> dirBuf(tmpl.begin(), tmpl.end());
    dirBuf.push_back('\0');

    if (mkdtemp(dirBuf.data()) == nullptr)
    {
      status = std::string("Cannot create a temp folder: ") + strerror(errno);
      return;
    }

    std::string dir = dirBuf.data(), local = dir + "/" + e.name, err;
    int fd = open(local.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    status = "Downloading " + e.name + "...";
    draw();
    int rc = -1;

    if (usingSftp())
    {
      try
      {
        connectSftp();
        sftp.download(path, local);
        rc = 0;
      }

      catch (const std::exception &e)
      {
        err = e.what();
      }
    }

    else
    {
      rc = fd < 0 ? -1 : ssh.download(path, fd, err);
    }

    if (fd >= 0)
    {
      close(fd);
    }

    std::string before, after, ignore;

    if (rc != 0 || !readFile(local, before))
    {
      status = "Cannot read " + e.name + ": " + (err.empty() ? "cat returned " + std::to_string(rc) : firstLine(err));
      removeLocal(dir, ignore);
      return;
    }

    runInTerminal({"sh", "-c", editor + " \"$1\"", "sh", local}, "Editing: " + tgt.ssh_host + ":" + path);

    if (!readFile(local, after) || after == before)
    {
      status = "No changes: " + e.name;
      removeLocal(dir, ignore);
      return;
    }

    // пока правили, подключение могло закрыться (оно живет 30 минут без дела)
    int in = ssh.ensureConnection() ? open(local.c_str(), O_RDONLY) : -1;
    err.clear();

    if (usingSftp())
    {
      try
      {
        connectSftp();
        sftp.upload(local, path);
        rc = 0;
      }

      catch (const std::exception &e)
      {
        err = e.what();
        rc = -1;
      }
    }

    else
    {
      rc = in < 0 ? -1 : ssh.upload(path, in, err);
    }

    if (in >= 0)
    {
      close(in);
    }

    if (rc != 0)
    {
      // временный файл с правками оставляем; выводим путь до него
      status = "Upload failed (" + (err.empty() ? "code " + std::to_string(rc) : firstLine(err)) +
               "), your edit is kept in " + local;
      return;
    }

    removeLocal(dir, ignore);
    reloadPane(p);
    status = "Saved: " + tgt.ssh_host + ":" + path;
  }

  void deleteSelected()
  {
    Pane &p = panes[active];
    std::vector<std::string> names = selection(p, false);

    if (names.empty())
    {
      return;
    }

    std::string what = describe(names);

    if (ask("Delete " + what + (p.remote ? " on the server" : "") + "?  y yes  n no", "yn") != 'y')
    {
      status = "Cancelled";
      return;
    }

    std::string err;
    std::vector<std::string> paths;

    for (const auto &n : names)
    {
      paths.push_back(joinPath(p.cwd, n));
    }

    removePaths(p.remote, paths, err);

    int keepAt = p.highlight;
    p.marked.clear();
    loadPane(p, p.cwd, "");
    p.highlight = std::max(0, std::min(keepAt, (int)p.files.size() - 1));
    status = err.empty() ? "Deleted: " + what : "Delete failed: " + err;
  }

  void renameCurrent()
  {
    Pane &p = panes[active];

    if (p.files.empty())
    {
      return;
    }

    std::string old = p.files[p.highlight].name;
    std::string name = inputLine("Rename to: ", old);

    if (name.empty() || name == old)
    {
      return;
    }

    std::string from = joinPath(p.cwd, old), to = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote && usingSftp())
    {
      try
      {
        connectSftp();
        sftp.rename(from, to);
      }

      catch (const std::exception &e)
      {
        err = e.what();
      }
    }

    else if (p.remote)
    {
      ssh.rename(from, to, err);
    }

    else
    {
      struct stat st{};

      if (lstat(to.c_str(), &st) == 0)
      {
        err = "already exists";
      }

      else if (rename(from.c_str(), to.c_str()) != 0)
      {
        err = strerror(errno);
      }
    }

    if (!err.empty())
    {
      status = "Rename failed: " + err;
      return;
    }

    p.marked.erase(old);
    loadPane(p, p.cwd, name);
    status = "Renamed: " + old + " -> " + name;
  }

  void makeDir()
  {
    Pane &p = panes[active];
    std::string name = inputLine("New folder: ");

    if (name.empty())
    {
      return;
    }

    std::string path = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote && usingSftp())
    {
      try
      {
        connectSftp();
        sftp.mkdir(path);
      }

      catch (const std::exception &e)
      {
        err = e.what();
      }
    }

    else if (p.remote)
    {
      ssh.makeDir(path, err);
    }

    else
    {
      struct stat st{};

      if (lstat(path.c_str(), &st) == 0)
      {
        err = "already exists";
      }

      else
      {
        makeDirs(path, err);
      }
    }

    if (!err.empty())
    {
      status = "Cannot create folder: " + err;
      return;
    }

    // переходим к созданной директории (для "a/b" - на "a")
    loadPane(p, p.cwd, name.substr(0, name.find('/')));
    status = "Created: " + name;
  }

  // создаем пустой файл; уже существующий не трогаем
  void makeFile()
  {
    Pane &p = panes[active];
    std::string name = inputLine("New file: ");

    if (name.empty())
    {
      return;
    }

    std::string path = name[0] == '/' ? name : joinPath(p.cwd, name), err;

    if (p.remote && usingSftp())
    {
      try
      {
        connectSftp();
        sftp.create(path);
      }

      catch (const std::exception &e)
      {
        err = e.what();
      }
    }

    else if (p.remote)
    {
      ssh.makeFile(path, err);
    }

    else
    {
      int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);

      if (fd < 0)
      {
        err = errno == EEXIST ? "already exists" : strerror(errno);
      }

      else
      {
        close(fd);
      }
    }

    if (!err.empty())
    {
      status = "Cannot create file: " + err;
      return;
    }

    loadPane(p, p.cwd, name.substr(0, name.find('/')));
    status = "Created: " + name;
  }

  // ~ - домашняя папка
  // путь без / в начале - от текущей папки
  void gotoPath()
  {
    Pane &p = panes[active];
    std::string in = inputLine("Go to: ", p.cwd);

    if (in.empty() || in == p.cwd)
    {
      return;
    }

    std::string target;

    if (p.remote)
    {
      if (in == "~" || in == "~/")
      {
        target = "";
      }

      else if (in.compare(0, 2, "~/") == 0)
      {
        target = in.substr(2);
      }

      else
      {
        target = in[0] == '/' ? in : joinPath(p.cwd, in);
      }
    }

    else
    {
      const char *home = getenv("HOME");

      if (home && (in == "~" || in.compare(0, 2, "~/") == 0))
      {
        in = home + in.substr(1);
      }

      target = absolutePath(in[0] == '/' ? in : joinPath(p.cwd, in));

      if (target.empty())
      {
        status = "No such folder: " + in;
        return;
      }
    }

    loadPane(p, target, "");
  }

  void cycleSort()
  {
    sortMode = (sortMode + 1) % 3;

    for (Pane &p : panes)
    {
      std::string focus = p.files.empty() ? "" : p.files[p.highlight].name;
      sortEntries(p.files, sortMode);

      for (size_t i = 0; i < p.files.size(); ++i)
      {
        if (p.files[i].name == focus)
        {
          p.highlight = (int)i;
        }
      }
    }

    status = std::string("Sort by ") + SORT_NAMES[sortMode];
  }

  // список всех биндов с учетом конфига
  void showHelp()
  {
    int n = ACT_COUNT - 1, top = 0;

    while (true)
    {
      erase();
      int rows = getmaxy(stdscr), cols = getmaxx(stdscr);
      int perCol = std::max(1, rows - 3);
      int ncol = n > perCol && cols >= 104 ? 2 : 1, colW = cols / ncol;
      bool scroll = n > perCol * ncol;
      top = scroll ? std::max(0, std::min(top, n - perCol)) : 0;
      attron(A_BOLD);
      mvaddstr(0, 0, fitCols("Keys (change them in ~/.scpanel.config, see README)", cols).c_str());
      attroff(A_BOLD);

      for (int i = top; i < n; ++i)
      {
        int c = (i - top) / perCol, r = (i - top) % perCol;

        if (c >= ncol)
        {
          break;
        }

        int a = i + 1;
        mvaddstr(r + 2, c * colW, fitCols("  " + fitCols(keysText(a), 18) + "  " + ACTIONS[a].help, colW).c_str());
      }

      mvaddstr(rows - 1, 0,
               fitCols(scroll ? "Up/Down scroll  any other key - back" : "Press any key", cols - 1).c_str());
      refresh();
      int ch = getch();

      if (ch == KEY_RESIZE)
      {
        continue;
      }

      if (scroll && (ch == KEY_UP || ch == KEY_DOWN || ch == KEY_PPAGE || ch == KEY_NPAGE))
      {
        top += ch == KEY_UP ? -1 : ch == KEY_DOWN ? 1 : ch == KEY_PPAGE ? -perCol : perCol;
        continue;
      }

      return;
    }
  }

  // 1 - ищем вниз, -1 - вверх (p - предыдущее совпадение); на последнем совпадении переходим на другой конец
  bool findMatch(Pane &p, const std::string &q, int from, int step = 1)
  {
    if (q.empty() || p.files.empty())
    {
      return false;
    }

    std::wstring wq = lowerW(q);
    int n = (int)p.files.size();

    for (int k = 0; k < n; ++k)
    {
      int i = ((from + step * k) % n + n) % n;

      if (lowerW(p.files[i].name).find(wq) != std::wstring::npos)
      {
        p.highlight = i;
        return true;
      }
    }

    return false;
  }

  // поиск во время ввода
  // курсор прыгает на первое совпадение
  void searchKey(int ch)
  {
    if (ch == 27)
    {
      searching = false;
      return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
    {
      searching = false;

      if (!search.empty())
      {
        lastSearch = search;
      }

      return;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
    {
      popUtf8(search);
    }

    else if (ch >= 32 && ch < 256)
    {
      search += (char)ch;
    }

    else
    {
      return;
    }

    searchMiss = !search.empty() && !findMatch(panes[active], search, searchFrom);
  }

  void drawPane(Pane &p, int x, int w, int listH, bool act)
  {
    std::string title =
        p.remote ? std::string(usingSftp() ? " [SFTP] " : " [SSH] ") + tgt.ssh_host + ":" + p.cwd : " " + p.cwd;
    const char *sortTag[] = {"", "  [newest]", "  [largest]"};
    title += sortTag[sortMode];
    int hattr = act ? A_REVERSE : A_BOLD;
    attron(hattr);
    mvaddstr(0, x, fitCols(title, w).c_str());
    attroff(hattr);

    if (p.highlight < p.top)
    {
      p.top = p.highlight;
    }

    if (p.highlight >= p.top + listH)
    {
      p.top = p.highlight - listH + 1;
    }

    if (p.files.empty())
    {
      mvaddstr(1, x, fitCols("  (empty)", w).c_str());
    }

    const int sizeW = 7;

    for (int r = 0; r < listH && p.top + r < (int)p.files.size(); ++r)
    {
      int i = p.top + r;
      const Entry &e = p.files[i];
      bool mk = p.marked.count(e.name) > 0;
      std::string name = e.name + (e.is_dir ? "/" : "");
      std::string line = mk ? "* " : "  ";

      if (w >= 2 + sizeW + 6)
      {
        std::string size = e.is_dir ? "" : humanSize(e.size);
        line += fitCols(name, w - 2 - sizeW) + std::string(sizeW - size.size(), ' ') + size;
      }

      else
      {
        line += fitCols(name, w - 2);
      }

      int attr = mk ? A_BOLD : 0;

      if (act && i == p.highlight)
      {
        attr |= A_REVERSE;
      }

      attron(attr);
      mvaddstr(r + 1, x, line.c_str());
      attroff(attr);
    }
  }

  // две нижние строки во время передачи:
  // [=========>          ]  45%  123M / 274M
  // Uploading: backup.tar (rsync)  12M/s  ETA 0:12  Esc cancel
  void drawProgress(int rows, int cols)
  {
    int pct = prog.total > 0 ? (int)(prog.done * 100 / prog.total) : 0;
    pct = std::max(0, std::min(100, pct));
    std::string right = "  " + std::to_string(pct) + "%  " + sizeText(prog.done) + " / " + sizeText(prog.total);
    int barW = std::max(3, cols - 1 - colsOf(right) - 2);
    int fill = barW * pct / 100;
    std::string bar = std::string(fill, '=');

    if (fill < barW)
    {
      bar += (fill > 0 ? ">" : " ") + std::string(barW - fill - 1, ' ');
    }

    attron(A_BOLD);
    mvaddstr(rows - 2, 0, fitCols("[" + bar + "]" + right, cols).c_str());
    attroff(A_BOLD);
    std::string speed = prog.speed > 0 ? sizeText((long long)prog.speed) + "/s" : "--/s";
    std::string eta = prog.eta >= 0 ? formatDuration(prog.eta) : "--";
    mvaddstr(rows - 1, 0, fitCols(prog.title + "  " + speed + "  ETA " + eta + "  Esc cancel", cols - 1).c_str());
  }

  void draw()
  {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    if (rows < 5 || cols < 30)
    {
      mvaddstr(0, 0, "Window is too small");
      refresh();
      return;
    }

    int listH = rows - 3; // заголовок сверху, две строки снизу
    int lw = (cols - 1) / 2;
    drawPane(panes[0], 0, lw, listH, active == 0);
    drawPane(panes[1], lw + 1, cols - lw - 1, listH, active == 1);
    mvvline(0, lw, ACS_VLINE, listH + 1);
    // инфа о файле под курсором
    Pane &p = panes[active];
    std::string info;

    if (!p.files.empty())
    {
      const Entry &e = p.files[p.highlight];
      info = permString(e.type, e.mode) + "  " + e.owner + " " + e.group + "  " + humanSize(e.size) + "  " +
             formatDate(e.mtime) + "  " + e.name;
    }

    if (!p.marked.empty())
    {
      info = "Marked: " + std::to_string(p.marked.size()) + "  |  " + info;
    }

    if (prog.on)
    {
      drawProgress(rows, cols);
      refresh();
      return;
    }

    attron(A_DIM);
    mvaddstr(rows - 2, 0, fitCols(info, cols).c_str());
    attroff(A_DIM);
    std::string bottom;

    if (searching)
    {
      bottom = "Search: " + search + "_" + (searchMiss ? "   (no matches)" : "");
    }

    else
    {
      bottom = status.empty() ? helpLine() : status;
    }

    // В последнюю клетку экрана не пишем
    // а то curses от этого может прокрутить экран
    mvaddstr(rows - 1, 0, fitCols(bottom, cols - 1).c_str());
    refresh();
  }
};
