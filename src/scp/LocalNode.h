#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>
#include <set>

#include "scp/SCP.h"
#include "util/HashOfHash.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class LocalNode
{
  protected:
    const uint256 mNodeID;
    const SecretKey mSecretKey;
    SCPQuorumSet mQSet;
    Hash mQSetHash;

    // alternative qset used during externalize {{mNodeID}}
    Hash gSingleQSetHash;                      // hash of the singleton qset
    std::shared_ptr<SCPQuorumSet> mSingleQSet; // {{mNodeID}}

    SCP* mSCP;

  public:
    LocalNode(SecretKey const& secretKey, SCPQuorumSet const& qSet, SCP* scp);

    uint256 const& getNodeID();

    void updateQuorumSet(SCPQuorumSet const& qSet);

    SCPQuorumSet const& getQuorumSet();
    Hash const& getQuorumSetHash();
    SecretKey const& getSecretKey();

    // returns the quorum set {{X}}
    static SCPQuorumSetPtr getSingletonQSet(uint256 const& nodeID);

    // runs proc over all nodes contained in qset
    static void forAllNodes(SCPQuorumSet const& qset,
                            std::function<void(uint256 const&)> proc);

    // returns the weight of the node within the qset
    // normalized between 0-UINT64_MAX
    static uint64 getNodeWeight(uint256 const& nodeID,
                                SCPQuorumSet const& qset);

    // Tests this node against nodeSet for the specified qSethash. Triggers the
    // retrieval of qSetHash for this node and may throw a QuorumSlicesNotFound
    // exception
    static bool isQuorumSlice(SCPQuorumSet const& qSet,
                              std::vector<uint256> const& nodeSet);
    static bool isVBlocking(SCPQuorumSet const& qSet,
                            std::vector<uint256> const& nodeSet);

    // Tests this node against a map of nodeID -> T for the specified qSetHash.
    // Triggers the retrieval of qSetHash for this node and may throw a
    // QuorumSlicesNotFound exception.

    // `isVBlocking` tests if the filtered nodes V are a v-blocking set for
    // this node.
    static bool isVBlocking(
        SCPQuorumSet const& qSet, std::map<uint256, SCPStatement> const& map,
        std::function<bool(uint256 const&, SCPStatement const&)> const& filter =
            [](uint256 const&, SCPStatement const&)
        {
            return true;
        });

    // `isQuorum` tests if the filtered nodes V form a quorum
    // (meaning for each v \in V there is q \in Q(v)
    // included in V and we have quorum on V for qSetHash). `qfun` extracts the
    // SCPQuorumSetPtr from the SCPStatement for its associated node in map
    // (required for transitivity)
    static bool isQuorum(
        SCPQuorumSet const& qSet, std::map<uint256, SCPStatement> const& map,
        std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
        std::function<bool(uint256 const&, SCPStatement const&)> const& filter =
            [](uint256 const&, SCPStatement const&)
        {
            return true;
        });

  protected:
    // returns a quorum set {{ nodeID }}
    static SCPQuorumSet buildSingletonQSet(uint256 const& nodeID);

    // called recursively
    static bool isQuorumSliceInternal(SCPQuorumSet const& qset,
                                      std::vector<uint256> const& nodeSet);
    static bool isVBlockingInternal(SCPQuorumSet const& qset,
                                    std::vector<uint256> const& nodeSet);
    static void forAllNodesInternal(SCPQuorumSet const& qset,
                                    std::function<void(uint256 const&)> proc);
};
}
