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
    hashes.resize(20);

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
}

BOOST_AUTO_TEST_SUITE_END()
