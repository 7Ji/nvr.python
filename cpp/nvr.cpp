#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sendfile.h>
#include <dirent.h>

class Camera {
  public:
    Camera(
        const char * const name,
        const char * const host,
        const char * const path
    ) {
        uint lenName = std::strlen(name);
        if (lenName > _nameMaxLen) {
            throw std::runtime_error("Name too long");
        } else if (lenName == 0) {
            throw std::runtime_error("Name is empty");
        }
        _lenName = lenName;
        std::strncpy(_name, name, lenName);
        std::snprintf(_url, _urlMaxLen, "rtsp://%s/%s", host, path);
        std::snprintf(_pathFormat, _urlMaxLen, "hot/%s_%%Y%%m%%d_%%H%%M%%S.mkv", name);
    }
    void printUrl() {
        std::puts(_url);
    }
    void start() {
        __pid_t pid = fork();
        switch (pid) {
            case -1:
                throw std::runtime_error("Failed to fork");
            case 0:
                break;
            default:
                _pid = pid;
                return;
        }
        time_t timeNow, timeFuture, timeDiff;
        tm tmStructNow, tmStructFuture;
        int minute;
        char path[_urlMaxLen];
        int status;
        int r;
        std::vector<__pid_t>::iterator iter;
        while (true) {
            timeNow = time(NULL);
            localtime_r(&timeNow, &tmStructNow);
            strftime(path, _urlMaxLen, _pathFormat, &tmStructNow);
            tmStructFuture = tmStructNow;
            minute = (tmStructNow.tm_min + 11) / 10 * 10;
            if (minute >= 60) {
                tmStructFuture.tm_min = minute - 60;
                ++tmStructFuture.tm_hour;
            } else {
                tmStructFuture.tm_min = minute;
            }
            tmStructFuture.tm_sec = 0;
            timeFuture = mktime(&tmStructFuture);
            timeDiff = timeFuture - timeNow;
            if (timeDiff <= 59) {
                throw std::runtime_error("Duration time too short");
            }
            record(path, timeDiff);
            for (iter = _children.begin(); iter != _children.end();) {
                pid = *iter;
                r = waitpid(pid, &status, WNOHANG);
                switch (r) {
                    case -1:
                        printf("Failed to wait for forked ffmpeg, error: %d, %s\n", errno, strerror(errno));
                        throw std::runtime_error("Child illegal");
                    case 0:
                        ++iter;
                        break;
                    default:
                        if (r == pid) {
                            iter = _children.erase(iter);
                            break;
                        } else {
                            printf("Got pid %d when waiting for %d\n", r, pid);
                            throw std::runtime_error("Waited PID is different from reported");
                        }
                }
            }
        }
    }

    void record(const char * const path, time_t const duration) {
        __pid_t pid = fork();
        switch (pid) {
            case -1:
                printf("Failed to fork to record to %s\n", path);
                throw std::runtime_error("Failed to fork");
            case 0:
                break;
            default:
                _children.push_back(pid);
                sleep(duration);
                return;
        }
        int fdNull = open("/dev/null", O_WRONLY | O_CREAT, 0666);
        dup2(fdNull, 1);
        dup2(fdNull, 2);
        char durationStr[64];
        snprintf(durationStr, 64, "%ld", duration + 10);
        execl(
            /* Executable */
            "/usr/bin/ffmpeg",
            /* Itself */
            "/usr/bin/ffmpeg",
            /* Input */
            "-use_wallclock_as_timestamps", "1",
            "-i", _url,
            /* Codec */
            "-c", "copy",
            /* Duration */
            "-t", durationStr,
            /* Output */
            "-y", path,
            /* Sentry */
            NULL
        );
    }

    void wait() {
        printf("Waiting camera thread, pid %d\n", _pid);
        int status;
        waitpid(_pid, &status, 0);
    }

  private:
    static const uint _nameMaxLen = 128;
    static const uint _urlMaxLen = 1024;
    std::vector <__pid_t> _children;
    char _name[_nameMaxLen];
    uint _lenName;
    char _url[_urlMaxLen];
    char _pathFormat[_urlMaxLen];
    __pid_t _pid;
};

class Directory {
  public:
    struct Entry {
        char name[256];
        char path[512];
        time_t ctime;
    };
    Directory(const char *const path, uint fullPercent, uint cleanPercent) {
        std::strncpy(_path, path, _pathMaxLen);
        if (mkdir(path, 0700) == -1) {
            switch (errno) {
                case EEXIST:
                    break;
                default:
                    printf("Failed to create directory %s\n", _path);
                    throw std::runtime_error("Failed to create directory");
            }
        }
        _fullPercent = fullPercent;
        _cleanPercent = cleanPercent;
    }
    void watch() {
        __pid_t pid = fork();
        switch (pid) {
            case -1:
                printf("Failed to fork to watch %s\n", _path);
                throw std::runtime_error("Failed to fork");
            case 0:
                break;
            default:
                _pid = pid;
                return;
        }
        update();
        fsblkcnt_t min = _fsTotal  / 100 * _fullPercent;
        fsblkcnt_t optimal = _fsTotal / 100 * _cleanPercent;
        while (true) {
            if (_fsFree < min ) {
                while (_fsFree < optimal) {
                    clean();
                    updateSpace();
                }
            }
            sleep(10);
            if (_entries.size() == 0) {
                updateEntries();
            }
            updateSpace();
        }
    }
    void wait() {
        printf("Waiting directory thread, pid %d\n", _pid);
        int status;
        sleep(60);
        waitpid(_pid, &status, 0);
    }
  protected:
    std::vector <Entry> _entries;
    static const uint _pathMaxLen = 128;

  private:
    void updateEntries() {
        _entries.clear();
        DIR *d = opendir(_path);
        if (d == NULL) {
            printf("Failed to open directory %s, error: %d, %s\n", _path, errno, strerror(errno));
            throw std::runtime_error("Failed to open directory");
        }
        dirent *dirEntry;
        Entry entry;
        struct stat st;
        while ((dirEntry = readdir(d))) {
            switch (dirEntry->d_name[0]) {
                case '.':
                    switch (dirEntry->d_name[1]) {
                        case '\0': // . itself
                            continue;
                        case '.': 
                            if (dirEntry->d_name[2] == '\0') continue; // .. parent
                        default:
                            break;
                    }
                    break;
                case '\0': // Empty name, WTF?
                    continue;
                default:
                    break;
            }
            if (dirEntry->d_type != DT_REG) {
                continue;
            }
            memset(&entry, 0, sizeof entry);
            strncpy(entry.name, dirEntry->d_name, 256);
            snprintf(entry.path, 512, "%s/%s", _path, dirEntry->d_name);
            stat(entry.path, &st);
            entry.ctime = st.st_ctim.tv_sec;
            _entries.push_back(entry);
        }
        std::sort(_entries.begin(), _entries.end(), compareEntry);
        // for (Entry &entry : _entries) {
        //     printf("%ld: %s\n", entry.ctime, entry.path);
        // }
        closedir(d);
    }
    void updateSpace() {
        struct statvfs stVFS;
        int r = statvfs(_path, &stVFS);
        switch (r) {
            case 0:
                break;
            case -1:
                printf("Failed to get disk space of %s, errno: %d, content: %s\n", _path, errno, strerror(errno));
                throw std::runtime_error("Failed to get disk space");
            default:
                printf("Impossible return value from statvfs: %d\n", r);
                throw std::runtime_error("Impossible return value from statvfs");
        }
        _fsFree = stVFS.f_bfree;
        _fsTotal = stVFS.f_blocks;
    }
    void update() {
        updateEntries();
        updateSpace();
    }
    virtual void clean() = 0;
    char _path[_pathMaxLen];
    static bool compareEntry(Entry &a, Entry &b) {
        // Newest first
        return a.ctime > b.ctime;
    }
    __pid_t _pid;
    fsblkcnt_t _fsFree;
    fsblkcnt_t _fsTotal;
    uint _fullPercent;
    uint _cleanPercent;
};

class HotDirectory: public Directory {
  public:
    HotDirectory(const char *const path, const char *const archived) : Directory(path, 90, 10) {
        strncpy(_archived, archived, _pathMaxLen);
    }
  private:
    char _archived[_pathMaxLen];
    void clean() {
        Entry entry = _entries.back();
        _entries.pop_back();
        char target[1024];
        if (snprintf(target, 1024, "%s/%s", _archived, entry.name) < 0) {
            printf("Failed to generate archived path for %s\n", entry.name);
            throw std::runtime_error("Failed to generate new name");
        }
        if (rename(entry.path, target) < 0) {
            int errnoStack = errno;
            switch (errnoStack) {
                case ENOENT: // Old does not exist, ignore it
                    return;
                case EXDEV: 
                    move(entry.path, target);
                    break;
                default:
                    printf("Failed to rename %s to %s: %d, %s\n", entry.path, target, errnoStack, strerror(errnoStack));
                    throw std::runtime_error("Failed to rename");
            }
        }
    }
    void move(const char *const pathIn, const char *const pathOut) {
        struct stat st;
        stat(pathIn, &st);
        int fin = open(pathIn, O_RDONLY);
        if (fin < 0) {
            printf("Failed to open input file %s, error: %d, %s\n", pathIn, errno, strerror(errno));
            throw std::runtime_error("Failed to open old file");
        }
        int fout = open(pathOut, O_WRONLY | O_CREAT, 0644);
        if (fout < 0) {
            printf("Failed to open target file %s, error: %d, %s\n", pathOut, errno, strerror(errno));
            throw std::runtime_error("Failed to open target file");
        }
        size_t remain = st.st_size;
        ssize_t r;
        while (remain) {
            r = sendfile(fout, fin, NULL, remain);
            if (r < 0) {
                printf("Failed to send file, error: %d, %s\n", errno, strerror(errno));
                throw std::runtime_error("Failed to send file");
            }
            remain -= r;
        }
        close(fin);
        close(fout);
        if (unlink(pathIn) < 0) {
            printf("Failed to remove file %s, error: %d, %s\n", pathIn, errno, strerror(errno));
            throw std::runtime_error("Failed to remove file");
        }
    }
};

class ArchivedDirectory: public Directory {
  public:
    ArchivedDirectory(const char *const path) : Directory(path, 95, 90) {}
  private:
    void clean() {
        printf("Cleaning archived...\n");
        Entry entry = _entries[-1];
        _entries.pop_back();
        if (unlink(entry.path) < 0) {
            printf("Failed to remove file %s, error: %d, %s\n", entry.path, errno, strerror(errno));
            throw std::runtime_error("Failed to remove file");
        }
    }
};


int main() {
    ArchivedDirectory dirArchived("archived");
    HotDirectory dirHot("hot", "archived");
    dirArchived.watch();
    dirHot.watch();
    const char host[] = "127.0.0.1:8554";
    Camera cameras[] = {
        Camera("Rooftop", host, "rooftop"),
        Camera("Road", host, "road"),
        Camera("Garden", host, "garden"),
    };
    for (Camera &camera : cameras) {
        camera.start();
    }
    for (Camera &camera : cameras) {
        camera.wait();
    }
    dirArchived.wait();
    dirHot.wait();
    return 0;
}