// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2016 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "data/tx_invalid.json.h"
#include "data/tx_valid.json.h"
#include "test/test_bitcoin.h"

#include "clientversion.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "key.h"
#include "keystore.h"
#include "main.h" // For CheckTransaction
#include "policy/policy.h"
#include "script/script.h"
#include "script/script_error.h"
#include "utilstrencodings.h"
#include "transaction_utils.h"

#include <map>
#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/assign/list_of.hpp>

#include <univalue.h>

using namespace std;

// In script_tests.cpp
extern UniValue read_json(const std::string& jsondata);

static std::map<string, unsigned int> mapFlagNames = boost::assign::map_list_of
    (string("NONE"), (unsigned int)SCRIPT_VERIFY_NONE)
    (string("P2SH"), (unsigned int)SCRIPT_VERIFY_P2SH)
    (string("STRICTENC"), (unsigned int)SCRIPT_VERIFY_STRICTENC)
    (string("DERSIG"), (unsigned int)SCRIPT_VERIFY_DERSIG)
    (string("LOW_S"), (unsigned int)SCRIPT_VERIFY_LOW_S)
    (string("SIGPUSHONLY"), (unsigned int)SCRIPT_VERIFY_SIGPUSHONLY)
    (string("MINIMALDATA"), (unsigned int)SCRIPT_VERIFY_MINIMALDATA)
    (string("NULLDUMMY"), (unsigned int)SCRIPT_VERIFY_NULLDUMMY)
    (string("DISCOURAGE_UPGRADABLE_NOPS"), (unsigned int)SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS)
    (string("CLEANSTACK"), (unsigned int)SCRIPT_VERIFY_CLEANSTACK)
    (string("CHECKLOCKTIMEVERIFY"), (unsigned int)SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)
    (string("CHECKSEQUENCEVERIFY"), (unsigned int)SCRIPT_VERIFY_CHECKSEQUENCEVERIFY)
    (std::string("SIGHASH_FORKID"), (unsigned int)SCRIPT_ENABLE_SIGHASH_FORKID);

unsigned int ParseScriptFlags(string strFlags)
{
    if (strFlags.empty()) {
        return 0;
    }
    unsigned int flags = 0;
    vector<string> words;
    boost::algorithm::split(words, strFlags, boost::algorithm::is_any_of(","));

    BOOST_FOREACH(string word, words)
    {
        if (!mapFlagNames.count(word))
            BOOST_ERROR("Bad test: unknown verification flag '" << word << "'");
        flags |= mapFlagNames[word];
    }

    return flags;
}

string FormatScriptFlags(unsigned int flags)
{
    if (flags == 0) {
        return "";
    }
    string ret;
    std::map<string, unsigned int>::const_iterator it = mapFlagNames.begin();
    while (it != mapFlagNames.end()) {
        if (flags & it->second) {
            ret += it->first + ",";
        }
        it++;
    }
    return ret.substr(0, ret.size() - 1);
}

BOOST_FIXTURE_TEST_SUITE(transaction_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(tx_valid)
{
    // Read tests from test/data/tx_valid.json
    // Format is an array of arrays
    // Inner arrays are either [ "comment" ]
    // or [[[prevout hash, prevout index, prevout scriptPubKey], [input 2], ...],"], serializedTransaction, verifyFlags
    // ... where all scripts are stringified scripts.
    //
    // verifyFlags is a comma separated list of script verification flags to apply, or "NONE"
    UniValue tests = read_json(std::string(json_tests::tx_valid, json_tests::tx_valid + sizeof(json_tests::tx_valid)));

    ScriptError err;
    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        string strTest = test.write();
        if (test[0].isArray())
        {
            if (test.size() != 3 || !test[1].isStr() || !test[2].isStr())
            {
                BOOST_ERROR("Bad test: " << strTest);
                continue;
            }

            map<COutPoint, CScript> mapprevOutScriptPubKeys;
            UniValue inputs = test[0].get_array();
            bool fValid = true;
        for (unsigned int inpIdx = 0; inpIdx < inputs.size(); inpIdx++) {
            const UniValue& input = inputs[inpIdx];
                if (!input.isArray())
                {
                    fValid = false;
                    break;
                }
                UniValue vinput = input.get_array();
                if (vinput.size() != 3)
                {
                    fValid = false;
                    break;
                }

                mapprevOutScriptPubKeys[COutPoint(uint256S(vinput[0].get_str()), vinput[1].get_int())] = ParseScript(vinput[2].get_str());
            }
            if (!fValid)
            {
                BOOST_ERROR("Bad test: " << strTest);
                continue;
            }

            string transaction = test[1].get_str();
            CDataStream stream(ParseHex(transaction), SER_NETWORK, PROTOCOL_VERSION);
            CTransaction tx;
            stream >> tx;

            CValidationState state;
            BOOST_CHECK_MESSAGE(CheckTransaction(tx, state), strTest);
            BOOST_CHECK(state.IsValid());

            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                if (!mapprevOutScriptPubKeys.count(tx.vin[i].prevout))
                {
                    BOOST_ERROR("Bad test: " << strTest);
                    break;
                }

                CAmount amount = 0;
                unsigned int verify_flags = ParseScriptFlags(test[2].get_str());
                BOOST_CHECK_MESSAGE(VerifyScript(tx.vin[i].scriptSig, mapprevOutScriptPubKeys[tx.vin[i].prevout],
                                                 verify_flags, TransactionSignatureChecker(&tx, i, amount), &err),
                                    strTest);
                BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_OK, ScriptErrorString(err));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(tx_invalid)
{
    // Read tests from test/data/tx_invalid.json
    // Format is an array of arrays
    // Inner arrays are either [ "comment" ]
    // or [[[prevout hash, prevout index, prevout scriptPubKey], [input 2], ...],"], serializedTransaction, verifyFlags
    // ... where all scripts are stringified scripts.
    //
    // verifyFlags is a comma separated list of script verification flags to apply, or "NONE"
    UniValue tests = read_json(std::string(json_tests::tx_invalid, json_tests::tx_invalid + sizeof(json_tests::tx_invalid)));

    ScriptError err;
    for (unsigned int idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        string strTest = test.write();
        if (test[0].isArray())
        {
            if (test.size() != 3 || !test[1].isStr() || !test[2].isStr())
            {
                BOOST_ERROR("Bad test: " << strTest);
                continue;
            }

            map<COutPoint, CScript> mapprevOutScriptPubKeys;
            UniValue inputs = test[0].get_array();
            bool fValid = true;
            for (unsigned int inpIdx = 0; inpIdx < inputs.size(); inpIdx++) {
                const UniValue& input = inputs[inpIdx];
                if (!input.isArray())
                {
                    fValid = false;
                    break;
                }
                UniValue vinput = input.get_array();
                if (vinput.size() != 3)
                {
                    fValid = false;
                    break;
                }

                mapprevOutScriptPubKeys[COutPoint(uint256S(vinput[0].get_str()), vinput[1].get_int())] = ParseScript(vinput[2].get_str());
            }
            if (!fValid)
            {
                BOOST_ERROR("Bad test: " << strTest);
                continue;
            }

            string transaction = test[1].get_str();
            CDataStream stream(ParseHex(transaction), SER_NETWORK, PROTOCOL_VERSION);
            CTransaction tx;
            stream >> tx;

            CValidationState state;
            fValid = CheckTransaction(tx, state) && state.IsValid();

            for (unsigned int i = 0; i < tx.vin.size() && fValid; i++)
            {
                if (!mapprevOutScriptPubKeys.count(tx.vin[i].prevout))
                {
                    BOOST_ERROR("Bad test: " << strTest);
                    break;
                }

                CAmount amount = 0;
                unsigned int verify_flags = ParseScriptFlags(test[2].get_str());
                fValid = VerifyScript(tx.vin[i].scriptSig, mapprevOutScriptPubKeys[tx.vin[i].prevout],
                                      verify_flags, TransactionSignatureChecker(&tx, i, amount), &err);
            }
            BOOST_CHECK_MESSAGE(!fValid, strTest);
            BOOST_CHECK_MESSAGE(err != SCRIPT_ERR_OK, ScriptErrorString(err));
        }
    }
}

BOOST_AUTO_TEST_CASE(basic_transaction_tests)
{
    // Random real transaction (e2769b09e784f32f62ef849763d4f45b98e07ba658647343b915ff832b110436)
    unsigned char ch[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x6b, 0xff, 0x7f, 0xcd, 0x4f, 0x85, 0x65, 0xef, 0x40, 0x6d, 0xd5, 0xd6, 0x3d, 0x4f, 0xf9, 0x4f, 0x31, 0x8f, 0xe8, 0x20, 0x27, 0xfd, 0x4d, 0xc4, 0x51, 0xb0, 0x44, 0x74, 0x01, 0x9f, 0x74, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x49, 0x30, 0x46, 0x02, 0x21, 0x00, 0xda, 0x0d, 0xc6, 0xae, 0xce, 0xfe, 0x1e, 0x06, 0xef, 0xdf, 0x05, 0x77, 0x37, 0x57, 0xde, 0xb1, 0x68, 0x82, 0x09, 0x30, 0xe3, 0xb0, 0xd0, 0x3f, 0x46, 0xf5, 0xfc, 0xf1, 0x50, 0xbf, 0x99, 0x0c, 0x02, 0x21, 0x00, 0xd2, 0x5b, 0x5c, 0x87, 0x04, 0x00, 0x76, 0xe4, 0xf2, 0x53, 0xf8, 0x26, 0x2e, 0x76, 0x3e, 0x2d, 0xd5, 0x1e, 0x7f, 0xf0, 0xbe, 0x15, 0x77, 0x27, 0xc4, 0xbc, 0x42, 0x80, 0x7f, 0x17, 0xbd, 0x39, 0x01, 0x41, 0x04, 0xe6, 0xc2, 0x6e, 0xf6, 0x7d, 0xc6, 0x10, 0xd2, 0xcd, 0x19, 0x24, 0x84, 0x78, 0x9a, 0x6c, 0xf9, 0xae, 0xa9, 0x93, 0x0b, 0x94, 0x4b, 0x7e, 0x2d, 0xb5, 0x34, 0x2b, 0x9d, 0x9e, 0x5b, 0x9f, 0xf7, 0x9a, 0xff, 0x9a, 0x2e, 0xe1, 0x97, 0x8d, 0xd7, 0xfd, 0x01, 0xdf, 0xc5, 0x22, 0xee, 0x02, 0x28, 0x3d, 0x3b, 0x06, 0xa9, 0xd0, 0x3a, 0xcf, 0x80, 0x96, 0x96, 0x8d, 0x7d, 0xbb, 0x0f, 0x91, 0x78, 0xff, 0xff, 0xff, 0xff, 0x02, 0x8b, 0xa7, 0x94, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x19, 0x76, 0xa9, 0x14, 0xba, 0xde, 0xec, 0xfd, 0xef, 0x05, 0x07, 0x24, 0x7f, 0xc8, 0xf7, 0x42, 0x41, 0xd7, 0x3b, 0xc0, 0x39, 0x97, 0x2d, 0x7b, 0x88, 0xac, 0x40, 0x94, 0xa8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x19, 0x76, 0xa9, 0x14, 0xc1, 0x09, 0x32, 0x48, 0x3f, 0xec, 0x93, 0xed, 0x51, 0xf5, 0xfe, 0x95, 0xe7, 0x25, 0x59, 0xf2, 0xcc, 0x70, 0x43, 0xf9, 0x88, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00};
    vector<unsigned char> vch(ch, ch + sizeof(ch) -1);
    CDataStream stream(vch, SER_DISK, CLIENT_VERSION);
    CMutableTransaction tx;
    stream >> tx;
    CValidationState state;
    BOOST_CHECK_MESSAGE(CheckTransaction(tx, state) && state.IsValid(), "Simple deserialized transaction should be valid.");

    // Check that duplicate txins fail
    tx.vin.push_back(tx.vin[0]);
    BOOST_CHECK_MESSAGE(!CheckTransaction(tx, state) || !state.IsValid(), "Transaction with duplicate txins should be invalid.");
}

//
// Helper: create two dummy transactions, each with
// two outputs.  The first has 11 and 50 CENT outputs
// paid to a TX_PUBKEY, the second 21 and 22 CENT outputs
// paid to a TX_PUBKEYHASH.
//
static std::vector<CMutableTransaction>
SetupDummyInputs(CBasicKeyStore& keystoreRet, CCoinsViewCache& coinsRet)
{
    std::vector<CMutableTransaction> dummyTransactions;
    dummyTransactions.resize(2);

    // Add some keys to the keystore:
    CKey key[4];
    for (int i = 0; i < 4; i++)
    {
        key[i].MakeNewKey(i % 2);
        keystoreRet.AddKey(key[i]);
    }

    // Create some dummy input transactions
    dummyTransactions[0].vout.resize(2);
    dummyTransactions[0].vout[0].nValue = 11*CENT;
    dummyTransactions[0].vout[0].scriptPubKey << ToByteVector(key[0].GetPubKey()) << OP_CHECKSIG;
    dummyTransactions[0].vout[1].nValue = 50*CENT;
    dummyTransactions[0].vout[1].scriptPubKey << ToByteVector(key[1].GetPubKey()) << OP_CHECKSIG;
    coinsRet.ModifyCoins(dummyTransactions[0].GetHash())->FromTx(dummyTransactions[0], 0);

    dummyTransactions[1].vout.resize(2);
    dummyTransactions[1].vout[0].nValue = 21*CENT;
    dummyTransactions[1].vout[0].scriptPubKey = GetScriptForDestination(key[2].GetPubKey().GetID());
    dummyTransactions[1].vout[1].nValue = 22*CENT;
    dummyTransactions[1].vout[1].scriptPubKey = GetScriptForDestination(key[3].GetPubKey().GetID());
    coinsRet.ModifyCoins(dummyTransactions[1].GetHash())->FromTx(dummyTransactions[1], 0);

    return dummyTransactions;
}

BOOST_AUTO_TEST_CASE(test_Get)
{
    CBasicKeyStore keystore;
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CMutableTransaction> dummyTransactions = SetupDummyInputs(keystore, coins);

    CMutableTransaction t1;
    t1.vin.resize(3);
    t1.vin[0].prevout.hash = dummyTransactions[0].GetHash();
    t1.vin[0].prevout.n = 1;
    t1.vin[0].scriptSig << std::vector<unsigned char>(65, 0);
    t1.vin[1].prevout.hash = dummyTransactions[1].GetHash();
    t1.vin[1].prevout.n = 0;
    t1.vin[1].scriptSig << std::vector<unsigned char>(65, 0) << std::vector<unsigned char>(33, 4);
    t1.vin[2].prevout.hash = dummyTransactions[1].GetHash();
    t1.vin[2].prevout.n = 1;
    t1.vin[2].scriptSig << std::vector<unsigned char>(65, 0) << std::vector<unsigned char>(33, 4);
    t1.vout.resize(2);
    t1.vout[0].nValue = 90*CENT;
    t1.vout[0].scriptPubKey << OP_1;

    BOOST_CHECK(AreInputsStandard(t1, coins));
    BOOST_CHECK_EQUAL(coins.GetValueIn(t1), (50+21+22)*CENT);
}

BOOST_AUTO_TEST_CASE(test_IsStandard)
{
    LOCK(cs_main);
    CBasicKeyStore keystore;
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CMutableTransaction> dummyTransactions = SetupDummyInputs(keystore, coins);

    CMutableTransaction t;
    t.vin.resize(1);
    t.vin[0].prevout.hash = dummyTransactions[0].GetHash();
    t.vin[0].prevout.n = 1;
    t.vin[0].scriptSig << std::vector<unsigned char>(65, 0);
    t.vout.resize(1);
    t.vout[0].nValue = 90*CENT;
    CKey key;
    key.MakeNewKey(true);
    t.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());

    string reason;
    BOOST_CHECK(IsStandardTx(t, reason));

    // Check dust with default relay fee:
    CAmount nDustThreshold = 182 * minRelayTxFee.GetFeePerK()/1000 * 3;
    BOOST_CHECK_EQUAL(nDustThreshold, 546);
    // dust:
    t.vout[0].nValue = nDustThreshold - 1;
    BOOST_CHECK(!IsStandardTx(t, reason));
    // not dust:
    t.vout[0].nValue = nDustThreshold;
    BOOST_CHECK(IsStandardTx(t, reason));

    // Check dust with odd relay fee to verify rounding:
    // nDustThreshold = 182 * 1234 / 1000 * 3
    minRelayTxFee = CFeeRate(1234);
    // dust:
    t.vout[0].nValue = 672 - 1;
    BOOST_CHECK(!IsStandardTx(t, reason));
    // not dust:
    t.vout[0].nValue = 672;
    BOOST_CHECK(IsStandardTx(t, reason));
    minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);

    t.vout[0].scriptPubKey = CScript() << OP_1;
    BOOST_CHECK(!IsStandardTx(t, reason));

    // MAX_OP_RETURN_RELAY-byte TX_NULL_DATA (standard)
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef3804678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38");
    BOOST_CHECK_EQUAL(MAX_OP_RETURN_RELAY, t.vout[0].scriptPubKey.size());
    BOOST_CHECK(IsStandardTx(t, reason));

    // MAX_OP_RETURN_RELAY+1-byte TX_NULL_DATA (non-standard)
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef3804678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef3800");
    BOOST_CHECK_EQUAL(MAX_OP_RETURN_RELAY + 1, t.vout[0].scriptPubKey.size());
    BOOST_CHECK(!IsStandardTx(t, reason));

    // Data payload can be encoded in any way...
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("");
    BOOST_CHECK(IsStandardTx(t, reason));
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("00") << ParseHex("01");
    BOOST_CHECK(IsStandardTx(t, reason));
    // OP_RESERVED *is* considered to be a PUSHDATA type opcode by IsPushOnly()!
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << OP_RESERVED << -1 << 0 << ParseHex("01") << 2 << 3 << 4 << 5 << 6 << 7 << 8 << 9 << 10 << 11 << 12 << 13 << 14 << 15 << 16;
    BOOST_CHECK(IsStandardTx(t, reason));
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << 0 << ParseHex("01") << 2 << ParseHex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    BOOST_CHECK(IsStandardTx(t, reason));

    // ...so long as it only contains PUSHDATA's
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << OP_RETURN;
    BOOST_CHECK(!IsStandardTx(t, reason));

    // TX_NULL_DATA w/o PUSHDATA
    t.vout.resize(1);
    t.vout[0].scriptPubKey = CScript() << OP_RETURN;
    BOOST_CHECK(IsStandardTx(t, reason));

    // Only one TX_NULL_DATA permitted in all cases
    t.vout.resize(2);
    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38");
    t.vout[1].scriptPubKey = CScript() << OP_RETURN << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38");
    BOOST_CHECK(!IsStandardTx(t, reason));

    t.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38");
    t.vout[1].scriptPubKey = CScript() << OP_RETURN;
    BOOST_CHECK(!IsStandardTx(t, reason));

    t.vout[0].scriptPubKey = CScript() << OP_RETURN;
    t.vout[1].scriptPubKey = CScript() << OP_RETURN;
    BOOST_CHECK(!IsStandardTx(t, reason));
}

BOOST_AUTO_TEST_CASE(test_version4)
{
    TxUtils::allowNewTransactions();
    // 10 random transactions, make sure that save/load works.
    for (int i = 0; i < 14; ++i) {
        CMutableTransaction tx1;
        if (i < 2) {
            tx1.vin.push_back(CTxIn()); // do what a coinbase tx does, empty input.
            if (i == 1)
                tx1.vin[0].scriptSig = CScript() << 101 << CScriptNum(12512);
            tx1.vout.push_back(CTxOut());
            CTxOut &txout = tx1.vout.back();
            txout.nValue = 10000;
            TxUtils::RandomScript(txout.scriptPubKey);
        } else {
            TxUtils::RandomTransaction(tx1, TxUtils::SingleOutput);
            // clean them a little because nSequence has some double meanings.

            if (i < 4) { // test TxRelativeBlockLock // TxRelativeTimeLock
                for (CTxIn &in : tx1.vin) {
                    in.nSequence = insecure_rand() & CTxIn::SEQUENCE_LOCKTIME_MASK;
                    if (i == 2)
                        in.nSequence += CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG; // TxRelativeTimeLock
                }
            } else {
                for (unsigned int in = 1; in < tx1.vin.size(); ++in) { // only keep the sequenc on the first one.
                    tx1.vin[in].nSequence = CTxIn::SEQUENCE_FINAL;
                }
            }
        }
        tx1.nVersion = 4;
        CDataStream ds1(0, 0);
        tx1.Serialize(ds1, 0, 0);
        std::vector<char> tx1Data(ds1.begin(), ds1.end());
        CTransaction tx2(tx1);
        CDataStream ds2(0, 0);
        tx2.Serialize(ds2, 0, 0);
        std::vector<char> tx2Data(ds2.begin(), ds2.end());
        BOOST_CHECK_EQUAL(tx1Data.size(), tx2Data.size());
        BOOST_CHECK(tx1Data == tx2Data);
        BOOST_CHECK(tx1.GetHash() == tx2.GetHash());

        CMutableTransaction tx3;
        {
            CDataStream ssData(tx1Data, SER_NETWORK, PROTOCOL_VERSION);
            ssData >> tx3;
        }
        BOOST_CHECK_EQUAL(tx1.vin.size(), tx3.vin.size());
        BOOST_CHECK(tx1.vin.front().prevout == tx3.vin.front().prevout);
        BOOST_CHECK(tx1.vout.size() == tx3.vout.size());
        BOOST_CHECK(tx1.vout.front().nValue == tx3.vout.front().nValue);
        BOOST_CHECK(tx1.vout.front().scriptPubKey == tx3.vout.front().scriptPubKey);
        BOOST_CHECK(tx1.vout == tx3.vout);
        BOOST_CHECK(tx1.nVersion == tx3.nVersion);
        BOOST_CHECK(tx1.vin.front().nSequence == tx3.vin.front().nSequence);
        BOOST_CHECK(tx1.GetHash() == tx3.GetHash());

        CTransaction tx4;
        {
            CDataStream ssData(tx1Data, SER_NETWORK, PROTOCOL_VERSION);
            ssData >> tx4;
        }
        BOOST_CHECK(tx1.GetHash() == tx4.GetHash());

        BOOST_CHECK_EQUAL(tx1.vin.size(), tx2.vin.size());
        BOOST_CHECK_EQUAL(tx1.vin.size(), tx3.vin.size());
        BOOST_CHECK_EQUAL(tx1.vin.size(), tx4.vin.size());
        for (unsigned int i = 0; i < tx1.vin.size(); i++) {
            BOOST_CHECK(tx1.vin.at(i).scriptSig == tx2.vin.at(i).scriptSig);
            BOOST_CHECK(tx1.vin.at(i).scriptSig == tx3.vin.at(i).scriptSig);
            BOOST_CHECK(tx1.vin.at(i).scriptSig == tx4.vin.at(i).scriptSig);
        }

        BOOST_CHECK(tx1.vin.at(0).prevout.IsNull() == tx2.vin.at(0).prevout.IsNull());
        BOOST_CHECK(tx1.vin.at(0).prevout.IsNull() == tx3.vin.at(0).prevout.IsNull());
        BOOST_CHECK(tx1.vin.at(0).prevout.IsNull() == tx4.vin.at(0).prevout.IsNull());
    }

    // load an existing transaction to check some properties.
    // This one is not made with the Bitcoin serializer, just to make sure we have more
    // than one compatible implementation.
    CTransaction tx;
    {
        CDataStream stream(ParseHex("040000000b20b18d97af2e95f38bf67df6aa23a5640c45632a"
            "13e52cd6658e9fb38b81a4093b1001331976a914a8ff9fad879c48667fb8a5ed68f41042aa7e74f2"
            "88ac28ddf6e704331976a914663f9689189018de83ad1c2ea14e011e7ecbc5b488ac28808ec2c940"
            "1b4830450221009b6bc5e6e021b59d8b22ab60224b22ab7e454c45923f0d5b6a621b185dc99a2202"
            "2015457191d829db95a339d4c89205b8e53aaaa271206f0db20141a6c2b0b1ed1a012341044feaa0"
            "598155a3e43590597c26f593f75fc23d83fa0b1fed35479175d072ec2057466b05e39a71a2a10690"
            "6be7a5812afbc03f24b513d2dd03c8844b9764d50704"), 0, 0);
        stream >> tx;
    }
    TxUtils::disallowNewTransactions();

    BOOST_CHECK_EQUAL(tx.vin.size(), 1);
    BOOST_CHECK_EQUAL(tx.vin.front().prevout.n, 1);
    BOOST_CHECK(tx.vin.front().prevout.hash == uint256(ParseHex("b18d97af2e95f38bf67df6aa23a5640c45632a13e52cd6658e9fb38b81a4093b")));
    BOOST_CHECK_EQUAL(tx.vin.front().scriptSig.size(), 139);
    BOOST_CHECK_EQUAL(tx.vout.size(), 2);
    BOOST_CHECK_EQUAL(tx.vout.front().nValue, 199095300);
    BOOST_CHECK_EQUAL(tx.vout.front().scriptPubKey.size(), 25);
    BOOST_CHECK_EQUAL(tx.vout.at(1).nValue, 301000000);
    BOOST_CHECK_EQUAL(tx.vout.at(1).scriptPubKey.size(), 25);
}

// lets try different ways of serializing and see if they all work.
// The point is that the code should not really have any issues with mixing tags at all,
// except in some corner cases like with optional tags.

BOOST_AUTO_TEST_CASE(test_serialization_order_simple)
{
    TxUtils::allowNewTransactions();
    const int nType = 0, nVersion = 0;

    CMutableTransaction baseTransaction;
    while (baseTransaction.vin.size() < 2)
        TxUtils::RandomTransaction(baseTransaction, TxUtils::SingleOutput);
    const CTxIn baseIn = baseTransaction.vin.front();
    const CTxOut baseOut = baseTransaction.vout.front();
    CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
    ser_writedata32(s, 4);
    CMFToken ph(Consensus::TxInPrevHash, baseIn.prevout.hash);
    ph.Serialize<CDataStream>(s, nType, nVersion);
    CMFToken index(Consensus::TxInPrevIndex, (uint64_t) baseIn.prevout.n);
    index.Serialize<CDataStream>(s, nType, nVersion);
    CMFToken token(Consensus::TxOutValue, (uint64_t) baseOut.nValue);
    token.Serialize<CDataStream>(s, nType, nVersion);
    std::vector<char> script(baseOut.scriptPubKey.begin(), baseOut.scriptPubKey.end());
    token = CMFToken(Consensus::TxOutScript, script);
    token.Serialize<CDataStream>(s, nType, nVersion);

    std::vector<char> data(s.begin(), s.end());
    CTransaction tx;
    {
        CDataStream ssData(data, SER_NETWORK, PROTOCOL_VERSION);
        ssData >> tx;
    }
    BOOST_CHECK_EQUAL(tx.vin.size(), 1);
    BOOST_CHECK_EQUAL(tx.vout.size(), 1);
    BOOST_CHECK(baseIn.prevout.hash == tx.vin.front().prevout.hash);
    BOOST_CHECK_EQUAL(baseIn.prevout.n, tx.vin.front().prevout.n);
    BOOST_CHECK_EQUAL(baseOut.nValue, tx.vout.front().nValue);
    BOOST_CHECK(baseOut.scriptPubKey == tx.vout.front().scriptPubKey);
    TxUtils::disallowNewTransactions();
}


BOOST_AUTO_TEST_CASE(test_serialization_order_mixed)
{
    TxUtils::allowNewTransactions();
    const int nType = 0, nVersion = 0;

    CMutableTransaction baseTransaction;
    while (baseTransaction.vin.size() < 2)
        TxUtils::RandomTransaction(baseTransaction, TxUtils::SingleOutput);
    const CTxIn baseIn = baseTransaction.vin.front();
    const CTxOut baseOut = baseTransaction.vout.front();
    const CTxIn baseIn2 = baseTransaction.vin.front();
    CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
    ser_writedata32(s, 4);
    CMFToken ph(Consensus::TxInPrevHash, baseIn.prevout.hash);
    ph.Serialize<CDataStream>(s, nType, nVersion);
    CMFToken index(Consensus::TxInPrevIndex, (uint64_t) baseIn.prevout.n);
    index.Serialize<CDataStream>(s, nType, nVersion);
    std::vector<char> script(baseOut.scriptPubKey.begin(), baseOut.scriptPubKey.end());
    CMFToken token(Consensus::TxOutScript, script); // swap with next
    token.Serialize<CDataStream>(s, nType, nVersion);
    token = CMFToken(Consensus::TxOutValue, (uint64_t) baseOut.nValue);
    token.Serialize<CDataStream>(s, nType, nVersion);

    // an in after an out.
    ph = CMFToken(Consensus::TxInPrevHash, baseIn2.prevout.hash);
    ph.Serialize<CDataStream>(s, nType, nVersion);
    index = CMFToken(Consensus::TxInPrevIndex, (uint64_t) baseIn2.prevout.n);
    index.Serialize<CDataStream>(s, nType, nVersion);

    std::vector<char> data(s.begin(), s.end());
    CTransaction tx;
    {
        CDataStream ssData(data, SER_NETWORK, PROTOCOL_VERSION);
        ssData >> tx;
    }
    BOOST_CHECK_EQUAL(tx.vin.size(), 2);
    BOOST_CHECK_EQUAL(tx.vout.size(), 1);
    BOOST_CHECK(baseIn.prevout.hash == tx.vin.front().prevout.hash);
    BOOST_CHECK_EQUAL(baseIn.prevout.n, tx.vin.front().prevout.n);
    BOOST_CHECK(baseIn2.prevout.hash == tx.vin.at(1).prevout.hash);
    BOOST_CHECK_EQUAL(baseIn2.prevout.n, tx.vin.at(1).prevout.n);
    BOOST_CHECK_EQUAL(baseOut.nValue, tx.vout.front().nValue);
    BOOST_CHECK(baseOut.scriptPubKey == tx.vout.front().scriptPubKey);

    TxUtils::disallowNewTransactions();
}

BOOST_AUTO_TEST_CASE(test_hashtype_version4)
{
    /*
     * Create various SIGHASH_SINGLE tx-es and combine them.
     * See if it still validates.
     *
     * Create a TX, sign with SIGHASH_NONE and then change the output. See if it still validates.
     */
    TxUtils::allowNewTransactions();

    CMutableTransaction tx1;
    while (tx1.vin.size() < 2 || tx1.vout.size() < 2)
        TxUtils::RandomTransaction(tx1, TxUtils::AnyOutputCount);
    tx1.nVersion = 4;

    int amount = 50000;

    { // SIGHASH_SINGLE
        const uint256 a = SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount, SIGHASH_SINGLE);
        const uint256 b = SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount, SIGHASH_SINGLE);

        CMutableTransaction copyOfTx1(tx1);
        // check amount first.
        BOOST_CHECK(SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount - 1, SIGHASH_SINGLE) != a);
        BOOST_CHECK(SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount - 1, SIGHASH_SINGLE) != b);

        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) == b);
        // outputs are flexible
        copyOfTx1.vout[1].nValue--; // Change 'b', not 'a'
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) != b);
        copyOfTx1.vout[0].nValue--; // change output.
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) != b);
        copyOfTx1 = tx1; // restore
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) == b);

        // inputs can't be changed.
        copyOfTx1.vin[0].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) != b);
        copyOfTx1.vin[1].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_SINGLE) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_SINGLE) != b);
    }

    { // SIGHASH_ANYONECANPAY
        const uint256 a = SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount, SIGHASH_ANYONECANPAY);
        const uint256 b = SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount, SIGHASH_ANYONECANPAY);

        CMutableTransaction copyOfTx1(tx1);
        // check amount first.
        BOOST_CHECK(SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount - 1, SIGHASH_ANYONECANPAY) != a);
        BOOST_CHECK(SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount - 1, SIGHASH_ANYONECANPAY) != b);

        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_ANYONECANPAY) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_ANYONECANPAY) == b);
        // outputs are totally rigid
        copyOfTx1.vout[1].nValue--; // Change 'b', not 'a'
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_ANYONECANPAY) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_ANYONECANPAY) != b);
        copyOfTx1.vout[0].nValue--; // change output.
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_ANYONECANPAY) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_ANYONECANPAY) != b);
        copyOfTx1 = tx1; // restore

        // Input are flexible
        copyOfTx1.vin[0].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_ANYONECANPAY) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_ANYONECANPAY) == b);
        copyOfTx1.vin[1].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_ANYONECANPAY) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_ANYONECANPAY) != b);
    }

    { // SIGHASH_NONE
        const uint256 a = SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount, SIGHASH_NONE);
        const uint256 b = SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount, SIGHASH_NONE);

        CMutableTransaction copyOfTx1(tx1);
        // check amount first.
        BOOST_CHECK(SignatureHash(tx1.vin[0].scriptSig, tx1, 0, amount - 1, SIGHASH_NONE) != a);
        BOOST_CHECK(SignatureHash(tx1.vin[1].scriptSig, tx1, 1, amount - 1, SIGHASH_NONE) != b);

        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_NONE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_NONE) == b);
        // outputs are flexible
        copyOfTx1.vout[1].nValue--; // Change 'b', not 'a'
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_NONE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_NONE) == b);
        copyOfTx1.vout[0].nValue--; // change output.
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_NONE) == a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_NONE) == b);
        copyOfTx1 = tx1; // restore

        // inputs can't be changed.
        copyOfTx1.vin[0].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_NONE) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_NONE) != b);
        copyOfTx1.vin[1].prevout.n++;
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[0].scriptSig, copyOfTx1, 0, amount, SIGHASH_NONE) != a);
        BOOST_CHECK(SignatureHash(copyOfTx1.vin[1].scriptSig, copyOfTx1, 1, amount, SIGHASH_NONE) != b);
    }

    TxUtils::disallowNewTransactions();
}

BOOST_AUTO_TEST_CASE(test_version4_isStandard)
{
    CMutableTransaction tx;
    TxUtils::RandomTransaction(tx, TxUtils::SingleOutput);
    // clean them a little because nSequence has some double meanings.
    for (unsigned int in = 1; in < tx.vin.size(); ++in) { // only keep the sequenc on the first one.
        tx.vin[in].nSequence = CTxIn::SEQUENCE_FINAL;
    }
    // our test doesn't seem to create a standard output script. So do that here.
    CKey key;
    key.MakeNewKey(true);
    tx.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());

    std::string reason;
    tx.nVersion = 1;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), true);
    tx.nVersion = 2;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), true);
    tx.nVersion = 3;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), false);
    tx.nVersion = 4;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), false);
    TxUtils::allowNewTransactions();
    tx.nVersion = 3;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), false);
    tx.nVersion = 5;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), false);
    tx.nVersion = 4;
    BOOST_CHECK_EQUAL(IsStandardTx(tx, reason), true);

    // 0 simple, is standard;
    // 1 add illegal token. not standard
    // 2 add illegal token, but dont set flag. is standard;
    for (int i = 0 ; i < 3; ++i) {
        try {
            CDataStream s(0, 4);
            ser_writedata32(s, 4);
            CMFToken hash(Consensus::TxInPrevHash, tx.vin[0].prevout.hash);
            hash.Serialize(s, 0, 4);
            CMFToken outValue(Consensus::TxOutValue, (uint64_t) 1000);
            outValue.Serialize(s, 0, 4);
            std::vector<char> script(tx.vout[0].scriptPubKey.begin(), tx.vout[0].scriptPubKey.end());
            CMFToken outScript(Consensus::TxOutScript, script);
            outScript.Serialize(s, 0, 4);

            if (i >= 1) {
                CMFToken invalidToken(10, true); // 9 is currently the max, 10 is thus a soft-fork and will be ignored
                invalidToken.Serialize(s, 0, 4);
            }
            CMFToken end(Consensus::TxEnd, true);
            end.Serialize(s, 0, 4);

            std::vector<char> txData(s.begin(), s.end());
            CDataStream stream(txData, 0, 4);
            CTransaction tx2;
            tx2.Unserialize(stream, 0, 4);

            mapArgs.clear();
            if (i < 2)
                mapArgs["-ft-strict"] = "1";
            BOOST_CHECK_EQUAL(IsStandardTx(tx2, reason), i != 1);
        } catch (...) {
            BOOST_CHECK(false);
        }
    }

    TxUtils::disallowNewTransactions();
}

BOOST_AUTO_TEST_SUITE_END()
