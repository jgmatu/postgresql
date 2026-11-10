#include "Spawn.hpp"

#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <system_error>

namespace pgdemo::controller {

pid_t spawn_process(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "fork");
    if (pid > 0) return pid;

    // Child. Handlers first, then the mask: a pending signal must not reach
    // the parent's handlers once unblocked.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    for (const int sig : {SIGINT, SIGTERM, SIGCHLD, SIGPIPE}) ::sigaction(sig, &dfl, nullptr);
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent) ::_exit(127);
    ::close_range(3, ~0U, 0);
    ::execv(argv[0], argv.data());
    ::_exit(127);
}

} // namespace pgdemo::controller
