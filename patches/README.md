# libpqxx patch: blocking `wait_notification()`

`libpqxx.patch.full` applies to the upstream **libpqxx 8.0.0** release tarball. CMake applies it automatically during `FetchContent` (see [`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake)).

## Why

Upstream offers `connection::await_notification()`. It waits on the connection socket, but **it wakes up by itself from time to time**: `pqxx::internal::wait_fd()` defaults to a timeout of 1 second. A listener thread written around it spins once per second even when nothing happens.

This demo dedicates one connection and one thread to `LISTEN`. That thread should sleep in the kernel until PostgreSQL sends data on the socket, and do nothing otherwise.

## What the patch changes

| File | Change |
|---|---|
| `include/pqxx/connection.hxx` | Declares `int wait_notification(sl = sl::current())`. |
| `src/connection.cxx` | Implements it. It first drains notifications that are already buffered (`get_notifs()`). If there are none, it calls `wait_fd(socket, read=true, write=false, -1, 0)` and then runs `get_notifs()` again. |
| `include/pqxx/internal/wait.hxx` | `wait_fd()` takes `int seconds` with default `-1` instead of `unsigned seconds = 1`. |
| `src/wait.cxx` | A negative `seconds` value means "no timeout" in all three back ends: `poll(fd, 1, -1)`, `WSAPoll(..., -1)` and `select(..., NULL)`. A non-negative value keeps the original behaviour. |

`wait_notification()` returns when:

- one or more notifications were delivered to their handlers (the return value is the count);
- the socket became readable for another reason (for example, a server notice); the function then returns 0;
- the backend went away; `get_notifs()` then throws `pqxx::broken_connection`. The listener thread uses that exception as its shutdown signal.

## Licence

libpqxx is distributed under the BSD 3-Clause licence. Copyright (c) 2000-2026 Jeroen T. Vermeulen. The patch is a derivative of libpqxx source and is provided under the same terms. See <https://github.com/jtv/libpqxx/blob/master/COPYING>.
