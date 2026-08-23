// Writing to a socket whose peer has closed must not kill the process.
//
// The default disposition of SIGPIPE is to terminate, so the error return that
// basic_socketbuf::sync() carefully checks never arrived -- the process was
// already gone.  For a mail client that is not exotic: a server dropping an
// idle IMAP connection is routine, and the symptom is jlib-mail vanishing
// without a word.
//
// Reproduced over loopback, which needs no server and no network.  The first
// write usually succeeds, because it only has to reach the kernel's buffer;
// the second is the one that finds out the peer is gone.
//
// The first section deliberately writes to a raw, unprotected socket in a
// child process, to show the signal really does fire here.  Without that this
// test could pass on a platform that never raises SIGPIPE at all, and would be
// saying nothing.
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sys.hh>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what
              << ": got " << got << ", expected " << want << "\n";
}

/** A listener on loopback that accepts one connection and then drops it. */
static int listener(unsigned short* port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                       // any free port

    if(::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0) return -1;
    if(::listen(fd, 1) < 0) return -1;

    socklen_t len = sizeof(a);
    if(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &len) < 0) return -1;

    *port = ntohs(a.sin_port);
    return fd;
}

/**
 * Does an unprotected write to a dead peer actually raise SIGPIPE here?
 *
 * In a child, because if the answer is yes the child dies -- which is the
 * point.  A test that cannot demonstrate the failure cannot demonstrate the
 * fix either.
 */
static int raw_write_dies() {
    unsigned short port = 0;
    const int lfd = listener(&port);
    if(lfd < 0) return -1;

    const pid_t pid = ::fork();
    if(pid < 0) { ::close(lfd); return -1; }

    if(pid == 0) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);

        if(::connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0)
            ::_exit(2);

        // deliberately no nosigpipe() here
        const int peer = ::accept(lfd, 0, 0);
        ::close(peer);

        // Paced deliberately.  Closing a TCP peer sends a FIN, and writes keep
        // succeeding until the RST comes back -- so a tight loop of small
        // writes can finish before the failure ever arrives, and this probe
        // then reports "no SIGPIPE here" on a platform that raises it.
        for(int i = 0; i < 40; i++) {
            if(::write(fd, "this goes nowhere\r\n", 19) < 0) break;
            ::usleep(20000);
        }

        ::_exit(0);        // survived: no SIGPIPE on this platform
    }

    ::close(lfd);

    int status = 0;
    ::waitpid(pid, &status, 0);

    return (WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE) ? 1 : 0;
}

int main() {
    using namespace jlib::sys;

    const int raw = raw_write_dies();
    if(raw < 0) {
        std::cerr << "could not set up a loopback socket, skipping" << std::endl;
        return 77;
    }

    std::cout << "unprotected write to a dead peer raises SIGPIPE here: "
              << (raw ? "yes" : "no") << "\n";

    if(!raw) {
        // Nothing to protect against on this platform, so nothing to prove.
        std::cerr << "this platform does not raise SIGPIPE for it; skipping"
                  << std::endl;
        return 77;
    }

    check("unprotected write to a dead peer dies", 1, 1);

    // The same writes through socketstream, and in a child for the same reason
    // the probe above used one: if it is going to be killed, this process has
    // to survive in order to say so.  Checking sock.good() in-process would
    // pass whether or not any write ever happened.
    unsigned short port = 0;
    const int lfd = listener(&port);
    if(lfd < 0) {
        std::cerr << "could not set up a loopback socket, skipping" << std::endl;
        return 77;
    }

    const pid_t pid = ::fork();
    if(pid < 0) { ::close(lfd); return 77; }

    if(pid == 0) {
        ::alarm(20);        // rather than hang the suite

        try {
            socketstream sock("127.0.0.1", port);

            const int peer = ::accept(lfd, 0, 0);
            if(peer < 0) ::_exit(3);
            ::close(peer);

            for(int i = 0; i < 40 && sock; i++) {
                sock << "this goes nowhere\r\n";
                sock.flush();
                ::usleep(20000);
            }
        }
        catch(std::exception&) {
            // An exception is a fine outcome; being killed is not.
        }

        ::_exit(0);
    }

    ::close(lfd);

    int status = 0;
    ::waitpid(pid, &status, 0);

    if(WIFEXITED(status) && WEXITSTATUS(status) == 3) {
        std::cerr << "could not connect over loopback, skipping" << std::endl;
        return 77;
    }

    const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;

    check("the same through socketstream survives", killed ? 0 : 1, 1);

    return failures ? 1 : 0;
}
