#pragma once

/*
все что больше - передаем через rsync (нужен на двух серверах)
меняется в ~/.scpanel.config в rsync_from
*/
static const long long RSYNC_FROM_DEFAULT = 100LL * 1024 * 1024; // 100 МБ
static const long long RSYNC_NEVER = -1;                         // rsync_from = never - всегда scp

// клавиши можно переопределить в ~/.scpanel.config в секции [keys]
// name - как действие пишется в конфиге, keys - дефолт клавиши, help - строка помощи
enum Act
{
  ACT_NONE,
  ACT_UP,
  ACT_DOWN,
  ACT_PAGEUP,
  ACT_PAGEDOWN,
  ACT_HOME,
  ACT_END,
  ACT_PARENT,
  ACT_OPEN,
  ACT_PANEL,
  ACT_MARK,
  ACT_MARKALL,
  ACT_SEND,
  ACT_VIEW,
  ACT_EDIT,
  ACT_RENAME,
  ACT_MKDIR,
  ACT_NEWFILE,
  ACT_DELETE,
  ACT_GOTO,
  ACT_SORT,
  ACT_SEARCH,
  ACT_NEXT,
  ACT_PREV,
  ACT_REFRESH,
  ACT_HELP,
  ACT_QUIT,
  ACT_CANCEL, // работает только пока идет передача поэтому может делить действие с другими биндами
  ACT_COUNT
};

struct ActionInfo
{
  const char *name, *keys, *help;
};

static const ActionInfo ACTIONS[ACT_COUNT] = {
    {"", "", ""},
    {"up", "Up", "move up"},
    {"down", "Down", "move down"},
    {"pageup", "PgUp", "page up"},
    {"pagedown", "PgDn", "page down"},
    {"home", "Home", "first item"},
    {"end", "End", "last item"},
    {"parent", "Left Backspace", "go to parent folder"},
    {"open", "Enter Right", "open folder / send file"},
    {"panel", "Tab", "other panel"},
    {"mark", "Space", "mark"},
    {"markall", "a", "mark all / unmark all"},
    {"send", "s F5", "send to the other panel"},
    {"view", "v F3", "view file"},
    {"edit", "e F4", "edit file"},
    {"rename", "m F6", "rename"},
    {"mkdir", "f F7", "new folder"},
    {"newfile", "c", "new empty file"},
    {"delete", "d F8 Del", "delete"},
    {"goto", "g", "go to path"},
    {"sort", "o", "sort: name / date / size"},
    {"search", "/", "search"},
    {"next", "n", "next match"},
    {"prev", "p", "previous match"},
    {"refresh", "r", "refresh"},
    {"help", "h F1", "this help"},
    {"quit", "q Ctrl+C", "quit"},
    {"cancel", "Esc q Ctrl+C", "cancel transfer (while it runs)"},
};

// недокачанное
static const char *PARTIAL_DIR = ".rsync-partial";

static const char *SORT_NAMES[] = {"name", "date (newest first)", "size (largest first)"};
