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
#include <boost/test/auto_unit_test.hpp>
#include <Application.h>
#include <BlocksDB.h>
#include <chain.h>
#include <chainparams.h>

#include "test/test_bitcoin.h"

class MyTestingFixture : public TestingSetup
{
public:
    MyTestingFixture() : TestingSetup(CBaseChainParams::REGTEST, BlocksDbOnDisk) {}
};

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

    // Create BlockIndex
    CBlockIndex *index = new CBlockIndex();
    index->nHeight = 1;
    index->nTime = 18000;
    index->pprev = Blocks::indexMap.begin()->second;
    const uint256 hash = CDiskBlockIndex(index).GetBlockHash();
    index->phashBlock = &hash;
    Blocks::DB::instance()->appendBlock(index, 0);
    Blocks::indexMap.insert(std::make_pair(hash, index));
    Blocks::DB::instance()->appendHeader(index);

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

BOOST_AUTO_TEST_SUITE_END()
