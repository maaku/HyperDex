// Copyright (c) 2012-2013, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// STL
#include <algorithm>
#include <sstream>

// Replicant
#include <replicant_state_machine.h>

// HyperDex
#include "common/serialization.h"
#include "coordinator/coordinator.h"
#include "coordinator/transitions.h"
#include "coordinator/util.h"

#define ALARM_INTERVAL 10

using hyperdex::coordinator;
using hyperdex::region;
using hyperdex::region_intent;
using hyperdex::server;
using hyperdex::transfer;

namespace
{

std::string
to_string(const po6::net::location& loc)
{
    std::ostringstream oss;
    oss << loc;
    return oss.str();
}

template <typename T>
void
remove(const T& t, std::vector<T>* v)
{
    for (size_t i = 0; i < v->size(); )
    {
        if ((*v)[i] == t)
        {
            for (size_t j = 0; j + 1 < v->size(); ++j)
            {
                (*v)[j] = (*v)[j + 1];
            }

            v->pop_back();
        }
        else
        {
            ++i;
        }
    }
}

template <class T, class TID>
void
remove_id(const TID& id, std::vector<T>* v)
{
    for (size_t i = 0; i < v->size(); )
    {
        if ((*v)[i].id == id)
        {
            for (size_t j = i + 1; j < v->size(); ++j)
            {
                (*v)[j - 1] = (*v)[j];
            }

            v->pop_back();
        }
        else
        {
            ++i;
        }
    }
}

} // namespace

coordinator :: coordinator()
    : m_cluster(0)
    , m_counter(1)
    , m_version(0)
    , m_servers()
    , m_permutation()
    , m_spares()
    , m_desired_spares(0)
    , m_spaces()
    , m_intents()
    , m_deferred_init()
    , m_transfers()
    , m_config_ack_through(0)
    , m_config_ack_barrier()
    , m_config_stable_through(0)
    , m_config_stable_barrier()
    , m_checkpoint(0)
    , m_checkpoint_stable_through(0)
    , m_checkpoint_gc_through(0)
    , m_checkpoint_stable_barrier()
    , m_latest_config()
{
    assert(m_config_ack_through == m_config_ack_barrier.min_version());
    assert(m_config_stable_through == m_config_stable_barrier.min_version());
    assert(m_checkpoint_stable_through == m_checkpoint_stable_barrier.min_version());
}

coordinator :: ~coordinator() throw ()
{
}

void
coordinator :: init(replicant_state_machine_context* ctx, uint64_t token)
{
    FILE* log = replicant_state_machine_log_stream(ctx);

    if (m_cluster != 0)
    {
        fprintf(log, "cannot initialize HyperDex cluster with id %lu "
                     "because it is already initialized to %lu\n", token, m_cluster);
        // we lie to the client and pretend all is well
        return generate_response(ctx, COORD_SUCCESS);
    }

    replicant_state_machine_alarm(ctx, "alarm", ALARM_INTERVAL);
    fprintf(log, "initializing HyperDex cluster with id %lu\n", token);
    m_cluster = token;
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_register(replicant_state_machine_context* ctx,
                               const server_id& sid,
                               const po6::net::location& bind_to)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (srv)
    {
        std::string str(to_string(srv->bind_to));
        fprintf(log, "cannot register server(%lu) because the id belongs to "
                     "server(%lu, %s)\n", sid.get(), srv->id.get(), str.c_str());
        return generate_response(ctx, hyperdex::COORD_DUPLICATE);
    }

    srv = new_server(sid);
    srv->state = server::ASSIGNED;
    srv->bind_to = bind_to;
    fprintf(log, "registered server(%lu)\n", sid.get());
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_online(replicant_state_machine_context* ctx,
                             const server_id& sid,
                             const po6::net::location* bind_to)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (!srv)
    {
        fprintf(log, "cannot bring server(%lu) online because "
                     "the server doesn't exist\n", sid.get());
        return generate_response(ctx, hyperdex::COORD_NOT_FOUND);
    }

    if (srv->state != server::ASSIGNED &&
        srv->state != server::NOT_AVAILABLE &&
        srv->state != server::SHUTDOWN &&
        srv->state != server::AVAILABLE)
    {
        fprintf(log, "cannot bring server(%lu) online because the server is "
                     "%s\n", sid.get(), server::to_string(srv->state));
        return generate_response(ctx, hyperdex::COORD_NO_CAN_DO);
    }

    bool changed = false;

    if (bind_to && srv->bind_to != *bind_to)
    {
        std::string from(to_string(srv->bind_to));
        std::string to(to_string(*bind_to));

        for (size_t i = 0; i < m_servers.size(); ++i)
        {
            if (m_servers[i].id != sid &&
                m_servers[i].bind_to == *bind_to)
            {
                fprintf(log, "cannot change server(%lu) to %s "
                             "because that address is in use by "
                             "server(%lu)\n", sid.get(), to.c_str(),
                             m_servers[i].id.get());
                return generate_response(ctx, hyperdex::COORD_DUPLICATE);
            }
        }

        fprintf(log, "changing server(%lu)'s address from %s to %s\n",
                     sid.get(), from.c_str(), to.c_str());
        srv->bind_to = *bind_to;
        changed = true;
    }

    if (srv->state != server::AVAILABLE)
    {
        fprintf(log, "changing server(%lu) from %s to %s\n",
                     sid.get(), server::to_string(srv->state),
                     server::to_string(server::AVAILABLE));
        srv->state = server::AVAILABLE;

        if (m_spares.size() < m_desired_spares)
        {
            m_spares.push_back(sid);
        }
        else
        {
            m_permutation.push_back(sid);
        }

        rebalance_replica_sets(ctx);
        changed = true;
    }

    if (changed)
    {
        generate_next_configuration(ctx);
    }

    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_offline(replicant_state_machine_context* ctx,
                              const server_id& sid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (!srv)
    {
        fprintf(log, "cannot bring server(%lu) offline because "
                     "the server doesn't exist\n", sid.get());
        return generate_response(ctx, hyperdex::COORD_NOT_FOUND);
    }

    if (srv->state != server::ASSIGNED &&
        srv->state != server::NOT_AVAILABLE &&
        srv->state != server::AVAILABLE &&
        srv->state != server::SHUTDOWN)
    {
        fprintf(log, "cannot bring server(%lu) offline because the server is "
                     "%s\n", sid.get(), server::to_string(srv->state));
        return generate_response(ctx, hyperdex::COORD_NO_CAN_DO);
    }

    if (srv->state != server::NOT_AVAILABLE)
    {
        fprintf(log, "changing server(%lu) from %s to %s\n",
                     sid.get(), server::to_string(srv->state),
                     server::to_string(server::NOT_AVAILABLE));
        srv->state = server::NOT_AVAILABLE;
        remove(sid, &m_permutation);
        remove(sid, &m_spares);
        rebalance_replica_sets(ctx);
        generate_next_configuration(ctx);
    }

    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_shutdown(replicant_state_machine_context* ctx,
                               const server_id& sid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (!srv)
    {
        fprintf(log, "cannot shutdown server(%lu) because "
                     "the server doesn't exist\n", sid.get());
        return generate_response(ctx, hyperdex::COORD_NOT_FOUND);
    }

    if (srv->state != server::ASSIGNED &&
        srv->state != server::NOT_AVAILABLE &&
        srv->state != server::AVAILABLE &&
        srv->state != server::SHUTDOWN)
    {
        fprintf(log, "cannot shutdown server(%lu) because the server is "
                     "%s\n", sid.get(), server::to_string(srv->state));
        return generate_response(ctx, hyperdex::COORD_NO_CAN_DO);
    }

    if (srv->state != server::SHUTDOWN)
    {
        fprintf(log, "changing server(%lu) from %s to %s\n",
                     sid.get(), server::to_string(srv->state),
                     server::to_string(server::SHUTDOWN));
        srv->state = server::SHUTDOWN;
        // XXX we can do it better
        remove(sid, &m_permutation);
        remove(sid, &m_spares);
        rebalance_replica_sets(ctx);
        generate_next_configuration(ctx);
    }

    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_kill(replicant_state_machine_context* ctx,
                           const server_id& sid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (!srv)
    {
        fprintf(log, "cannot kill server(%lu) because "
                     "the server doesn't exist\n", sid.get());
        return generate_response(ctx, hyperdex::COORD_NOT_FOUND);
    }

    if (srv->state != server::KILLED)
    {
        fprintf(log, "changing server(%lu) from %s to %s\n",
                     sid.get(), server::to_string(srv->state),
                     server::to_string(server::KILLED));
        srv->state = server::KILLED;
        remove(sid, &m_permutation);
        remove(sid, &m_spares);
        rebalance_replica_sets(ctx);
        generate_next_configuration(ctx);
    }

    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_forget(replicant_state_machine_context* ctx,
                             const server_id& sid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    server* srv = get_server(sid);

    if (!srv)
    {
        fprintf(log, "cannot forget server(%lu) because "
                     "the server doesn't exist\n", sid.get());
        return generate_response(ctx, hyperdex::COORD_NOT_FOUND);
    }

    for (size_t i = 0; i < m_servers.size(); ++i)
    {
        if (m_servers[i].id == sid)
        {
            std::swap(m_servers[i], m_servers[m_servers.size() - 1]);
            m_servers.pop_back();
        }
    }

    std::stable_sort(m_servers.begin(), m_servers.end());
    remove(sid, &m_permutation);
    remove(sid, &m_spares);
    rebalance_replica_sets(ctx);
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: server_suspect(replicant_state_machine_context* ctx,
                              const server_id& sid, uint64_t version)
{
    if (m_version == version)
    {
        server_offline(ctx, sid);
    }

    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: space_add(replicant_state_machine_context* ctx, const space& _s)
{
    FILE* log = replicant_state_machine_log_stream(ctx);

    if (!_s.validate())
    {
        fprintf(log, "could not add space \"%s\" because the space does not validate\n", _s.name);
        return generate_response(ctx, COORD_MALFORMED);
    }

    if (m_spaces.find(std::string(_s.name)) != m_spaces.end())
    {
        fprintf(log, "could not add space \"%s\" because there is already a space with that name\n", _s.name);
        return generate_response(ctx, COORD_DUPLICATE);
    }

    // Assign unique ids throughout the space
    std::tr1::shared_ptr<space> s(new space(_s));
    s->id = space_id(m_counter);
    ++m_counter;

    for (size_t i = 0; i < s->subspaces.size(); ++i)
    {
        s->subspaces[i].id = subspace_id(m_counter);
        ++m_counter;

        for (size_t j = 0; j < s->subspaces[i].regions.size(); ++j)
        {
            s->subspaces[i].regions[j].id = region_id(m_counter);
            ++m_counter;
            s->subspaces[i].regions[j].replicas.clear();
        }
    }

    m_spaces.insert(std::make_pair(std::string(s->name), s));
    fprintf(log, "successfully added space \"%s\" with space_id(%lu)\n", s->name, s->id.get());
    initial_space_layout(ctx, s.get());
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: space_rm(replicant_state_machine_context* ctx, const char* name)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    space_map_t::iterator it;
    it = m_spaces.find(std::string(name));

    if (it == m_spaces.end())
    {
        fprintf(log, "could not remove space \"%s\" because it doesn't exist\n", name);
        return generate_response(ctx, COORD_NOT_FOUND);
    }
    else
    {
        space_id sid(it->second->id.get());
        fprintf(log, "successfully removed space \"%s\"/space_id(%lu)\n", name, sid.get());
        std::vector<region_id> rids;
        regions_in_space(it->second, &rids);
        std::sort(rids.begin(), rids.end());
        m_spaces.erase(it);

        for (size_t i = 0; i < m_intents.size(); )
        {
            if (std::binary_search(rids.begin(), rids.end(), m_intents[i].id))
            {
                std::swap(m_intents[i], m_intents.back());
                m_intents.pop_back();
            }
            else
            {
                ++i;
            }
        }

        for (size_t i = 0; i < m_transfers.size(); )
        {
            if (std::binary_search(rids.begin(), rids.end(), m_transfers[i].rid))
            {
                std::swap(m_transfers[i], m_transfers.back());
                m_transfers.pop_back();
            }
            else
            {
                ++i;
            }
        }

        remove(sid, &m_deferred_init);
        generate_next_configuration(ctx);
        return generate_response(ctx, COORD_SUCCESS);
    }
}

void
coordinator :: transfer_go_live(replicant_state_machine_context* ctx,
                                uint64_t version,
                                const transfer_id& xid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    transfer* xfer = get_transfer(xid);

    if (!xfer)
    {
        if (version < m_version)
        {
            return;
        }

        fprintf(log, "cannot make transfer_id(%lu) live because it doesn't exist\n", xid.get());
        return generate_response(ctx, COORD_SUCCESS);
    }

    region* reg = get_region(xfer->rid);

    if (!reg)
    {
        fprintf(log, "cannot make transfer_id(%lu) live because it doesn't exist\n", xid.get());
        INVARIANT_BROKEN("transfer refers to nonexistent region");
        return generate_response(ctx, COORD_SUCCESS);
    }

    // If the transfer is already live
    if (reg->replicas.size() > 1 &&
        reg->replicas[reg->replicas.size() - 2].si == xfer->src &&
        reg->replicas[reg->replicas.size() - 1].si == xfer->dst)
    {
        return generate_response(ctx, COORD_SUCCESS);
    }

    if (reg->replicas.empty() || reg->replicas.back().si != xfer->src)
    {
        INVARIANT_BROKEN("transfer in a bad state");
        return generate_response(ctx, COORD_SUCCESS);
    }

    reg->replicas.push_back(replica(xfer->dst, xfer->vdst));
    fprintf(log, "transfer_id(%lu) is live\n", xid.get());
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: transfer_complete(replicant_state_machine_context* ctx,
                                 uint64_t version,
                                 const transfer_id& xid)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    transfer* xfer = get_transfer(xid);

    if (!xfer)
    {
        if (version < m_version)
        {
            return;
        }

        fprintf(log, "cannot complete transfer_id(%lu) because it doesn't exist\n", xid.get());
        return generate_response(ctx, COORD_SUCCESS);
    }

    region* reg = get_region(xfer->rid);

    if (!reg)
    {
        fprintf(log, "cannot complete transfer_id(%lu) because it doesn't exist\n", xid.get());
        INVARIANT_BROKEN("transfer refers to nonexistent region");
        return generate_response(ctx, COORD_SUCCESS);
    }

    if (!(reg->replicas.size() > 1 &&
          reg->replicas[reg->replicas.size() - 2].si == xfer->src &&
          reg->replicas[reg->replicas.size() - 1].si == xfer->dst))
    {
        fprintf(log, "cannot complete transfer_id(%lu) because it is not live\n", xid.get());
        return generate_response(ctx, COORD_SUCCESS);
    }

    del_transfer(xfer->id);
    fprintf(log, "transfer_id(%lu) is complete\n", xid.get());
    converge_intent(ctx, reg);
    generate_next_configuration(ctx);
    return generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: config_get(replicant_state_machine_context* ctx)
{
    assert(m_cluster != 0 && m_version != 0);
    assert(m_latest_config.get());
    const char* output = reinterpret_cast<const char*>(m_latest_config->data());
    size_t output_sz = m_latest_config->size();
    replicant_state_machine_set_response(ctx, output, output_sz);
}

void
coordinator :: config_ack(replicant_state_machine_context* ctx,
                          const server_id& sid, uint64_t version)
{
    m_config_ack_barrier.pass(version, sid);
    check_ack_condition(ctx);
}

void
coordinator :: config_stable(replicant_state_machine_context* ctx,
                             const server_id& sid, uint64_t version)
{
    m_config_stable_barrier.pass(version, sid);
    check_stable_condition(ctx);
}

void
coordinator :: checkpoint(replicant_state_machine_context* ctx)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    uint64_t cond_state = 0;

    if (replicant_state_machine_condition_broadcast(ctx, "checkp", &cond_state) < 0)
    {
        fprintf(log, "could not broadcast on \"checkp\" condition\n");
    }

    ++m_checkpoint;
    fprintf(log, "establishing checkpoint %lu\n", m_checkpoint);
    assert(cond_state == m_checkpoint);
    assert(m_checkpoint_stable_through <= m_checkpoint);
    std::vector<server_id> sids;
    servers_in_configuration(&sids);
    m_checkpoint_stable_barrier.new_version(m_checkpoint, sids);
    check_checkpoint_stable_condition(ctx);
}

void
coordinator :: checkpoint_stable(replicant_state_machine_context* ctx,
                                 const server_id& sid,
                                 uint64_t config,
                                 uint64_t number)
{
    if (config < m_version)
    {
        return generate_response(ctx, COORD_NO_CAN_DO);
    }

    m_checkpoint_stable_barrier.pass(number, sid);
    check_checkpoint_stable_condition(ctx);
    generate_response(ctx, COORD_SUCCESS);
}

void
coordinator :: alarm(replicant_state_machine_context* ctx)
{
    replicant_state_machine_alarm(ctx, "alarm", ALARM_INTERVAL);
    checkpoint(ctx);
}

void
coordinator :: debug_dump(replicant_state_machine_context* ctx)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    fprintf(log, "=== begin debug dump ===========================================================\n");
    fprintf(log, "permutation:\n");

    for (size_t i = 0; i < m_permutation.size(); ++i)
    {
        fprintf(log, " - %lu\n", m_permutation[i].get());
    }

    fprintf(log, "spares (desire %lu):\n", m_desired_spares);

    for (size_t i = 0; i < m_spares.size(); ++i)
    {
        fprintf(log, " - %lu\n", m_spares[i].get());
    }

    fprintf(log, "intents:\n");

    for (size_t i = 0; i < m_intents.size(); ++i)
    {
        fprintf(log, " - region=%lu, checkpoint=%lu replicas=[", m_intents[i].id.get(), m_intents[i].checkpoint);

        for (size_t j = 0; j < m_intents[i].replicas.size(); ++j)
        {
            if (j == 0)
            {
                fprintf(log, "%lu", m_intents[i].replicas[j].get());
            }
            else
            {
                fprintf(log, ", %lu", m_intents[i].replicas[j].get());
            }
        }

        fprintf(log, "]\n");
    }

    fprintf(log, "transfers:\n");

    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        fprintf(log, " - id=%lu rid=%lu src=%lu vsrc=%lu dst=%lu vdst=%lu\n",
                m_transfers[i].id.get(), m_transfers[i].rid.get(),
                m_transfers[i].src.get(), m_transfers[i].vsrc.get(),
                m_transfers[i].dst.get(), m_transfers[i].vdst.get());
    }

    fprintf(log, "config ack through: %lu\n", m_config_ack_through);
    fprintf(log, "config stable through: %lu\n", m_config_stable_through);
    fprintf(log, "checkpoint: latest=%lu, stable=%lu, gc=%lu\n",
            m_checkpoint, m_checkpoint_stable_through, m_checkpoint_gc_through);
    fprintf(log, "=== end debug dump =============================================================\n");
}

server*
coordinator :: new_server(const server_id& sid)
{
    size_t idx = m_servers.size();
    m_servers.push_back(server(sid));

    for (; idx > 0; --idx)
    {
        if (m_servers[idx - 1].id < m_servers[idx].id)
        {
            break;
        }

        std::swap(m_servers[idx - 1], m_servers[idx]);
    }

    return &m_servers[idx];
}

server*
coordinator :: get_server(const server_id& sid)
{
    for (size_t i = 0; i < m_servers.size(); ++i)
    {
        if (m_servers[i].id == sid)
        {
            return &m_servers[i];
        }
    }

    return NULL;
}

namespace
{

bool
compare_space_ptr_by_r_p(const std::tr1::shared_ptr<hyperdex::space>& lhs,
                         const std::tr1::shared_ptr<hyperdex::space>& rhs)
{
    if (lhs->fault_tolerance < rhs->fault_tolerance)
    {
        return true;
    }
    else if (lhs->fault_tolerance == rhs->fault_tolerance)
    {
        return lhs->predecessor_width < rhs->predecessor_width;
    }
    else
    {
        return false;
    }
}

} // namespace

void
coordinator :: rebalance_replica_sets(replicant_state_machine_context* ctx)
{
    uint64_t R = 0;
    uint64_t P = 0;
    std::vector<server_id> replica_storage;
    std::vector<replica_set> replica_sets;
    std::vector<space_ptr> spaces;
    spaces.reserve(m_spaces.size());

    for (space_map_t::iterator it = m_spaces.begin();
            it != m_spaces.end(); ++it)
    {
        spaces.push_back(it->second);
    }

    std::sort(spaces.begin(), spaces.end(), compare_space_ptr_by_r_p);

    for (size_t i = 0; i < spaces.size(); ++i)
    {
        if (std::find(m_deferred_init.begin(),
                      m_deferred_init.end(),
                      spaces[i]->id) != m_deferred_init.end())
        {
            remove(spaces[i]->id, &m_deferred_init);
            initial_space_layout(ctx, spaces[i].get());
            continue;
        }

        if (spaces[i]->fault_tolerance + 1 != R ||
            spaces[i]->predecessor_width != P)
        {
            R = spaces[i]->fault_tolerance + 1;
            P = spaces[i]->predecessor_width;
            compute_replica_sets(R, P, m_permutation,
                                 &replica_storage,
                                 &replica_sets);
        }

        setup_intents(ctx, replica_sets, spaces[i].get(), false);
    }
}

void
coordinator :: initial_space_layout(replicant_state_machine_context* ctx,
                                    space* s)
{
    if (m_permutation.empty())
    {
        m_deferred_init.push_back(s->id);
        return;
    }

    uint64_t R = s->fault_tolerance + 1;
    uint64_t P = s->predecessor_width;
    std::vector<server_id> replica_storage;
    std::vector<replica_set> replica_sets;
    compute_replica_sets(R, P, m_permutation,
                         &replica_storage,
                         &replica_sets);
    setup_intents(ctx, replica_sets, s, true);
}

region*
coordinator :: get_region(const region_id& rid)
{
    for (space_map_t::iterator it = m_spaces.begin();
            it != m_spaces.end(); ++it)
    {
        space& s(*it->second);

        for (size_t i = 0; i < s.subspaces.size(); ++i)
        {
            subspace& ss(s.subspaces[i]);

            for (size_t j = 0; j < ss.regions.size(); ++j)
            {
                if (ss.regions[j].id == rid)
                {
                    return &ss.regions[j];
                }
            }
        }
    }

    return NULL;
}

void
coordinator :: setup_intents(replicant_state_machine_context* ctx,
                             const std::vector<replica_set>& replica_sets,
                             space* s, bool skip_transfers)
{
    if (replica_sets.size() == 0)
    {
        return;
    }

    for (size_t ss_idx = 0; ss_idx < s->subspaces.size(); ++ss_idx)
    {
        subspace& ss(s->subspaces[ss_idx]);

        for (size_t reg_idx = 0; reg_idx < ss.regions.size(); ++reg_idx)
        {
            region* reg = &ss.regions[reg_idx];
            size_t idx = (reg_idx * replica_sets.size()) / ss.regions.size();
            assert(idx < replica_sets.size());
            bool need_change = replica_sets[idx].size() != reg->replicas.size();

            for (size_t i = 0; !need_change && i < replica_sets[idx].size(); ++i)
            {
                need_change = replica_sets[idx][i] != reg->replicas[i].si;
            }

            if (!need_change)
            {
                continue;
            }

            region_intent* ri = get_region_intent(reg->id);

            if (!ri)
            {
                ri = new_region_intent(reg->id);
            }

            ri->replicas.resize(replica_sets[idx].size());

            for (size_t i = 0; i < replica_sets[idx].size(); ++i)
            {
                ri->replicas[i] = replica_sets[idx][i];
            }

            if (!skip_transfers)
            {
                converge_intent(ctx, reg, ri);
            }
            else
            {
                assert(reg->replicas.empty());

                for (size_t i = 0; i < ri->replicas.size(); ++i)
                {
                    reg->replicas.push_back(replica(ri->replicas[i], virtual_server_id(m_counter)));
                    ++m_counter;
                }
            }
        }
    }
}

void
coordinator :: converge_intent(replicant_state_machine_context* ctx,
                               region* reg)
{
    region_intent* ri = get_region_intent(reg->id);

    if (ri)
    {
        converge_intent(ctx, reg, ri);
    }
}

void
coordinator :: converge_intent(replicant_state_machine_context* ctx,
                               region* reg, region_intent* ri)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    // if there is a transfer
    transfer* xfer = get_transfer(reg->id);

    if (xfer)
    {
        for (size_t i = 0; i < ri->replicas.size(); ++i)
        {
            // there's already a transfer in progress that's helping this region
            // converge to the region_intent
            if (xfer->dst == ri->replicas[i])
            {
                return;
            }
        }

        del_transfer(xfer->id);
        xfer = NULL;
    }

    // there are no transfers for this region at this point

    // remove excess, unneeded servers
    while (reg->replicas.size() > ri->replicas.size())
    {
        for (size_t i = 0; i < reg->replicas.size(); ++i)
        {
            if (std::find(ri->replicas.begin(),
                          ri->replicas.end(),
                          reg->replicas[i].si) != ri->replicas.end())
            {
                continue;
            }

            fprintf(log, "removing server_id(%lu) from region_id(%lu) "
                         "to make progress toward desired state\n",
                         reg->replicas[i].si.get(), reg->id.get());

            for (size_t j = i + 1; j < reg->replicas.size(); ++j)
            {
                reg->replicas[j - 1] = reg->replicas[j];
            }

            reg->replicas.pop_back();
            break;
        }
    }

    // add up to one server from the region_intent
    for (size_t i = 0; i < ri->replicas.size(); ++i)
    {
        bool found = false;

        for (size_t j = 0; j < reg->replicas.size(); ++j)
        {
            if (ri->replicas[i] == reg->replicas[j].si)
            {
                found = true;
                break;
            }
        }

        if (found)
        {
            continue;
        }

        xfer = new_transfer(reg, ri->replicas[i]);
        assert(xfer);
        fprintf(log, "adding server_id(%lu) to region_id(%lu) "
                     "copying from server_id(%lu)/virtual_server_id(%lu) "
                     "using transfer_id(%lu)/virtual_server_id(%lu)\n",
                     xfer->dst.get(), reg->id.get(),
                     xfer->src.get(), xfer->vsrc.get(),
                     xfer->id.get(), xfer->vdst.get());
        return;
    }

    assert(ri->replicas.size() == reg->replicas.size());

    if (ri->checkpoint == 0)
    {
        ri->checkpoint = m_checkpoint;
    }

    if (ri->checkpoint >= m_checkpoint_stable_through)
    {
        return;
    }

    // now shuffle to make the order correct
    for (size_t i = 0; i < reg->replicas.size(); ++i)
    {
        if (reg->replicas[i].si == ri->replicas[i])
        {
            continue;
        }

        server_id sid = reg->replicas[i].si;

        for (size_t j = i + 1; j < reg->replicas.size(); ++j)
        {
            reg->replicas[j - 1] = reg->replicas[j];
        }

        reg->replicas.pop_back();
        xfer = new_transfer(reg, sid);
        assert(xfer);
        fprintf(log, "rolling server_id(%lu) to the back of region_id(%lu) "
                     "using transfer_id(%lu)/virtual_server_id(%lu)\n",
                     xfer->dst.get(), reg->id.get(),
                     xfer->id.get(), xfer->vdst.get());
        return;
    }

    for (size_t i = 0; i < m_intents.size(); ++i)
    {
        if (&m_intents[i] == ri)
        {
            std::swap(m_intents[i], m_intents.back());
            m_intents.pop_back();
            break;
        }
    }
}

region_intent*
coordinator :: new_region_intent(const region_id& rid)
{
    size_t idx = m_intents.size();
    m_intents.push_back(region_intent(rid));

    for (; idx > 0; --idx)
    {
        if (m_intents[idx - 1].id < m_intents[idx].id)
        {
            break;
        }

        std::swap(m_intents[idx - 1], m_intents[idx]);
    }

    return &m_intents[idx];
}

region_intent*
coordinator :: get_region_intent(const region_id& rid)
{
    for (size_t i = 0; i < m_intents.size(); ++i)
    {
        if (m_intents[i].id == rid)
        {
            return &m_intents[i];
        }
    }

    return NULL;
}

transfer*
coordinator :: new_transfer(region* reg,
                            const server_id& sid)
{
    assert(!reg->replicas.empty());
    m_transfers.push_back(transfer());
    transfer* t = &m_transfers.back();
    t->id = transfer_id(m_counter);
    ++m_counter;
    t->rid = reg->id;
    t->src = reg->replicas.back().si;
    t->vsrc = reg->replicas.back().vsi;
    t->dst = sid;
    t->vdst = virtual_server_id(m_counter);
    ++m_counter;
    return t;
}

transfer*
coordinator :: get_transfer(const region_id& rid)
{
    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        if (m_transfers[i].rid == rid)
        {
            return &m_transfers[i];
        }
    }

    return NULL;
}

transfer*
coordinator :: get_transfer(const transfer_id& xid)
{
    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        if (m_transfers[i].id == xid)
        {
            return &m_transfers[i];
        }
    }

    return NULL;
}

void
coordinator :: del_transfer(const transfer_id& xid)
{
    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        if (m_transfers[i].id != xid)
        {
            continue;
        }

        for (size_t j = i + 1; j < m_transfers.size(); ++j)
        {
            m_transfers[j - 1] = m_transfers[j];
        }

        m_transfers.pop_back();
        break;
    }
}

void
coordinator :: check_ack_condition(replicant_state_machine_context* ctx)
{
    if (m_config_ack_through < m_config_ack_barrier.min_version())
    {
        FILE* log = replicant_state_machine_log_stream(ctx);
        fprintf(log, "acked through version %lu\n", m_config_ack_barrier.min_version());
    }

    while (m_config_ack_through < m_config_ack_barrier.min_version())
    {
        replicant_state_machine_condition_broadcast(ctx, "ack", &m_config_ack_through);
    }
}

void
coordinator :: check_stable_condition(replicant_state_machine_context* ctx)
{
    if (m_config_stable_through < m_config_stable_barrier.min_version())
    {
        FILE* log = replicant_state_machine_log_stream(ctx);
        fprintf(log, "stable through version %lu\n", m_config_stable_barrier.min_version());
    }

    while (m_config_stable_through < m_config_stable_barrier.min_version())
    {
        replicant_state_machine_condition_broadcast(ctx, "stable", &m_config_stable_through);
    }
}

void
coordinator :: generate_next_configuration(replicant_state_machine_context* ctx)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    uint64_t cond_state;

    if (replicant_state_machine_condition_broadcast(ctx, "config", &cond_state) < 0)
    {
        fprintf(log, "could not broadcast on \"config\" condition\n");
    }

    ++m_version;
    fprintf(log, "issuing new configuration version %lu\n", m_version);
    assert(cond_state == m_version);
    std::vector<server_id> sids;
    servers_in_configuration(&sids);
    m_config_ack_barrier.new_version(m_version, sids);
    m_config_stable_barrier.new_version(m_version, sids);
    check_ack_condition(ctx);
    check_stable_condition(ctx);

    m_latest_config.reset();
    size_t sz = 6 * sizeof(uint64_t);

    for (size_t i = 0; i < m_servers.size(); ++i)
    {
        sz += pack_size(m_servers[i]);
    }

    for (std::map<std::string, std::tr1::shared_ptr<space> >::iterator it = m_spaces.begin();
            it != m_spaces.end(); ++it)
    {
        sz += pack_size(*it->second);
    }

    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        sz += pack_size(m_transfers[i]);
    }

    std::auto_ptr<e::buffer> new_config(e::buffer::create(sz));
    e::buffer::packer pa = new_config->pack_at(0);
    pa = pa << m_cluster << m_version
            << uint64_t(m_servers.size())
            << uint64_t(m_spaces.size())
            << uint64_t(m_transfers.size());

    for (size_t i = 0; i < m_servers.size(); ++i)
    {
        pa = pa << m_servers[i];
    }

    for (std::map<std::string, std::tr1::shared_ptr<space> >::iterator it = m_spaces.begin();
            it != m_spaces.end(); ++it)
    {
        pa = pa << *it->second;
    }

    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        pa = pa << m_transfers[i];
    }

    m_latest_config = new_config;
}

void
coordinator :: servers_in_configuration(std::vector<server_id>* sids)
{
    for (std::map<std::string, std::tr1::shared_ptr<space> >::iterator it = m_spaces.begin();
            it != m_spaces.end(); ++it)
    {
        space& s(*it->second);

        for (size_t i = 0; i < s.subspaces.size(); ++i)
        {
            subspace& ss(s.subspaces[i]);

            for (size_t j = 0; j < ss.regions.size(); ++j)
            {
                region& reg(ss.regions[j]);

                for (size_t k = 0; k < reg.replicas.size(); ++k)
                {
                    sids->push_back(reg.replicas[k].si);
                }
            }
        }
    }

    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
        sids->push_back(m_transfers[i].src);
        sids->push_back(m_transfers[i].dst);
    }

    std::sort(sids->begin(), sids->end());
    std::vector<server_id>::iterator sit;
    sit = std::unique(sids->begin(), sids->end());
    sids->resize(sit - sids->begin());
}

void
coordinator :: regions_in_space(space_ptr s, std::vector<region_id>* rids)
{
    for (size_t i = 0; i < s->subspaces.size(); ++i)
    {
        subspace& ss(s->subspaces[i]);

        for (size_t j = 0; j < ss.regions.size(); ++j)
        {
            rids->push_back(ss.regions[j].id);
        }
    }
}

#define OUTSTANDING_CHECKPOINTS 10

void
coordinator :: check_checkpoint_stable_condition(replicant_state_machine_context* ctx)
{
    FILE* log = replicant_state_machine_log_stream(ctx);
    assert(m_checkpoint_stable_through <= m_checkpoint);

    if (m_checkpoint_stable_through < m_checkpoint_stable_barrier.min_version())
    {
        fprintf(log, "checkpoint %lu done\n", m_checkpoint_stable_barrier.min_version());
    }

    bool stabilized = false;

    while (m_checkpoint_stable_through < m_checkpoint_stable_barrier.min_version())
    {
        stabilized = true;
        replicant_state_machine_condition_broadcast(ctx, "checkps", &m_checkpoint_stable_through);
    }

    bool gc = false;

    while (m_checkpoint_gc_through + OUTSTANDING_CHECKPOINTS < m_checkpoint_stable_barrier.min_version())
    {
        gc = true;
        replicant_state_machine_condition_broadcast(ctx, "checkpgc", &m_checkpoint_gc_through);
    }

    if (gc && m_checkpoint_gc_through > 0)
    {
        fprintf(log, "garbage collect <= checkpoint %lu\n", m_checkpoint_gc_through);
    }

    assert(m_checkpoint_stable_through <= m_checkpoint);
    assert(m_checkpoint_gc_through + OUTSTANDING_CHECKPOINTS <= m_checkpoint ||
           m_checkpoint <= OUTSTANDING_CHECKPOINTS);

    if (stabilized)
    {
        std::vector<region_id> rids;
        rids.reserve(m_intents.size());

        // two pass because converge_intent can alter m_intents
        for (size_t i = 0; i < m_intents.size(); ++i)
        {
            if (m_intents[i].checkpoint > 0 &&
                m_intents[i].checkpoint < m_checkpoint_stable_through)
            {
                rids.push_back(m_intents[i].id);
            }
        }

        for (size_t i = 0; i < rids.size(); ++i)
        {
            region* reg = get_region(rids[i]);
            converge_intent(ctx, reg);
        }

        if (!rids.empty())
        {
            generate_next_configuration(ctx);
        }
    }
}
