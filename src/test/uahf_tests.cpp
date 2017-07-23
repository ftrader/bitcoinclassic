/*
 * This file is part of the bitcoin-classic project
 * Copyright (C) 2017 Tom Zander <tomz@freedommail.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "transaction_utils.h"
#include "test/test_bitcoin.h"

#include <boost/test/auto_unit_test.hpp>
#include <Application.h>
#include <BlocksDB.h>
#include <chain.h>
#include <chainparams.h>
#include <primitives/transaction.h>
#include <main.h>
#include <consensus/validation.h>

#include <vector>

class MyTestingFixture : public TestingSetup
{
public:
    MyTestingFixture() : TestingSetup(CBaseChainParams::REGTEST, BlocksDbOnDisk) {}
};

static CBlockIndex *createBlockIndex(CBlockIndex *prev, int height, int time, uint256 *hash)
{
    assert(hash);
    CBlockIndex *index = new CBlockIndex();
    index->nHeight = height;
    index->nTime = time;
    index->pprev = prev;
    *hash = CDiskBlockIndex(index).GetBlockHash();
    index->phashBlock = hash;
    Blocks::DB::instance()->appendBlock(index, 0);
    Blocks::indexMap.insert(std::make_pair(*hash, index));
    Blocks::DB::instance()->appendHeader(index);
    return index;
}

static CBlock createBlock(CBlockIndex *parent, const std::vector<CTransaction>& txns)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.resize(1);
    coinbase.vin[0].scriptSig = CScript() << (parent->nHeight + 1) << OP_0;
    coinbase.vout[0].nValue = 50 * COIN;

    CBlock block;
    block.vtx.push_back(coinbase);
    block.nVersion = 4;
    block.hashPrevBlock = *parent->phashBlock;
    block.nTime = parent->GetMedianTimePast() + 20;
    block.nBits = 0x207fffff;
    block.nNonce = 0;

    block.vtx.reserve(txns.size() + 1);
    for (const CTransaction &tx : txns) {
        block.vtx.push_back(tx);
    }

    return block;
}


BOOST_FIXTURE_TEST_SUITE(UAHF, MyTestingFixture)

BOOST_AUTO_TEST_CASE(Test_Enabling)
{
    mapArgs["-uahfstarttime"] = "0";
    MockApplication::doInit();
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFDisabled);
    BOOST_CHECK_EQUAL(Application::uahfStartTime(), 0);

    mapArgs["-uahfstarttime"] = "-1";
    MockApplication::doInit();
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFDisabled);
    BOOST_CHECK_EQUAL(Application::uahfStartTime(), 0);

    mapArgs["-uahfstarttime"] = "1";
    MockApplication::doInit();
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFWaiting);
    BOOST_CHECK_EQUAL(Application::uahfStartTime(), 1);

    mapArgs["-uahfstarttime"] = "12352";
    MockApplication::doInit();
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFWaiting);
    BOOST_CHECK_EQUAL(Application::uahfStartTime(), 12352);

    BOOST_CHECK(Blocks::DB::instance()->uahfForkBlock() == uint256());
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFWaiting);

    uint256 hash;
    createBlockIndex(Blocks::indexMap.begin()->second, 1, 18000, &hash);

    Blocks::DB::instance()->setUahfForkBlock(hash);
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFActive);

    mapArgs["-uahfstarttime"] = "0";
    MockApplication::doInit();
    Blocks::DB::createInstance(0, false);
    BOOST_CHECK(Blocks::DB::instance()->uahfForkBlock() == uint256());
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFDisabled);

    mapArgs["-uahfstarttime"] = "12352";
    MockApplication::doInit();
    Blocks::DB::createInstance(0, false);
    Blocks::DB::instance()->CacheAllBlockInfos();
    BOOST_CHECK(Blocks::DB::instance()->uahfForkBlock() == hash);
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFActive);

    mapArgs["-uahfstarttime"] = "18001"; // TODO check if this should be 18000 instead
    MockApplication::doInit();
    Blocks::DB::createInstance(0, false);
    Blocks::DB::instance()->CacheAllBlockInfos();
    BOOST_CHECK(Blocks::DB::instance()->uahfForkBlock() == hash);
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFWaiting);
}

BOOST_AUTO_TEST_CASE(Test_BlockValidation)
{
    std::vector<uint256> hashes;
    hashes.resize(21);

    // create 20 block-indexes.
    CBlockIndex *tip = Blocks::indexMap.begin()->second;
    for (int i = 0; i < 20; ++i) {
        tip = createBlockIndex(tip, i + 1, i * 100, &hashes[i]);
    }

    // Create block with block index.
    std::vector<CTransaction> transactions;
    CBlock block = createBlock(tip, transactions);
    uint256 hash;
    mapArgs["-uahfstarttime"] = "1400"; // that makes our upcoming block the first on the new chain
    MockApplication::doInit();

    CValidationState state;
    bool accepted = ContextualCheckBlock(block, state, tip);
    BOOST_CHECK(!accepted);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-too-small");

    transactions = TxUtils::transactionsForBlock(1000000);
    block = createBlock(tip, transactions);
    BOOST_CHECK(::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > 1000000);

    accepted = ContextualCheckBlock(block, state, tip);
    BOOST_CHECK(accepted);
    BOOST_CHECK_EQUAL(Application::uahfChainState(), Application::UAHFWaiting);

    // Accept it so we can create small blocks again.
    tip = createBlockIndex(tip, 20, 2500, &hashes[20]);

    // Base transaction is valid before the fork.
    mapArgs["-uahfstarttime"] = "2000";
    MockApplication::doInit();
    transactions.clear();
    CMutableTransaction tx;
    TxUtils::RandomTransaction(tx, TxUtils::SingleOutput);
    transactions.push_back(tx);
    block = createBlock(tip, transactions);
    BOOST_CHECK(ContextualCheckBlock(block, state, tip));


    // Base transaction is still valid after sunset.
    mapArgs["-uahfstarttime"] = "1400";
    MockApplication::doInit();
    BOOST_CHECK(ContextualCheckBlock(block, state, tip));

    // Wrong commitment, still valid.
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << OP_0;
    transactions[0] = tx;
    block = createBlock(tip, transactions);
    BOOST_CHECK(ContextualCheckBlock(block, state, tip));

    const Consensus::Params &params = Params().GetConsensus();
    // Anti replay commitment, not valid anymore.
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << params.antiReplayOpReturnCommitment;
    transactions[0] = tx;
    block = createBlock(tip, transactions);
    BOOST_CHECK_EQUAL(ContextualCheckBlock(block, state, tip), false);

    // Anti replay commitment, **At** sunset.
    tip->nHeight = Params().GetConsensus().antiReplayOpReturnSunsetHeight - 1; // (remember, tip is pindexPREV)
    BOOST_CHECK_EQUAL(ContextualCheckBlock(block, state, tip), false);

    // Anti replay commitment, disabled after sunset.
    logDebug() << "sunset" << Params().GetConsensus().antiReplayOpReturnSunsetHeight;
    tip->nHeight = Params().GetConsensus().antiReplayOpReturnSunsetHeight;
    BOOST_CHECK(ContextualCheckBlock(block, state, tip));

    // Anti replay commitment, disabled before start time.
    mapArgs["-uahfstarttime"] = "3000";
    MockApplication::doInit();
    BOOST_CHECK(ContextualCheckBlock(block, state, tip));
}

BOOST_AUTO_TEST_CASE(Test_isCommitment) {
    std::vector<unsigned char> data{};

    // Empty commitment.
    auto s = CScript() << OP_RETURN << data;
    BOOST_CHECK(s.isCommitment(data));

    // Commitment to a value of the wrong size.
    data.push_back(42);
    BOOST_CHECK(!s.isCommitment(data));

    // Not a commitment.
    s = CScript() << data;
    BOOST_CHECK(!s.isCommitment(data));

    // Non empty commitment.
    s = CScript() << OP_RETURN << data;
    BOOST_CHECK(s.isCommitment(data));

    // Commitment to the wrong value.
    data[0] = 0x42;
    BOOST_CHECK(!s.isCommitment(data));

    // Commitment to a larger value.
    std::string str = "Bitcoin: A peer-to-peer Electronic Cash System";
    data = std::vector<unsigned char>(str.begin(), str.end());
    BOOST_CHECK(!s.isCommitment(data));

    s = CScript() << OP_RETURN << data;
    BOOST_CHECK(s.isCommitment(data));

    // 64 bytes commitment, still valid.
    data.resize(64);
    s = CScript() << OP_RETURN << data;
    BOOST_CHECK(s.isCommitment(data));

    // Commitment is too large.
    data.push_back(23);
    s = CScript() << OP_RETURN << data;
    BOOST_CHECK(!s.isCommitment(data));

    // Check with the actual replay commitment we are going to use.
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params &params = Params().GetConsensus();
    s = CScript() << OP_RETURN << params.antiReplayOpReturnCommitment;
    BOOST_CHECK(s.isCommitment(params.antiReplayOpReturnCommitment));
}

BOOST_AUTO_TEST_SUITE_END()
