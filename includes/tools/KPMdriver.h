#pragma once

#include <unistd.h>
#include <optional>
#include <vector>
#include <memory>
#include <string>
#include <cstdio>
#include <cstring>
#include <sys/syscall.h>
#include <sys/ioctl.h>

// 需要C++17
#define __NR_syscall_  18
#define __FLAGS        1UL << 0
#define __CHECKSUCCESS (1UL << 30)

enum class DriverType : int {
    ERR = -1,
    DITPRO_KPM = 0,
    DITS_KO = 1,
    QX_KO = 2,
    RT_KO = 3
};

enum class MemoryOp : uint64_t {
    INIT = 1UL << 1,
    READ = 1UL << 2,
    WRITE = 1UL << 3,
    CPU = 1UL << 4,
    PROT_NC = 1UL << 5,
    READLIST = 1UL << 6,
    READARRAY = 1UL << 7,
    UNINSTALL = 1UL << 9
};

enum class OtherOp : uint64_t {
    PROCESS_PID = 1UL << 10,
    MODULE_BASE = 1UL << 11,
    HIDE_PID = 1UL << 12,
    UNHIDE_PID = 1UL << 13,
    HIDE_EVENT = 1UL << 14,
    UNHIDE_EVENT = 1UL << 15,
    GETUSERMAPS = 1UL << 22,
    LINGYE = 1UL << 25,//零页
};

constexpr uint64_t operator|(MemoryOp lhs, MemoryOp rhs) {
    return static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs);
}

constexpr uint64_t operator|(uint64_t lhs, MemoryOp rhs) {
    return lhs | static_cast<uint64_t>(rhs);
}

constexpr uint64_t operator|(MemoryOp lhs, uint64_t rhs) {
    return static_cast<uint64_t>(lhs) | rhs;
}

constexpr uint64_t operator|(OtherOp lhs, OtherOp rhs) {
    return static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs);
}

constexpr uint64_t operator|(uint64_t lhs, OtherOp rhs) {
    return static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs);
}

struct Dit_uct_base {
    int pid;
    const char *name;
    unsigned long start;
    unsigned long end;
};

struct Dit_uct {
    uint64_t addr;
    void *buffer;
    uint64_t size;
} __attribute__((aligned(8)));

struct Dit_uct_kpm_list {
    uint64_t addr[10];
    void *buffer;
    uint64_t size;
} __attribute__((aligned(8)));

struct Dit_uct_array {
    uint64_t count;     //数量
    uint64_t array_addr;//地址
    void *buffer;       //缓冲区
    uint64_t size;      //列表页大小
};

struct user_maps {
    unsigned int pid;
    unsigned long count;
    char *lists;
};

class driver {
public:
    template<typename T>
    T read(unsigned long addr) {
        T res{};
        if (!read(addr, &res, sizeof(T))) return res;
        return {};
    }

    long read(unsigned long addr, void *buffer, unsigned long size) {
        struct Dit_uct cm = { addr, buffer, size };
        uint64_t flags = (__FLAGS | MemoryOp::READ);
        return call(flags, &cm);
    }

    long read(std::initializer_list<unsigned long> nums, void *buffer, unsigned long size) {
        struct Dit_uct_kpm_list cm;
        memset(cm.addr, -1, sizeof cm.addr);
        size_t i = 0;
        for (unsigned long val: nums) {
            if (i >= 10) return size;
            cm.addr[i++] = val;
        }
        cm.buffer = buffer;
        cm.size = size;

        uint64_t flags = static_cast<uint64_t>(MemoryOp::READ) | __FLAGS | MemoryOp::READLIST;

        return call(flags, &cm);
    }

    // 直接通过一次系统调用遍历数组
    long read_array(unsigned long count, unsigned long addr, void *buffer, unsigned long size) {
        struct Dit_uct_array cm = { count, addr, buffer, size };
        uint64_t flags = static_cast<uint64_t>(MemoryOp::READ) | __FLAGS | static_cast<uint64_t>(MemoryOp::READARRAY);
        return call(flags, &cm);
    }

    // 初始化读取 返回大于0成功，dpk返回版本号，ds返回当前pid
    int init_pid(int pid) {
        return call((__FLAGS | MemoryOp::INIT), pid) > 0;
    }

    std::optional<int> get_pid(std::string_view name) {
        if (auto pid = call((__FLAGS | OtherOp::PROCESS_PID), name.data()); pid > 2) {
            return pid;
        }
        return std::nullopt;
    }

    // 获取bss就填true
    std::optional<unsigned long> get_base(int pid, std::string_view name, bool bss = false) {
        struct Dit_uct_base cm = { pid, name.data(), bss, bss };
        if (call((__FLAGS | OtherOp::MODULE_BASE), &cm) == 0) {
            return cm.start;
        }
        return std::nullopt;
    }

    std::optional<std::pair<unsigned long, unsigned long>> get_base_range(int pid, std::string_view name, bool bss = false) {
        struct Dit_uct_base cm = { pid, name.data(), bss, bss };
        if (call((__FLAGS | OtherOp::MODULE_BASE), &cm) == 0) {
            return std::pair{ cm.start, cm.end };
        }
        return std::nullopt;
    }

    void start() {
        type = find_dis();
        if (type == DriverType::DITS_KO) {
            printf("你好，亲爱的 dits\n");
        } else if (type == DriverType::DITPRO_KPM) {
            printf("你好，亲爱的 ditpro\n");
        } else {
            printf("%s\n","没有找到驱动\n");
        }
    }

    // 获取maps
    std::optional<std::vector<std::string>> getMaps(int pid) {
        std::vector<std::string> vec;
        constexpr int len = 10000;
        constexpr int bufferSize = 512;
        struct user_maps cm;
        cm.pid = pid;
        cm.count = len;
        cm.lists = (char *) malloc(len * bufferSize);
        if (!cm.lists) return std::nullopt;
        // 分配物理页
        memset(cm.lists, 0, len * bufferSize);
        if (auto n = call((__FLAGS | OtherOp::GETUSERMAPS), &cm); n > 2) {
            for (int i = 0; i < n; i++) {
                vec.push_back(cm.lists + i * 512);
            }
            return vec;
        }

        return std::nullopt;
    }

    void UnMem() { call((__FLAGS | MemoryOp::UNINSTALL)); }
    // 隐藏进程
    int hideProc() { return call((__FLAGS | OtherOp::HIDE_PID), gettid()); }
    // 恢复进程，隐藏进程之后程序退出必须调用，否则重启
    int unProc() { return call((__FLAGS | OtherOp::UNHIDE_PID)); }

    ~driver() {
        UnMem();
        unProc();
    }

private:
    int fd{ -1 };
    DriverType type{ -1 };
    DriverType find_dis() {
        if (auto check = __CHECKSUCCESS; syscall(__NR_syscall_, &check) == 616) {
            int flags = 616;
            fd = syscall(__NR_syscall_, &flags);
            return (fd > 0) ? DriverType::DITS_KO : DriverType::ERR ;
        } else if (syscall(__NR_syscall_, (__FLAGS | __CHECKSUCCESS)) == 616) {
            return DriverType::DITPRO_KPM;
        }
        return DriverType::ERR;
    }

    template<typename... Args>
    long call(Args &&...args) {
        switch (type) {
            case DriverType::DITPRO_KPM:
                return syscall(__NR_syscall_, std::forward<decltype(args)>(args)...);
            case DriverType::DITS_KO:
                return ioctl(fd, std::forward<decltype(args)>(args)...);
            default:
                return -1;
        }
    }
};

// "inline" di sini penting: header ini bisa di-#include di banyak .cpp
// tanpa error multiple-definition (linker akan merge jadi satu instance).
inline driver Core;
