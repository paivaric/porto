#include <string>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <iomanip>
#include <condition_variable>

#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/path.hpp"
#include "util/log.hpp"
#include "util/namespace.hpp"
#include "unix.hpp"

extern "C" {
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <poll.h>
#include <linux/capability.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
}

bool TTask::Exists() const {
    return Pid && (!kill(Pid, 0) || errno != ESRCH);
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        TError(EError::Unknown, "Task is not running");
    L_ACT() << "kill " << signal << " " << Pid << std::endl;
    if (kill(Pid, signal))
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");
    return TError::Success();
}

bool TTask::IsZombie() const {
    std::string path = "/proc/" + std::to_string(Pid) + "/stat";
    FILE *file;
    char state;
    int res;

    file = fopen(path.c_str(), "r");
    if (!file)
        return false;
    res = fscanf(file, "%*d (%*[^)]) %c", &state);
    fclose(file);
    if (res != 1)
        return false;
    return state == 'Z';
}

pid_t TTask::GetPPid() const {
    std::string path = "/proc/" + std::to_string(Pid) + "/stat";
    int res, ppid;
    FILE *file;

    file = fopen(path.c_str(), "r");
    if (!file)
        return 0;
    res = fscanf(file, "%*d (%*[^)]) %*c %d", &ppid);
    fclose(file);
    if (res != 1)
        return 0;
    return ppid;
}

pid_t GetPid() {
    return getpid();
}

pid_t GetPPid() {
    return getppid();
}

pid_t GetTid() {
    return syscall(SYS_gettid);
}

TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens) {
    struct dirent *de;
    FILE *file;
    DIR *dir;
    int child_pid, parent_pid;

    childrens.clear();

    dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (!dir)
        goto full_scan;

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::to_string(pid) + "/task/" +
                      std::string(de->d_name) + "/children").c_str(), "r");
        if (!file) {
            if (atoi(de->d_name) != pid)
                continue;
            closedir(dir);
            goto full_scan;
        }

        while (fscanf(file, "%d", &child_pid) == 1)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);

    return TError::Success();

full_scan:
    dir = opendir("/proc");
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open /proc");

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::string(de->d_name) + "/stat").c_str(), "r");
        if (!file)
            continue;

        if (fscanf(file, "%d (%*[^)]) %*c %d", &child_pid, &parent_pid) == 2 &&
                parent_pid == pid)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);
    return TError::Success();
}

uint64_t GetCurrentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool WaitDeadline(uint64_t deadline, uint64_t wait) {
    uint64_t now = GetCurrentTimeMs();
    if (!deadline || int64_t(deadline - now) < 0)
        return true;
    if (deadline - now < wait)
        wait = deadline - now;
    if (wait)
        usleep(wait * 1000);
    return false;
}

uint64_t GetTotalMemory() {
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return 0;
    return (uint64_t)si.totalram * si.mem_unit;
}

static __thread std::string *processName;

void SetProcessName(const std::string &name) {
    delete processName;
    processName = nullptr;
    prctl(PR_SET_NAME, (void *)name.c_str());
}

void SetDieOnParentExit(int sig) {
    (void)prctl(PR_SET_PDEATHSIG, sig, 0, 0, 0);
}

std::string GetTaskName(pid_t pid) {
    if (pid) {
        std::string name;
        if (TPath("/proc/" + std::to_string(pid) + "/comm").ReadAll(name, 32))
            return "???";
        return name.substr(0, name.length() - 1);
    }

    if (!processName) {
        char name[17];

        if (prctl(PR_GET_NAME, (void *)name) < 0)
            strncpy(name, program_invocation_short_name, sizeof(name));

        processName = new std::string(name);
    }

    return *processName;
}

TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap) {
    std::vector<std::string> lines;
    TError error = TPath("/proc/" + std::to_string(pid) + "/cgroup").ReadLines(lines);
    if (error)
        return error;

    std::vector<std::string> tokens;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens, 3);
        if (error)
            return error;
        cgmap[tokens[1]] = tokens[2];
    }

    return TError::Success();
}

std::string GetHostName() {
    char buf[HOST_NAME_MAX + 1];
    int ret = gethostname(buf, sizeof(buf));
    if (ret < 0)
        return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

TError SetHostName(const std::string &name) {
    int ret = sethostname(name.c_str(), name.length());
    if (ret < 0)
        return TError(EError::Unknown, errno, "sethostname(" + name + ")");

    return TError::Success();
}

bool FdHasEvent(int fd) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    (void)poll(&pfd, 1, 0);
    return pfd.revents != 0;
}

TError SetOomScoreAdj(int value) {
    return TPath("/proc/self/oom_score_adj").WriteAll(std::to_string(value));
}

std::string FormatExitStatus(int status) {
    if (WIFSIGNALED(status))
        return StringFormat("exit signal: %d (%s)", WTERMSIG(status),
                            strsignal(WTERMSIG(status)));
    return StringFormat("exit code: %d", WEXITSTATUS(status));
}

int GetNumCores() {
    int ncores = sysconf(_SC_NPROCESSORS_CONF);
    if (ncores <= 0) {
        TError error(EError::Unknown, "Can't get number of CPU cores");
        L_ERR() << error << std::endl;
        return 1;
    }

    return ncores;
}

void DumpMallocInfo() {
    struct mallinfo mi = mallinfo();
    L() << "Total non-mapped bytes (arena):\t" << mi.arena << std::endl;
    L() << "# of free chunks (ordblks):\t" << mi.ordblks << std::endl;
    L() << "# of free fastbin blocks (smblks):\t" << mi.smblks << std::endl;
    L() << "# of mapped regions (hblks):\t" << mi.hblks << std::endl;
    L() << "Bytes in mapped regions (hblkhd):\t" << mi.hblkhd << std::endl;
    L() << "Max. total allocated space (usmblks):\t" << mi.usmblks << std::endl;
    L() << "Free bytes held in fastbins (fsmblks):\t" << mi.fsmblks << std::endl;
    L() << "Total allocated space (uordblks):\t" << mi.uordblks << std::endl;
    L() << "Total free space (fordblks):\t" << mi.fordblks << std::endl;
    L() << "Topmost releasable block (keepcost):\t" << mi.keepcost << std::endl;
}

void TUnixSocket::Close() {
    if (SockFd >= 0)
        close(SockFd);
    SockFd = -1;
}

void TUnixSocket::operator=(int sock) {
    Close();
    SockFd = sock;
}

TUnixSocket& TUnixSocket::operator=(TUnixSocket&& Sock) {
    Close();
    SockFd = Sock.SockFd;
    Sock.SockFd = -1;
    return *this;
}

TError TUnixSocket::SocketPair(TUnixSocket &sock1, TUnixSocket &sock2) {
    int sockfds[2];
    int ret, one = 1;

    ret = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockfds);
    if (ret)
        return TError(EError::Unknown, errno, "socketpair(AF_UNIX)");

    if (setsockopt(sockfds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0 ||
        setsockopt(sockfds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0) {
        close(sockfds[0]);
        close(sockfds[1]);
        return TError(EError::Unknown, errno, "setsockopt(SO_PASSCRED)");
    }

    sock1 = sockfds[0];
    sock2 = sockfds[1];
    return TError::Success();
}

TError TUnixSocket::SendInt(int val) const {
    ssize_t ret;

    ret = write(SockFd, &val, sizeof(val));
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot send int");
    if (ret != sizeof(val))
        return TError(EError::Unknown, "partial read of int: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::RecvInt(int &val) const {
    ssize_t ret;

    ret = read(SockFd, &val, sizeof(val));
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot receive int");
    if (ret != sizeof(val))
        return TError(EError::Unknown, "partial read of int: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::SendPid(pid_t pid) const {
    struct iovec iovec = {
        .iov_base = &pid,
        .iov_len = sizeof(pid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    ucred->pid = pid;
    ucred->uid = getuid();
    ucred->gid = getgid();

    ret = sendmsg(SockFd, &msghdr, 0);
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot report real pid");
    if (ret != sizeof(pid))
        return TError(EError::Unknown, "partial sendmsg: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::RecvPid(pid_t &pid, pid_t &vpid) const {
    struct iovec iovec = {
        .iov_base = &vpid,
        .iov_len = sizeof(vpid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));

    ret = recvmsg(SockFd, &msghdr, 0);
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot receive real pid");
    if (ret != sizeof(pid))
        return TError(EError::Unknown, "partial recvmsg: " + std::to_string(ret));
    cmsg = CMSG_FIRSTHDR(&msghdr);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_CREDENTIALS)
        return TError(EError::Unknown, "no credentials after recvmsg");
    pid = ucred->pid;
    return TError::Success();
}

TError TUnixSocket::SendError(const TError &error) const {
    return error.Serialize(SockFd);
}

TError TUnixSocket::RecvError() const {
    TError error;

    TError::Deserialize(SockFd, error);
    return error;
}

TError TUnixSocket::SendFd(int fd) const {
    char data[1];
    struct iovec iovec = {
        .iov_base = data,
        .iov_len = sizeof(data),
    };
    char buffer[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(cmsg)) = fd;

    ssize_t ret = sendmsg(SockFd, &msghdr, 0);

    if (ret <= 0)
        return TError(EError::Unknown, errno, "cannot send fd");
    if (ret != sizeof(data))
        return TError(EError::Unknown, "partial sendmsg: " + std::to_string(ret));

    return TError::Success();
}

TError TUnixSocket::RecvFd(int &fd) const {
    char data[1];
    struct iovec iovec = {
        .iov_base = data,
        .iov_len = sizeof(data),
    };
    char buffer[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    int ret = recvmsg(SockFd, &msghdr, 0);
    if (ret <= 0)
        return TError(EError::Unknown, errno, "cannot receive fd");
    if (ret != sizeof(data))
        return TError(EError::Unknown, "partial recvmsg: " + std::to_string(ret));

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr); cmsg;
         cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {

        if ((cmsg->cmsg_level == SOL_SOCKET) &&
            (cmsg->cmsg_type == SCM_RIGHTS)) {
            fd = *((int*) CMSG_DATA(cmsg));
            return TError::Success();
        }
    }
    return TError(EError::Unknown, "no rights after recvmsg");
}

TError TUnixSocket::SetRecvTimeout(int timeout_ms) const {
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(SockFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))
        return TError(EError::Unknown, errno, "setsockopt(SO_RCVTIMEO)");

    return TError::Success();
}

TError SetSysctl(const std::string &name, const std::string &value) {
    std::string path = "/proc/sys/" + name;
    std::replace(path.begin() + 10, path.end(), '.', '/');
    L_ACT() << "Set sysctl " << name << " = " << value << std::endl;
    return TPath(path).WriteAll(value);
}

TError TranslatePid(pid_t pid, pid_t pidns, pid_t &result) {
    TUnixSocket sock, sk;
    TNamespaceFd ns;
    TError error;
    pid_t child;

    error = TUnixSocket::SocketPair(sock, sk);
    if (error)
        return error;
    error = ns.Open(pidns, "ns/pid");
    if (error)
        return error;
    child = fork();
    if (child < 0)
        return TError(EError::Unknown, errno, "fork");
    if (child) {
        error = sock.RecvPid(result, pid);
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return error;
    }
    error = ns.SetNs(CLONE_NEWPID);
    if (error)
        _exit(EXIT_FAILURE);
    child = fork();
    if (child < 0)
        _exit(EXIT_FAILURE);
    if (!child)
        sk.SendPid(pid);
    else
        waitpid(child, nullptr, 0);
    _exit(0);
}

static std::mutex ForkLock;
static bool PostFork = false;
static struct timeval ForkTime;
static struct tm ForkLocalTime;

static std::map<pid_t, TTask *> Tasks;
static std::condition_variable TasksCV;

// after that fork use only syscalls and signal-safe functions
TError TTask::Fork(bool detach) {
    PORTO_ASSERT(!PostFork);
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    gettimeofday(&ForkTime, NULL);
    localtime_r(&ForkTime.tv_sec, &ForkLocalTime);
    pid_t ret = fork();
    if (ret < 0)
        return TError(EError::Unknown, errno, "TTask::Fork");
    Pid = ret;
    if (!Pid)
        PostFork = true;
    else if (!detach)
        Tasks[Pid] = this;
    return TError::Success();
}

TError TTask::Wait() {
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    if (Pid) {
        pid_t pid = Pid;
        int status;
        lock.unlock();
        /* main thread could be blocked on lock that we're holding */
        if (waitpid(pid, &status, 0) == pid)
            pid = 0;
        lock.lock();
        if (!pid) {
            Tasks.erase(Pid);
            Pid = 0;
            Status = status;
        }
    }
    while (Pid) {
        if (kill(Pid, 0) && errno == ESRCH) {
            Tasks.erase(Pid);
            Pid = 0;
            Status = 100;
            return TError(EError::Unknown, "task not found");
        }
        if (Tasks.find(Pid) == Tasks.end())
            return TError(EError::Unknown, "detached task");
        TasksCV.wait(lock);
    }
    if (Status)
        return TError(EError::Unknown, FormatExitStatus(Status));
    return TError::Success();
}

void TTask::Detach() {
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    Tasks.erase(Pid);
}

bool TTask::Deliver(pid_t pid, int status) {
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    auto it = Tasks.find(pid);
    if (it == Tasks.end())
        return false;
    it->second->Pid = 0;
    it->second->Status = status;
    lock.unlock();
    TasksCV.notify_all();
    return true;
}

// localtime_r isn't safe after fork because of lock inside
static void CurrentTime(struct timeval &tv, struct tm &tm) {
    gettimeofday(&tv, NULL);
    if (!PostFork) {
        localtime_r(&tv.tv_sec, &tm);
    } else {
        struct timeval delta;
        time_t diff;

        timersub(&tv, &ForkTime, &delta);
        tm = ForkLocalTime;
        diff = tm.tm_sec + delta.tv_sec;
        tm.tm_sec = diff % 60;
        diff = tm.tm_min + diff / 60;
        tm.tm_min = diff % 60;
        diff = tm.tm_hour + diff / 60;
        tm.tm_hour = diff % 24;
        tm.tm_mday += diff / 24;
    }
}

std::string CurrentTimeFormat(const char *fmt, bool msec) {
    std::stringstream ss;
    struct timeval tv;
    struct tm tm;
    char buf[256];

    CurrentTime(tv, tm);
    strftime(buf, sizeof(buf), fmt, &tm);
    ss << buf;

    if (msec)
        ss << "," << std::setw(6) << std::setfill('0') << tv.tv_usec;

    return ss.str();
}

TError TPidFile::Load() {
    std::string str;
    TError error;
    int pid;

    Pid = 0;
    error = Path.ReadAll(str, 32);
    if (error)
        return error;
    error = StringToInt(str, pid);
    if (error)
        return error;
    if (kill(pid, 0) && errno == ESRCH)
        return TError(EError::Unknown, errno, "Task not found");
    str = GetTaskName(pid);
    if (str != Name)
        return TError(EError::Unknown, "Wrong task name: " + str + " expected: " + Name);
    Pid = pid;
    return TError::Success();
}

bool TPidFile::Running() {
    if (Pid && (!kill(Pid, 0) || errno != ESRCH) && GetTaskName(Pid) == Name)
        return true;
    Pid = 0;
    return false;
}

TError TPidFile::Save(pid_t pid) {
    TFile file;
    TError error = file.CreateTrunc(Path, 0644);
    if (error)
        return error;
    error = file.WriteAll(std::to_string(pid));
    if (error)
        return error;
    Pid = pid;
    return TError::Success();
}

TError TPidFile::Remove() {
    Pid = 0;
    return Path.Unlink();
}
