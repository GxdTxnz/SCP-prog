#pragma once

#include "utils.hpp"

struct Entry
{
  std::string name, owner, group;
  char type = '?';     // d директория, l ссылка, f файл, ? хз
  bool is_dir = false; // директория или ссылка на директорию
  unsigned mode = 0;   // права (755 и тд)
  long long size = 0;
  long long mtime = 0; // когда меняли, секунды с 1970
};

static std::string permString(char type, unsigned mode)
{
  if (type == '?')
  {
    return "??????????";
  }

  std::string s(10, '-');
  s[0] = (type == 'f') ? '-' : type;
  const char rwx[] = "rwxrwxrwx";

  for (int i = 0; i < 9; ++i)
  {
    if (mode & (0400u >> i))
    {
      s[i + 1] = rwx[i];
    }
  }

  return s;
}

static char typeOf(mode_t m)
{
  if (S_ISDIR(m))
  {
    return 'd';
  }

  if (S_ISLNK(m))
  {
    return 'l';
  }

  if (S_ISREG(m))
  {
    return 'f';
  }

  if (S_ISFIFO(m))
  {
    return 'p';
  }

  if (S_ISSOCK(m))
  {
    return 's';
  }

  if (S_ISCHR(m))
  {
    return 'c';
  }

  if (S_ISBLK(m))
  {
    return 'b';
  }

  return '?';
}

// Сначала директории потом файлы
// mode: 0 - по алфавиту, 1 - новые сверху, 2 - большие сверху
static void sortEntries(std::vector<Entry> &v, int mode)
{
  std::sort(v.begin(), v.end(),
            [mode](const Entry &a, const Entry &b)
            {
              if (a.is_dir != b.is_dir)
              {
                return a.is_dir;
              }

              if (mode == 1 && a.mtime != b.mtime)
              {
                return a.mtime > b.mtime;
              }

              if (mode == 2 && a.size != b.size)
              {
                return a.size > b.size;
              }

              return a.name < b.name;
            });
}

static bool readLocalDir(const std::string &path, std::vector<Entry> &out, std::string &err)
{
  DIR *dir = opendir(path.c_str());

  if (dir == nullptr)
  {
    err = "Cannot open " + path + ": " + strerror(errno);
    return false;
  }

  out.clear();

  while (dirent *d = readdir(dir))
  {
    Entry e;
    e.name = d->d_name;

    if (e.name == "." || e.name == "..")
    {
      continue;
    }

    std::string full = joinPath(path, e.name);
    struct stat lst{};

    if (lstat(full.c_str(), &lst) == 0)
    {
      e.type = typeOf(lst.st_mode);
      e.mode = lst.st_mode & 0777;
      e.size = lst.st_size;
      e.mtime = lst.st_mtime;
      struct stat st{};
      e.is_dir = e.type == 'd' || (e.type == 'l' && stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
      passwd *pw = getpwuid(lst.st_uid);
      group *gr = getgrgid(lst.st_gid);
      e.owner = pw ? pw->pw_name : std::to_string(lst.st_uid);
      e.group = gr ? gr->gr_name : std::to_string(lst.st_gid);
    }

    out.push_back(e);
  }

  closedir(dir);
  return true;
}

// размер файлов/директорий для выбора между scp и rsync
static long long g_sum = 0, g_limit = 0;

static int sumCallback(const char *, const struct stat *sb, int flag, struct FTW *)
{
  if (flag == FTW_F)
  {
    g_sum += sb->st_size;
  }

  return g_sum >= g_limit ? 1 : 0;
}

static long long localSize(const std::vector<std::string> &paths, long long limit)
{
  g_sum = 0;
  g_limit = limit;

  for (const auto &p : paths)
  {
    if (nftw(p.c_str(), sumCallback, 32, FTW_PHYS) == 1)
    {
      break;
    }
  }

  return g_sum;
}

// удаление директории со всем содержимым
static int g_rmErrno = 0;

static int rmCallback(const char *path, const struct stat *, int, struct FTW *)
{
  if (remove(path) == 0)
  {
    return 0;
  }

  g_rmErrno = errno;
  return -1;
}

static bool removeLocal(const std::string &path, std::string &err)
{
  g_rmErrno = 0;

  if (nftw(path.c_str(), rmCallback, 32, FTW_DEPTH | FTW_PHYS) == 0)
  {
    return true;
  }

  err = path + ": " + strerror(g_rmErrno ? g_rmErrno : errno);
  return false;
}

// логика как в mkdir -p
static bool makeDirs(const std::string &path, std::string &err)
{
  for (size_t pos = 1; pos <= path.size(); ++pos)
  {
    if (pos != path.size() && path[pos] != '/')
    {
      continue;
    }

    std::string part = path.substr(0, pos);

    if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
    {
      err = part + ": " + strerror(errno);
      return false;
    }
  }

  return true;
}
