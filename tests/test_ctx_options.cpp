/* SPDX-License-Identifier: MPL-2.0 */

#include <limits>
#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

#define WAIT_FOR_BACKGROUND_THREAD_INSPECTION (0)

#ifdef ZMQ_HAVE_LINUX
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h> // for sleep()
#include <sched.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>

#define TEST_POLICY                                                            \
    (SCHED_OTHER) // NOTE: SCHED_OTHER is the default Linux scheduler

bool is_allowed_to_raise_priority ()
{
    // NOTE1: if setrlimit() fails with EPERM, this means that current user has not enough permissions.
    // NOTE2: even for privileged users (e.g., root) getrlimit() would usually return 0 as nice limit; the only way to
    //        discover if the user is able to increase the nice value is to actually try to change the rlimit:
    struct rlimit rlim;
    rlim.rlim_cur = 40;
    rlim.rlim_max = 40;
    if (setrlimit (RLIMIT_NICE, &rlim) == 0) {
        // rlim_cur == 40 means that this process is allowed to set a nice value of -20
        if (WAIT_FOR_BACKGROUND_THREAD_INSPECTION)
            printf ("This process has enough permissions to raise ZMQ "
                    "background thread priority!\n");
        return true;
    }

    if (WAIT_FOR_BACKGROUND_THREAD_INSPECTION)
        printf ("This process has NOT enough permissions to raise ZMQ "
                "background thread priority.\n");
    return false;
}

#else

#define TEST_POLICY (0)

bool is_allowed_to_raise_priority ()
{
    return false;
}

#endif


#ifdef ZMQ_HAVE_LINUX

//  Returns the tid of the calling process' thread named name_, or 0 if no
//  such thread appears within a second. libzmq names a background thread from
//  inside the thread itself, right after applying its scheduling parameters,
//  so a thread that can be found by name has already been pinned.
static pid_t await_thread_named (const char *name_)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        DIR *tasks = opendir ("/proc/self/task");
        if (tasks) {
            for (struct dirent *e = readdir (tasks); e; e = readdir (tasks)) {
                char path[288], comm[32];
                snprintf (path, sizeof (path), "/proc/self/task/%s/comm",
                          e->d_name);
                FILE *f = fopen (path, "r");
                if (!f)
                    continue;
                const char *read = fgets (comm, sizeof (comm), f);
                fclose (f);
                if (!read)
                    continue;
                comm[strcspn (comm, "\n")] = '\0';
                if (strcmp (comm, name_) == 0) {
                    closedir (tasks);
                    return static_cast<pid_t> (atoi (e->d_name));
                }
            }
            closedir (tasks);
        }
        msleep (10);
    }
    return 0;
}

static int cpu_set_count (const cpu_set_t *set_)
{
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET (i, set_))
            n++;
    return n;
}

//  Brings up a context with two I/O threads and cpus_ as its affinity list,
//  then asserts that each ZMQbg/IO/<n> ends up with expected_count_ CPUs in
//  its mask -- and, when pinned, that the CPU is the n-th of the list. The
//  name prefix keeps the two contexts of a run distinguishable in /proc.
static void check_io_thread_affinity (int prefix_,
                                      int pin_,
                                      const int *cpus_,
                                      int expected_count_)
{
    void *ctx = zmq_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_ctx_set (ctx, ZMQ_IO_THREADS, 2));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (ctx, ZMQ_THREAD_NAME_PREFIX, prefix_));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (ctx, ZMQ_THREAD_AFFINITY_CPU_ADD, cpus_[0]));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (ctx, ZMQ_THREAD_AFFINITY_CPU_ADD, cpus_[1]));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (ctx, ZMQ_THREAD_AFFINITY_CPU_PIN, pin_));

    //  I/O threads are launched on creation of the first socket.
    void *socket = zmq_socket (ctx, ZMQ_PUSH);
    TEST_ASSERT_NOT_NULL (socket);

    for (int n = 0; n < 2; n++) {
        char name[16];
        snprintf (name, sizeof (name), "%d/ZMQbg/IO/%d", prefix_, n);
        const pid_t tid = await_thread_named (name);
        TEST_ASSERT_TRUE_MESSAGE (tid > 0, name);

        cpu_set_t mask;
        CPU_ZERO (&mask);
        TEST_ASSERT_SUCCESS_ERRNO (
          sched_getaffinity (tid, sizeof (mask), &mask));
        TEST_ASSERT_EQUAL_INT_MESSAGE (expected_count_, cpu_set_count (&mask),
                                       name);
        if (expected_count_ == 1)
            TEST_ASSERT_TRUE_MESSAGE (CPU_ISSET (cpus_[n], &mask), name);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zmq_close (socket));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_ctx_term (ctx));
}

void test_ctx_thread_affinity_pin ()
{
    cpu_set_t available;
    CPU_ZERO (&available);
    if (sched_getaffinity (0, sizeof (available), &available) != 0)
        TEST_IGNORE_MESSAGE ("sched_getaffinity failed");

    int cpus[2];
    int found = 0;
    for (int i = 0; i < CPU_SETSIZE && found < 2; i++)
        if (CPU_ISSET (i, &available))
            cpus[found++] = i;
    if (found < 2)
        TEST_IGNORE_MESSAGE ("needs at least 2 usable CPUs");

    //  Off (the default): every I/O thread gets the whole list as one mask.
    check_io_thread_affinity (1, 0, cpus, 2);

    //  On: I/O thread n is pinned to the n-th CPU of the list alone.
    check_io_thread_affinity (2, 1, cpus, 1);
}

#endif

void test_ctx_thread_opts ()
{
    // verify that setting negative values (e.g., default values) fail:
    TEST_ASSERT_FAILURE_ERRNO (
      EINVAL, zmq_ctx_set (get_test_context (), ZMQ_THREAD_SCHED_POLICY,
                           ZMQ_THREAD_SCHED_POLICY_DFLT));
    TEST_ASSERT_FAILURE_ERRNO (EINVAL, zmq_ctx_set (get_test_context (),
                                                    ZMQ_THREAD_PRIORITY,
                                                    ZMQ_THREAD_PRIORITY_DFLT));


    // test scheduling policy:

    // set context options that alter the background thread CPU scheduling/affinity settings;
    // as of ZMQ 4.2.3 this has an effect only on POSIX systems (nothing happens on Windows, but still it should return success):
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_THREAD_SCHED_POLICY, TEST_POLICY));
    TEST_ASSERT_EQUAL_INT (
      TEST_POLICY, zmq_ctx_get (get_test_context (), ZMQ_THREAD_SCHED_POLICY));

    // test priority:

    // in theory SCHED_OTHER supports only the static priority 0 but quoting the docs
    //     http://man7.org/linux/man-pages/man7/sched.7.html
    // "The thread to run is chosen from the static priority 0 list based on
    // a dynamic priority that is determined only inside this list.  The
    // dynamic priority is based on the nice value [...]
    // The nice value can be modified using nice(2), setpriority(2), or sched_setattr(2)."
    // ZMQ will internally use nice(2) to set the nice value when using SCHED_OTHER.
    // However changing the nice value of a process requires appropriate permissions...
    // check that the current effective user is able to do that:
    if (is_allowed_to_raise_priority ()) {
        TEST_ASSERT_SUCCESS_ERRNO (zmq_ctx_set (
          get_test_context (), ZMQ_THREAD_PRIORITY,
          1 /* any positive value different than the default will be ok */));
    }


    // test affinity:

    // this should result in background threads being placed only on the
    // first CPU available on this system; try experimenting with other values
    // (e.g., 5 to use CPU index 5) and use "top -H" or "taskset -pc" to see the result

    int cpus_add[] = {0, 1};
    for (unsigned int idx = 0; idx < sizeof (cpus_add) / sizeof (cpus_add[0]);
         idx++) {
        TEST_ASSERT_SUCCESS_ERRNO (zmq_ctx_set (
          get_test_context (), ZMQ_THREAD_AFFINITY_CPU_ADD, cpus_add[idx]));
    }

    // you can also remove CPUs from list of affinities:
    int cpus_remove[] = {1};
    for (unsigned int idx = 0;
         idx < sizeof (cpus_remove) / sizeof (cpus_remove[0]); idx++) {
        TEST_ASSERT_SUCCESS_ERRNO (zmq_ctx_set (get_test_context (),
                                                ZMQ_THREAD_AFFINITY_CPU_REMOVE,
                                                cpus_remove[idx]));
    }


    // test per-thread affinity pinning:

    // off by default: the whole affinity list is applied as one mask to every
    // background thread. When on, each I/O thread gets a single CPU of the
    // list instead; see test_ctx_thread_affinity_pin below.
    TEST_ASSERT_EQUAL_INT (
      0, zmq_ctx_get (get_test_context (), ZMQ_THREAD_AFFINITY_CPU_PIN));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_THREAD_AFFINITY_CPU_PIN, 1));
    TEST_ASSERT_EQUAL_INT (
      1, zmq_ctx_get (get_test_context (), ZMQ_THREAD_AFFINITY_CPU_PIN));
    TEST_ASSERT_FAILURE_ERRNO (
      EINVAL,
      zmq_ctx_set (get_test_context (), ZMQ_THREAD_AFFINITY_CPU_PIN, -1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_THREAD_AFFINITY_CPU_PIN, 0));


    // test INTEGER thread name prefix:

    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_THREAD_NAME_PREFIX, 1234));
    TEST_ASSERT_EQUAL_INT (
      1234, zmq_ctx_get (get_test_context (), ZMQ_THREAD_NAME_PREFIX));

#ifdef ZMQ_BUILD_DRAFT_API
    // test STRING thread name prefix:

    const char prefix[] = "MyPrefix9012345"; // max len is 16 chars

    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set_ext (get_test_context (), ZMQ_THREAD_NAME_PREFIX, prefix,
                       sizeof (prefix) / sizeof (char)));

    char buf[16];
    size_t buflen = sizeof (buf) / sizeof (char);
    zmq_ctx_get_ext (get_test_context (), ZMQ_THREAD_NAME_PREFIX, buf, &buflen);
    TEST_ASSERT_EQUAL_STRING (prefix, buf);
#endif
}

void test_ctx_zero_copy ()
{
#ifdef ZMQ_ZERO_COPY_RECV
    int zero_copy;
    // Default value is 1.
    zero_copy = zmq_ctx_get (get_test_context (), ZMQ_ZERO_COPY_RECV);
    TEST_ASSERT_EQUAL_INT (1, zero_copy);

    // Test we can set it to 0.
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_ZERO_COPY_RECV, 0));
    zero_copy = zmq_ctx_get (get_test_context (), ZMQ_ZERO_COPY_RECV);
    TEST_ASSERT_EQUAL_INT (0, zero_copy);

    // Create a TCP socket pair using the context and test that messages can be
    // received. Note that inproc sockets cannot be used for this test.
    void *pull = zmq_socket (get_test_context (), ZMQ_PULL);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pull, endpoint, sizeof endpoint);

    void *push = zmq_socket (get_test_context (), ZMQ_PUSH);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (push, endpoint));

    const char *small_str = "abcd";
    const char *large_str =
      "01234567890123456789012345678901234567890123456789";

    send_string_expect_success (push, small_str, 0);
    send_string_expect_success (push, large_str, 0);

    recv_string_expect_success (pull, small_str, 0);
    recv_string_expect_success (pull, large_str, 0);

    // Clean up.
    TEST_ASSERT_SUCCESS_ERRNO (zmq_close (push));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_close (pull));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_ZERO_COPY_RECV, 1));
    TEST_ASSERT_EQUAL_INT (
      1, zmq_ctx_get (get_test_context (), ZMQ_ZERO_COPY_RECV));
#endif
}

void test_ctx_option_max_sockets ()
{
    TEST_ASSERT_EQUAL_INT (ZMQ_MAX_SOCKETS_DFLT,
                           zmq_ctx_get (get_test_context (), ZMQ_MAX_SOCKETS));
}

void test_ctx_option_socket_limit ()
{
#if defined(ZMQ_USE_SELECT)
    TEST_ASSERT_EQUAL_INT (FD_SETSIZE - 1, zmq_ctx_get (ctx, ZMQ_SOCKET_LIMIT));
#elif defined(ZMQ_USE_POLL) || defined(ZMQ_USE_EPOLL)                          \
  || defined(ZMQ_USE_DEVPOLL) || defined(ZMQ_USE_KQUEUE)
    TEST_ASSERT_EQUAL_INT (65535, zmq_ctx_get (ctx, ZMQ_SOCKET_LIMIT));
#endif
}

void test_ctx_option_io_threads ()
{
    TEST_ASSERT_EQUAL_INT (ZMQ_IO_THREADS_DFLT,
                           zmq_ctx_get (get_test_context (), ZMQ_IO_THREADS));
}

void test_ctx_option_ipv6 ()
{
    TEST_ASSERT_EQUAL_INT (0, zmq_ctx_get (get_test_context (), ZMQ_IPV6));
}

void test_ctx_option_msg_t_size ()
{
#if defined(ZMQ_MSG_T_SIZE)
    TEST_ASSERT_EQUAL_INT (sizeof (zmq_msg_t),
                           zmq_ctx_get (get_test_context (), ZMQ_MSG_T_SIZE));
#endif
}

void test_ctx_option_ipv6_set ()
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_IPV6, true));
    TEST_ASSERT_EQUAL_INT (1, zmq_ctx_get (get_test_context (), ZMQ_IPV6));
}

void test_ctx_option_blocky ()
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_IPV6, true));

    void *router = test_context_socket (ZMQ_ROUTER);
    int value;
    size_t optsize = sizeof (int);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_getsockopt (router, ZMQ_IPV6, &value, &optsize));
    TEST_ASSERT_EQUAL_INT (1, value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_getsockopt (router, ZMQ_LINGER, &value, &optsize));
    TEST_ASSERT_EQUAL_INT (-1, value);
    test_context_socket_close (router);

#if WAIT_FOR_BACKGROUND_THREAD_INSPECTION
    // this is useful when you want to use an external tool (like top or taskset) to view
    // properties of the background threads
    printf ("Sleeping for 100sec. You can now use 'top -H -p $(pgrep -f "
            "test_ctx_options)' and 'taskset -pc <ZMQ background thread PID>' "
            "to view ZMQ background thread properties.\n");
    sleep (100);
#endif

    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_ctx_set (get_test_context (), ZMQ_BLOCKY, false));
    TEST_ASSERT_EQUAL_INT (0, TEST_ASSERT_SUCCESS_ERRNO ((zmq_ctx_get (
                                get_test_context (), ZMQ_BLOCKY))));
    router = test_context_socket (ZMQ_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_getsockopt (router, ZMQ_LINGER, &value, &optsize));
    TEST_ASSERT_EQUAL_INT (0, value);
    test_context_socket_close (router);
}

void test_ctx_option_invalid ()
{
    TEST_ASSERT_EQUAL_INT (-1, zmq_ctx_set (get_test_context (), -1, 0));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (-1, zmq_ctx_get (get_test_context (), -1));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_ctx_option_max_sockets);
    RUN_TEST (test_ctx_option_socket_limit);
    RUN_TEST (test_ctx_option_io_threads);
    RUN_TEST (test_ctx_option_ipv6);
    RUN_TEST (test_ctx_option_msg_t_size);
    RUN_TEST (test_ctx_option_ipv6_set);
    RUN_TEST (test_ctx_thread_opts);
#ifdef ZMQ_HAVE_LINUX
    RUN_TEST (test_ctx_thread_affinity_pin);
#endif
    RUN_TEST (test_ctx_zero_copy);
    RUN_TEST (test_ctx_option_blocky);
    RUN_TEST (test_ctx_option_invalid);
    return UNITY_END ();
}
