#ifndef __ZMQ_PACING_BUDGET_HPP_INCLUDED__
#define __ZMQ_PACING_BUDGET_HPP_INCLUDED__

#include <set>

#include "fd.hpp"
#include "macros.hpp"
#include "mutex.hpp"
#include "tcp.hpp"

namespace zmq
{
//  Vibe: shares one egress budget (bytes/sec) across all live TCP connections
//  of a socket. Every membership change re-tunes SO_MAX_PACING_RATE on every
//  fd to budget / count, so the socket aggregate stays bounded no matter how
//  peers scale in or out.
class pacing_budget_t
{
  public:
    pacing_budget_t (int budget_) : _budget (budget_) {}

    void add (fd_t fd_)
    {
        scoped_lock_t lock (_sync);
        _fds.insert (fd_);
        retune ();
    }

    void remove (fd_t fd_)
    {
        scoped_lock_t lock (_sync);
        if (_fds.erase (fd_))
            retune ();
    }

  private:
    void retune ()
    {
        if (_fds.empty ())
            return;
        const int rate = _budget / static_cast<int> (_fds.size ());
        for (std::set<fd_t>::const_iterator it = _fds.begin (),
                                            end = _fds.end ();
             it != end; ++it)
            tune_tcp_max_pacing_rate (*it, rate);
    }

    mutex_t _sync;
    std::set<fd_t> _fds;
    const int _budget;

    ZMQ_NON_COPYABLE_NOR_MOVABLE (pacing_budget_t)
};
}

#endif
