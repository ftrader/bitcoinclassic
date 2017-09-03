/*
 * This file is part of the bitcoin-classic project
 * Copyright (C) 2017 Calin Culianu <calin.culianu@gmail.com>
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

#include "util.h"
#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

#include "BlocksDB_p.h"
#include "BlocksDB.h"

#ifndef WIN32

BOOST_FIXTURE_TEST_SUITE(blocksdb_mapfile_tests, TestingSetup)

static void writeToFiles(int nFiles, size_t n2w, size_t atPos) {
    for (int i = 0; i < nFiles; ++i) {
        CDiskBlockPos pos(i,atPos);
        FILE *f = Blocks::openFile(pos, false);
        size_t nw = 0;
        char buf[1024];
        memset(buf,char(i%128),sizeof(buf));
        while (nw < n2w) {
            const size_t bytes = std::min(n2w-nw,sizeof(buf));
            fwrite(buf, bytes, 1, f);
            nw += sizeof(buf);
        }
        fclose(f);
    }
}

BOOST_AUTO_TEST_CASE(mapFile_extendFile_test)
{
    Blocks::DBPrivate pvt, pvt2;

    // first create dummy block files
    const int nFiles = 100;
    const double szMB = 0.128;
    const size_t szBytes = szMB*1024*1024;
    LogPrintf("Creating %d dummy blk files, each %fMB in size...\n", nFiles, szMB);

    writeToFiles(nFiles, szBytes, 0);

    std::vector<std::pair<std::shared_ptr<char>, size_t> > bufs;
    bufs.reserve(nFiles);

    for (int i = 0; i < nFiles; ++i) {
        size_t filesz, filesz2;
        auto buf = pvt.mapFile(i, Blocks::DB::ForwardBlock, &filesz);
        //LogPrintf("%d got size %d\n", i, int(filesz));
        auto buf2 = pvt.mapFile(i, Blocks::DB::ForwardBlock, &filesz2);
        //LogPrintf("%d (second) got size %d\n", i, int(filesz2));
        BOOST_CHECK(filesz == filesz2);
        BOOST_CHECK(buf.get() != nullptr && buf.get()[filesz-1] == char(i%128));

        // keep buffers around to prevent mmap's from going away, heh ;)
        bufs.emplace_back(std::make_pair(buf, filesz));
    }

    LogPrintf("Extending %d dummy blk files by %fMB each...\n", nFiles, szMB);
    writeToFiles(nFiles, szBytes, szBytes);
    const size_t expected_size = 2*szBytes;

    for (int i = 1; i < nFiles; ++i) {
        size_t filesz, filesz_old;
        auto buf = pvt2.mapFile(i, Blocks::DB::ForwardBlock, &filesz);
        auto buf_old = pvt.mapFile(i, Blocks::DB::ForwardBlock, &filesz_old);
        //LogPrintf("%d got size %d expected %d, old=%d\n", i, int(filesz), int(expected_size), int(filesz_old));
        BOOST_CHECK(filesz == expected_size);
        BOOST_CHECK(filesz_old < filesz);
        BOOST_CHECK(buf.get() != nullptr && buf.get()[filesz-1] == char(i%128));
        pvt.fileHasGrown(i); // notify -- will make following tests pass..
    }

    for (int i = 1; i < nFiles; ++i) {
        size_t filesz;
        auto buf = pvt.mapFile(i, Blocks::DB::ForwardBlock, &filesz); // should pickup *NEW* size now

        //LogPrintf("%d got size %d expected %d, old=%d\n", i, int(filesz), int(expected_size), int(filesz_old));
        BOOST_CHECK(filesz == expected_size);
        LogPrintf("%d extant buf (pointing to old size) = %p -- new buf (pointing to new size) = %p\n",
                  i,
                  reinterpret_cast<void *>(bufs[i].first.get()),
                  reinterpret_cast<void *>(buf.get()));
        BOOST_CHECK(buf.get() != nullptr && buf.get()[filesz-1] == char(i%128));
    }

}

BOOST_AUTO_TEST_SUITE_END()
#endif
