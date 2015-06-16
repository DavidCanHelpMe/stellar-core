// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "lib/catch.hpp"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"
#include "scp/LocalNode.h"

namespace stellar
{

using xdr::operator<;
using xdr::operator==;

#define CREATE_VALUE(X)                                                        \
    static const Hash X##ValueHash = sha256("SEED_VALUE_HASH_" #X);            \
    static const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);

CREATE_VALUE(x);
CREATE_VALUE(y);
CREATE_VALUE(z);

class TestSCP : public SCP
{
  public:
    TestSCP(SecretKey const& secretKey, SCPQuorumSet const& qSetLocal)
        : SCP(secretKey, qSetLocal)
    {
        mPriorityLookup = [&](uint256 const& n)
        {
            return (n == secretKey.getPublicKey()) ? 1000 : 1;
        };
    }
    void
    storeQuorumSet(SCPQuorumSetPtr qSet)
    {
        Hash qSetHash = sha256(xdr::xdr_to_opaque(*qSet.get()));
        mQuorumSets[qSetHash] = qSet;
    }

    bool
    validateValue(uint64 slotIndex, Hash const& nodeID, Value const& value)
    {
        return true;
    }

    bool
    validateBallot(uint64 slotIndex, Hash const& nodeID,
                   SCPBallot const& ballot)
    {
        return true;
    }

    void
    ballotDidPrepare(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    void
    ballotDidPrepared(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    void
    ballotDidCommit(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    void
    ballotDidCommitted(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    void
    ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot)
    {
        mHeardFromQuorums[slotIndex].push_back(ballot);
    }

    void
    valueExternalized(uint64 slotIndex, Value const& value)
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = value;
    }

    SCPQuorumSetPtr
    getQSet(Hash const& qSetHash)
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {

            return mQuorumSets[qSetHash];
        }
        return SCPQuorumSetPtr();
    }

    void
    emitEnvelope(SCPEnvelope const& envelope)
    {
        mEnvs.push_back(envelope);
    }

    // used to test BallotProtocol and bypass nomination
    bool
    bumpState(uint64 slotIndex, Value const& v)
    {
        return getSlot(slotIndex)->bumpState(v, true);
    }

    // only used by nomination protocol
    Value
    combineCandidates(uint64 slotIndex,
                      std::set<Value> const& candidates) override
    {
        REQUIRE(candidates == mExpectedCandidates);
        REQUIRE(!mCompositeValue.empty());

        return mCompositeValue;
    }

    std::set<Value> mExpectedCandidates;
    Value mCompositeValue;

    // override the internal hashing scheme in order to make tests
    // more predictable.
    uint64
    computeHash(uint64 slotIndex, bool isPriority, int32 roundNumber,
                uint256 const& nodeID) override
    {
        uint64 res;
        if (isPriority)
        {
            res = mPriorityLookup(nodeID);
        }
        else
        {
            res = 0;
        }
        return res;
    }

    std::function<uint64(uint256 const&)> mPriorityLookup;

    std::map<Hash, SCPQuorumSetPtr> mQuorumSets;
    std::vector<SCPEnvelope> mEnvs;
    std::map<uint64, Value> mExternalizedValues;
    std::map<uint64, std::vector<SCPBallot>> mHeardFromQuorums;

    Value const&
    getLatestCompositeCandidate(uint64 slotIndex)
    {
        return getSlot(slotIndex)->getLatestCompositeCandidate();
    }
};

static SCPEnvelope
makeEnvelope(SecretKey const& secretKey, uint64 slotIndex,
             SCPStatement const& statement)
{
    SCPEnvelope envelope;
    envelope.statement = statement;
    envelope.statement.nodeID = secretKey.getPublicKey();
    envelope.statement.slotIndex = slotIndex;

    envelope.signature = secretKey.sign(xdr::xdr_to_opaque(envelope.statement));

    return envelope;
}

static SCPEnvelope
makeExternalize(SecretKey const& secretKey, Hash const& qSetHash,
                uint64 slotIndex, SCPBallot const& commitBallot, uint32 nP)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_EXTERNALIZE);
    auto& ext = st.pledges.externalize();
    ext.commit = commitBallot;
    ext.nP = nP;
    ext.commitQuorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makeConfirm(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            uint32 prepareCounter, SCPBallot const& commitBallot, uint32 nP)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_CONFIRM);
    auto& con = st.pledges.confirm();
    con.commit = commitBallot;
    con.nPrepared = prepareCounter;
    con.nP = nP;
    con.quorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makePrepare(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            SCPBallot const& ballot, SCPBallot* prepared = nullptr,
            uint32 nC = 0, uint32 nP = 0, SCPBallot* preparedPrime = nullptr)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_PREPARE);
    auto& p = st.pledges.prepare();
    p.ballot = ballot;
    p.quorumSetHash = qSetHash;
    if (prepared)
    {
        p.prepared.activate() = *prepared;
    }

    p.nC = nC;
    p.nP = nP;

    if (preparedPrime)
    {
        p.preparedPrime.activate() = *preparedPrime;
    }

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makeNominate(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
             std::vector<Value> votes, std::vector<Value> accepted)
{
    std::sort(votes.begin(), votes.end());
    std::sort(accepted.begin(), accepted.end());

    SCPStatement st;
    st.pledges.type(SCP_ST_NOMINATE);
    auto& nom = st.pledges.nominate();
    nom.quorumSetHash = qSetHash;
    for (auto const& v : votes)
    {
        nom.votes.emplace_back(v);
    }
    for (auto const& a : accepted)
    {
        nom.accepted.emplace_back(a);
    }
    return makeEnvelope(secretKey, slotIndex, st);
}

void
verifyPrepare(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, SCPBallot const& ballot,
              SCPBallot* prepared = nullptr, uint32 nC = 0, uint32 nP = 0,
              SCPBallot* preparedPrime = nullptr)
{
    auto exp = makePrepare(secretKey, qSetHash, slotIndex, ballot, prepared, nC,
                           nP, preparedPrime);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyConfirm(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, uint32 nPrepared,
              SCPBallot const& commit, uint32 nP)
{
    auto exp =
        makeConfirm(secretKey, qSetHash, slotIndex, nPrepared, commit, nP);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyExternalize(SCPEnvelope const& actual, SecretKey const& secretKey,
                  Hash const& qSetHash, uint64 slotIndex,
                  SCPBallot const& commit, uint32 nP)
{
    auto exp = makeExternalize(secretKey, qSetHash, slotIndex, commit, nP);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyNominate(SCPEnvelope const& actual, SecretKey const& secretKey,
               Hash const& qSetHash, uint64 slotIndex, std::vector<Value> votes,
               std::vector<Value> accepted)
{
    auto exp = makeNominate(secretKey, qSetHash, slotIndex, votes, accepted);
    REQUIRE(exp.statement == actual.statement);
}

TEST_CASE("vblocking and quorum", "[scp]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    std::vector<uint256> nodeSet;
    nodeSet.push_back(v0NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == false);

    nodeSet.push_back(v2NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v3NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v1NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);
}

TEST_CASE("protocol core5", "[scp][ballotprotocol]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey, qSet);

    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    REQUIRE(xValue < yValue);

    CLOG(INFO, "SCP") << "";
    CLOG(INFO, "SCP") << "BEGIN TEST";

    auto nodesAllPledgeToCommit = [&]()
    {
        SCPBallot b(1, xValue);
        SCPEnvelope prepare1 = makePrepare(v1SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare2 = makePrepare(v2SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare3 = makePrepare(v3SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare4 = makePrepare(v4SecretKey, qSetHash, 0, b);

        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, b);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 2);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0][0] == b);

        // We have a quorum including us

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, b, &b);

        scp.receiveEnvelope(prepare4);
        REQUIRE(scp.mEnvs.size() == 2);

        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared2 = makePrepare(v2SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared3 = makePrepare(v3SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared4 = makePrepare(v4SecretKey, qSetHash, 0, b, &b);

        scp.receiveEnvelope(prepared4);
        scp.receiveEnvelope(prepared3);
        REQUIRE(scp.mEnvs.size() == 2);

        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 3);

        // confirms prepared
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash, 0, b, &b, b.counter,
                      b.counter);

        // extra statement doesn't do anything
        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 3);
    };

    SECTION("bumpState x")
    {
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPBallot expectedBallot(1, xValue);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, expectedBallot);
    }

    SECTION("normal round (1,x)")
    {
        nodesAllPledgeToCommit();
        REQUIRE(scp.mEnvs.size() == 3);

        SCPBallot b(1, xValue);

        // bunch of prepare messages with "commit b"
        SCPEnvelope preparedC1 =
            makePrepare(v1SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC2 =
            makePrepare(v2SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC3 =
            makePrepare(v3SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC4 =
            makePrepare(v4SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);

        // those should not trigger anything just yet
        scp.receiveEnvelope(preparedC1);
        scp.receiveEnvelope(preparedC2);
        REQUIRE(scp.mEnvs.size() == 3);

        // this should cause the node to accept 'commit b' (quorum)
        // and therefore send a "CONFIRM" message
        scp.receiveEnvelope(preparedC3);
        REQUIRE(scp.mEnvs.size() == 4);

        verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash, 0, 1, b, b.counter);

        // bunch of confirm messages
        SCPEnvelope confirm1 =
            makeConfirm(v1SecretKey, qSetHash, 0, b.counter, b, b.counter);
        SCPEnvelope confirm2 =
            makeConfirm(v2SecretKey, qSetHash, 0, b.counter, b, b.counter);
        SCPEnvelope confirm3 =
            makeConfirm(v3SecretKey, qSetHash, 0, b.counter, b, b.counter);
        SCPEnvelope confirm4 =
            makeConfirm(v3SecretKey, qSetHash, 0, b.counter, b, b.counter);

        // those should not trigger anything just yet
        scp.receiveEnvelope(confirm1);
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == 4);

        scp.receiveEnvelope(confirm3);
        // this causes our node to
        // externalize (confirm commit c)
        REQUIRE(scp.mEnvs.size() == 5);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        verifyExternalize(scp.mEnvs[4], v0SecretKey, qSetHash, 0, b, b.counter);

        // extra vote should not do anything
        scp.receiveEnvelope(confirm4);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mExternalizedValues.size() == 1);

        // duplicate should just no-op
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mExternalizedValues.size() == 1);

        SECTION("bumpToBallot prevented once committed")
        {
            SCPBallot b2;
            SECTION("bumpToBallot prevented once committed (by value)")
            {
                b2 = SCPBallot(1, yValue);
            }
            SECTION("bumpToBallot prevented once committed (by counter)")
            {
                b2 = SCPBallot(2, xValue);
            }
            SECTION(
                "bumpToBallot prevented once committed (by value and counter)")
            {
                b2 = SCPBallot(2, yValue);
            }

            SCPEnvelope confirm1b2, confirm2b2, confirm3b2, confirm4b2;
            confirm1b2 = makeConfirm(v1SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter);
            confirm2b2 = makeConfirm(v2SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter);
            confirm3b2 = makeConfirm(v3SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter);
            confirm4b2 = makeConfirm(v4SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter);

            scp.receiveEnvelope(confirm1b2);
            scp.receiveEnvelope(confirm2b2);
            scp.receiveEnvelope(confirm3b2);
            scp.receiveEnvelope(confirm4b2);
            REQUIRE(scp.mEnvs.size() == 5);
            REQUIRE(scp.mExternalizedValues.size() == 1);
        }
    }

    SECTION("prepare (a), then prepared (b) by v-blocking")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;

        SECTION("x<y, prepare (1,x), prepared (1,y) by v-blocking")
        {
            A = xValue;
            B = yValue;
            shouldswitch = true;
            expectedBallot = SCPBallot(1, B);
        }
        SECTION("x<y, prepare (1,x), prepared (2,y) by v-blocking")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        SECTION("x<y, prepare (1,y), prepared (2,x) by v-blocking")
        {
            A = yValue;
            B = xValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }

        REQUIRE(scp.bumpState(0, A));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, SCPBallot(1, A));

        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0,
                                            expectedBallot, &expectedBallot);
        scp.receiveEnvelope(prepared1);

        int i = 1;
        REQUIRE(scp.mEnvs.size() == i);

        // this triggers the prepared message
        SCPEnvelope prepared2 = makePrepare(v2SecretKey, qSetHash, 0,
                                            expectedBallot, &expectedBallot);
        scp.receiveEnvelope(prepared2);

        REQUIRE(scp.mEnvs.size() == i + 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        verifyPrepare(scp.mEnvs[i], v0SecretKey, qSetHash, 0, expectedBallot,
                      &expectedBallot);
    }

    SECTION("pristine prepared")
    {
        SCPBallot b(1, xValue);

        SECTION("by v-blocking")
        {
            SCPEnvelope prepared1 =
                makePrepare(v1SecretKey, qSetHash, 0, b, &b);
            SCPEnvelope prepared2 =
                makePrepare(v2SecretKey, qSetHash, 0, b, &b);

            scp.receiveEnvelope(prepared1);
            REQUIRE(scp.mEnvs.size() == 0);

            scp.receiveEnvelope(prepared2);
        }
        SECTION("by quorum")
        {
            SCPEnvelope prepare1 = makePrepare(v1SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare2 = makePrepare(v2SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare3 = makePrepare(v3SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare4 = makePrepare(v4SecretKey, qSetHash, 0, b);

            scp.receiveEnvelope(prepare1);
            scp.receiveEnvelope(prepare2);
            scp.receiveEnvelope(prepare2);
            scp.receiveEnvelope(prepare3);
            REQUIRE(scp.mEnvs.size() == 0);
            scp.receiveEnvelope(prepare4);
        }
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, b, &b);
    }
    SECTION("prepare (a), prepared (b) by quorum")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;

        SECTION("x<y, prepare (1,x), prepared (1,y) by quorum")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(1, B);
        }
        SECTION("x<y, prepare (1,x), prepared (2,y) by quorum")
        {
            A = xValue;
            B = yValue;
            shouldswitch = true;
            expectedBallot = SCPBallot(2, B);
        }
        REQUIRE(scp.bumpState(0, A));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, SCPBallot(1, A));

        SCPEnvelope prepare1 =
            makePrepare(v1SecretKey, qSetHash, 0, expectedBallot);

        scp.receiveEnvelope(prepare1);

        int prepOffset = 1;
        REQUIRE(scp.mEnvs.size() == prepOffset);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope prepare2 =
            makePrepare(v2SecretKey, qSetHash, 0, expectedBallot);
        scp.receiveEnvelope(prepare2);

        if (shouldswitch)
        {
            // the 2nd prepare message causes the node to abandon its current
            // ballot.
            REQUIRE(scp.mEnvs.size() == prepOffset + 1);
            verifyPrepare(scp.mEnvs[prepOffset], v0SecretKey, qSetHash, 0,
                          SCPBallot(2, A));
            prepOffset++;
        }
        else
        {
            REQUIRE(scp.mEnvs.size() == prepOffset);
        }

        SCPEnvelope prepare3 =
            makePrepare(v3SecretKey, qSetHash, 0, expectedBallot);

        // this won't be sufficient to prepare:
        // the local node doesn't agree with the other ones
        scp.receiveEnvelope(prepare3);

        REQUIRE(scp.mEnvs.size() == prepOffset);

        // 4 nodes are present
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        SCPEnvelope prepare4 =
            makePrepare(v4SecretKey, qSetHash, 0, expectedBallot);

        scp.receiveEnvelope(prepare4);

        // quorum changed its mind
        REQUIRE(scp.mHeardFromQuorums[0].size() == 2);

        REQUIRE(scp.mEnvs.size() == 1 + prepOffset);

        int i = prepOffset;
        verifyPrepare(scp.mEnvs[i], v0SecretKey, qSetHash, 0, expectedBallot,
                      &expectedBallot);
    }

    SECTION("prepare (a), confirms prepared (b)")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;

        SECTION("x<y, prepare (1,x), confirms prepared (1,y) by quorum")
        {
            A = xValue;
            B = yValue;
            shouldswitch = true;
            expectedBallot = SCPBallot(1, B);
        }
        SECTION("x<y, prepare (1,x), confirms prepared (2,y) by quorum")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        SECTION("x<y, prepare (1,y), confirms prepared (2,x) by quorum")
        {
            A = yValue;
            B = xValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }

        REQUIRE(scp.bumpState(0, A));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0, SCPBallot(1, A));

        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0,
                                            expectedBallot, &expectedBallot);

        scp.receiveEnvelope(prepared1);

        int i = 1;
        REQUIRE(scp.mEnvs.size() == i);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope prepared2 = makePrepare(v2SecretKey, qSetHash, 0,
                                            expectedBallot, &expectedBallot);

        // cause the node to
        // prepared (v-blocking)
        scp.receiveEnvelope(prepared2);

        REQUIRE(scp.mEnvs.size() == 1 + i);

        verifyPrepare(scp.mEnvs[i], v0SecretKey, qSetHash, 0, expectedBallot,
                      &expectedBallot);
        i++;

        SCPEnvelope prepared3 = makePrepare(v3SecretKey, qSetHash, 0,
                                            expectedBallot, &expectedBallot);

        // this causes the node to :
        // set P
        // set 'c' and 'b' to 'P'
        scp.receiveEnvelope(prepared3);

        REQUIRE(scp.mEnvs.size() == i + 1);

        verifyPrepare(scp.mEnvs[i], v0SecretKey, qSetHash, 0, expectedBallot,
                      &expectedBallot, expectedBallot.counter,
                      expectedBallot.counter);
        i++;

        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        REQUIRE(scp.mExternalizedValues.size() == 0);
    }
    SECTION("prepared (a), accept commit by quorum (b)")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;

        SECTION("x<y, prepared (1,x), accept commit (2,y) by quorum")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        SECTION("x<y, prepared (1,y), accept commit (2,x) by quorum")
        {
            A = yValue;
            B = xValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        REQUIRE(scp.bumpState(0, A));

        SCPBallot sourceBallot(1, A);

        SCPEnvelope pcommitting1 =
            makePrepare(v1SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);
        SCPEnvelope pcommitting2 =
            makePrepare(v2SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);

        scp.receiveEnvelope(pcommitting1);
        scp.receiveEnvelope(pcommitting2);

        // moved to prepared (v-blocking)
        REQUIRE(scp.mEnvs.size() == 2);

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, sourceBallot,
                      &sourceBallot);

        // actual test
        SCPEnvelope committing1 = makePrepare(
            v1SecretKey, qSetHash, 0, expectedBallot, &expectedBallot,
            expectedBallot.counter, expectedBallot.counter);

        scp.receiveEnvelope(committing1);

        int i = 2;
        REQUIRE(scp.mEnvs.size() == i);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope committing2 = makePrepare(
            v2SecretKey, qSetHash, 0, expectedBallot, &expectedBallot,
            expectedBallot.counter, expectedBallot.counter);

        // this causes the node to :
        // prepared B (v-blocking criteria of accept)
        scp.receiveEnvelope(committing2);
        REQUIRE(scp.mEnvs.size() == i + 1);

        verifyPrepare(scp.mEnvs[i], v0SecretKey, qSetHash, 0, expectedBallot,
                      &expectedBallot, 0, 0, &sourceBallot);

        i++;

        SCPEnvelope committing3 = makePrepare(
            v3SecretKey, qSetHash, 0, expectedBallot, &expectedBallot,
            expectedBallot.counter, expectedBallot.counter);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        // this causes 2 transitions:
        // confirm as prepared -> set P, c and b
        // accept commit (quorum)
        scp.receiveEnvelope(committing3);

        REQUIRE(scp.mEnvs.size() == 1 + i);

        verifyConfirm(scp.mEnvs[i], v0SecretKey, qSetHash, 0,
                      expectedBallot.counter, expectedBallot,
                      expectedBallot.counter);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);
    }

    SECTION("prepared (a), accept commit by v-blocking (b)")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;

        SECTION("x<y, prepared (1,x), accept commit (2,y) by v-blocking")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        SECTION("x<y, prepared (1,y), accept commit (2,x) by v-blocking")
        {
            A = yValue;
            B = xValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
        }
        REQUIRE(scp.bumpState(0, A));

        SCPBallot sourceBallot(1, A);

        SCPEnvelope pcommitting1 =
            makePrepare(v1SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);
        SCPEnvelope pcommitting2 =
            makePrepare(v2SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);

        scp.receiveEnvelope(pcommitting1);
        scp.receiveEnvelope(pcommitting2);

        // moved to prepared (v-blocking)
        REQUIRE(scp.mEnvs.size() == 2);

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, sourceBallot,
                      &sourceBallot);

        // actual test
        SCPEnvelope confirm1 =
            makeConfirm(v1SecretKey, qSetHash, 0, expectedBallot.counter,
                        expectedBallot, expectedBallot.counter);

        scp.receiveEnvelope(confirm1);

        int i = 2;
        REQUIRE(scp.mEnvs.size() == i);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope confirm2 =
            makeConfirm(v2SecretKey, qSetHash, 0, expectedBallot.counter,
                        expectedBallot, expectedBallot.counter);

        // this causes the node to:
        // accept commit B (v-blocking criteria of accept)
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == i + 1);

        verifyConfirm(scp.mEnvs[i], v0SecretKey, qSetHash, 0,
                      expectedBallot.counter, expectedBallot,
                      expectedBallot.counter);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);
    }
    SECTION("prepared (a), confirm commit (b)")
    {
        Value A, B;
        bool shouldswitch = false;
        SCPBallot expectedBallot;
        bool extraPrepared = false;
        bool acceptExtraCommit = false;

        SECTION("x<y, prepared (1,x), confirm commit (2,y)")
        {
            A = xValue;
            B = yValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
            SECTION("extra prepared")
            {
                extraPrepared = true;
                SECTION("accept extra commit")
                {
                    acceptExtraCommit = true;
                }
            }
        }
        SECTION("x<y, prepared (1,y), confirm commit (2,x)")
        {
            A = yValue;
            B = xValue;
            shouldswitch = false;
            expectedBallot = SCPBallot(2, B);
            SECTION("extra prepared")
            {
                extraPrepared = true;
                SECTION("accept extra commit")
                {
                    acceptExtraCommit = true;
                }
            }
        }
        REQUIRE(scp.bumpState(0, A));

        SCPBallot sourceBallot(1, A);

        SCPEnvelope pcommitting1 =
            makePrepare(v1SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);
        SCPEnvelope pcommitting2 =
            makePrepare(v2SecretKey, qSetHash, 0, sourceBallot, &sourceBallot,
                        sourceBallot.counter, sourceBallot.counter);

        scp.receiveEnvelope(pcommitting1);
        scp.receiveEnvelope(pcommitting2);

        // moved to prepared (v-blocking)
        REQUIRE(scp.mEnvs.size() == 2);

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, sourceBallot,
                      &sourceBallot);

        // actual test
        SCPEnvelope confirm1 =
            makeConfirm(v1SecretKey, qSetHash, 0, expectedBallot.counter,
                        expectedBallot, expectedBallot.counter);

        scp.receiveEnvelope(confirm1);

        int i = 2;
        REQUIRE(scp.mEnvs.size() == i);

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope confirm2 =
            makeConfirm(v2SecretKey, qSetHash, 0, expectedBallot.counter,
                        expectedBallot, expectedBallot.counter);

        // this causes the node to :
        // prepared B (v-blocking criteria of accept)
        // accept commit (v-blocking criteria) -> CONFIRM

        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == i + 1);

        verifyConfirm(scp.mEnvs[i], v0SecretKey, qSetHash, 0,
                      expectedBallot.counter, expectedBallot,
                      expectedBallot.counter);

        i++;

        uint32 prepared = expectedBallot.counter;

        uint32 expectedP = expectedBallot.counter;

        if (extraPrepared)
        {
            // verify that we can accept new ballots as prepared
            prepared++;

            expectedP = acceptExtraCommit ? prepared : expectedBallot.counter;

            SCPEnvelope pconfirm1 = makeConfirm(
                v1SecretKey, qSetHash, 0, prepared, expectedBallot, expectedP);

            scp.receiveEnvelope(pconfirm1);

            REQUIRE(scp.mEnvs.size() == i);

            SCPEnvelope pconfirm2 = makeConfirm(
                v2SecretKey, qSetHash, 0, prepared, expectedBallot, expectedP);

            scp.receiveEnvelope(pconfirm2);

            REQUIRE(scp.mEnvs.size() == i + 1);

            // bumps 'p' (v-blocking) and
            // if acceptExtraCommit: P (v-blocking)
            verifyConfirm(scp.mEnvs[i], v0SecretKey, qSetHash, 0, prepared,
                          expectedBallot, expectedP);

            i++;
        }

        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        SCPEnvelope confirm3 = makeConfirm(v3SecretKey, qSetHash, 0, prepared,
                                           expectedBallot, expectedP);

        // this causes:
        // confirm commit c -> EXTERNALIZE
        scp.receiveEnvelope(confirm3);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        REQUIRE(scp.mEnvs.size() == 1 + i);

        verifyExternalize(scp.mEnvs[i], v0SecretKey, qSetHash, 0,
                          expectedBallot, expectedP);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.mExternalizedValues[0] == B);
    }

    SECTION("prepare (1,y), receives accept commit messages (1,x)")
    {
        REQUIRE(scp.bumpState(0, yValue));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash, 0,
                      SCPBallot(1, yValue));

        SCPBallot expectedBallot(1, xValue);
        SCPEnvelope com1 = makePrepare(v1SecretKey, qSetHash, 0, expectedBallot,
                                       &expectedBallot, 1, 1);
        SCPEnvelope com2 = makePrepare(v2SecretKey, qSetHash, 0, expectedBallot,
                                       &expectedBallot, 1, 1);
        SCPEnvelope com3 = makePrepare(v3SecretKey, qSetHash, 0, expectedBallot,
                                       &expectedBallot, 1, 1);
        SCPEnvelope com4 = makePrepare(v4SecretKey, qSetHash, 0, expectedBallot,
                                       &expectedBallot, 1, 1);

        scp.receiveEnvelope(com1);
        scp.receiveEnvelope(com2);
        scp.receiveEnvelope(com3);
        REQUIRE(scp.mEnvs.size() == 1);

        // quorum accepts commit (1,x)
        // -> we confirm commit (1,x)
        scp.receiveEnvelope(com4);

        REQUIRE(scp.mEnvs.size() == 2);
        verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash, 0, 1, expectedBallot,
                      1);
    }
    SECTION("single prepared(1,y) on pristine slot should not bump")
    {
        SCPBallot b(1, yValue);
        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0, b, &b);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 0);
    }
    SECTION("confirm(1,y) on pristine slot should not bump")
    {
        SCPBallot b(1, yValue);

        SCPEnvelope confirm1 =
            makeConfirm(v1SecretKey, qSetHash, 0, b.counter, b, b.counter);

        scp.receiveEnvelope(confirm1);
        REQUIRE(scp.mEnvs.size() == 0);
    }
    SECTION("bumpToBallot prevented after CONFIRM")
    {
        nodesAllPledgeToCommit();
        REQUIRE(scp.mEnvs.size() == 3);

        SCPBallot b(1, xValue);

        SCPEnvelope committing1 =
            makePrepare(v1SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope committing2 =
            makePrepare(v2SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope committing3 =
            makePrepare(v3SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);

        scp.receiveEnvelope(committing1);
        scp.receiveEnvelope(committing2);
        REQUIRE(scp.mEnvs.size() == 3);
        scp.receiveEnvelope(committing3);
        // this caused our node to emit CONFIRM (quorum)
        REQUIRE(scp.mEnvs.size() == 4);

        SCPBallot by(2, yValue);

        SCPEnvelope externalize1 =
            makeExternalize(v1SecretKey, qSetHash, 0, by, by.counter);
        SCPEnvelope externalize2 =
            makeExternalize(v2SecretKey, qSetHash, 0, by, by.counter);
        SCPEnvelope externalize3 =
            makeExternalize(v3SecretKey, qSetHash, 0, by, by.counter);
        SCPEnvelope externalize4 =
            makeExternalize(v4SecretKey, qSetHash, 0, by, by.counter);

        scp.receiveEnvelope(externalize1);
        REQUIRE(scp.mEnvs.size() == 4);
        scp.receiveEnvelope(externalize2);
        REQUIRE(scp.mEnvs.size() == 4);
        scp.receiveEnvelope(externalize3);
        REQUIRE(scp.mEnvs.size() == 4);
        scp.receiveEnvelope(externalize4);
        REQUIRE(scp.mEnvs.size() == 4);
    }
    SECTION("prepared x, then prepared y -> prepared prime is set properly")
    {
        SCPBallot bx(1, xValue);
        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0, bx, &bx,
                                            bx.counter, bx.counter);
        SCPEnvelope prepared2 = makePrepare(v2SecretKey, qSetHash, 0, bx, &bx,
                                            bx.counter, bx.counter);

        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        // nothing happens with one message
        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(prepared2);
        // v-blocking -> prepared
        REQUIRE(scp.mEnvs.size() == 2);

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, bx, &bx);

        // now causes to switch to y by moving the v-blocking
        // set to y
        SCPBallot by(2, yValue);
        SCPEnvelope prepared1y = makePrepare(v1SecretKey, qSetHash, 0, by, &by,
                                             by.counter, by.counter);
        SCPEnvelope prepared2y = makePrepare(v2SecretKey, qSetHash, 0, by, &by,
                                             by.counter, by.counter);

        // nothing happens with one message
        scp.receiveEnvelope(prepared1y);
        REQUIRE(scp.mEnvs.size() == 2);

        scp.receiveEnvelope(prepared2y);
        // v-blocking -> prepared
        REQUIRE(scp.mEnvs.size() == 3);

        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash, 0, by, &by, 0, 0,
                      &bx);

        // now causes to switch to z by moving the v-blocking
        // set to z
        SCPBallot bz(3, zValue);
        SCPEnvelope prepared1z = makePrepare(v1SecretKey, qSetHash, 0, bz, &bz,
                                             bz.counter, bz.counter);
        SCPEnvelope prepared2z = makePrepare(v2SecretKey, qSetHash, 0, bz, &bz,
                                             bz.counter, bz.counter);

        // nothing happens with one message
        scp.receiveEnvelope(prepared1z);
        REQUIRE(scp.mEnvs.size() == 3);

        scp.receiveEnvelope(prepared2z);
        // v-blocking -> prepared
        REQUIRE(scp.mEnvs.size() == 4);

        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash, 0, bz, &bz, 0, 0,
                      &by);
    }
    SECTION("timeout when P is set -> stay locked on P")
    {
        SCPBallot bx(1, xValue);
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope prepare1 = makePrepare(v1SecretKey, qSetHash, 0, bx, &bx);
        SCPEnvelope prepare2 = makePrepare(v2SecretKey, qSetHash, 0, bx, &bx);
        scp.receiveEnvelope(prepare1);
        scp.receiveEnvelope(prepare2);
        // v-blocking -> prepared
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash, 0, bx, &bx);

        // confirm prepared
        SCPEnvelope prepare3 = makePrepare(v3SecretKey, qSetHash, 0, bx, &bx);
        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash, 0, bx, &bx,
                      bx.counter, bx.counter);

        // now, see if we can timeout and move to a different value
        REQUIRE(scp.bumpState(0, yValue));
        REQUIRE(scp.mEnvs.size() == 4);
        SCPBallot newbx(2, xValue);
        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash, 0, newbx, &bx,
                      bx.counter, bx.counter);
    }
}

TEST_CASE("nomination tests core5", "[scp][nominationprotocol]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    REQUIRE(xValue < yValue);

    SECTION("nomination - v0 is top")
    {
        TestSCP scp(v0SecretKey, qSet);
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

        SECTION("others nominate what v0 says (x) -> prepare x")
        {
            scp.mExpectedCandidates.emplace(xValue);
            scp.mCompositeValue = xValue;
            REQUIRE(scp.nominate(0, xValue, false));

            std::vector<Value> votes, accepted;
            votes.emplace_back(xValue);

            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash, 0, votes,
                           accepted);

            SCPEnvelope nom1 =
                makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom2 =
                makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom3 =
                makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom4 =
                makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

            // nothing happens yet
            scp.receiveEnvelope(nom1);
            scp.receiveEnvelope(nom2);
            REQUIRE(scp.mEnvs.size() == 1);

            // this causes 'x' to be accepted (quorum)
            scp.receiveEnvelope(nom3);
            REQUIRE(scp.mEnvs.size() == 2);

            accepted.emplace_back(xValue);
            verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash, 0, votes,
                           accepted);

            // extra message doesn't do anything
            scp.receiveEnvelope(nom4);
            REQUIRE(scp.mEnvs.size() == 2);

            SCPEnvelope acc1 =
                makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc2 =
                makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc3 =
                makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc4 =
                makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

            // nothing happens yet
            scp.receiveEnvelope(acc1);
            scp.receiveEnvelope(acc2);
            REQUIRE(scp.mEnvs.size() == 2);

            scp.mCompositeValue = xValue;
            // this causes the node to send a prepare message (quorum)
            scp.receiveEnvelope(acc3);
            REQUIRE(scp.mEnvs.size() == 3);

            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash, 0,
                          SCPBallot(1, xValue));

            scp.receiveEnvelope(acc4);
            REQUIRE(scp.mEnvs.size() == 3);

            SECTION("nominate x -> accept x -> prepare (x) ; others accepted y "
                    "-> update latest to (z=x+y)")
            {
                std::vector<Value> votes2 = votes;
                votes2.emplace_back(yValue);

                SCPEnvelope acc1_2 =
                    makeNominate(v1SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc2_2 =
                    makeNominate(v2SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc3_2 =
                    makeNominate(v3SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc4_2 =
                    makeNominate(v4SecretKey, qSetHash, 0, votes2, votes2);

                scp.receiveEnvelope(acc1_2);
                REQUIRE(scp.mEnvs.size() == 3);

                // v-blocking
                scp.receiveEnvelope(acc2_2);
                REQUIRE(scp.mEnvs.size() == 4);
                verifyNominate(scp.mEnvs[3], v0SecretKey, qSetHash, 0, votes2,
                               votes2);

                scp.mExpectedCandidates.insert(yValue);
                scp.mCompositeValue = zValue;
                // this updates the composite value to use next time
                // but does not prepare it
                scp.receiveEnvelope(acc3_2);
                REQUIRE(scp.mEnvs.size() == 4);

                REQUIRE(scp.getLatestCompositeCandidate(0) == zValue);

                scp.receiveEnvelope(acc4_2);
                REQUIRE(scp.mEnvs.size() == 4);
            }
        }
        SECTION("self nominates 'x', others nominate y -> prepare y")
        {
            std::vector<Value> myVotes, accepted;
            myVotes.emplace_back(xValue);

            scp.mExpectedCandidates.emplace(xValue);
            scp.mCompositeValue = xValue;
            REQUIRE(scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash, 0, myVotes,
                           accepted);

            std::vector<Value> votes;
            votes.emplace_back(yValue);

            std::vector<Value> acceptedY = accepted;

            acceptedY.emplace_back(yValue);

            SCPEnvelope acc1 =
                makeNominate(v1SecretKey, qSetHash, 0, votes, acceptedY);
            SCPEnvelope acc2 =
                makeNominate(v2SecretKey, qSetHash, 0, votes, acceptedY);
            SCPEnvelope acc3 =
                makeNominate(v3SecretKey, qSetHash, 0, votes, acceptedY);
            SCPEnvelope acc4 =
                makeNominate(v4SecretKey, qSetHash, 0, votes, acceptedY);

            SECTION("via quorum")
            {
                SCPEnvelope nom1 =
                    makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom2 =
                    makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom3 =
                    makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom4 =
                    makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

                // nothing happens yet
                scp.receiveEnvelope(nom1);
                scp.receiveEnvelope(nom2);
                scp.receiveEnvelope(nom3);
                REQUIRE(scp.mEnvs.size() == 1);

                // this causes 'y' to be accepted (quorum)
                scp.receiveEnvelope(nom4);
                REQUIRE(scp.mEnvs.size() == 2);

                myVotes.emplace_back(yValue);
                verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash, 0, myVotes,
                               acceptedY);

                // nothing happens yet
                scp.receiveEnvelope(acc1);
                scp.receiveEnvelope(acc2);
                REQUIRE(scp.mEnvs.size() == 2);
            }
            SECTION("via v-blocking")
            {
                scp.receiveEnvelope(acc1);
                REQUIRE(scp.mEnvs.size() == 1);

                // this causes 'y' to be accepted (quorum)
                scp.receiveEnvelope(acc2);
                REQUIRE(scp.mEnvs.size() == 2);

                myVotes.emplace_back(yValue);
                verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash, 0, myVotes,
                               acceptedY);
            }

            scp.mExpectedCandidates.clear();
            scp.mExpectedCandidates.insert(yValue);
            scp.mCompositeValue = yValue;
            // this causes the node to send a prepare message (quorum)
            scp.receiveEnvelope(acc3);
            REQUIRE(scp.mEnvs.size() == 3);

            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash, 0,
                          SCPBallot(1, yValue));

            scp.receiveEnvelope(acc4);
            REQUIRE(scp.mEnvs.size() == 3);
        }
    }
    SECTION("v1 is top node")
    {
        TestSCP scp(v0SecretKey, qSet);
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

        scp.mPriorityLookup = [&](uint256 const& n)
        {
            return (n == v1NodeID) ? 1000 : 1;
        };

        std::vector<Value> votesX, votesY, votesZ, emptyV;
        votesX.emplace_back(xValue);
        votesY.emplace_back(yValue);
        votesZ.emplace_back(zValue);

        SCPEnvelope nom1 =
            makeNominate(v1SecretKey, qSetHash, 0, votesY, emptyV);
        SCPEnvelope nom2 =
            makeNominate(v2SecretKey, qSetHash, 0, votesZ, emptyV);

        SECTION("nomination waits for v1")
        {
            REQUIRE(!scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 0);

            SCPEnvelope nom3 =
                makeNominate(v3SecretKey, qSetHash, 0, votesZ, emptyV);
            SCPEnvelope nom4 =
                makeNominate(v4SecretKey, qSetHash, 0, votesZ, emptyV);

            // nothing happens with non top nodes
            scp.receiveEnvelope(nom2);
            scp.receiveEnvelope(nom3);
            REQUIRE(scp.mEnvs.size() == 0);

            scp.mExpectedCandidates.emplace(yValue);
            scp.mCompositeValue = yValue;

            scp.receiveEnvelope(nom1);
            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash, 0, votesY,
                           emptyV);

            scp.receiveEnvelope(nom4);
            REQUIRE(scp.mEnvs.size() == 1);
        }
        SECTION("v1 dead, timeout")
        {
            REQUIRE(!scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 0);

            scp.receiveEnvelope(nom2);
            REQUIRE(scp.mEnvs.size() == 0);

            SECTION("v0 is new top node")
            {
                scp.mPriorityLookup = [&](uint256 const& n)
                {
                    return (n == v0NodeID) ? 1000 : 1;
                };
                scp.mExpectedCandidates.emplace(xValue);
                scp.mCompositeValue = xValue;

                REQUIRE(scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash, 0, votesX,
                               emptyV);
            }
            SECTION("v2 is new top node")
            {
                scp.mPriorityLookup = [&](uint256 const& n)
                {
                    return (n == v2NodeID) ? 1000 : 1;
                };
                scp.mExpectedCandidates.emplace(zValue);
                scp.mCompositeValue = zValue;

                REQUIRE(scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash, 0, votesZ,
                               emptyV);
            }
            SECTION("v3 is new top node")
            {
                scp.mPriorityLookup = [&](uint256 const& n)
                {
                    return (n == v3NodeID) ? 1000 : 1;
                };
                // nothing happens, we don't have any message for v3
                REQUIRE(!scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 0);
            }
        }
    }
}
}