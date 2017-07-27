// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (C) 2016 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "miner.h"
#include "pubkey.h"

#include "amount.h"
#include "Application.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "main.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "utilstrencodings.h"

#include <boost/tuple/tuple.hpp>
#include <queue>
#include <script/standard.cpp>

#ifdef ENABLE_WALLET
# include <wallet/wallet.h>
# include <init.h>
# include <boost/algorithm/hex.hpp>
#endif

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

class ScoreCompare
{
public:
    ScoreCompare() {}

    bool operator()(const CTxMemPool::txiter a, const CTxMemPool::txiter b)
    {
        return CompareTxMemPoolEntryByScore()(*b,*a); // Convert to less than
    }
};

int64_t Mining::UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

CScript Mining::GetCoinbase() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_coinbase;
}

void Mining::SetCoinbase(const CScript &coinbase)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_coinbase = coinbase;
}


CBlockTemplate* Mining::CreateNewBlock(const CChainParams& chainparams) const
{
    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience
    pblock->nTime = GetAdjustedTime();

    // Create coinbase tx
    CMutableTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_coinbase.empty())
            throw std::runtime_error("Require coinbase to be set before mining");
        txNew.vout[0].scriptPubKey = m_coinbase;
    }

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create (in bytes):
    uint32_t nBlockMaxSize = std::max<uint32_t>(1000, GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    const uint32_t nBlockPrioritySize = std::min<uint32_t>(GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE), nBlockMaxSize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    uint32_t nBlockMinSize = std::min<uint32_t>(GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE), nBlockMaxSize);

    // Collect memory pool transactions into the block
    CTxMemPool::setEntries inBlock;
    CTxMemPool::setEntries waitSet;

    // This vector will be sorted into a priority queue:
    std::vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash> waitPriMap;
    typedef std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    std::priority_queue<CTxMemPool::txiter, std::vector<CTxMemPool::txiter>, ScoreCompare> clearedTxs;
    bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    const uint32_t nCoinbaseReserveSize = 1000;
    uint64_t nBlockSize = nCoinbaseReserveSize;
    uint64_t nBlockTx = 0;
    unsigned int nBlockSigOps = 100;
    int lastFewTxs = 0;
    CAmount nFees = 0;
    bool fCreatedValidBlock = false;

    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();

        if (Application::uahfChainState() == Application::UAHFWaiting) { // we are not allowed to mine over 1MB yet.
            nBlockMaxSize = std::min<uint32_t>(1000000, nBlockMaxSize);
            nBlockMinSize = std::min<uint32_t>(nBlockMaxSize, nBlockMinSize);
        }

        const int nHeight = pindexPrev->nHeight + 1;
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

        pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
        // -regtest only: allow overriding block.nVersion with
        // -blockversion=N to test forking scenarios
        if (chainparams.MineBlocksOnDemand())
            pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

        UpdateTime(pblock, Params().GetConsensus(), pindexPrev);

        int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                                ? nMedianTimePast
                                : pblock->GetBlockTime();

        bool fPriorityBlock = nBlockPrioritySize > 0;
        if (fPriorityBlock) {
            vecPriority.reserve(mempool.mapTx.size());
            for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
                 mi != mempool.mapTx.end(); ++mi)
            {
                double dPriority = mi->GetPriority(nHeight);
                CAmount dummy;
                mempool.ApplyDeltas(mi->GetTx().GetHash(), dPriority, dummy);
                vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
            }
            std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
        }

        CTxMemPool::indexed_transaction_set::nth_index<3>::type::iterator mi = mempool.mapTx.get<3>().begin();
        CTxMemPool::txiter iter;

        while (mi != mempool.mapTx.get<3>().end() || !clearedTxs.empty())
        {
            bool priorityTx = false;
            if (fPriorityBlock && !vecPriority.empty()) { // add a tx from priority queue to fill the blockprioritysize
                priorityTx = true;
                iter = vecPriority.front().second;
                actualPriority = vecPriority.front().first;
                std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                vecPriority.pop_back();
            }
            else if (clearedTxs.empty()) { // add tx with next highest score
                iter = mempool.mapTx.project<0>(mi);
                mi++;
            }
            else {  // try to add a previously postponed child tx
                iter = clearedTxs.top();
                clearedTxs.pop();
            }

            if (inBlock.count(iter))
                continue; // could have been added to the priorityBlock

            const CTransaction& tx = iter->GetTx();

            bool fOrphan = false;
            BOOST_FOREACH(CTxMemPool::txiter parent, mempool.GetMemPoolParents(iter))
            {
                if (!inBlock.count(parent)) {
                    fOrphan = true;
                    break;
                }
            }
            if (fOrphan) {
                if (priorityTx)
                    waitPriMap.insert(std::make_pair(iter,actualPriority));
                else
                    waitSet.insert(iter);
                continue;
            }

            unsigned int nTxSize = iter->GetTxSize();
            if (fPriorityBlock &&
                (nBlockSize + nTxSize >= nBlockPrioritySize || !AllowFree(actualPriority))) {
                fPriorityBlock = false;
                waitPriMap.clear();
            }
            if (!priorityTx &&
                (iter->GetModifiedFee() < ::minRelayTxFee.GetFee(nTxSize) && nBlockSize >= nBlockMinSize)) {
                break;
            }
            if (nBlockSize + nTxSize >= nBlockMaxSize) {
                if (nBlockSize >  nBlockMaxSize - 100 || lastFewTxs > 50) {
                    break;
                }
                // Once we're within 1000 bytes of a full block, only look at 50 more txs
                // to try to fill the remaining space.
                if (nBlockSize > nBlockMaxSize - 1000) {
                    lastFewTxs++;
                }
                continue;
            }

            if (!IsFinalTx(tx, nHeight, nLockTimeCutoff))
                continue;

            const uint64_t maxSigOps = Policy::blockSigOpAcceptLimit(nBlockSize + nTxSize - nCoinbaseReserveSize);
            unsigned int nTxSigOps = iter->GetSigOpCount();
            if (nBlockSigOps + nTxSigOps >= maxSigOps) {
                if (nBlockSigOps > maxSigOps - 2) {
                    break;
                }
                continue;
            }

            CAmount nTxFees = iter->GetFee();
            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                double dPriority = iter->GetPriority(nHeight);
                CAmount dummy;
                mempool.ApplyDeltas(tx.GetHash(), dPriority, dummy);
                LogPrintf("priority %.1f fee %s txid %s\n",
                          dPriority , CFeeRate(iter->GetModifiedFee(), nTxSize).ToString(), tx.GetHash().ToString());
            }

            inBlock.insert(iter);

            // Add transactions that depend on this one to the priority queue
            BOOST_FOREACH(CTxMemPool::txiter child, mempool.GetMemPoolChildren(iter))
            {
                if (fPriorityBlock) {
                    waitPriIter wpiter = waitPriMap.find(child);
                    if (wpiter != waitPriMap.end()) {
                        vecPriority.push_back(TxCoinAgePriority(wpiter->second,child));
                        std::push_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                        waitPriMap.erase(wpiter);
                    }
                }
                else {
                    if (waitSet.count(child)) {
                        clearedTxs.push(child);
                        waitSet.erase(child);
                    }
                }
            }
        }
        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld sigops %d\n", nBlockSize, nBlockTx, nFees, nBlockSigOps);

        // Compute final coinbase transaction.
        if (flexTransActive.load())
            txNew.nVersion = 4;
        txNew.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0 << m_coinbaseComment;
        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
        pblock->nNonce         = 0;
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CValidationState state;
        fCreatedValidBlock = TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false);
        if (!fCreatedValidBlock) {
            if (pblock->vtx.size() <= 1) {
                // This should REALLY never happen! Empty block that is invalid.
                throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", 
                                                   __func__, FormatStateMessage(state)));
            }
            // This should also never happen... but if an invalid transaction somehow entered
            // the mempool due to a bug, remove all the transactions in the block
            // and try again (it is not worth trying to figure out which transaction(s)
            // are causing the block to be invalid).
            LogPrintf("%s: TestBlockValidity failed: %s, retrying with smaller mempool",
                      __func__, FormatStateMessage(state));
            std::list<CTransaction> unused;
            BOOST_REVERSE_FOREACH(const CTransaction& tx, pblock->vtx) {
                mempool.remove(tx, unused, true);
            }
        }
    }

    if (!fCreatedValidBlock) {
        pblocktemplate.reset();
        return CreateNewBlock(chainparams); // recurse with smaller mempool
    }

    return pblocktemplate.release();
}

void Mining::IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    if (m_hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        m_hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) << m_coinbaseComment;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// The nonce is usually preserved between calls, but periodically or if the
// nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at
// zero.
//
bool static ScanHash(const CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash)
{
    // Write the first 76 bytes of the block header to a double-SHA256 state.
    CHash256 hasher;
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *pblock;
    assert(ss.size() == 80);
    hasher.Write((unsigned char*)&ss[0], 76);

    while (true) {
        nNonce++;

        // Write the last 4 bytes of the block header (the nonce) to a copy of
        // the double-SHA256 state, and compute the result.
        CHash256(hasher).Write((unsigned char*)&nNonce, 4).Finalize((unsigned char*)phash);

        // Return the nonce if the hash has at least some zero bits,
        // caller will check if it has enough to reach the target
        if (((uint16_t*)phash)[15] == 0)
            return true;

        // If nothing found after trying for a while, return -1
        if ((nNonce & 0xfff) == 0)
            return false;
    }
}

static bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("BitcoinMiner: generated block is stale");
    }

    // Inform about the new block
    GetMainSignals().BlockFound(pblock->GetHash());

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, chainparams, NULL, pblock, true, NULL))
        return error("BitcoinMiner: ProcessNewBlock, block not accepted");

    return true;
}

void static BitcoinMiner(const CChainParams& chainparams)
{
    LogPrintf("BitcoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("bitcoin-miner");

    unsigned int nExtraNonce = 0;
    Mining *mining = Mining::instance();
    const CScript coinbaseScript = mining->GetCoinbase();

    try {
        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    if (!fvNodesEmpty && !IsInitialBlockDownload())
                        break;
                    MilliSleep(1000);
                } while (true);
            }

            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = chainActive.Tip();

            std::unique_ptr<CBlockTemplate> pblocktemplate(mining->CreateNewBlock(chainparams));
            if (!pblocktemplate.get())
            {
                LogPrintf("Error in BitcoinMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            mining->IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("Running BitcoinMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            uint256 hash;
            uint32_t nNonce = 0;
            while (true) {
                // Check if something found
                if (ScanHash(pblock, nNonce, &hash))
                {
                    if (UintToArith256(hash) <= hashTarget)
                    {
                        // Found a solution
                        pblock->nNonce = nNonce;
                        assert(hash == pblock->GetHash());

                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("BitcoinMiner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
                        ProcessBlockFound(pblock, chainparams);
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);

                        // In regression test mode, stop mining after a block is found.
                        if (chainparams.MineBlocksOnDemand())
                            throw boost::thread_interrupted();

                        break;
                    }
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    break;
                if (nNonce >= 0xffff0000)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nTime every few seconds
                if (mining->UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                           // so that we can use the correct time.
                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("BitcoinMiner terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("BitcoinMiner runtime error: %s\n", e.what());
        return;
    }
}

CScript Mining::ScriptForCoinbase(const std::string &coinbase)
{
    if (coinbase.empty())
        throw std::runtime_error("Please pass in a coinbase");

    if (IsHex(coinbase)) {
        std::vector<unsigned char> data(ParseHex(coinbase));
        CPubKey pubKey(data.begin(), data.end());
        if (!pubKey.IsFullyValid())
            throw std::runtime_error("Pubkey is not a valid public key");

        CScript answer;
        answer << ToByteVector(data) << OP_CHECKSIG;
        return answer;
    }
    throw std::runtime_error("pubkey not in recognized format");
}

void Mining::GenerateBitcoins(bool fGenerate, int nThreads, const CChainParams& chainparams, const std::string &coinbase_)
{
    if (nThreads < 0)
        nThreads = GetNumCores();

    Mining *miningInstance = instance();

    if (miningInstance->m_minerThreads != 0) // delete old
    {
        miningInstance->m_minerThreads->interrupt_all();
        delete miningInstance->m_minerThreads;
        miningInstance->m_minerThreads = 0;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    std::string coinbase(coinbase_);
#ifdef ENABLE_WALLET
    if (coinbase.empty()) {
        // try to get it from the wallet
        boost::shared_ptr<CReserveScript> coinbaseScript;
        GetMainSignals().ScriptForMining(coinbaseScript);

        if (pwalletMain) {
            boost::shared_ptr<CReserveKey> rKey(new CReserveKey(pwalletMain));
            CPubKey pubkey;
            if (rKey->GetReservedKey(pubkey)) {
                std::vector<unsigned char> v = ToByteVector(pubkey);
                boost::algorithm::hex(v.begin(), v.end(), back_inserter(coinbase));
                rKey->KeepKey();
            }
        }
    }
#endif

    miningInstance->SetCoinbase(ScriptForCoinbase(coinbase));
    miningInstance->m_minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        miningInstance->m_minerThreads->create_thread(boost::bind(&BitcoinMiner, boost::cref(chainparams)));
}

Mining* Mining::s_instance = 0;

void Mining::Stop()
{
    delete s_instance;
    s_instance = 0;
}

Mining *Mining::instance()
{
    if (s_instance == 0)
        s_instance = new Mining();
    return s_instance;
}

Mining::~Mining()
{
    if (m_minerThreads) {
        m_minerThreads->interrupt_all();
        delete m_minerThreads;
    }
}

Mining::Mining()
    : m_minerThreads(0)
{
    // read args to create m_coinbaseComment
    std::int32_t sizeLimit = Policy::blockSizeAcceptLimit();

    std::stringstream ss;
    ss << std::fixed;
    if ((sizeLimit % 1000000) != 0)
        ss << std::setprecision(1) << sizeLimit / 1E6;
    else
        ss << (int) (sizeLimit / 1E6);
    std::string comment = "EB" + ss.str();
    m_coinbaseComment =  std::vector<unsigned char>(comment.begin(), comment.end());
}
