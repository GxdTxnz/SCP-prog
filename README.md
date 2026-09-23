# scpanel

## Supported OS

- Debian 10+
- Ubuntu 18.04+
- RHEL 7+
- CentOS 7+
- Fedora
- Astra Linux 1.7+
- ALT Linux p9+

## Installation

You can install scpanel for your current system by building it from source with the commands below
(install the build dependencies for your OS first, see [Dependencies](#dependencies)):

```bash
make
sudo make install     # removes with: sudo make uninstall
```

Or pick the package for your system and install it:

```bash
sudo apt install ./scpanel_*_amd64.deb        # Debian, Ubuntu, Astra Linux
sudo dnf install ./scpanel-*.x86_64.rpm       # RHEL/CentOS 8+, Fedora
sudo yum install ./scpanel-*.x86_64.rpm       # RHEL/CentOS 7
apt-get install ./scpanel-*.x86_64.rpm        # ALT Linux (as root)
```

## Building

If you are changing (or have changed) the code and want to build a binary for your current system,
use the commands from [Installation](#installation) above

If you are changing (or have changed) the code and want to build a binary for all supported OSes listed above,
use the following commands (see [Dependencies](#dependencies) for what you need):

```bash
make portable         # -> dist/scpanel
sudo make install     # installs dist/scpanel if it exists
```

If you are changing (or have changed) the code and want to build packages for all supported OSes listed above,
use the following commands (see [Dependencies](#dependencies) for what you need):

```bash
make packages VERSION=1.0.0    # -> dist/scpanel_1.0.0-1_amd64.deb, dist/scpanel-1.0.0-1.x86_64.rpm
```

When building the binary or packages for all OSes, please run the compatibility tests
(see [Dependencies](#dependencies) for what you need):

```bash
scripts/test-distros.sh                  # runs dist/scpanel on every supported OS
TIMEOUT=600 scripts/test-packages.sh     # installs dist/*.deb and dist/*.rpm on every supported OS
```

## Dependencies

Building for your current system (`make`) needs a C++17 compiler (GCC 7+), make, pkg-config and ncurses with wide-character support:

| OS                                          | Command                                                                      |
| ------------------------------------------- | ---------------------------------------------------------------------------- |
| Debian 10+, Astra Linux 1.7+, Ubuntu 20.04+ | `sudo apt install g++ make pkg-config libncurses-dev`                        |
| Ubuntu 18.04                                | `sudo apt install g++ make pkg-config libncursesw5-dev`                      |
| RHEL/CentOS 8+, Fedora                      | `sudo dnf install gcc-c++ make pkgconf-pkg-config ncurses-devel`             |
| ALT Linux                                   | `apt-get install gcc-c++ make pkg-config libncursesw-devel` (as root)        |
| RHEL/CentOS 7                               | not supported: GCC 4.8 is too old, use the package or `make portable` binary |

Building for all OSes (`make portable`) and building packages (`make packages`):

- x86_64 machine
- make
- Docker (your user in the `docker` group) or Podman: Docker is used if installed, otherwise Podman. To choose explicitly: `make portable DOCKER=podman`
- Internet access: the build images (`quay.io/pypa/manylinux2014_x86_64`, `goreleaser/nfpm`) and the ncurses source are downloaded during the build

Compatibility tests (`scripts/test-*.sh`):

- Docker or Podman and Internet access, as above
- `script` (util-linux) and `timeout` (coreutils), present on any usual Linux

## Usage

```bash
scpanel [user@host[:path]]
```

Without arguments scpanel shows the hosts from `~/.ssh/config` (including files added with `Include`).
If you connect to an address that is not there, scpanel offers to save it to `~/.ssh/config`,
so it appears in the list next time. Nothing is saved without your confirmation

## Controls

| Key              | Action                     | Name in config   |
| ---------------- | -------------------------- | ---------------- |
| ↑ / ↓            | move up / down             | up, down         |
| PgUp / PgDn      | page up / down             | pageup, pagedown |
| Home / End       | go to first / last item    | home, end        |
| Enter / →        | open directory / send file | open             |
| ← / Backspace    | go to parent directory     | parent           |
| Tab              | switch panel               | panel            |
| Space            | select                     | mark             |
| a                | select all / unselect all  | markall          |
| s / F5           | send to the other panel    | send             |
| v / F3           | view file                  | view             |
| e / F4           | edit file                  | edit             |
| m / F6           | rename                     | rename           |
| f / F7           | create directory           | mkdir            |
| c                | create empty file          | newfile          |
| d / F8 / Del     | delete                     | delete           |
| g                | go to path                 | goto             |
| o                | sort: name / date / size   | sort             |
| /                | search                     | search           |
| n                | next match                 | next             |
| p                | previous match             | prev             |
| r                | refresh                    | refresh          |
| Esc / q / Ctrl+C | cancel transfer            | cancel           |
| h / F1           | show all keys              | help             |
| q / Ctrl+C       | quit                       | quit             |

## Configuration

Settings are read from `~/.scpanel.config` at startup. The file is optional: without it the defaults above are used

```ini
[keys]
# action = keys separated by spaces; replaces the default keys of that action
send = s F5
quit = Q
# empty value removes all keys from the action
sort =

[options]
# initial sort: name, date or size
sort = date
# transfers of this size and larger go through rsync (if it is installed on both machines), smaller ones through scp;
# size with K, M, G or T suffix (1024-based), 0 = always rsync, never = always scp; default 100M
rsync_from = 100M
```

Key names: a single character (`s`, `/`), `F1`..`F12`, `Ctrl+A`..`Ctrl+Z`, `Up`, `Down`, `Left`, `Right`,
`PgUp`, `PgDn`, `Home`, `End`, `Enter`, `Tab`, `Space`, `Esc`, `Backspace`, `Del`, `Ins`.

If a key from the config is already used by another action, the config wins
`cancel` works only while a transfer is running, so it can share keys with other actions
Ctrl+C always cancels a running transfer. Mistakes in the config are shown at startup; `h` shows the keys actually in use
