#include <unordered_map>

#include "log.hpp"
#include "util/unix.hpp"
#include "util/folder.hpp"

extern "C" {
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
}

using namespace std;

TError TFolder::Create(mode_t mode, bool recursive) {
    TLogger::Log("mkdir " + path);

    if (recursive) {
        string copy(path);
        char *dup = strdup(copy.c_str());
        char *p = dirname(dup);
        TFolder f(p);
        free(dup);
        if (!f.Exists()) {
            TError error(f.Create(mode, true));
            if (error)
                return error;
        }
    }

    if (mkdir(path.c_str(), mode) < 0)
        return TError(EError::Unknown, errno, "mkdir(" + path + ", " + to_string(mode) + ")");

    return TError::Success();
}

TError TFolder::Remove(bool recursive) {
    if (recursive) {
        vector<string> items;
        TError error = Items(TFile::Any, items);
        if (error)
            return error;

        for (auto f : items) {
            TFile child(f);
            TError error;

            if (child.Type() == TFile::Directory)
                error = TFolder(f).Remove(recursive);
            else
                error = child.Remove();

            if (error)
                return error;
        }
    }

    TLogger::Log("rmdir " + path);

    int ret = RetryBusy(10, 100, [&]{ return rmdir(path.c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "rmdir(" + path + ")");

    return TError::Success();
}

bool TFolder::Exists() {
    struct stat st;

    int ret = stat(path.c_str(), &st);

    if (ret == 0 && S_ISDIR(st.st_mode))
        return true;
    else
        return false;
}

TError TFolder::Subfolders(std::vector<std::string> &list) {
    return Items(TFile::Directory, list);
}

TError TFolder::Items(const TFile::EFileType type, std::vector<std::string> &list) {
    DIR *dirp;
    struct dirent dp, *res;

    dirp = opendir(path.c_str());
    if (!dirp)
        return TError(EError::Unknown, "Cannot open directory " + path);

    while (!readdir_r(dirp, &dp, &res) && res != nullptr) {
        if (string(".") == res->d_name || string("..") == res->d_name)
            continue;

        static unordered_map<unsigned char, TFile::EFileType> d_type_to_type =
            {{DT_UNKNOWN, TFile::Unknown},
             {DT_FIFO, TFile::Fifo},
             {DT_CHR, TFile::Character},
             {DT_DIR, TFile::Directory},
             {DT_BLK, TFile::Block},
             {DT_REG, TFile::Regular},
             {DT_LNK, TFile::Link},
             {DT_SOCK, TFile::Socket}};

        if (type == TFile::Any || type == d_type_to_type[res->d_type])
            list.push_back(string(res->d_name));
    }

    closedir(dirp);
    return TError::Success();
}
