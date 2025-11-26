#pragma once
#include <string>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>

namespace ipc
{

  inline void safeUnlink(const std::string &p) { ::unlink(p.c_str()); }

  inline void makeFifo(const std::string &p)
  {
    safeUnlink(p);
    if (::mkfifo(p.c_str(), 0666) != 0)
    {
      std::perror(("mkfifo " + p).c_str());
      std::exit(1);
    }
  }

  inline int openFifoRDWR(const std::string &p)
  {
    int fd = ::open(p.c_str(), O_RDWR);
    if (fd < 0)
    {
      std::perror(("open " + p).c_str());
      std::exit(2);
    }
    return fd;
  }

  inline void writeAll(int fd, const std::string &s)
  {
    const char *p = s.c_str();
    size_t left = s.size();
    while (left)
    {
      ssize_t n = ::write(fd, p, left);
      if (n < 0)
      {
        if (errno == EINTR)
          continue;
        std::perror("write");
        std::exit(3);
      }
      p += (size_t)n;
      left -= (size_t)n;
    }
  }

  inline void writeLine(int fd, const std::string &s)
  {
    writeAll(fd, s);
    writeAll(fd, "\n");
  }

  inline bool readLineFd(int fd, std::string &out)
  {
    out.clear();
    char c = 0;
    while (true)
    {
      ssize_t n = ::read(fd, &c, 1);
      if (n == 0)
        return false;
      if (n < 0)
      {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (c == '\n')
        break;
      if (c != '\r')
        out.push_back(c);
    }
    return true;
  }

  inline bool tryReadControl(int reqFd, std::string &cmd, std::string &payload)
  {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(reqFd, &set);
    timeval tv{0, 0};
    int rv = select(reqFd + 1, &set, nullptr, nullptr, &tv);
    if (rv <= 0)
      return false;

    std::string line;
    if (!readLineFd(reqFd, line))
      return false;

    std::istringstream iss(line);
    iss >> cmd;
    std::getline(iss, payload);
    if (!payload.empty() && payload[0] == ' ')
      payload.erase(0, 1);

    return (cmd == "CANCEL" || cmd == "STOP");
  }

  struct TermiosGuard
  {
    termios oldt{};
    bool active = false;

    void enableRaw()
    {
      if (tcgetattr(STDIN_FILENO, &oldt) == 0)
      {
        termios t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
          active = true;
      }
    }
    ~TermiosGuard()
    {
      if (active)
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }
  };

  inline bool readKeyNonBlocking(char &ch)
  {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval tv{0, 100000}; // 100ms
    int rv = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv);
    if (rv > 0 && FD_ISSET(STDIN_FILENO, &set))
    {
      char c;
      ssize_t n = ::read(STDIN_FILENO, &c, 1);
      if (n == 1)
      {
        ch = c;
        return true;
      }
    }
    return false;
  }

}
