// Copyright (c) 2017 Stephen McCarthy
// Copyright (c) 2017 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "allowed_args.h"

#include "chainparams.h"
#include "httpserver.h"
#include "init.h"
#include "main.h"
#include "miner.h"
#include "net.h"
#include "policy/policy.h"
#include "script/sigcache.h"
#include "tinyformat.h"
#include "torcontrol.h"
#include "BlocksDB.h"
#include "qt/guiconstants.h"
#include "wallet/wallet.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"

#include <set>

namespace AllowedArgs {

enum HelpMessageMode {
    HMM_BITCOIND,
    HMM_BITCOIN_QT
};

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message)
{
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message)
{
    return std::string(optIndent, ' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent, ' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

AllowedArgs& AllowedArgs::addHeader(const std::string& strHeader, bool debug)
{

    m_helpList.push_back(HelpComponent{strHeader + "\n\n", debug});
    return *this;
}

AllowedArgs& AllowedArgs::addDebugArg(const std::string& strArgsDefinition, CheckValueFunc checkValueFunc, const std::string& strHelp)
{
    return addArg(strArgsDefinition, checkValueFunc, strHelp, true);
}

AllowedArgs& AllowedArgs::addArg(const std::string& strArgsDefinition, CheckValueFunc checkValueFunc, const std::string& strHelp, bool debug)
{
    std::string strArgs = strArgsDefinition;
    std::string strExampleValue;
    size_t is_index = strArgsDefinition.find('=');
    if (is_index != std::string::npos) {
        strExampleValue = strArgsDefinition.substr(is_index + 1);
        strArgs = strArgsDefinition.substr(0, is_index);
    }

    if (strArgs == "")
        strArgs = ",";

    std::stringstream streamArgs(strArgs);
    std::string strArg;
    bool firstArg = true;
    while (std::getline(streamArgs, strArg, ',')) {
        m_args[strArg] = checkValueFunc;

        std::string optionText = std::string(optIndent, ' ') + "-" + strArg;
        if (!strExampleValue.empty())
            optionText += "=" + strExampleValue;
        optionText += "\n";
        m_helpList.push_back(HelpComponent{optionText, debug || !firstArg});

        firstArg = false;
    }

    std::string helpText = std::string(msgIndent, ' ') + FormatParagraph(strHelp, screenWidth - msgIndent, msgIndent) + "\n\n";
    m_helpList.push_back(HelpComponent{helpText, debug});

    return *this;
}

void AllowedArgs::checkArg(const std::string& strArg, const std::string& strValue) const
{
    if (!m_args.count(strArg))
        throw std::runtime_error(strprintf(_("unrecognized option '%s'"), strArg));

    if (!m_args.at(strArg)(strValue))
        throw std::runtime_error(strprintf(_("invalid value '%s' for option '%s'"), strValue, strArg));
}

std::string AllowedArgs::helpMessage() const
{
    const bool showDebug = GetBoolArg("-help-debug", false);
    std::string helpMessage;

    for (HelpComponent helpComponent : m_helpList)
        if (showDebug || !helpComponent.debug)
            helpMessage += helpComponent.text;

    return helpMessage;
}

//////////////////////////////////////////////////////////////////////////////
//
// CheckValueFunc functions
//

static const std::set<std::string> boolStrings{"", "1", "0", "t", "f", "y", "n", "true", "false", "yes", "no"};
static const std::set<char> intChars{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const std::set<char> amountChars{'.', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

static bool validateString(const std::string& str, const std::set<char>& validChars)
{
    for (const char& c : str)
        if (!validChars.count(c))
            return false;
    return true;
}

static bool optionalBool(const std::string& str)
{
    return (boolStrings.count(str) != 0);
}

static bool requiredStr(const std::string& str)
{
    return !str.empty();
}

static bool optionalStr(const std::string& str)
{
    return true;
}

static bool requiredInt(const std::string& str)
{
    if (str.empty() || str == "-")
        return false;

    // Allow the first character to be '-', to allow negative numbers.
    return validateString(str[0] == '-' ? str.substr(1) : str, intChars);
}

static bool optionalInt(const std::string& str)
{
    if (str.empty())
        return true;
    return requiredInt(str);
}

static bool requiredAmount(const std::string& str)
{
    if (str.empty())
        return false;
    return validateString(str, amountChars);
}

//////////////////////////////////////////////////////////////////////////////
//
// Argument definitions
//

// When adding new arguments to a category, please keep alphabetical ordering,
// where appropriate. Do not translate _(...) addDebugArg help text: there are
// many technical terms, and only a very small audience, so it would be an
// unnecessary stress to translators.

static void addHelpOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Help options:"))
        .addArg("?,h,help", optionalBool, _("This help message"))
        .addArg("version", optionalBool, _("Print version and exit"))
        .addArg("help-debug", optionalBool, _("Show all debugging options (usage: --help -help-debug)"))
        ;
}

static void addChainSelectionOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Chain selection options:"))
        .addArg("testnet-ft", optionalBool, _("Use the flexible-transactions testnet"))
        .addArg("testnet", optionalBool, _("Use the test chain"))
        .addDebugArg("regtest", optionalBool,
            "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
            "This is intended for regression testing tools and app development.")
        ;
}

static void addConfigurationLocationOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Configuration location options:"))
        .addArg("conf=<file>", requiredStr, strprintf(_("Specify configuration file (default: %s)"), BITCOIN_CONF_FILENAME))
        .addArg("datadir=<dir>", requiredStr, _("Specify data directory"))
        ;
}

static void addGeneralOptions(AllowedArgs& allowedArgs, HelpMessageMode mode)
{
    allowedArgs
        .addHeader(_("General options:"))
        .addArg("alertnotify=<cmd>", requiredStr, _("Execute command when a relevant alert is received or we see a really long fork (%s in cmd is replaced by message)"))
        .addArg("blocknotify=<cmd>", requiredStr, _("Execute command when the best block changes (%s in cmd is replaced by block hash)"))
        .addDebugArg("blocksonly", optionalBool, strprintf(_("Whether to operate in a blocks only mode (default: %u)"), DEFAULT_BLOCKSONLY))
        .addArg("checkblocks=<n>", requiredInt, strprintf(_("How many blocks to check at startup (default: %u, 0 = all)"), DEFAULT_CHECKBLOCKS))
        .addArg("checklevel=<n>", requiredInt, strprintf(_("How thorough the block verification of -checkblocks is (0-4, default: %u)"), DEFAULT_CHECKLEVEL))
        ;

#ifndef WIN32
    if (mode == HMM_BITCOIND)
        allowedArgs.addArg("daemon", optionalBool, _("Run in the background as a daemon and accept commands"));
#endif

    allowedArgs
        .addArg("dbcache=<n>", requiredInt, strprintf(_("Set database cache size in megabytes (%d to %d, default: %d)"), nMinDbCache, nMaxDbCache, nDefaultDbCache))
        .addArg("loadblock=<file>", requiredStr, _("Imports blocks from external blk000??.dat file on startup"))
        .addArg("maxorphantx=<n>", requiredInt, strprintf(_("Keep at most <n> unconnectable transactions in memory (default: %u)"), DEFAULT_MAX_ORPHAN_TRANSACTIONS))
        .addArg("maxmempool=<n>", requiredInt, strprintf(_("Keep the transaction memory pool below <n> megabytes (default: %u)"), DEFAULT_MAX_MEMPOOL_SIZE))
        .addArg("mempoolexpiry=<n>", requiredInt, strprintf(_("Do not keep transactions in the mempool longer than <n> hours (default: %u)"), DEFAULT_MEMPOOL_EXPIRY))
        .addArg("par=<n>", requiredInt, strprintf(_("Set the number of script verification threads (%u to %d, 0 = auto, <0 = leave that many cores free, default: %d)"),
            -GetNumCores(), MAX_SCRIPTCHECK_THREADS, DEFAULT_SCRIPTCHECK_THREADS))
#ifndef WIN32
        .addArg("pid=<file>", requiredStr, strprintf(_("Specify pid file (default: %s)"), BITCOIN_PID_FILENAME))
#endif
        .addArg("prune=<n>", requiredInt, strprintf(_("Reduce storage requirements by pruning (deleting) old blocks. This mode is incompatible with -txindex and -rescan. "
                "Warning: Reverting this setting requires re-downloading the entire blockchain. "
                "(default: 0 = disable pruning blocks, >%u = target size in MiB to use for block files)"), MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024))
        .addArg("reindex", optionalBool, _("Rebuild block chain index from current blk000??.dat files on startup"))
        .addArg("txindex", optionalBool, strprintf(_("Maintain a full transaction index, used by the getrawtransaction rpc call (default: %u)"), DEFAULT_TXINDEX))
        .addArg("uahfstarttime", requiredInt, "BCC (UAHF) chain start time, in seconds since epoch")
        ;
}

static void addConnectionOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Connection options:"))
        .addArg("addnode=<ip>", requiredStr, _("Add a node to connect to and attempt to keep the connection open"))
        .addArg("banscore=<n>", requiredInt, strprintf(_("Threshold for disconnecting misbehaving peers (default: %u)"), DEFAULT_BANSCORE_THRESHOLD))
        .addArg("bantime=<n>", requiredInt, strprintf(_("Number of seconds to keep misbehaving peers from reconnecting (default: %u)"), DEFAULT_MISBEHAVING_BANTIME))
        .addArg("bind=<addr>", requiredStr, _("Bind to given address and always listen on it. Use [host]:port notation for IPv6"))
        .addArg("connect=<ip>", optionalStr, _("Connect only to the specified node(s)"))
        .addArg("discover", optionalBool, _("Discover own IP addresses (default: 1 when listening and no -externalip or -proxy)"))
        .addArg("dns", optionalBool, _("Allow DNS lookups for -addnode, -seednode and -connect") + " " + strprintf(_("(default: %u)"), DEFAULT_NAME_LOOKUP))
        .addArg("dnsseed", optionalBool, _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect)"))
        .addArg("externalip=<ip>", requiredStr, _("Specify your own public address"))
        .addArg("forcednsseed", optionalBool, strprintf(_("Always query for peer addresses via DNS lookup (default: %u)"), DEFAULT_FORCEDNSSEED))
        .addArg("listen", optionalBool, _("Accept connections from outside (default: 1 if no -proxy or -connect)"))
        .addArg("listenonion", optionalBool, strprintf(_("Automatically create Tor hidden service (default: %d)"), DEFAULT_LISTEN_ONION))
        .addArg("maxconnections=<n>", optionalInt, strprintf(_("Maintain at most <n> connections to peers (default: %u)"), DEFAULT_MAX_PEER_CONNECTIONS))
        .addArg("min-thin-peers=<n>", requiredInt, strprintf(_("Maintain at minimum <n> connections to thin-capable peers (default: %d)"), DEFAULT_MIN_THIN_PEERS))
        .addArg("maxreceivebuffer=<n>", requiredInt, strprintf(_("Maximum per-connection receive buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXRECEIVEBUFFER))
        .addArg("maxsendbuffer=<n>", requiredInt, strprintf(_("Maximum per-connection send buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXSENDBUFFER))
        .addArg("onion=<ip:port>", requiredStr, strprintf(_("Use separate SOCKS5 proxy to reach peers via Tor hidden services (default: %s)"), "-proxy"))
        .addArg("onlynet=<net>", requiredStr, _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)"))
        .addArg("permitbaremultisig", optionalBool, strprintf(_("Relay non-P2SH multisig (default: %u)"), DEFAULT_PERMIT_BAREMULTISIG))
        .addArg("peerbloomfilters", optionalBool, strprintf(_("Support filtering of blocks and transaction with bloom filters (default: %u)"), 1))
        .addDebugArg("enforcenodebloom", optionalBool, strprintf("Enforce minimum protocol version to limit use of bloom filters (default: %u)", 0))
        .addArg("port=<port>", requiredInt, strprintf(_("Listen for connections on <port> (default: %u or testnet: %u)"), DEFAULT_MAINNET_PORT, DEFAULT_TESTNET_PORT))
        .addArg("proxy=<ip:port>", requiredStr, _("Connect through SOCKS5 proxy"))
        .addArg("proxyrandomize", optionalBool, strprintf(_("Randomize credentials for every proxy connection. This enables Tor stream isolation (default: %u)"), DEFAULT_PROXYRANDOMIZE))
        .addArg("seednode=<ip>", requiredStr, _("Connect to a node to retrieve peer addresses, and disconnect"))
        .addArg("timeout=<n>", requiredInt, strprintf(_("Specify connection timeout in milliseconds (minimum: 1, default: %d)"), DEFAULT_CONNECT_TIMEOUT))
        .addArg("torcontrol=<ip>:<port>", requiredStr, strprintf(_("Tor control port to use if onion listening enabled (default: %s)"), DEFAULT_TOR_CONTROL))
        .addArg("torpassword=<pass>", requiredStr, _("Tor control port password (default: empty)"))
#ifdef USE_UPNP
#if USE_UPNP
        .addArg("upnp", optionalBool, _("Use UPnP to map the listening port (default: 1 when listening and no -proxy)"))
#else
        .addArg("upnp", optionalBool, _("Use UPnP to map the listening port (default: 0)"))
#endif
#endif
        .addArg("whitebind=<addr>", requiredStr, _("Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6"))
        .addArg("whitelist=<netmask>", requiredStr, _("Whitelist peers connecting from the given netmask or IP address. Can be specified multiple times.") +
            " " + _("Whitelisted peers cannot be DoS banned and their transactions are always relayed, even if they are already in the mempool, useful e.g. for a gateway"))
        .addArg("whitelistrelay", optionalBool, strprintf(_("Accept relayed transactions received from whitelisted peers even when not relaying transactions (default: %d)"), DEFAULT_WHITELISTRELAY))
        .addArg("whitelistforcerelay", optionalBool, strprintf(_("Force relay of transactions from whitelisted peers even they violate local relay policy (default: %d)"), DEFAULT_WHITELISTFORCERELAY))
        .addArg("maxuploadtarget=<n>", requiredInt, strprintf(_("Tries to keep outbound traffic under the given target (in MiB per 24h), 0 = no limit (default: %d)"), DEFAULT_MAX_UPLOAD_TARGET))
        ;
}

static void addWalletOptions(AllowedArgs& allowedArgs)
{
#ifdef ENABLE_WALLET
    allowedArgs
        .addHeader(_("Wallet options:"))
        .addArg("disablewallet", optionalBool, _("Do not load the wallet and disable wallet RPC calls"))
        .addArg("keypool=<n>", requiredInt, strprintf(_("Set key pool size to <n> (default: %u)"), DEFAULT_KEYPOOL_SIZE))
        .addArg("fallbackfee=<amt>", requiredAmount, strprintf(_("A fee rate (in %s/kB) that will be used when fee estimation has insufficient data (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_FALLBACK_FEE)))
        .addArg("mintxfee=<amt>", requiredAmount, strprintf(_("Fees (in %s/kB) smaller than this are considered zero fee for transaction creation (default: %s)"),
                CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MINFEE)))
        .addArg("paytxfee=<amt>", requiredAmount, strprintf(_("Fee (in %s/kB) to add to transactions you send (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_FEE)))
        .addArg("rescan", optionalBool, _("Rescan the block chain for missing wallet transactions on startup"))
        .addArg("salvagewallet", optionalBool, _("Attempt to recover private keys from a corrupt wallet.dat on startup"))
        .addArg("sendfreetransactions", optionalBool, strprintf(_("Send transactions as zero-fee transactions if possible (default: %u)"), DEFAULT_SEND_FREE_TRANSACTIONS))
        .addArg("spendzeroconfchange", optionalBool, strprintf(_("Spend unconfirmed change when sending transactions (default: %u)"), DEFAULT_SPEND_ZEROCONF_CHANGE))
        .addArg("txconfirmtarget=<n>", requiredInt, strprintf(_("If paytxfee is not set, include enough fee so transactions begin confirmation on average within n blocks (default: %u)"), DEFAULT_TX_CONFIRM_TARGET))
        .addArg("maxtxfee=<amt>", requiredAmount, strprintf(_("Maximum total fees (in %s) to use in a single wallet transaction; setting this too low may abort large transactions (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MAXFEE)))
        .addArg("upgradewallet", optionalInt, _("Upgrade wallet to latest format on startup"))
        .addArg("wallet=<file>", requiredStr, _("Specify wallet file (within data directory)") + " " + strprintf(_("(default: %s)"), "wallet.dat"))
        .addArg("walletbroadcast", optionalBool, _("Make the wallet broadcast transactions") + " " + strprintf(_("(default: %u)"), DEFAULT_WALLETBROADCAST))
        .addArg("walletnotify=<cmd>", requiredStr, _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)"))
        .addArg("zapwallettxes=<mode>", optionalInt, _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") +
            " " + _("(1 = keep tx meta data e.g. account owner and payment request information, 2 = drop tx meta data)"))
        ;
#endif
}

static void addZmqOptions(AllowedArgs& allowedArgs)
{
#if ENABLE_ZMQ
    allowedArgs
        .addHeader(_("ZeroMQ notification options:"))
        .addArg("zmqpubhashblock=<address>", requiredStr, _("Enable publish hash block in <address>"))
        .addArg("zmqpubhashtx=<address>", requiredStr, _("Enable publish hash transaction in <address>"))
        .addArg("zmqpubrawblock=<address>", requiredStr, _("Enable publish raw block in <address>"))
        .addArg("zmqpubrawtx=<address>", requiredStr, _("Enable publish raw transaction in <address>"))
        ;
#endif
}

static void addDebuggingOptions(AllowedArgs& allowedArgs, HelpMessageMode mode)
{
    std::string debugCategories = "addrman, bench, coindb, db, lock, rand, rpc, selectcoins, mempool, mempoolrej, net, proxy, prune, http, libevent, tor, zmq, thin, NWM";
    if (mode == HMM_BITCOIN_QT)
        debugCategories += ", qt";

    allowedArgs
        .addHeader(_("Debugging/Testing options:"))
        .addArg("uacomment=<cmt>", requiredStr, _("Append comment to the user agent string"))
        .addDebugArg("checkblockindex", optionalBool, strprintf("Do a full consistency check for mapBlockIndex, setBlockIndexCandidates, chainActive and mapBlocksUnlinked occasionally (default: %u)", false))
        .addDebugArg("checkmempool=<n>", requiredInt, strprintf("Run checks every <n> transactions (default: %u)", 0))
        .addDebugArg("checkpoints", optionalBool, strprintf("Disable expensive verification for known chain history (default: %u)", DEFAULT_CHECKPOINTS_ENABLED))
#ifdef ENABLE_WALLET
        .addDebugArg("dblogsize=<n>", requiredInt, strprintf("Flush wallet database activity from memory to disk log every <n> megabytes (default: %u)", DEFAULT_WALLET_DBLOGSIZE))
#endif
        .addDebugArg("disablesafemode", optionalBool, strprintf("Disable safemode, override a real safe mode event (default: %u)", DEFAULT_DISABLE_SAFEMODE))
        .addDebugArg("testsafemode", optionalBool, strprintf("Force safe mode (default: %u)", DEFAULT_TESTSAFEMODE))
        .addDebugArg("dropmessagestest=<n>", requiredInt, "Randomly drop 1 of every <n> network messages")
        .addDebugArg("fuzzmessagestest=<n>", requiredInt, "Randomly fuzz 1 of every <n> network messages")
#ifdef ENABLE_WALLET
        .addDebugArg("flushwallet", optionalBool, strprintf("Run a thread to flush wallet periodically (default: %u)", DEFAULT_FLUSHWALLET))
#endif
        .addDebugArg("stopafterblockimport", optionalBool, strprintf("Stop running after importing blocks from disk (default: %u)", DEFAULT_STOPAFTERBLOCKIMPORT))
        .addDebugArg("limitancestorcount=<n>", requiredInt, strprintf("Do not accept transactions if number of in-mempool ancestors is <n> or more (default: %u)", DEFAULT_ANCESTOR_LIMIT))
        .addDebugArg("limitancestorsize=<n>", requiredInt, strprintf("Do not accept transactions whose size with all in-mempool ancestors exceeds <n> kilobytes (default: %u)", DEFAULT_ANCESTOR_SIZE_LIMIT))
        .addDebugArg("limitdescendantcount=<n>", requiredInt, strprintf("Do not accept transactions if any ancestor would have <n> or more in-mempool descendants (default: %u)", DEFAULT_DESCENDANT_LIMIT))
        .addDebugArg("limitdescendantsize=<n>", requiredInt, strprintf("Do not accept transactions if any ancestor would have more than <n> kilobytes of in-mempool descendants (default: %u).", DEFAULT_DESCENDANT_SIZE_LIMIT))
        .addArg("debug=<category>", optionalStr, strprintf(_("Output debugging information (default: %u, supplying <category> is optional)"), 0) + ". " +
            _("If <category> is not supplied or if <category> = 1, output all debugging information.") + _("<category> can be:") + " " + debugCategories + ".")
        .addArg("flextrans", optionalBool, "Accept and relay transactions of version 4")
        .addArg("ft-strict", optionalBool, "On incoming FlexTrans transactions reject tx that have not specified tokens. default: false")
        .addArg("gen", optionalBool,  strprintf(_("Generate coins (default: %u)"), DEFAULT_GENERATE))
        .addArg("genproclimit=<n>", requiredInt, strprintf(_("Set the number of threads for coin generation if enabled (-1 = all cores, default: %d)"), DEFAULT_GENERATE_THREADS))
        .addArg("gencoinbase=<pubkey>", requiredStr, "When generating coins a coinbase has to be provided in the form of a public key")
        .addArg("logips", optionalBool, strprintf(_("Include IP addresses in debug output (default: %u)"), DEFAULT_LOGIPS))
        .addArg("logtimestamps", optionalBool, strprintf(_("Prepend debug output with timestamp (default: %u)"), DEFAULT_LOGTIMESTAMPS))
        .addDebugArg("mocktime=<n>", requiredInt, "Replace actual time with <n> seconds since epoch (default: 0)")
        .addDebugArg("limitfreerelay=<n>", optionalInt, strprintf("Continuously rate-limit free transactions to <n>*1000 bytes per minute (default: %u)", DEFAULT_LIMITFREERELAY))
        .addDebugArg("relaypriority", optionalBool, strprintf("Require high priority for relaying free or low-fee transactions (default: %u)", DEFAULT_RELAYPRIORITY))
        .addDebugArg("maxsigcachesize=<n>", requiredInt, strprintf("Limit size of signature cache to <n> MiB (default: %u)", DEFAULT_MAX_SIG_CACHE_SIZE))
        .addArg("printtoconsole", optionalBool, _("Send trace/debug info to console instead of debug.log file"))
        .addDebugArg("printpriority", optionalBool, strprintf("Log transaction priority and fee per kB when mining blocks (default: %u)", DEFAULT_PRINTPRIORITY))
#ifdef ENABLE_WALLET
        .addDebugArg("privdb", optionalBool, strprintf("Sets the DB_PRIVATE flag in the wallet db environment (default: %u)", DEFAULT_WALLET_PRIVDB))
#endif
        .addArg("shrinkdebugfile", optionalBool, _("Shrink debug.log file on client startup (default: 1 when no -debug)"))
        ;
}

static void addNodeRelayOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Node relay options:"))
        .addDebugArg("acceptnonstdtxn", optionalBool, strprintf("Relay and mine \"non-standard\" transactions (%sdefault: %u)", "testnet/regtest only; ", true))
        .addArg("blocksizeacceptlimit=<n>", requiredAmount, strprintf("This node will not accept blocks larger than this limit. Unit is in MB (default: %.1f)", DEFAULT_BLOCK_ACCEPT_SIZE / 1e6))
        .addDebugArg("blocksizeacceptlimitbytes,excessiveblocksize=<n>", requiredInt, strprintf("This node will not accept blocks larger than this limit. Unit is in bytes. Superseded by -blocksizeacceptlimit (default: %u)", DEFAULT_BLOCK_ACCEPT_SIZE))
        .addArg("bytespersigop=<n>", requiredInt, strprintf(_("Minimum bytes per sigop in transactions we relay and mine (default: %u)"), DEFAULT_BYTES_PER_SIGOP))
        .addArg("datacarrier", optionalBool, strprintf(_("Relay and mine data carrier transactions (default: %u)"), DEFAULT_ACCEPT_DATACARRIER))
        .addArg("datacarriersize=<n>", requiredInt, strprintf(_("Maximum size of data in data carrier transactions we relay and mine (default: %u)"), MAX_OP_RETURN_RELAY))
        .addArg("expeditedblock=<host>", requiredStr, _("Request expedited blocks from this host whenever we are connected to it"))
        .addArg("maxexpeditedblockrecipients=<n>", requiredInt, _("The maximum number of nodes this node will forward expedited blocks to"))
        .addArg("maxexpeditedtxrecipients=<n>", requiredInt, _("The maximum number of nodes this node will forward expedited transactions to"))
        .addArg("minrelaytxfee=<amt>", requiredAmount, strprintf(_("Fees (in %s/kB) smaller than this are considered zero fee for relaying, mining and transaction creation (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_MIN_RELAY_TX_FEE)))
        .addArg("use-thinblocks", optionalBool, _("Enable thin blocks to speed up the relay of blocks (default: 1)"))
        ;
}

static void addBlockCreationOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("Block creation options:"))
        .addArg("blockminsize=<n>", requiredInt, strprintf(_("Set minimum block size in bytes (default: %u)"), DEFAULT_BLOCK_MIN_SIZE))
        .addArg("blockmaxsize=<n>", requiredInt, strprintf("Set maximum block size in bytes (default: %d)", DEFAULT_BLOCK_MAX_SIZE))
        .addArg("blockprioritysize=<n>", requiredInt, strprintf(_("Set maximum size of high-priority/low-fee transactions in bytes (default: %d)"), DEFAULT_BLOCK_PRIORITY_SIZE))
        .addDebugArg("blockversion=<n>", requiredInt, "Override block version to test forking scenarios")
        ;
}

static void addRpcServerOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("RPC server options:"))
        .addArg("server", optionalBool, _("Accept command line and JSON-RPC commands"))
        .addArg("rest", optionalBool, strprintf(_("Accept public REST requests (default: %u)"), DEFAULT_REST_ENABLE))
        .addArg("rpcbind=<addr>", requiredStr, _("Bind to given address to listen for JSON-RPC connections. Use [host]:port notation for IPv6. This option can be specified multiple times (default: bind to all interfaces)"))
        .addArg("rpccookiefile=<loc>", requiredStr, _("Location of the auth cookie (default: data dir)"))
        .addArg("rpcuser=<user>", requiredStr, _("Username for JSON-RPC connections"))
        .addArg("rpcpassword=<pw>", requiredStr, _("Password for JSON-RPC connections"))
        .addArg("rpcauth=<userpw>", requiredStr, _("Username and hashed password for JSON-RPC connections. The field <userpw> comes in the format: <USERNAME>:<SALT>$<HASH>. A canonical python script is included in share/rpcuser. This option can be specified multiple times"))
        .addArg("rpcport=<port>", requiredInt, strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or testnet: %u)"), BaseParams(CBaseChainParams::MAIN).RPCPort(), BaseParams(CBaseChainParams::TESTNET).RPCPort()))
        .addArg("rpcallowip=<ip>", requiredStr, _("Allow JSON-RPC connections from specified source. Valid for <ip> are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This option can be specified multiple times"))
        .addArg("rpcthreads=<n>", requiredInt, strprintf(_("Set the number of threads to service RPC calls (default: %d)"), DEFAULT_HTTP_THREADS))
        .addDebugArg("rpcworkqueue=<n>", requiredInt, strprintf("Set the depth of the work queue to service RPC calls (default: %d)", DEFAULT_HTTP_WORKQUEUE))
        .addDebugArg("rpcservertimeout=<n>", requiredInt, strprintf("Timeout during HTTP requests (default: %d)", DEFAULT_HTTP_SERVER_TIMEOUT))
        ;
}

static void addAdminServerOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader("Admin server options: (Experimental!)")
        .addArg("adminserver", optionalBool, "Accept connections on the admin-server (default 0)")
        .addArg("admincookiefile=<loc>", requiredStr, "Location of the adminserver auth cookie (default: data dir)")
        .addArg("adminlisten=<addr>", requiredStr, strprintf("Bind to given address to listen for admin server connections. Use [host]:port notation for IPv6. This option can be specified multiple times (default 127.0.0.1:%s and [::1]:%s)", BaseParams(CBaseChainParams::MAIN).AdminServerPort(), BaseParams(CBaseChainParams::MAIN).AdminServerPort()));
        ;
}

static void addUiOptions(AllowedArgs& allowedArgs)
{
    allowedArgs
        .addHeader(_("UI Options:"))
        .addDebugArg("allowselfsignedrootcertificates", optionalBool, strprintf("Allow self signed root certificates (default: %u)", DEFAULT_SELFSIGNED_ROOTCERTS))
        .addArg("choosedatadir", optionalBool, strprintf(_("Choose data directory on startup (default: %u)"), DEFAULT_CHOOSE_DATADIR))
        .addArg("lang=<lang>", requiredStr, _("Set language, for example \"de_DE\" (default: system locale)"))
        .addArg("min", optionalBool, _("Start minimized"))
        .addArg("rootcertificates=<file>", optionalStr, _("Set SSL root certificates for payment request (default: -system-)"))
        .addArg("splash", optionalBool, strprintf(_("Show splash screen on startup (default: %u)"), DEFAULT_SPLASHSCREEN))
        .addArg("resetguisettings", optionalBool, _("Reset all settings changes made over the GUI"))
        .addDebugArg("uiplatform=<platform>", requiredStr, strprintf("Select platform to customize UI for (one of windows, macosx, other; default: %s)", DEFAULT_UIPLATFORM))
        ;
}

static void addAllNodeOptions(AllowedArgs& allowedArgs, HelpMessageMode mode)
{
    addHelpOptions(allowedArgs);
    addConfigurationLocationOptions(allowedArgs);
    addGeneralOptions(allowedArgs, mode);
    addConnectionOptions(allowedArgs);
    addWalletOptions(allowedArgs);
    addZmqOptions(allowedArgs);
    addDebuggingOptions(allowedArgs, mode);
    addChainSelectionOptions(allowedArgs);
    addNodeRelayOptions(allowedArgs);
    addBlockCreationOptions(allowedArgs);
    addRpcServerOptions(allowedArgs);
    addAdminServerOptions(allowedArgs);
    if (mode == HMM_BITCOIN_QT)
        addUiOptions(allowedArgs);
}

BitcoinCli::BitcoinCli()
{
    addHelpOptions(*this);
    addChainSelectionOptions(*this);
    addConfigurationLocationOptions(*this);

    addHeader(_("RPC client options:"))
        .addArg("rpcconnect=<ip>", requiredStr, strprintf(_("Send commands to node running on <ip> (default: %s)"), DEFAULT_RPCCONNECT))
        .addArg("rpcport=<port>", requiredInt, strprintf(_("Connect to JSON-RPC on <port> (default: %u or testnet: %u)"), BaseParams(CBaseChainParams::MAIN).RPCPort(), BaseParams(CBaseChainParams::TESTNET).RPCPort()))
        .addArg("rpcwait", optionalBool, _("Wait for RPC server to start"))
        .addArg("rpcuser=<user>", requiredStr, _("Username for JSON-RPC connections"))
        .addArg("rpcpassword=<pw>", requiredStr, _("Password for JSON-RPC connections"))
        .addArg("rpcclienttimeout=<n>", requiredInt, strprintf(_("Timeout during HTTP requests (default: %d)"), DEFAULT_HTTP_CLIENT_TIMEOUT))
        ;
}

Bitcoind::Bitcoind()
{
    addAllNodeOptions(*this, HMM_BITCOIND);
}

BitcoinQt::BitcoinQt()
{
    addAllNodeOptions(*this, HMM_BITCOIN_QT);
}

BitcoinTx::BitcoinTx()
{
    addHelpOptions(*this);
    addChainSelectionOptions(*this);

    addHeader(_("Transaction options:"))
        .addArg("create", optionalBool, _("Create new, empty TX."))
        .addArg("json", optionalBool, _("Select JSON output"))
        .addArg("txid", optionalBool, _("Output only the hex-encoded transaction id of the resultant transaction."))
        .addDebugArg("", optionalBool, "Read hex-encoded bitcoin transaction from stdin.")
        ;
}

ConfigFile::ConfigFile()
{
    // Merges all allowed args from BitcoinCli, Bitcoind, and BitcoinQt.
    // Excludes args from BitcoinTx, because bitcoin-tx does not read
    // from the config file. Does not set a help message, because the
    // program does not output a config file help message anywhere.

    BitcoinCli bitcoinCli;
    Bitcoind bitcoind;
    BitcoinQt bitcoinQt;

    m_args.insert(bitcoinCli.getArgs().begin(), bitcoinCli.getArgs().end());
    m_args.insert(bitcoind.getArgs().begin(), bitcoind.getArgs().end());
    m_args.insert(bitcoinQt.getArgs().begin(), bitcoinQt.getArgs().end());
}

} // namespace AllowedArgs
