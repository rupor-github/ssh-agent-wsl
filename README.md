ssh-agent-wsl
--------------

Since Windows April update official OpenSSH port exited beta (and it has been available for a long time). It has very
convenient ssh-agent service (with support for persistence and Windows security). Unfortunately is not readily available from WSL.
This project aims to correct this situation accessing SSH keys held by Windows own ssh-agent service from inside the
[Windows Subsystem for Linux](https://msdn.microsoft.com/en-us/commandline/wsl/about).

The source (and this documentation) here is heavily based on
[`weasel-pageant`](https://github.com/vuori/weasel-pageant) 1.1.1 by Valtteri Vuorikoski, which is based on
[`ssh-pageant`](https://github.com/cuviper/ssh-pageant) 1.4 by Josh Stone.

`ssh-agent-wsl` works like `ssh-agent`, except that it leaves the key storage to
Windows ssh-agent service. It sets up an authentication socket and prints the environment
variables, which allows the OpenSSH client to use it. It works by executing from the
WSL side a Win32 helper program which interfaces with Windows service communicating with
it through pipes.

This allows you to share set of SSH keys between multiple WSL and Windows SSH sessions easily.

**SECURITY NOTICE:** All the usual security caveats applicable to WSL apply.
Most importantly, all interaction with the Win32 world happens with the credentials of
the user who started the WSL environment. In practice, *if you allow someone else to
log in to your WSL environment remotely, they may be able to access the SSH keys stored in
your ssh-agent with `ssh-agent-wsl`.* This is a fundamental feature of WSL; if you
are not sure of what you're doing, do not allow remote access to your WSL environment
(i.e. by starting an SSH server).

**COMPATIBILITY NOTICE:** `ssh-agent-wsl` was tested on Windows 10 1803 (April Update) and should work on anything starting with
1703 (Creators update) but would not work on a version of Windows 10 older than 1703, because
it requires the new [Windows/Ubuntu interoperability support](https://blogs.msdn.microsoft.com/wsl/2016/10/19/windows-and-ubuntu-interoperability/)
feature shipped with version 1703.

Non-Ubuntu distributions (available since 1709) have not been tested, but they should work as well.

## Installation

### From binaries

Download the archive from the [releases page](https://github.com/rupor-github/ssh-agent-wsl/releases)
and unpack it in a convenient location *on the Windows part of your drive*.
Because WSL can only execute Win32 binaries from `drvfs` locations, `ssh-agent-wsl`
*will not work* if unpacked inside the WSL filesystem (onto an `lxfs` mount).
(Advanced users may place only `helper.exe` on `drvfs`, but in general it is easier
to keep the pieces together.)

### From source

Everything could be build under WSL. Windows binary requires MinGW installed, so do something like
`sudo apt install build-essential cmake mingw-w64`

To build everything execute (or use `./build-release.sh`):

```
cd linux
mkdir build
cd build
cmake ..
make install
cd ../..
cd win32
mkdir build
cd build
cmake ..
make install
cd ../..
```

Results will be available in `./bin` directory.

The release binaries have been built on Ubuntu 16.04 WSL.

## Usage

Using `ssh-agent-wsl` is very similar to using `ssh-agent` on Linux and similar operating systems.

1. Ensure that Windows ssh-agent service is started (you may want to switch its startup mode to "automatic").
2. Edit your `~/.bashrc` (or `~/.bash_profile`) to add the following:

        eval $(<location where you unpacked the zip>/ssh-agent-wsl -r)

    To explain:

    * This leverages the `-r`/`--reuse` option which will only start a new daemon if
      one is not already running in the current window. If the agent socket appears to
      be active, it will just print environment variables and exit.

    * Using `eval` will set the environment variables in the current shell.
      By default, `ssh-agent-wsl` tries to detect the current shell and output
      appropriate commands. If detection fails, then use the `-S SHELL` option
      to define a shell type manually.

3. Restart your shell or type (when using bash) `. ~/.bashrc`. Typing `ssh-add -l`
   should now list the keys you have registered in Windows ssh-agent.

You may even replace your WSL copy of ssh-agent with ssh-agent-wsl (renaming or linking it) to avoid modifying your scripts.
I am using excellent [oh-my-zsh](https://github.com/robbyrussell/oh-my-zsh) and have slightly modified version of `ssh-agent` plugin for this purpose:
```
function _start_agent() {
	echo starting ssh-agent-wsl...
	ssh-agent-wsl -s | sed 's/^echo/#echo/' >! $_ssh_env_cache
	chmod 600 $_ssh_env_cache
	. $_ssh_env_cache > /dev/null
}
```

After adding keys to Windows ssh-agent you may remove them from your home .ssh directory (keys are securely persisted in Windows
registry, available for your account only) - do not forget to adjust IdentitiesOnly directive in your ssh config accordingly).

NOTE: do not mix usage of ssh-agent-wsl and ssh-agent, only one of them should be used - they are using the same environment
variables.

## Options

`ssh-agent-wsl` aims to be compatible with `ssh-agent` options, with a few extras:

    $ ssh-agent-wsl -h
    Usage: ssh-agent-wsl [options] [command [arg ...]]
    Options:
      -h, --help     Show this help.
      -v, --version  Display version information.
      -c             Generate C-shell commands on stdout.
      -s             Generate Bourne shell commands on stdout.
      -S SHELL       Generate shell command for "bourne", "csh", or "fish".
      -k             Kill the current ssh-agent-wsl.
      -d             Enable debug mode.
      -q             Enable quiet mode.
      -a SOCKET      Create socket on a specific path.
      -r, --reuse    Allow to reuse an existing -a SOCKET.
      -H, --helper   Path to the Win32 helper binary (default: /mnt/e/projects/misc/ssh-agent-wsl/bin/pipe-connector.exe).
      -t TIME        Limit key lifetime in seconds (not supported by Windows port of ssh-agent).

By default, the Win32 helper will be searched for in the same directory where `ssh-agent-wsl`
is stored. If you have placed it elsewhere, the `-H` flag can be used to set the location.

## Known issues

* If you have an `SSH_AUTH_SOCK` variable set inside `screen`, `tmux` or similar,
  you exit the WSL console from which the `screen` was *initially started* and attach
  to the session from another window, the agent connection will not be usable. This is
  due to WSL/Win32 interop limitations.

* There is a slight delay when exiting a WSL console before the window actually closes.
  This is due to a polling loop which works around a WSL incompatibility with Unix session
  semantics.

## Uninstallation

To uninstall, just remove the extracted files and any modifications you made
to your shell initialization files (e.g. `.bashrc`).

------------------------------------------------------------------------------

Based on `weasel-pegeant` Copyright 2017, 2018  Valtteri Vuorikoski.

Based on `ssh-pageant`, copyright (C) 2009-2014  Josh Stone.

Licensed under the GNU GPL version 3 or later, http://gnu.org/licenses/gpl.html

This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.

See the `COPYING` file for license details.
