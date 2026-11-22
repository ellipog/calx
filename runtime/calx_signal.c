/* Signals a program handles, behind std.signal (CLX-377): SIGINT, SIGTERM and SIGHUP on POSIX,
 * and on Windows the console's Ctrl+C, Ctrl+Break and close, the system logging off or shutting
 * down, and the Service Control Manager's stop.
 *
 * A handler only records what arrived, in `arrived`, a bit for each kind, and passes the news on:
 * to whatever `wait` blocks on, and to each `Net` whose poll std.net said a signal wakes, through
 * the list calx_rt.c keeps, so a server can stop cleanly. On POSIX the handler may have stopped
 * any thread anywhere, so it does only what's async-signal-safe: it sets the bit and writes a byte
 * to a pipe. A thread of the runtime's own reads the pipe and does the rest. On Windows the
 * system runs a console handler on a thread of its own already, which does the rest itself.
 *
 * The kinds are the same on each system, so a program handles them the same way: `Interrupt` is
 * SIGINT or Ctrl+C, `Terminate` is SIGTERM, Ctrl+Break (which is how a parent asks a child on
 * Windows), logging off, shutting down or a service's stop, and `Hangup` is SIGHUP or the console
 * closing.
 *
 * std.term puts the terminal back at a signal "where the program hasn't set its own handler". On
 * POSIX that's because it only sets one where the disposition is still the default, and these
 * handlers replace its handlers. On Windows the handler that runs first decides, so these
 * handlers say in `cx_signal_console` which events are the program's, and std.term's leaves those
 * alone. Either way the terminal is put back as the program ends, as it always is.
 *
 * This file is compiled into a program only when it uses std.signal. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "synchronization.lib")
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <link.h>
#endif
#endif
#include <stdint.h>
#include <string.h>

#include "calx_rt.h"

int cx_in_parallel(void);
CX_COLD void cx_panic_text(const char* at, const char* message);
/* In calx_rt.c: wakes each poll std.net said a signal wakes, and what's called as the program
 * ends, with whether it meant to and its status. */
void cx_signal_wakes_fire(void);
extern void (*cx_signal_at_exit)(int normal, int code);
#ifdef _WIN32
extern unsigned cx_signal_console;
void cx_signal_exit(void);
#endif

enum { K_INTERRUPT = 0, K_TERMINATE = 1, K_HANGUP = 2, K_COUNT = 3 };

static unsigned handled; /* a bit for each kind the program handles */
static unsigned arrived; /* a bit for each kind that arrived and hasn't been taken */

static void check_thread(void) {
    if (cx_in_parallel()) {
        cx_panic_text("std.signal", "signals can't be waited for inside a `parallel for`, since what each iteration "
                                    "saw would depend on timing");
    }
}

#ifdef _WIN32

/* --- Windows ---------------------------------------------------------------------------------- */

static int svc_running; /* the Service Control Manager started this program, and it's running */

/* What arrived recorded, then those waiting told: `wait`'s WaitOnAddress, and the polls. */
static void record(int kind) {
    __atomic_or_fetch(&arrived, 1u << kind, __ATOMIC_SEQ_CST);
    WakeByAddressAll(&arrived);
    cx_signal_wakes_fire();
}

static int kind_of_event(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT: return K_INTERRUPT;
    case CTRL_BREAK_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: return K_TERMINATE;
    case CTRL_CLOSE_EVENT: return K_HANGUP;
    }
    return -1;
}

/* Runs on a thread the system starts for it. Returning TRUE says the program has it; FALSE passes
 * it to the next handler, in the end the system's, which ends the program. After a close, logoff
 * or shutdown the system ends the program as soon as the handler returns, so the handler waits
 * instead, and the program ends as it means to, until the system's time is up: 5 s for a close,
 * and what's set for a shutdown, 5 s for a program and 20 s for a service to begin with. */
static BOOL WINAPI on_control(DWORD event) {
    /* A service hears of every user logging off, which isn't its to stop at, and of the system
     * shutting down from the Service Control Manager too. */
    if (__atomic_load_n(&svc_running, __ATOMIC_ACQUIRE) && (event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT)) {
        return TRUE;
    }
    int kind = kind_of_event(event);
    if (kind < 0 || !(__atomic_load_n(&handled, __ATOMIC_ACQUIRE) & (1u << kind))) return FALSE;
    record(kind);
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) Sleep(INFINITE);
    return TRUE;
}

static unsigned events_of(unsigned kinds) {
    unsigned events = 0;
    if (kinds & (1u << K_INTERRUPT)) events |= 1u << CTRL_C_EVENT;
    if (kinds & (1u << K_TERMINATE)) events |= (1u << CTRL_BREAK_EVENT) | (1u << CTRL_LOGOFF_EVENT) | (1u << CTRL_SHUTDOWN_EVENT);
    if (kinds & (1u << K_HANGUP)) events |= 1u << CTRL_CLOSE_EVENT;
    return events;
}

void cx_signal_handle(cx_u32 kinds) {
    static int installed;
    kinds &= (1u << K_COUNT) - 1;
    __atomic_or_fetch(&cx_signal_console, events_of(kinds), __ATOMIC_SEQ_CST);
    __atomic_or_fetch(&handled, kinds, __ATOMIC_SEQ_CST);
    if (!__atomic_exchange_n(&installed, 1, __ATOMIC_ACQ_REL)) SetConsoleCtrlHandler(on_control, TRUE);
}

/* Waits until a kind arrives, or `timeout_ms` passes, or forever if it's negative. */
static void wait_arrival(cx_i64 timeout_ms) {
    ULONGLONG deadline = timeout_ms > 0 ? GetTickCount64() + (ULONGLONG)timeout_ms : 0;
    for (;;) {
        unsigned seen = __atomic_load_n(&arrived, __ATOMIC_ACQUIRE);
        if (seen || timeout_ms == 0) return;
        DWORD wait = INFINITE;
        if (timeout_ms > 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return;
            wait = (DWORD)(deadline - now);
        }
        WaitOnAddress(&arrived, &seen, sizeof seen, wait);
    }
}

/* As C's raise does on Windows: in this process. One the program handles is recorded, as if the
 * console had sent it. One it doesn't ends the program as the system's own handler would, with
 * STATUS_CONTROL_C_EXIT, the terminal put back first. */
cx_bool cx_signal_raise(cx_u32 kind) {
    if (kind >= K_COUNT) return 0;
    if (__atomic_load_n(&handled, __ATOMIC_ACQUIRE) & (1u << kind)) {
        record((int)kind);
        return 1;
    }
    cx_signal_exit();
    return 1;
}

/* --- Windows: running as a service -------------------------------------------------------------
 *
 * StartServiceCtrlDispatcher runs on a thread of its own, which the Service Control Manager calls
 * the service's main and its control handler on, while the program's own main goes on. A stop,
 * or the system's preshutdown, is recorded as Terminate, and the service says it's stopping; it
 * says it has stopped as the program ends, with its status. Started any other way, the dispatcher
 * fails at once, and `cx_signal_service` says so. */

static SERVICE_STATUS_HANDLE svc_handle;
static SERVICE_STATUS svc_status;
static HANDLE svc_decided; /* set once the service has started, or the dispatcher has failed */

static void svc_report(DWORD state, DWORD accepts, DWORD wait_hint) {
    svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    svc_status.dwCurrentState = state;
    svc_status.dwControlsAccepted = accepts;
    svc_status.dwWaitHint = wait_hint;
    svc_status.dwCheckPoint = state == SERVICE_STOP_PENDING ? svc_status.dwCheckPoint + 1 : 0;
    SetServiceStatus(svc_handle, &svc_status);
}

static DWORD WINAPI svc_control(DWORD control, DWORD type, LPVOID data, LPVOID context) {
    (void)type;
    (void)data;
    (void)context;
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_PRESHUTDOWN:
    case SERVICE_CONTROL_SHUTDOWN:
        /* The program has 30 s before the Service Control Manager thinks it stuck. */
        svc_report(SERVICE_STOP_PENDING, 0, 30000);
        record(K_TERMINATE);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE: return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Says the service has stopped, as the program ends: with no error if it ended as it meant to with
 * status 0, and with its status as the service's own code otherwise, 101 after a panic. */
static void svc_at_exit(int normal, int code) {
    if (!__atomic_exchange_n(&svc_running, 0, __ATOMIC_ACQ_REL)) return;
    int status = normal ? code : 101;
    svc_status.dwWin32ExitCode = status == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    svc_status.dwServiceSpecificExitCode = (DWORD)status;
    svc_report(SERVICE_STOPPED, 0, 0);
}

static void WINAPI svc_main(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;
    /* A process with one service: the name isn't checked. */
    svc_handle = RegisterServiceCtrlHandlerExW(L"", svc_control, NULL);
    if (svc_handle) {
        __atomic_store_n(&cx_signal_at_exit, svc_at_exit, __ATOMIC_RELEASE);
        __atomic_store_n(&svc_running, 1, __ATOMIC_RELEASE);
        svc_report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN, 0);
    }
    SetEvent(svc_decided);
}

static DWORD WINAPI svc_dispatch(LPVOID unused) {
    (void)unused;
    static SERVICE_TABLE_ENTRYW table[] = {{L"", svc_main}, {NULL, NULL}};
    /* Returns once the service has stopped, or at once if this program isn't one. */
    if (!StartServiceCtrlDispatcherW(table)) SetEvent(svc_decided);
    return 0;
}

cx_bool cx_signal_service(void) {
    static int asked, answer;
    if (__atomic_exchange_n(&asked, 1, __ATOMIC_ACQ_REL)) return (cx_bool)answer;
    svc_decided = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!svc_decided) return 0;
    HANDLE t = CreateThread(NULL, 64 * 1024, svc_dispatch, NULL, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    WaitForSingleObject(svc_decided, INFINITE);
    answer = __atomic_load_n(&svc_running, __ATOMIC_ACQUIRE);
    /* A service is asked to stop as Terminate, which the program handles once it is one. */
    if (answer) cx_signal_handle(1u << K_TERMINATE);
    return (cx_bool)answer;
}

#else

/* --- POSIX ------------------------------------------------------------------------------------ */

static int pipe_r = -1, pipe_w = -1;
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond;
static const int signal_of[K_COUNT] = {SIGINT, SIGTERM, SIGHUP};

/* Async-signal-safe: an atomic bit, and a write to a nonblocking pipe. A full pipe has a byte in
 * it already, which tells the thread as well. */
static void on_signal(int sig) {
    int saved = errno;
    for (int k = 0; k < K_COUNT; k++) {
        if (signal_of[k] == sig) __atomic_or_fetch(&arrived, 1u << k, __ATOMIC_SEQ_CST);
    }
    char b = 0;
    ssize_t r = write(pipe_w, &b, 1);
    (void)r;
    errno = saved;
}

/* Those waiting told: `wait`'s condition, and the polls. */
static void tell(void) {
    pthread_mutex_lock(&wait_mutex);
    pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_mutex);
    cx_signal_wakes_fire();
}

/* The runtime's thread that hears from the handler. It takes no signals itself. */
static void* listen_thread(void* unused) {
    (void)unused;
    for (;;) {
        char buf[64];
        ssize_t r = read(pipe_r, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return NULL;
        tell();
    }
}

#ifdef __linux__
/* Adds up the thread-local storage of the program and the libraries loaded with it. */
static int add_tls(struct dl_phdr_info* info, size_t size, void* total) {
    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* p = &info->dlpi_phdr[i];
        if (p->p_type == PT_TLS) *(size_t*)total += (size_t)p->p_memsz + (size_t)p->p_align;
    }
    return 0;
}
#endif

/* The listening thread's stack. The thread needs little of its own, but glibc carves each
 * thread's static TLS, the program's and its libraries' thread-local variables, out of its stack
 * and refuses to start one whose stack can't hold them with 16 KB to spare. So the TLS is added on
 * top, whatever its size (CLX-417). macOS keeps a thread's TLS apart from its stack. */
static size_t listen_stack(void) {
    size_t size = 64 * 1024;
#ifdef __linux__
    size_t tls = 0;
    dl_iterate_phdr(add_tls, &tls);
    size += tls;
#endif
    long page = sysconf(_SC_PAGESIZE);
    if (page > 0) size = (size + (size_t)page - 1) / (size_t)page * (size_t)page;
    return size;
}

/* Starts the listening thread with its signals blocked: with the stack `listen_stack` works out,
 * or with the system's default stack if that's refused, as on a C library that wants more than
 * it says. Gives 0 or the error. */
static int start_thread(void) {
    sigset_t all, was;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &was);
    int e = 0;
    for (int sized = 1; sized >= 0; sized--) {
        pthread_attr_t attr;
        e = pthread_attr_init(&attr);
        if (e) break;
        if (sized) e = pthread_attr_setstacksize(&attr, listen_stack());
        if (!e) e = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (!e) e = pthread_create(&t, &attr, listen_thread, NULL);
        pthread_attr_destroy(&attr);
        if (!e) break;
    }
    pthread_sigmask(SIG_SETMASK, &was, NULL);
    return e;
}

/* Gives 0, or the error that stopped it. */
static int start(void) {
    int fds[2];
    if (pipe(fds) != 0) return errno;
    for (int i = 0; i < 2; i++) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    pipe_r = fds[0];
    pipe_w = fds[1];
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#ifndef __APPLE__
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&wait_cond, &ca);
    pthread_condattr_destroy(&ca);
    return start_thread();
}

void cx_signal_handle(cx_u32 kinds) {
    static int started;
    check_thread();
    if (!started) {
        int e = start();
        if (e) {
            char text[160];
            snprintf(text, sizeof text, "couldn't start the thread that hears of signals: %s", strerror(e));
            cx_panic_text("std.signal", text);
        }
        started = 1;
    }
    for (int k = 0; k < K_COUNT; k++) {
        if (!(kinds & (1u << k)) || (handled & (1u << k))) continue;
        struct sigaction was;
        /* One the program was started ignoring stays ignored, as nohup ignores SIGHUP, and a shell
         * SIGINT for a program it runs in the background. */
        if (sigaction(signal_of[k], NULL, &was) == 0 && !(was.sa_flags & SA_SIGINFO) && was.sa_handler == SIG_IGN &&
            signal_of[k] != SIGTERM) {
            continue;
        }
        struct sigaction a;
        memset(&a, 0, sizeof a);
        a.sa_handler = on_signal;
        a.sa_flags = SA_RESTART;
        sigemptyset(&a.sa_mask);
        if (sigaction(signal_of[k], &a, NULL) == 0) __atomic_or_fetch(&handled, 1u << k, __ATOMIC_SEQ_CST);
    }
}

static void wait_arrival(cx_i64 timeout_ms) {
    if (timeout_ms == 0 || __atomic_load_n(&arrived, __ATOMIC_ACQUIRE)) return;
    if (pipe_r < 0) {
        /* Nothing is handled, so nothing will come: only the time passes. */
        struct timespec ts = {(time_t)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000000};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
        return;
    }
    pthread_mutex_lock(&wait_mutex);
#ifdef __APPLE__
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
#else
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
#endif
    while (!__atomic_load_n(&arrived, __ATOMIC_ACQUIRE)) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&wait_cond, &wait_mutex);
            continue;
        }
#ifdef __APPLE__
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        cx_i64 spent = (cx_i64)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (spent >= timeout_ms) break;
        cx_i64 left = timeout_ms - spent;
        struct timespec rel = {(time_t)(left / 1000), (long)(left % 1000) * 1000000};
        pthread_cond_timedwait_relative_np(&wait_cond, &wait_mutex, &rel);
#else
        if (pthread_cond_timedwait(&wait_cond, &wait_mutex, &deadline) == ETIMEDOUT) break;
#endif
    }
    pthread_mutex_unlock(&wait_mutex);
}

/* As C's raise does: the signal sent to this thread, so a handled one is recorded by the handler
 * as one from outside is, and one that isn't does what it would. */
cx_bool cx_signal_raise(cx_u32 kind) {
    if (kind >= K_COUNT) return 0;
    if (!(__atomic_load_n(&handled, __ATOMIC_ACQUIRE) & (1u << kind))) cx_out_flush();
    return raise(signal_of[kind]) == 0;
}

/* Only Windows has services; systemd and launchd stop a program with SIGTERM. */
cx_bool cx_signal_service(void) { return 0; }

#endif

/* --- Both ------------------------------------------------------------------------------------- */

/* The first kind that arrived and hasn't been taken, plus one, taken; 0 if there's none. Kinds
 * that arrive again before they're taken come as one. */
cx_u32 cx_signal_take(void) {
    unsigned seen = __atomic_load_n(&arrived, __ATOMIC_ACQUIRE);
    for (int k = 0; k < K_COUNT; k++) {
        if (seen & (1u << k)) {
            __atomic_and_fetch(&arrived, ~(1u << k), __ATOMIC_SEQ_CST);
            return (cx_u32)k + 1;
        }
    }
    return 0;
}

cx_bool cx_signal_pending(void) { return __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 0; }

/* As `take`, waiting up to `timeout_ms` for one, or forever if it's negative. */
cx_u32 cx_signal_wait(cx_i64 timeout_ms) {
    check_thread();
    if (timeout_ms < 0 && !__atomic_load_n(&handled, __ATOMIC_ACQUIRE) && !__atomic_load_n(&arrived, __ATOMIC_ACQUIRE)) {
        cx_panic_text("std.signal", "wait() would wait forever, since no signal is handled: call handle() first");
    }
    wait_arrival(timeout_ms);
    return cx_signal_take();
}
