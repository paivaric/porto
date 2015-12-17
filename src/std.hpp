#pragma once

#include <string>

#include <util/path.hpp>

constexpr const char* STD_TYPE_FILE = "file";
constexpr const char* STD_TYPE_FIFO = "fifo";
constexpr const char* STD_TYPE_PTY = "pty";

class TStdStream {
private:
    int Type; /* 0 - stdin, 1 - stdout, 2 - stderr */
    std::string Impl; /* file/pipe/pty */
    TPath PathOnHost;
    TPath PathInContainer;
    bool ManagedByPorto;
    int PipeFd;

    TError Open(const TPath &path, const TCred &cred) const;

public:
    TStdStream();
    TStdStream(int type, const std::string &impl,
               const TPath &inner_path, const TPath &host_path,
               bool managed_by_porto);

    TError Prepare(const TCred &cred);

    TError OpenOnHost(const TCred &cred) const; // called in child, but host ns
    TError OpenInChild(const TCred &cred) const; // called before actual execve

    TError Rotate(off_t limit, off_t &loss) const;
    TError Cleanup() const;

    TError Read(std::string &text, off_t limit, uint64_t base,
                const std::string &start_offset = "") const;
};
