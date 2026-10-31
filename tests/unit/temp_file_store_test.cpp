#include "chunkharbor/storage/TempFileStore.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <cstdarg>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>

int injectedOpenError = 0;
bool injectedChmodFailure = false, injectedStatFailure = false;
extern "C" long __real_syscall(long,...);
extern "C" int __real_fchmod(int,mode_t);
extern "C" int __real_fstat(int,struct stat*);
extern "C" long __wrap_syscall(long number,...) {
    if (number != SYS_openat2) { errno = ENOSYS; return -1; }
    va_list arguments; va_start(arguments,number);
    const auto dir = va_arg(arguments,int);
    const auto path = va_arg(arguments,const char*);
    const auto how = va_arg(arguments,const open_how*);
    const auto size = va_arg(arguments,std::size_t);
    va_end(arguments);
    if (injectedOpenError) { errno = injectedOpenError; injectedOpenError = 0; return -1; }
    return __real_syscall(number,dir,path,how,size);
}
extern "C" int __wrap_fchmod(int fd,mode_t mode) {
    if (injectedChmodFailure) { injectedChmodFailure = false; errno = EIO; return -1; }
    return __real_fchmod(fd,mode);
}
extern "C" int __wrap_fstat(int fd,struct stat* st) {
    if (injectedStatFailure) { injectedStatFailure = false; errno = EIO; return -1; }
    return __real_fstat(fd,st);
}

using namespace chunkharbor;
namespace fs = std::filesystem;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const std::invalid_argument&) { return; } catch (const std::system_error&) { return; }
    throw std::runtime_error("expected path/storage rejection");
}
struct Fixture {
    fs::path base;
    Fixture() {
        char pattern[] = "/tmp/chunkharbor-storage-XXXXXX";
        const char* path = ::mkdtemp(pattern);
        if (!path) throw std::runtime_error("mkdtemp failed");
        base = path;
        fs::create_directory(base/"root"); ::chmod((base/"root").c_str(),0700);
        fs::create_directories(base/"root"/"inside"/"deep");
        fs::create_directory(base/"outside");
        std::ofstream(base/"outside"/"sentinel") << "untouched";
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(base,ignored); }
};
std::size_t fdCount() { return std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator{}); }
void verifyFile(int fd, const fs::path& outside) {
    struct stat st{};
    check(::fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==0 && (st.st_mode&0777)==0600,
          "private unnamed regular file");
    check((::fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0,"close-on-exec");
    const auto target = fs::read_symlink("/proc/self/fd/"+std::to_string(fd)).string();
    check(target.find(outside.string())==std::string::npos,"escaped root");
}
int main() {
    try {
        Fixture fixture;
        const auto root = fixture.base/"root", outside = fixture.base/"outside";
        const auto before = fdCount();
        {
            TempFileStore store(root.string());
            auto first = store.create();
            verifyFile(first.get(),outside);
            struct stat st{}; check(::fstat(first.get(),&st)==0 && st.st_size==0,"initially empty");
            const auto binary = std::string("a\0b",3);
            check(::pwrite(first.get(),binary.data(),binary.size(),0)==3,"test write");
            char bytes[3]{}; check(::pread(first.get(),bytes,3,0)==3 && std::memcmp(bytes,binary.data(),3)==0,"test read");
            auto second = store.create("inside/deep"); verifyFile(second.get(),outside);
            const int old = second.get();
            UniqueFd moved(std::move(second)); check(!second && moved.get()==old,"move ownership");
            moved.reset(); check(::fcntl(old,F_GETFD)==-1 && errno==EBADF,"fd released");
            check(fs::is_empty(root/"inside"/"deep"),"no temp directory entry");
            for (const auto& path : {std::string{}, std::string("../outside"), std::string("inside/../.."),
                 std::string("/tmp"), std::string("inside//deep"), std::string("inside/./deep"),
                 std::string("inside/"), std::string("C:/x"), std::string("inside\\deep"),
                 std::string("inside\0/trap",12), std::string(256,'a'), std::string(4097,'a')})
                rejects([&] { store.create(path); });
            fs::create_directory_symlink(outside,root/"escape");
            fs::create_directory_symlink(root/"inside",root/"internal-link");
            rejects([&] { store.create("escape"); });
            rejects([&] { store.create("escape/nested"); });
            rejects([&] { store.create("internal-link/deep"); });
            rejects([&] { store.create("missing"); });
            std::ofstream(root/"regular") << "keep";
            rejects([&] { store.create("regular"); });
            const auto count = fdCount();
            for (int i=0; i<200; ++i) { rejects([&] { store.create("escape"); }); auto temp = store.create(); }
            check(fdCount()==count,"success/failure fd leak");
            for (int error : {ENOSYS,EOPNOTSUPP,EMFILE,EAGAIN}) {
                injectedOpenError = error; rejects([&] { store.create(); });
                check(injectedOpenError==0 && fdCount()==count,"unsupported/exhaustion cleanup");
            }
            injectedOpenError = EINTR;
            { auto retry = store.create(); verifyFile(retry.get(),outside); }
            injectedChmodFailure = true; rejects([&] { store.create(); });
            injectedStatFailure = true; rejects([&] { store.create(); });
            check(!injectedChmodFailure && !injectedStatFailure && fdCount()==count,"post-open failure cleanup");
            {
                struct Mask { mode_t previous = ::umask(0777); ~Mask() { ::umask(previous); } } mask;
                auto privateFile = store.create(); verifyFile(privateFile.get(),outside);
            }
            // Atomically rooted creation while another thread swaps a directory
            // with an outside symlink: only a safe file or an error is allowed.
            fs::create_directory(root/"race");
            std::atomic<bool> stop{false}; std::exception_ptr workerError;
            std::thread attacker([&] {
                try {
                    while (!stop.load()) {
                        fs::rename(root/"race",root/"held");
                        fs::create_directory_symlink(outside,root/"race");
                        fs::remove(root/"race");
                        fs::rename(root/"held",root/"race");
                    }
                } catch (...) { workerError = std::current_exception(); }
            });
            std::exception_ptr mainError;
            try {
                for (int i=0; i<1000; ++i) {
                    try { auto temp = store.create("race"); verifyFile(temp.get(),outside); }
                    catch (const std::system_error&) {} // ELOOP/ENOENT/EAGAIN are safe rejection.
                }
            } catch (...) { mainError = std::current_exception(); }
            stop = true; attacker.join();
            if (workerError) std::rethrow_exception(workerError);
            if (mainError) std::rethrow_exception(mainError);
            // Root identity remains pinned if the configuration pathname changes.
            fs::rename(root,fixture.base/"pinned");
            fs::create_directory_symlink(outside,root);
            auto pinned = store.create(); verifyFile(pinned.get(),outside);
            check(fs::is_empty(fixture.base/"pinned"/"race"),"race left no named files");
            fs::remove(root); fs::rename(fixture.base/"pinned",root);
        }
        check(fdCount()==before,"store destructor fd leak");
        UniqueFd survivor;
        {
            TempFileStore original(root.string());
            TempFileStore moved(std::move(original));
            rejects([&] { original.create(); });
            survivor = moved.create();
        }
        verifyFile(survivor.get(),outside); survivor.reset();
        check(fdCount()==before,"file can outlive store without leak");
        injectedOpenError = ENOSYS; rejects([&] { TempFileStore unsupported(root.string()); });
        check(injectedOpenError==0 && fdCount()==before,"constructor failure cleanup");
        fs::create_directory_symlink(root,fixture.base/"root-link");
        rejects([&] { TempFileStore bad((fixture.base/"root-link").string()); });
        rejects([&] { TempFileStore bad((fixture.base/"root-link"/"inside").string()); });
        rejects([] { TempFileStore bad("relative"); });
        rejects([&] { TempFileStore bad(root.string()+"/../root"); });
        rejects([&] { TempFileStore bad(root.string()+std::string(1,'\0')+"ignored"); });
        ::chmod(root.c_str(),0777); rejects([&] { TempFileStore bad(root.string()); }); ::chmod(root.c_str(),0700);
        check(fs::is_empty(outside)==false && std::distance(fs::directory_iterator(outside),fs::directory_iterator{})==1,"outside changed");
        std::ifstream sentinel(outside/"sentinel"); std::string text; sentinel >> text; check(text=="untouched","outside overwritten");
        std::cout << "anonymous staging, path/symlink/race rejection and FD ownership passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
