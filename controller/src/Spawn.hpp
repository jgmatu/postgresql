#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace pgdemo::controller {

/**
 * @brief `fork()` + `execve(argv[0], argv)`. The child does not inherit `ROS_SECURITY_*`.
 *
 * The argument vector is built before forking. Between fork and exec the child
 * only calls async-signal-safe functions: it restores default dispositions and
 * an empty signal mask, asks for SIGTERM when the forking thread dies
 * (`PR_SET_PDEATHSIG`), and closes every descriptor above stderr.
 *
 * Call it from a thread that lives as long as the children: the death signal
 * is tied to the thread that forked, not to the process.
 *
 * @param argv program path followed by its arguments
 * @return the child's pid
 * @throws std::system_error if fork fails
 */
pid_t spawn_process(const std::vector<std::string>& argv);

} // namespace pgdemo::controller
