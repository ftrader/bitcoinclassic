// Copyright (c) 2016 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_TRANSACTION_UTILS_H
#define BITCOIN_TEST_TRANSACTION_UTILS_H

#include <vector>
#include <string>

class CScript;
struct CMutableTransaction;
class CTransaction;

namespace TxUtils {
    void RandomScript(CScript &script);
    void RandomInScript(CScript &script);

    enum RandomTransactionType {
        SingleOutput,
        AnyOutputCount
    };

    void RandomTransaction(CMutableTransaction &tx, RandomTransactionType single);

    void allowNewTransactions();
    void disallowNewTransactions();

    // create one transaction and repeat it until it fills up the space.
    std::vector<CTransaction> transactionsForBlock(int minSize);

    std::string FormatScript(const CScript& script);
}

#endif
