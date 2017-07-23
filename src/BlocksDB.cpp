// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (C) 2017 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "BlocksDB.h"
#include "BlocksDB_p.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "Application.h"
#include "init.h" // for StartShutdown

#include "chain.h"
#include "main.h"
#include "uint256.h"
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>

static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';
static const char DB_UAHF_FORK_BLOCK = 'U';

namespace {
CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return NULL;

    // Return existing
    auto mi = Blocks::indexMap.find(hash);
    if (mi != Blocks::indexMap.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    mi = Blocks::indexMap.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, CDiskBlockPos *dbp = nullptr)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2E6, 1E6+8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock && Blocks::indexMap.find(block.hashPrevBlock) == Blocks::indexMap.end()) {
                    LogPrint("reindex", "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                            block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (Blocks::indexMap.count(hash) == 0 || (Blocks::indexMap[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    CValidationState state;
                    if (ProcessNewBlock(state, chainparams, NULL, &block, true, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                } else if (hash != chainparams.GetConsensus().hashGenesisBlock && Blocks::indexMap[hash]->nHeight % 1000 == 0) {
                    LogPrintf("Block Import: already had block %s at height %d\n", hash.ToString(), Blocks::indexMap[hash]->nHeight);
                }

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second, chainparams.GetConsensus()))
                        {
                            LogPrintf("%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                    head.ToString());
                            CValidationState dummy;
                            if (ProcessNewBlock(dummy, chainparams, NULL, &block, true, &it->second))
                            {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
        return false;
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

struct CImportingNow
{
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};

void reimportBlockFiles(std::vector<boost::filesystem::path> vImportFiles)
{
    const CChainParams& chainparams = Params();
    RenameThread("bitcoin-loadblk");
    bool fReindex = Blocks::DB::instance()->isReindexing();

    if (fReindex) {
        CImportingNow imp;
        int nFile = 0;
        while (true) {
            CDiskBlockPos pos(nFile, 0);
            if (!boost::filesystem::exists(Blocks::getFilepathForIndex(pos.nFile, "blk")))
                break; // No block files left to reindex
            FILE *file = Blocks::openFile(pos, true);
            if (!file)
                break; // This error is logged in OpenBlockFile
            LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
            LoadExternalBlockFile(chainparams, file, &pos);
            nFile++;
        }
        Blocks::DB::instance()->setIsReindexing(false);
        fReindex = false;
        LogPrintf("Reindexing finished\n");
        // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
        InitBlockIndex(chainparams);
    }

    // hardcoded $DATADIR/bootstrap.dat
    boost::filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (boost::filesystem::exists(pathBootstrap)) {
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            boost::filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LogPrintf("Importing bootstrap.dat...\n");
            LoadExternalBlockFile(chainparams, file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        } else {
            LogPrintf("Warning: Could not open bootstrap file %s\n", pathBootstrap.string());
        }
    }

    // -loadblock=
    BOOST_FOREACH(const boost::filesystem::path& path, vImportFiles) {
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            LogPrintf("Importing blocks file %s...\n", path.string());
            LoadExternalBlockFile(chainparams, file);
        } else {
            LogPrintf("Warning: Could not open blocks file %s\n", path.string());
        }
    }

    if (GetBoolArg("-stopafterblockimport", DEFAULT_STOPAFTERBLOCKIMPORT)) {
        LogPrintf("Stopping after block import\n");
        StartShutdown();
    }
}

}

namespace Blocks {
BlockMap indexMap;
}


Blocks::DB* Blocks::DB::s_instance = nullptr;

Blocks::DB *Blocks::DB::instance()
{
    return Blocks::DB::s_instance;
}

void Blocks::DB::createInstance(size_t nCacheSize, bool fWipe)
{
    Blocks::indexMap.clear();
    delete Blocks::DB::s_instance;
    Blocks::DB::s_instance = new Blocks::DB(nCacheSize, false, fWipe);
}

void Blocks::DB::createTestInstance(size_t nCacheSize)
{
    Blocks::indexMap.clear();
    delete Blocks::DB::s_instance;
    Blocks::DB::s_instance = new Blocks::DB(nCacheSize, true);
}

void Blocks::DB::startBlockImporter()
{
    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("-loadblock"))
    {
        BOOST_FOREACH(const std::string& strFile, mapMultiArgs["-loadblock"])
            vImportFiles.push_back(strFile);
    }
    Application::createThread(std::bind(&reimportBlockFiles, vImportFiles));
}



Blocks::DB::DB(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe),
      d(new DBPrivate())
{
    d->isReindexing = Exists(DB_REINDEX_FLAG);
}

Blocks::DB::~DB()
{
    delete d;
}

bool Blocks::DB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool Blocks::DB::setIsReindexing(bool fReindexing) {
    if (d->isReindexing == fReindexing)
        return true;
    d->isReindexing = fReindexing;
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool Blocks::DB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

bool Blocks::DB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool Blocks::DB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(std::make_pair(DB_TXINDEX, txid), pos);
}

bool Blocks::DB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(&GetObfuscateKey());
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(std::make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool Blocks::DB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool Blocks::DB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool Blocks::DB::CacheAllBlockInfos()
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));
    int maxFile = 0;

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                maxFile = std::max(pindexNew->nFile, maxFile);
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                pcursor->Next();
            } else {
                return error("CacheAllBlockInfos(): failed to read row");
            }
        } else {
            break;
        }
    }

    for (auto iter = Blocks::indexMap.begin(); iter != Blocks::indexMap.end(); ++iter) {
        iter->second->BuildSkip();
    }
    for (auto iter = Blocks::indexMap.begin(); iter != Blocks::indexMap.end(); ++iter) {
        appendHeader(iter->second);
    }

    if (Application::uahfChainState() != Application::UAHFDisabled) {
        Read(DB_UAHF_FORK_BLOCK, d->uahfStartBlock);
        setUahfForkBlock(d->uahfStartBlock);
    }

    return true;
}

bool Blocks::DB::isReindexing() const
{
    return d->isReindexing;
}

bool Blocks::DB::appendHeader(CBlockIndex *block)
{
    assert(block);
    assert(block->phashBlock);
    bool found = false;
    const bool valid = (block->nStatus & BLOCK_FAILED_MASK) == 0;
    assert(valid || block->pprev);  // can't mark the genesis as invalid.
    for (auto i = d->headerChainTips.begin(); i != d->headerChainTips.end(); ++i) {
        CBlockIndex *tip = *i;
        CBlockIndex *parent = block;
        while (parent && parent->nHeight > tip->nHeight) {
            parent = parent->pprev;
        }
        if (parent == tip) {
            if (!valid)
                block = block->pprev;
            d->headerChainTips.erase(i);
            d->headerChainTips.push_back(block);
            if (tip == d->headersChain.Tip()) {
                d->headersChain.SetTip(block);
                pindexBestHeader = block;
                return true;
            }
            found = true;
            break;
        }
    }

    if (!found) {
        for (auto i = d->headerChainTips.begin(); i != d->headerChainTips.end(); ++i) {
            if ((*i)->GetAncestor(block->nHeight) == block) { // known in this chain.
                if (valid)
                    return false;
                // if it is invalid, remove it and all children.
                const bool modifyingMainChain = d->headersChain.Contains(*i);
                d->headerChainTips.erase(i);
                block = block->pprev;
                d->headerChainTips.push_back(block);
                if (modifyingMainChain)
                    d->headersChain.SetTip(block);
                return modifyingMainChain;
            }
        }
        if (valid) {
            d->headerChainTips.push_back(block);
            if (d->headersChain.Height() == -1) { // add genesis
                d->headersChain.SetTip(block);
                pindexBestHeader = block;
                return true;
            }
        }
    }
    if (d->headersChain.Tip()->nChainWork < block->nChainWork) {
        // we changed what is to be considered the main-chain. Update the CChain instance.
        d->headersChain.SetTip(block);
        pindexBestHeader = block;
        return true;
    }
    return false;
}

bool Blocks::DB::appendBlock(CBlockIndex *block, int lastBlockFile)
{
    std::vector<std::pair<int, const CBlockFileInfo*> > files;
    std::vector<const CBlockIndex*> blocks;
    blocks.push_back(block);
    return WriteBatchSync(files, lastBlockFile, blocks);
}

const CChain &Blocks::DB::headerChain()
{
    return d->headersChain;
}

const std::list<CBlockIndex *> &Blocks::DB::headerChainTips()
{
    return d->headerChainTips;
}

bool Blocks::DB::setUahfForkBlock(const uint256 &blockId)
{
    bool writeOk = true;
    if (d->uahfStartBlock != blockId) {
        d->uahfStartBlock = blockId;
        writeOk = Write(DB_UAHF_FORK_BLOCK, d->uahfStartBlock);
    }

    if (writeOk && !d->uahfStartBlock.IsNull()) {
        auto mi = Blocks::indexMap.find(d->uahfStartBlock);
        if (mi != Blocks::indexMap.end()) {
            const CBlockIndex *bi = mi->second;
            if (bi->nTime > Application::uahfStartTime() && headerChain().Contains(bi))
                Application::setUahfChainState(Application::UAHFActive);
        }
    }
    return writeOk;
}

const uint256 &Blocks::DB::uahfForkBlock() const
{
    return d->uahfStartBlock;
}


///////////////////////////////////////////////

static FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = Blocks::getFilepathForIndex(pos.nFile, prefix);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* Blocks::openFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* Blocks::openUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path Blocks::getFilepathForIndex(int fileIndex, const char *prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, fileIndex);
}


///////////////////////////////////////////////

Blocks::DBPrivate::DBPrivate()
    : isReindexing(false)
{
}
