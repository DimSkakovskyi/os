#include <iostream>
#include <string>
#include <sstream>
#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <limits>
#include <stdexcept>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc_fifo.hpp"

static std::atomic<bool> g_cancel{false};
static std::atomic<bool> g_exitInput{false};

static std::mutex g_reasonMx;
static std::string g_reason;

static std::mutex g_ioMx;

template <class F>
static void withConsole(F &&f)
{
  std::lock_guard<std::mutex> lk(g_ioMx);
  f();
  std::cout.flush();
}

static void setReason(const std::string &r)
{
  std::lock_guard<std::mutex> lk(g_reasonMx);
  g_reason = r;
}
static std::string getReason()
{
  std::lock_guard<std::mutex> lk(g_reasonMx);
  return g_reason;
}

enum class WorkerState
{
  Starting,
  Idle,
  Working,
  Cancelled,
  Exited
};

static const char *stateStr(WorkerState s)
{
  switch (s)
  {
  case WorkerState::Starting:
    return "STARTING";
  case WorkerState::Idle:
    return "IDLE";
  case WorkerState::Working:
    return "WORKING";
  case WorkerState::Cancelled:
    return "CANCELLED";
  case WorkerState::Exited:
    return "EXITED";
  }
  return "?";
}

// ---- Сповільнення (щоб встигнути натиснути Cancel) ----
static const int POW_STEP_MS = 300; // 0.3s на крок степеня
static const int POLY_STEPS = 120;  // 120 кроків
static const int POLY_STEP_MS = 50; // 50ms -> ~6 секунд

static double pow_iter(double x, int p, int reqFd)
{
  double r = 1.0;
  for (int i = 0; i < p; ++i)
  {
    std::string cmd, payload;
    if (ipc::tryReadControl(reqFd, cmd, payload))
    {
      if (cmd == "STOP")
        throw std::runtime_error("STOP");
      throw std::runtime_error(std::string("CANCEL ") + payload);
    }
    r *= x;
    std::this_thread::sleep_for(std::chrono::milliseconds(POW_STEP_MS));
  }
  return r;
}

static double poly2(double x, double a, double b, double c, int reqFd)
{
  for (int k = 0; k < POLY_STEPS; ++k)
  {
    std::string cmd, payload;
    if (ipc::tryReadControl(reqFd, cmd, payload))
    {
      if (cmd == "STOP")
        throw std::runtime_error("STOP");
      throw std::runtime_error(std::string("CANCEL ") + payload);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(POLY_STEP_MS));
  }
  return a * x * x + b * x + c;
}

static void workerLoop(const std::string &reqPath, const std::string &resPath)
{
  int reqFd = ipc::openFifoRDWR(reqPath);
  int resFd = ipc::openFifoRDWR(resPath);

  ipc::writeLine(resFd, "READY");

  std::string line;
  while (ipc::readLineFd(reqFd, line))
  {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    try
    {
      if (cmd == "POW")
      {
        double x;
        int p;
        iss >> x >> p;
        double y = pow_iter(x, p, reqFd);
        ipc::writeLine(resFd, std::string("OK ") + std::to_string(y));
      }
      else if (cmd == "POLY")
      {
        double x, a, b, c;
        iss >> x >> a >> b >> c;
        double y = poly2(x, a, b, c, reqFd);
        ipc::writeLine(resFd, std::string("OK ") + std::to_string(y));
      }
      else if (cmd == "CANCEL")
      {
        std::string reason;
        std::getline(iss, reason);
        if (!reason.empty() && reason[0] == ' ')
          reason.erase(0, 1);
        ipc::writeLine(resFd, std::string("CANCELLED ") + reason);
        break;
      }
      else if (cmd == "STOP")
      {
        ipc::writeLine(resFd, "STOPPED");
        break;
      }
      else
      {
        ipc::writeLine(resFd, "ERR unknown_command");
      }
    }
    catch (const std::exception &e)
    {
      std::string msg = e.what();
      if (msg.rfind("STOP", 0) == 0)
      {
        ipc::writeLine(resFd, "STOPPED");
        break;
      }
      if (msg.rfind("CANCEL", 0) == 0)
      {
        std::string reason = msg.size() > 7 ? msg.substr(7) : "";
        if (!reason.empty() && reason[0] == ' ')
          reason.erase(0, 1);
        ipc::writeLine(resFd, std::string("CANCELLED ") + reason);
        break;
      }
      ipc::writeLine(resFd, std::string("ERR ") + msg);
    }
  }

  ::close(reqFd);
  ::close(resFd);
  std::exit(0);
}

struct WorkerCtl
{
  int id = 0;
  pid_t pid = -1;
  std::string reqPath, resPath;
  int reqFd = -1;
  int resFd = -1;
  std::atomic<WorkerState> state{WorkerState::Starting};
};

static double readOkOrThrow(int resFd)
{
  std::string line;
  while (ipc::readLineFd(resFd, line))
  {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "READY")
      continue;

    if (cmd == "OK")
    {
      double v;
      iss >> v;
      return v;
    }
    if (cmd == "CANCELLED")
      throw std::runtime_error("CANCELLED");
    if (cmd == "STOPPED")
      throw std::runtime_error("STOPPED");

    if (cmd == "ERR")
    {
      std::string rest;
      std::getline(iss, rest);
      if (!rest.empty() && rest[0] == ' ')
        rest.erase(0, 1);
      throw std::runtime_error("ERR " + rest);
    }
  }
  throw std::runtime_error("EOF");
}

static void printStatus(const WorkerCtl &w1, const WorkerCtl &w2)
{
  std::cout << "\n--- STATUS ---\n";
  std::cout << "worker#1 state=" << stateStr(w1.state.load()) << "\n";
  std::cout << "worker#2 state=" << stateStr(w2.state.load()) << "\n";
  std::cout << "cancel=" << (g_cancel.load() ? "true" : "false") << "\n";
  std::cout << "-------------\n";
}

int main()
{
  double x = 0.0;
  int p = 0;
  double a = 0.0, b = 0.0, c = 0.0;

  std::cout << "Enter x: ";
  std::cin >> x;
  std::cout << "Enter p (power): ";
  std::cin >> p;
  std::cout << "Enter a b c for ax^2+bx+c: ";
  std::cin >> a >> b >> c;
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

  std::string base = std::string("/tmp/va_simple_") + std::to_string(::getpid());

  WorkerCtl w1, w2;
  w1.id = 1;
  w2.id = 2;
  w1.reqPath = base + "_req1";
  w1.resPath = base + "_res1";
  w2.reqPath = base + "_req2";
  w2.resPath = base + "_res2";

  ipc::makeFifo(w1.reqPath);
  ipc::makeFifo(w1.resPath);
  ipc::makeFifo(w2.reqPath);
  ipc::makeFifo(w2.resPath);

  w1.pid = fork();
  if (w1.pid == 0)
    workerLoop(w1.reqPath, w1.resPath);

  w2.pid = fork();
  if (w2.pid == 0)
    workerLoop(w2.reqPath, w2.resPath);

  w1.reqFd = ipc::openFifoRDWR(w1.reqPath);
  w1.resFd = ipc::openFifoRDWR(w1.resPath);
  w2.reqFd = ipc::openFifoRDWR(w2.reqPath);
  w2.resFd = ipc::openFifoRDWR(w2.resPath);

  w1.state.store(WorkerState::Idle);
  w2.state.store(WorkerState::Idle);

  std::thread inputThread([&]()
                          {
    ipc::TermiosGuard tg;
    tg.enableRaw();

    withConsole([&]{
      std::cout << "\nKeys: [s]=status  [c]=cancel  [q]=quit\n";
    });

    while (!g_exitInput.load() && !g_cancel.load()) {
      char ch = 0;
      if (!ipc::readKeyNonBlocking(ch)) continue;

      if (ch == 's' || ch == 'S') {
        withConsole([&]{ printStatus(w1, w2); });
      } else if (ch == 'c' || ch == 'C') {
        withConsole([&]{
          printStatus(w1, w2);
          std::cout << "Cancel requested. Are you sure? (y/n): ";
        });

        // ВАЖЛИВО: якщо основний потік завершує — вийти з prompt
        while (true) {
          if (g_exitInput.load() || g_cancel.load()) return;

          char yn = 0;
          if (!ipc::readKeyNonBlocking(yn)) continue;

          withConsole([&]{ std::cout << yn << "\n"; });

          if (yn == 'y' || yn == 'Y') {
            setReason("User requested cancellation (c)");
            g_cancel.store(true);
            return;
          }
          if (yn == 'n' || yn == 'N') {
            withConsole([&]{ std::cout << "Cancel aborted.\n"; });
            break;
          }
        }
      } else if (ch == 'q' || ch == 'Q') {
        setReason("User quit (q)");
        g_cancel.store(true);
        return;
      }
    } });

  w1.state.store(WorkerState::Working);
  w2.state.store(WorkerState::Working);

  {
    std::ostringstream msg;
    msg << "POW " << std::setprecision(15) << x << " " << p;
    ipc::writeLine(w1.reqFd, msg.str());
  }
  {
    std::ostringstream msg;
    msg << "POLY " << std::setprecision(15) << x << " " << a << " " << b << " " << c;
    ipc::writeLine(w2.reqFd, msg.str());
  }

  auto fut1 = std::async(std::launch::async, [&]() -> double
                         {
    try {
      double y = readOkOrThrow(w1.resFd);
      w1.state.store(WorkerState::Idle);
      return y;
    } catch (...) {
      w1.state.store(WorkerState::Cancelled);
      throw;
    } });

  auto fut2 = std::async(std::launch::async, [&]() -> double
                         {
    try {
      double y = readOkOrThrow(w2.resFd);
      w2.state.store(WorkerState::Idle);
      return y;
    } catch (...) {
      w2.state.store(WorkerState::Cancelled);
      throw;
    } });

  while (!g_cancel.load())
  {
    auto r1 = fut1.wait_for(std::chrono::milliseconds(50));
    auto r2 = fut2.wait_for(std::chrono::milliseconds(50));
    if (r1 == std::future_status::ready && r2 == std::future_status::ready)
      break;
  }

  g_exitInput.store(true);
  if (inputThread.joinable())
    inputThread.join();

  if (g_cancel.load())
  {
    std::string reason = getReason();
    w1.state.store(WorkerState::Cancelled);
    w2.state.store(WorkerState::Cancelled);

    ipc::writeLine(w1.reqFd, std::string("CANCEL ") + reason);
    ipc::writeLine(w2.reqFd, std::string("CANCEL ") + reason);

    withConsole([&]
                {
      std::cout << "\nCANCELLED: " << reason << "\n";
      printStatus(w1, w2); });

    try
    {
      (void)fut1.get();
    }
    catch (...)
    {
    }
    try
    {
      (void)fut2.get();
    }
    catch (...)
    {
    }
  }
  else
  {
    try
    {
      double y1 = fut1.get();
      double y2 = fut2.get();
      double combined = y1 * y2;

      withConsole([&]
                  {
        std::cout << "\n";
        std::cout << "f1(x)=x^p = " << std::setprecision(15) << y1 << "\n";
        std::cout << "f2(x)=ax^2+bx+c = " << std::setprecision(15) << y2 << "\n";
        std::cout << "f1(x)*f2(x) = " << std::setprecision(15) << combined << "\n"; });

      ipc::writeLine(w1.reqFd, "STOP");
      ipc::writeLine(w2.reqFd, "STOP");
      w1.state.store(WorkerState::Exited);
      w2.state.store(WorkerState::Exited);
    }
    catch (const std::exception &e)
    {
      setReason(std::string("Error: ") + e.what());
      withConsole([&]
                  { std::cout << "\nError: " << e.what() << "\n"; });
      ipc::writeLine(w1.reqFd, std::string("CANCEL ") + getReason());
      ipc::writeLine(w2.reqFd, std::string("CANCEL ") + getReason());
    }
  }

  if (w1.reqFd >= 0)
    ::close(w1.reqFd);
  if (w1.resFd >= 0)
    ::close(w1.resFd);
  if (w2.reqFd >= 0)
    ::close(w2.reqFd);
  if (w2.resFd >= 0)
    ::close(w2.resFd);

  int st = 0;
  if (w1.pid > 0)
    waitpid(w1.pid, &st, 0);
  if (w2.pid > 0)
    waitpid(w2.pid, &st, 0);

  ipc::safeUnlink(w1.reqPath);
  ipc::safeUnlink(w1.resPath);
  ipc::safeUnlink(w2.reqPath);
  ipc::safeUnlink(w2.resPath);

  return 0;
}
