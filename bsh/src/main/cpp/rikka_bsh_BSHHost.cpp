#include <jni.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <cstdlib>
#include <wait.h>
#include <android/log.h>
#include <pthread.h>
#include "logging.h"
#include "pts.h"

static int setWindowSize(int ptmx, jlong size) {
    static_assert(sizeof(jlong) == sizeof(winsize));
    winsize w = *((winsize *) &size);

    LOGD("setWindowSize %d %d %d %d", w.ws_row, w.ws_col, w.ws_xpixel, w.ws_ypixel);

    if (ioctl(ptmx, TIOCSWINSZ, &w) == -1) {
        PLOGE("ioctl TIOCGWINSZ");
        return -1;
    }
    return 0;
}

static jintArray BSHHost_startHost(JNIEnv *env, jclass clazz, jint stdin_read_pipe, jint stdout_write_pipe) {
    int ptmx = open_ptmx();
    if (ptmx == -1) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unable to open ptmx");
        return nullptr;
    }
    LOGD("ptmx %d", ptmx);

    auto pid = fork();
    if (pid == -1) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unable to fork");
        return nullptr;
    }

    if (pid > 0) {
        auto called = std::make_shared<std::atomic_bool>(false);
        auto func = [pid, called]() {
            if (called->exchange(true)) {
                return;
            }

            LOGW("client dead, kill forked process");
            kill(pid, SIGKILL);
        };
        transfer_async(stdin_read_pipe, ptmx/*, func*/);
        transfer_async(ptmx, stdout_write_pipe, func);

        auto result = env->NewIntArray(2);
        env->SetIntArrayRegion(result, 0, 1, &pid);
        env->SetIntArrayRegion(result, 1, 1, &ptmx);
        return result;
    } else {
        if (setsid() < 0) {
            PLOGE("setsid");
            exit(1);
        }

        char pts_slave[PATH_MAX]{0};
        if (ptsname_r(ptmx, pts_slave, PATH_MAX - 1) == -1) {
            PLOGE("ptsname_r");
            exit(1);
        }

        int pts = open(pts_slave, O_RDWR);
        LOGD("pts %d", pts);

        dup2(pts, STDIN_FILENO);
        dup2(pts, STDOUT_FILENO);
        dup2(pts, STDERR_FILENO);

        close(pts);

        if (execv("/system/bin/sh", nullptr) == -1) {
            PLOGE("execv");
            exit(1);
        }
        exit(0);
    }
}

static void BSHHost_setWindowSize(JNIEnv *env, jclass clazz, jint ptmx, jlong size) {
    setWindowSize(ptmx, size);
}

static jint BSHHost_waitFor(JNIEnv *env, jclass clazz, jint pid) {
    if (pid < 0)
        return -1;

    int status;
    int w;
    do {
        w = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
        if (w == -1) {
            if (errno == ECHILD) {
                return 0;
            }
            PLOGE("waitpid");
            return -1;
        }

        if (WIFEXITED(status)) {
            LOGD("exited with %d", WEXITSTATUS(status));
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            LOGD("killed by signal %d", WTERMSIG(status));
            return 0;
        }
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));

    return -1;
}

int rikka_bsh_BSHHost_registerNatives(JNIEnv *env) {
    auto clazz = env->FindClass("rikka/bsh/BSHHost");
    JNINativeMethod methods[] = {
            {"start",         "(II)[I", (void *) BSHHost_startHost},
            {"setWindowSize", "(IJ)V",  (void *) BSHHost_setWindowSize},
            {"waitFor",       "(I)I",   (void *) BSHHost_waitFor},
    };
    return env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0]));
}