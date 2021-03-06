// Copyright (c) 2013, Cornell University
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

// HyperDex
#include "coordinator/replica_sets.h"

namespace
{

using hyperdex::replica_set;
using hyperdex::server_id;

void
_small_replica_sets(uint64_t, uint64_t,
                    const std::vector<server_id>& permutation,
                    std::vector<server_id>* replica_storage,
                    std::vector<replica_set>* replica_sets)
{
    for (size_t i = 0; i < permutation.size(); ++i)
    {
        replica_sets->push_back(replica_set(replica_storage->size(), permutation.size(), replica_storage));

        for (size_t j = 0; j < permutation.size(); ++j)
        {
            replica_storage->push_back(permutation[(i + j) % permutation.size()]);
        }
    }
}

void
_permutation_replica_sets(uint64_t R, uint64_t P,
                          const std::vector<server_id>& permutation,
                          std::vector<server_id>* replica_storage,
                          std::vector<replica_set>* replica_sets)
{
    for (size_t n = 0; n < permutation.size(); ++n)
    {
        for (size_t p = 1; p <= P; ++p)
        {
            if (n + p * (R - 1) >= permutation.size())
            {
                continue;
            }

            replica_sets->push_back(replica_set(replica_storage->size(), R, replica_storage));

            for (size_t r = 0; r < R; ++r)
            {
                size_t idx = n + p * r;
                replica_storage->push_back(permutation[idx]);
            }
        }
    }
}

} // namespace

void
hyperdex :: compute_replica_sets(uint64_t R, uint64_t P,
                                 const std::vector<server_id>& permutation,
                                 std::vector<server_id>* replica_storage,
                                 std::vector<replica_set>* replica_sets)
{
    assert(R > 0);
    replica_storage->clear();
    replica_sets->clear();

    if (permutation.size() <= R)
    {
        _small_replica_sets(R, P, permutation, replica_storage, replica_sets);
    }
    else
    {
        _permutation_replica_sets(R, P, permutation, replica_storage, replica_sets);
    }
}
