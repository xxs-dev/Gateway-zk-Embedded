#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

struct SharedState {
    pthread_mutex_t mutex;
};

int fail(const std::string& message) {
    std::cerr << "posix_robust_mutex_probe status=FAIL: " << message << std::endl;
    return 1;
}

int unsupported(const std::string& message) {
    std::cerr << "posix_robust_mutex_probe status=ENVIRONMENT_UNSUPPORTED: "
              << message << "; product status=NOT_PROVEN. "
              << "Do not waive the native AArch64 lifecycle gate." << std::endl;
    return 1;
}

std::string errorText(int error) {
    return std::to_string(error) + " (" + std::strerror(error) + ")";
}

}  // namespace

int main() {
    void* mapping = mmap(
        nullptr,
        sizeof(SharedState),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0
    );
    if (mapping == MAP_FAILED) {
        return fail("mmap failed: " + std::string(std::strerror(errno)));
    }
    auto* state = static_cast<SharedState*>(mapping);
    std::memset(state, 0, sizeof(*state));

    pthread_mutexattr_t attributes{};
    int rc = pthread_mutexattr_init(&attributes);
    if (rc != 0) {
        munmap(mapping, sizeof(SharedState));
        return fail("pthread_mutexattr_init returned " + errorText(rc));
    }
    rc = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        pthread_mutexattr_destroy(&attributes);
        munmap(mapping, sizeof(SharedState));
        return unsupported("pthread_mutexattr_setpshared returned " + errorText(rc));
    }
    rc = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    if (rc != 0) {
        pthread_mutexattr_destroy(&attributes);
        munmap(mapping, sizeof(SharedState));
        return unsupported("pthread_mutexattr_setrobust returned " + errorText(rc));
    }
    rc = pthread_mutex_init(&state->mutex, &attributes);
    const int destroyAttributesRc = pthread_mutexattr_destroy(&attributes);
    if (rc != 0 || destroyAttributesRc != 0) {
        if (rc == 0) {
            pthread_mutex_destroy(&state->mutex);
        }
        munmap(mapping, sizeof(SharedState));
        return fail(
            rc != 0
                ? "process-shared robust mutex initialization returned " + errorText(rc)
                : "pthread_mutexattr_destroy returned " + errorText(destroyAttributesRc)
        );
    }

    int readyPipe[2] = {-1, -1};
    if (pipe(readyPipe) != 0) {
        pthread_mutex_destroy(&state->mutex);
        munmap(mapping, sizeof(SharedState));
        return fail("pipe failed: " + std::string(std::strerror(errno)));
    }

    const pid_t child = fork();
    if (child < 0) {
        close(readyPipe[0]);
        close(readyPipe[1]);
        pthread_mutex_destroy(&state->mutex);
        munmap(mapping, sizeof(SharedState));
        return fail("fork failed: " + std::string(std::strerror(errno)));
    }
    if (child == 0) {
        close(readyPipe[0]);
        const int lockRc = pthread_mutex_lock(&state->mutex);
        if (lockRc != 0) {
            _exit(10);
        }
        const char ready = '1';
        if (write(readyPipe[1], &ready, 1) != 1) {
            _exit(11);
        }
        _exit(0);  // Deliberately abandon the mutex.
    }

    close(readyPipe[1]);
    char ready = 0;
    const ssize_t readyCount = read(readyPipe[0], &ready, 1);
    close(readyPipe[0]);

    int childStatus = 0;
    const pid_t waited = waitpid(child, &childStatus, 0);
    if (readyCount != 1 || ready != '1' || waited != child ||
        !WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0) {
        munmap(mapping, sizeof(SharedState));
        return fail("child did not acquire and abandon the shared mutex cleanly");
    }

    timespec deadline{};
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        munmap(mapping, sizeof(SharedState));
        return fail("clock_gettime failed: " + std::string(std::strerror(errno)));
    }
    deadline.tv_sec += 2;
    rc = pthread_mutex_timedlock(&state->mutex, &deadline);
    if (rc != EOWNERDEAD) {
        if (rc == 0) {
            pthread_mutex_unlock(&state->mutex);
        }
        munmap(mapping, sizeof(SharedState));
        return unsupported("expected EOWNERDEAD after owner exit, got " + errorText(rc));
    }

    rc = pthread_mutex_consistent(&state->mutex);
    if (rc == 0) {
        rc = pthread_mutex_unlock(&state->mutex);
    }
    if (rc == 0) {
        rc = pthread_mutex_destroy(&state->mutex);
    }
    munmap(mapping, sizeof(SharedState));
    if (rc != 0) {
        return fail("owner-death recovery cleanup returned " + errorText(rc));
    }

    std::cout << "posix_robust_mutex_probe status=PASS" << std::endl;
    return 0;
}
