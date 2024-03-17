#include <ncurses.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

void scp(const std::string& source, const std::string& destination)
{
  std::string command = "scp -r " + source + " " + destination;
  std::system(command.c_str());
}

bool isDirectory(const std::string& path)
{
  struct stat buf;
  if (stat(path.c_str(), &buf) != 0)
  {
    return false;
  }
  return S_ISDIR(buf.st_mode);
}

std::string selectDirectory(const std::string& path, const std::string& remote_address)
{
  std::vector<std::string> files;
  DIR* dir;
  struct dirent* entry;

  dir = opendir(path.c_str());
  if (dir == NULL)
  {
    perror("opendir");
    return "";
  }

  while ((entry = readdir(dir)) != NULL)
  {
    files.push_back(entry->d_name);
  }
  closedir(dir);

  initscr();
  keypad(stdscr, TRUE);
  start_color();
  init_pair(1, COLOR_BLACK, COLOR_WHITE);  // Цвет для выделения
  int highlight = 0;
  int choice = 0;

  // Отображение файлов
  while (1)
  {
    clear();
    for (int i = 0; i < files.size(); ++i)
    {
      if (i == highlight)
      {
        attron(COLOR_PAIR(1));
      }

      std::string file_path = path + "/" + files[i];
      struct stat file_stat;
      if (stat(file_path.c_str(), &file_stat) != 0)
      {
        continue;
      }

      // Получение владельца и группы файла
      struct passwd *pw = getpwuid(file_stat.st_uid);
      struct group  *gr = getgrgid(file_stat.st_gid);
      std::string owner = (pw != NULL) ? pw->pw_name : "unknown";
      std::string group = (gr != NULL) ? gr->gr_name : "unknown";

      // Формирование строки с правами доступа
      char permissions[11];
      snprintf(permissions, 11, "%s%s%s%s%s%s%s%s%s%s",
              (S_ISDIR(file_stat.st_mode)) ? "d" : "-",
              (file_stat.st_mode & S_IRUSR) ? "r" : "-",
              (file_stat.st_mode & S_IWUSR) ? "w" : "-",
              (file_stat.st_mode & S_IXUSR) ? "x" : "-",
              (file_stat.st_mode & S_IRGRP) ? "r" : "-",
              (file_stat.st_mode & S_IWGRP) ? "w" : "-",
              (file_stat.st_mode & S_IXGRP) ? "x" : "-",
              (file_stat.st_mode & S_IROTH) ? "r" : "-",
              (file_stat.st_mode & S_IWOTH) ? "w" : "-",
              (file_stat.st_mode & S_IXOTH) ? "x" : "-");

      // Вывод строк прав доступа, владельца, группы и имени файла
      mvprintw(i, 0, "%-10s %-8s %-8s %s", permissions, owner.c_str(), group.c_str(), files[i].c_str());
      attroff(COLOR_PAIR(1));
    }
    refresh();
    choice = getch();

    switch (choice)
    {
      case KEY_UP:
      highlight--;
      if (highlight == -1)
      {
        highlight = 0;
      }
      break;

      case KEY_DOWN:
        highlight++;
        if (highlight == files.size())
        {
          highlight = files.size() - 1;
        }
        break;

      case KEY_LEFT:
        endwin();
        return "..";

      case KEY_RIGHT:
        if (isDirectory(path + "/" + files[highlight]))
        {
          endwin();
          return files[highlight];
        }
        else
        {
          std::string source = path + "/" + files[highlight];
          std::string destination = remote_address + ":" + path;
          scp(source, destination);
        }
        break;

      default:
        break;
    }
  }
}

int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    printf("Usage: %s <remote_address>\n", argv[0]);
    return 1;
  }

  std::string remote_address = argv[1];
  std::string current_directory = ".";

  while (1)
  {
    std::string selected_file = selectDirectory(current_directory, remote_address);
    if (selected_file == "..")
    {
      size_t pos = current_directory.find_last_of("/\\");

      if (pos != std::string::npos)
      {
        current_directory = current_directory.substr(0, pos);
      }

      if (current_directory.empty())
      {
        current_directory = ".";
      }
    }

    else if (!selected_file.empty())
    {
      current_directory += "/" + selected_file;
    }

    else
    {
      break;  // Выход, если нажата клавиша Escape
    }
  }

  return 0;
}
