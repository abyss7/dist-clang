#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file/epoll.h>
#include <base/file/pipe.h>
#include <base/logging.h>

#include <base/using_log.h>

namespace dist_clang {
namespace base {

bool ProcessImpl::Run(ui16 sec_timeout, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  Pipe out, err;
  if (!out.IsValid()) {
    out.GetCreationError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetCreationError(error);
    return false;
  }

  LOG(VERBOSE) << "Running process: " << exec_path_ << " " << args_;

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, nullptr);
  } else if (child_pid != -1) {  // Main process.
    out[1].Close();
    err[1].Close();

    Epoll epoll;
    if (!epoll.IsValid()) {
      epoll.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(out[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(err[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct epoll_event, 2> events;

    int epoll_timeout = sec_timeout == UNLIMITED ? -1 : sec_timeout * 1000;
    int exhausted_fds = 0;
    while (exhausted_fds < 2 && !killed_) {
      auto event_count = epoll.Wait(events, epoll_timeout);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          GetLastError(error);
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        if (error) {
          error->assign("Timeout occured");
        }
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        auto* fd = reinterpret_cast<Data*>(events[i].data.ptr);

        if (events[i].events & EPOLLIN) {
          int bytes_available = 0;
          if (!fd->ReadyForRead(bytes_available, error)) {
            kill(child_pid);
            break;
          }

          auto buffer = UniquePtr<char[]>(new char[bytes_available]);
          auto bytes_read = read(fd->native(), buffer.get(), bytes_available);
          if (!bytes_read) {
            epoll.Delete(*fd);
            exhausted_fds++;
          } else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          } else {
            if (fd == &out[0]) {
              stdout.emplace_back(buffer, bytes_read);
              stdout_size += bytes_read;
            } else {
              stderr.emplace_back(buffer, bytes_read);
              stderr_size += bytes_read;
            }
          }
        } else {
          epoll.Delete(*fd);
          exhausted_fds++;
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    out[0].Close();
    err[0].Close();

    return WaitPid(child_pid, sec_timeout, error) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

bool ProcessImpl::Run(ui16 sec_timeout, Immutable input, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  Pipe in, out, err;
  if (!in.IsValid()) {
    in.GetCreationError(error);
    return false;
  }
  if (!out.IsValid()) {
    out.GetCreationError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetCreationError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, &in);
  } else if (child_pid != -1) {  // Main process.
    in[0].Close();
    out[1].Close();
    err[1].Close();

    in[1].MakeBlocking(false);

    Epoll epoll;
    if (!epoll.IsValid()) {
      epoll.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    if (!epoll.Add(in[1], EPOLLOUT, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(out[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(err[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct epoll_event, 3> events;

    int epoll_timeout = sec_timeout == UNLIMITED ? -1 : sec_timeout * 1000;
    int exhausted_fds = 0;
    while (exhausted_fds < 3 && !killed_) {
      auto event_count = epoll.Wait(events, epoll_timeout);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          GetLastError(error);
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        if (error) {
          error->assign("Timeout occured");
        }
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        auto* fd = reinterpret_cast<Data*>(events[i].data.ptr);

        if (events[i].events & EPOLLIN) {
          int bytes_available = 0;
          if (!fd->ReadyForRead(bytes_available, error)) {
            kill(child_pid);
            break;
          }

          auto buffer = UniquePtr<char[]>(new char[bytes_available]);
          auto bytes_read = read(fd->native(), buffer.get(), bytes_available);
          if (!bytes_read) {
            epoll.Delete(*fd);
            exhausted_fds++;
          } else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          } else {
            if (fd == &out[0]) {
              stdout.emplace_back(buffer, bytes_read);
              stdout_size += bytes_read;
            } else if (fd == &err[0]) {
              stderr.emplace_back(buffer, bytes_read);
              stderr_size += bytes_read;
            }
          }
        } else if (events[i].events & EPOLLOUT) {
          DCHECK(fd == &in[1]);

          auto bytes_sent = write(in[1].native(), input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            epoll.Delete(in[1]);
            in[1].Close();
            exhausted_fds++;
          } else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              epoll.Delete(in[1]);
              in[1].Close();
              exhausted_fds++;
            }
          }
        } else {
          epoll.Delete(*fd);
          exhausted_fds++;
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    out[0].Close();
    err[0].Close();

    return WaitPid(child_pid, sec_timeout, error) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

}  // namespace base
}  // namespace dist_clang
