#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file/pipe.h>

#include <signal.h>
#include <sys/event.h>

namespace dist_clang {
namespace base {

bool ProcessImpl::Run(ui16 sec_timeout, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  Pipe out, err;
  if (!out.IsValid()) {
    out.GetError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, nullptr);
  } else if (child_pid != -1) {  // Main process.
    out[1].Close();
    err[1].Close();

    Handle kq(kqueue());
    if (!kq.IsValid()) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    const int MAX_EVENTS = 2;
    struct kevent events[MAX_EVENTS];
    EV_SET(events + 0, out[0].native(), EVFILT_READ, EV_ADD, 0, 0, &out[0]);
    EV_SET(events + 1, err[0].native(), EVFILT_READ, EV_ADD, 0, 0, &err[0]);
    if (kevent(kq.native(), events, MAX_EVENTS, nullptr, 0, nullptr) == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    struct timespec timeout = {sec_timeout, 0};
    struct timespec* timeout_ptr = nullptr;
    if (sec_timeout != UNLIMITED) {
      timeout_ptr = &timeout;
    }
    size_t stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;

    int exhausted_fds = 0;
    while (exhausted_fds < 2 && !killed_) {
      auto event_count =
          kevent(kq.native(), nullptr, 0, events, MAX_EVENTS, timeout_ptr);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        auto* fd = reinterpret_cast<Handle*>(events[i].udata);

        if (events[i].filter == EVFILT_READ && events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = UniquePtr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd->native(), buffer.get(), buffer_size);
          if (!bytes_read) {
            EV_SET(events + i, fd->native(), EVFILT_READ, EV_DELETE, 0, 0, 0);
            kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
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
        } else if (events[i].filter == EVFILT_READ &&
                   events[i].flags & EV_EOF) {
          EV_SET(events + i, fd->native(), EVFILT_READ, EV_DELETE, 0, 0, 0);
          kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
          exhausted_fds++;
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    int status;
    CHECK(waitpid(child_pid, &status, 0) == child_pid);
    return !WEXITSTATUS(status) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

bool ProcessImpl::Run(ui16 sec_timeout, Immutable input, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  Pipe in, out, err;
  if (!in.IsValid()) {
    in.GetError(error);
    return false;
  }
  if (!out.IsValid()) {
    out.GetError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, &in);
  } else if (child_pid != -1) {  // Main process.
    in[0].Close();
    out[1].Close();
    err[1].Close();

    in[1].MakeNonBlocking();

    Handle kq(kqueue());
    if (!kq.IsValid()) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    const int MAX_EVENTS = 3;
    struct kevent events[MAX_EVENTS];
    EV_SET(events + 0, in[1].native(), EVFILT_WRITE, EV_ADD, 0, 0, &in[1]);
    EV_SET(events + 1, out[0].native(), EVFILT_READ, EV_ADD, 0, 0, &out[0]);
    EV_SET(events + 2, err[0].native(), EVFILT_READ, EV_ADD, 0, 0, &err[0]);
    if (kevent(kq.native(), events, MAX_EVENTS, nullptr, 0, nullptr) == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    struct timespec timeout = {sec_timeout, 0};
    struct timespec* timeout_ptr = nullptr;
    if (sec_timeout != UNLIMITED) {
      timeout_ptr = &timeout;
    }
    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;

    int exhausted_fds = 0;
    while (exhausted_fds < 3 && !killed_) {
      auto event_count =
          kevent(kq.native(), nullptr, 0, events, MAX_EVENTS, timeout_ptr);

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
        auto* fd = reinterpret_cast<Handle*>(events[i].udata);

        if (events[i].filter == EVFILT_READ && events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = UniquePtr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd->native(), buffer.get(), buffer_size);
          if (!bytes_read) {
            EV_SET(events + i, fd->native(), EVFILT_READ, EV_DELETE, 0, 0, 0);
            kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
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
        } else if (events[i].filter == EVFILT_WRITE && events[i].data) {
          DCHECK(fd == &in[1]);

          auto bytes_sent = write(fd->native(), input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            EV_SET(events + i, fd->native(), EVFILT_WRITE, EV_DELETE, 0, 0, 0);
            kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
            exhausted_fds++;
          } else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              EV_SET(events + i, fd->native(), EVFILT_WRITE, EV_DELETE, 0, 0,
                     0);
              kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
              in[1].Close();
              exhausted_fds++;
            }
          }
        } else if (events[i].flags & EV_EOF) {
          EV_SET(events + i, fd->native(), events[i].filter, EV_DELETE, 0, 0,
                 0);
          kevent(kq.native(), events + i, 1, nullptr, 0, nullptr);
          exhausted_fds++;
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    int status;
    CHECK(waitpid(child_pid, &status, 0) == child_pid);
    return !WEXITSTATUS(status) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

}  // namespace base
}  // namespace dist_clang
