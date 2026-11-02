#pragma once

#include "config.hpp"

/*
список серверов ~/.ssh/config
имена машин из ssh config вместе со всем что подключено через Include (шаблоны со звездой скип)
Include как у ssh
путь без / считается от ~/.ssh
*/

static void readSshConfig(const std::string &path, std::vector<std::string> &hosts, int depth)
{
  if (depth > 8) // защита от файлов которые подключают друг друга по кругу
  {
    return;
  }

  std::ifstream f(path);
  std::string line;

  while (std::getline(f, line))
  {
    std::replace(line.begin(), line.end(), '=', ' '); // бывает и "Host=name"
    std::istringstream iss(line);
    std::string key, name;
    iss >> key;
    key = lower(key);

    if (key == "include")
    {
      while (iss >> name)
      {
        if (name.compare(0, 2, "~/") == 0)
        {
          name = homeDir() + name.substr(1);
        }

        else if (name[0] != '/')
        {
          name = homeDir() + "/.ssh/" + name;
        }

        glob_t g{};

        if (glob(name.c_str(), 0, nullptr, &g) == 0)
        {
          for (size_t i = 0; i < g.gl_pathc; ++i)
          {
            readSshConfig(g.gl_pathv[i], hosts, depth + 1);
          }
        }

        globfree(&g);
      }

      continue;
    }

    if (key != "host")
    {
      continue;
    }

    while (iss >> name)
    {
      if (name.find_first_of("*?!") != std::string::npos)
      {
        continue;
      }

      if (std::find(hosts.begin(), hosts.end(), name) == hosts.end())
      {
        hosts.push_back(name);
      }
    }
  }
}

static std::vector<std::string> sshConfigHosts()
{
  std::vector<std::string> hosts;

  if (!homeDir().empty())
  {
    readSshConfig(homeDir() + "/.ssh/config", hosts, 0);
  }

  return hosts;
}

/*
дописать сервер в ~/.ssh/config чтобы добавился в список (и для обычного ssh тоже):
   Host alias
       HostName host
       User user
*/
static bool saveSshHost(const std::string &alias, const std::string &user, const std::string &host, std::string &err)
{
  std::string dir = homeDir() + "/.ssh", path = dir + "/config", old;
  mkdir(dir.c_str(), 0700);
  bool had = readFile(path, old);
  // новый файл - сразу на него 600
  int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);

  if (fd < 0)
  {
    err = path + ": " + strerror(errno);
    return false;
  }

  std::string text = (had && !old.empty() && old.back() != '\n') ? "\n" : "";
  text += (had && !old.empty() ? "\n" : "") + std::string("Host ") + alias + "\n    HostName " + host + "\n";

  if (!user.empty())
  {
    text += "    User " + user + "\n";
  }

  bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();

  if (!ok)
  {
    err = path + ": " + strerror(errno);
  }

  close(fd);
  return ok;
}
