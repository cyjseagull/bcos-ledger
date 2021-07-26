/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @file Ledger.cpp
 * @author: kyonRay
 * @date 2021-04-13
 */

#include "Ledger.h"
#include "utilities/BlockUtilities.h"
#include <bcos-framework/interfaces/protocol/CommonError.h>
#include <bcos-framework/libprotocol/ParallelMerkleProof.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>
#include <boost/lexical_cast.hpp>

using namespace bcos;
using namespace bcos::ledger;
using namespace bcos::protocol;
using namespace bcos::storage;
using namespace bcos::crypto;

void Ledger::asyncCommitBlock(bcos::protocol::BlockHeader::Ptr _header,
    std::function<void(Error::Ptr, LedgerConfig::Ptr)> _onCommitBlock)
{
    if (_header == nullptr)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncCommitBlock") << LOG_DESC("Header is nullptr");
        // TODO: add error code
        auto error = std::make_shared<Error>(-1, "[#asyncCommitBlock] Header is nullptr.");
        _onCommitBlock(error, nullptr);
        return;
    }

    // default parent block hash located in parentInfo[0]
    if (!isBlockShouldCommit(_header->number(), _header->parentInfo().at(0).blockHash.hex()))
    {
        // TODO: add error code
        auto error = std::make_shared<Error>(
            -1, "[#asyncCommitBlock] Wrong block number of wrong parent hash");
        _onCommitBlock(error, nullptr);
        return;
    }
    auto tableFactory = getMemoryTableFactory(_header->number());
    try
    {
        tbb::parallel_invoke(
            [this, _header, tableFactory]() { writeNumber(_header->number(), tableFactory); },
            [this, _header, tableFactory]() { writeHash2Number(_header, tableFactory); },
            [this, _header, tableFactory]() { writeNumber2BlockHeader(_header, tableFactory); });

        auto self = std::weak_ptr<Ledger>(std::dynamic_pointer_cast<Ledger>(shared_from_this()));
        tableFactory->asyncCommit([_header, _onCommitBlock, self](
                                      Error::Ptr _error, size_t _commitSize) {
            if ((_error && _error->errorCode() != CommonError::SUCCESS) || _commitSize < 1)
            {
                LEDGER_LOG(ERROR) << LOG_DESC("Commit Block failed in storage")
                                  << LOG_KV("number", _header->number());
                // TODO: add error code
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "[#asyncCommitBlock] Commit block error in storage" + _error->errorMessage());
                _onCommitBlock(error, nullptr);
                return;
            }
            auto ledger = self.lock();
            if (!ledger)
            {
                // TODO: add error code
                auto error = std::make_shared<Error>(-1, "");
                _onCommitBlock(error, nullptr);
                return;
            }
            auto blockNumber = _header->number();
            LEDGER_LOG(INFO) << LOG_BADGE("asyncCommitBlock") << LOG_DESC("commit block success")
                             << LOG_KV("blockNumber", blockNumber);
            {
                WriteGuard ll(ledger->m_blockNumberMutex);
                ledger->m_blockNumber = blockNumber;
            }
            ledger->m_blockHeaderCache.add(blockNumber, _header);
            ledger->notifyCommittedBlockNumber(blockNumber);
            ledger->asyncGetLedgerConfig(blockNumber, _header->hash(),
                [_header, _onCommitBlock](
                    Error::Ptr _error, WrapperLedgerConfig::Ptr _wrapperLedgerConfig) {
                    if (_error)
                    {
                        LEDGER_LOG(WARNING)
                            << LOG_DESC("asyncCommitBlock failed for asyncGetLedgerConfig failed")
                            << LOG_KV("number", _header->number())
                            << LOG_KV("hash", _header->hash().abridged());
                        _onCommitBlock(_error, nullptr);
                        return;
                    }
                    if (_wrapperLedgerConfig->sysConfigFetched() &&
                        _wrapperLedgerConfig->consensusConfigFetched())
                    {
                        LEDGER_LOG(INFO) << LOG_DESC("asyncCommitBlock: getLedgerConfig success")
                                         << LOG_KV("number", _header->number())
                                         << LOG_KV("hash", _header->hash().abridged());
                        _onCommitBlock(nullptr, _wrapperLedgerConfig->ledgerConfig());
                    }
                });
        });
    }
    catch (OpenSysTableFailed const& e)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncCommitBlock")
                          << LOG_DESC("System meets error when try to write block to storage")
                          << LOG_KV("EINFO", boost::diagnostic_information(e));
        raise(SIGTERM);
        BOOST_THROW_EXCEPTION(
            OpenSysTableFailed() << errinfo_comment(" write block to storage failed."));
    }
}

void Ledger::asyncStoreTransactions(std::shared_ptr<std::vector<bytesPointer>> _txToStore,
    crypto::HashListPtr _txHashList, std::function<void(Error::Ptr)> _onTxStored)
{
    if (!_txToStore || !_txHashList || _txHashList->size() != _txToStore->size())
    {
        // TODO: add error code
        auto error = std::make_shared<Error>(-1, "[#asyncStoreTransactions] error parameters");
        _onTxStored(error);
        return;
    }
    auto tableFactory = getMemoryTableFactory(-1);
    try
    {
        for (size_t i = 0; i < _txHashList->size(); ++i)
        {
            // TODO: remove the copy overhead
            auto txHashHex = _txHashList->at(i).hex();
            getStorageSetter()->setHashToTx(
                tableFactory, txHashHex, asString(*(_txToStore->at(i))));
            LEDGER_LOG(TRACE) << LOG_BADGE("setHashToTx") << LOG_DESC("write HASH_2_TX success")
                              << LOG_KV("txHashHex", txHashHex);
        }
        tableFactory->asyncCommit([_onTxStored](Error::Ptr _error, size_t _commitSize) {
            if (!_error || _error->errorCode() == CommonError::SUCCESS)
            {
                LEDGER_LOG(TRACE) << LOG_BADGE("asyncStoreTransactions")
                                  << LOG_DESC("write db success")
                                  << LOG_KV("commitSize", _commitSize);
                _onTxStored(nullptr);
            }
            else
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncStoreTransactions")
                                  << LOG_DESC("table commit failed");
                // TODO: add error code and msg
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "[#asyncStoreTransactions] table commit failed" + _error->errorMessage());
                _onTxStored(error);
            }
        });
    }
    catch (OpenSysTableFailed const& e)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncStoreTransactions")
                          << LOG_DESC("System meets error when try to write tx to storage")
                          << LOG_KV("EINFO", boost::diagnostic_information(e));
        raise(SIGTERM);
        BOOST_THROW_EXCEPTION(
            OpenSysTableFailed() << errinfo_comment(" write block to storage failed."));
    }
}

void Ledger::asyncStoreReceipts(storage::TableFactoryInterface::Ptr _tableFactory,
    protocol::Block::Ptr _block, std::function<void(Error::Ptr)> _onReceiptStored)
{
    if (_block == nullptr || _tableFactory == nullptr)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncStoreReceipts") << LOG_DESC("Error parameters");
        // TODO: add error code
        auto error = std::make_shared<Error>(-1, "block or tableFactory is null");
        _onReceiptStored(error);
        return;
    }
    auto blockNumber = _block->blockHeader()->number();
    try
    {
        tbb::parallel_invoke(
            [this, _block, _tableFactory]() { writeTotalTransactionCount(_block, _tableFactory); },
            [this, _block, _tableFactory]() { writeNumber2Nonces(_block, _tableFactory); },
            [this, _block, _tableFactory]() { writeNumber2Transactions(_block, _tableFactory); },
            [this, _block, _tableFactory]() { writeHash2Receipt(_block, _tableFactory); });

        auto self = std::weak_ptr<Ledger>(std::dynamic_pointer_cast<Ledger>(shared_from_this()));
        getStorage()->asyncAddStateCache(blockNumber, _tableFactory,
            [_block, _onReceiptStored, blockNumber, self](Error::Ptr _error) {
                auto ledger = self.lock();
                if (!_error || _error->errorCode() == CommonError::SUCCESS)
                {
                    ledger->m_transactionsCache.add(
                        blockNumber, blockTransactionListGetter(_block));
                    ledger->m_receiptCache.add(blockNumber, blockReceiptListGetter(_block));
                    _onReceiptStored(nullptr);
                }
                else
                {
                    LEDGER_LOG(ERROR)
                        << LOG_BADGE("asyncStoreReceipts") << LOG_DESC("add state cache failed")
                        << LOG_KV("errorCode", _error->errorCode())
                        << LOG_KV("errorMsg", _error->errorMessage())
                        << LOG_KV("number", blockNumber);
                    // TODO: add error code
                    auto error = std::make_shared<Error>(
                        _error->errorCode(), "add state cache failed" + _error->errorMessage());
                    _onReceiptStored(error);
                }
            });
    }
    catch (OpenSysTableFailed const& e)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncStoreReceipts")
                          << LOG_DESC("System meets error when try to write data to storage")
                          << LOG_KV("EINFO", boost::diagnostic_information(e));
        raise(SIGTERM);
        BOOST_THROW_EXCEPTION(
            OpenSysTableFailed() << errinfo_comment(" write block data to storage failed."));
    }
}

void Ledger::asyncGetBlockDataByNumber(bcos::protocol::BlockNumber _blockNumber, int32_t _blockFlag,
    std::function<void(Error::Ptr, bcos::protocol::Block::Ptr)> _onGetBlock)
{
    getBlock(_blockNumber, _blockFlag,
        [_blockNumber, _onGetBlock](Error::Ptr _error, protocol::Block::Ptr _block) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBlockDataByNumber")
                                  << LOG_DESC("callback error when get block")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                // TODO: to add errorCode and message
                auto error = std::make_shared<Error>(
                    _error->errorCode(), "callback error in getBlock" + _error->errorMessage());
                _onGetBlock(error, nullptr);
                return;
            }
            _onGetBlock(nullptr, _block);
        });
}

void Ledger::asyncGetBlockNumber(
    std::function<void(Error::Ptr, bcos::protocol::BlockNumber)> _onGetBlock)
{
    getLatestBlockNumber([_onGetBlock](BlockNumber _number) {
        if (_number == -1)
        {
            // TODO: to add errorCode
            auto error = std::make_shared<Error>(-1, "getLatestBlock error, callback -1");
            _onGetBlock(error, -1);
            return;
        }
        _onGetBlock(nullptr, _number);
    });
}

void Ledger::asyncGetBlockHashByNumber(bcos::protocol::BlockNumber _blockNumber,
    std::function<void(Error::Ptr, const bcos::crypto::HashType&)> _onGetBlock)
{
    if (_blockNumber < 0)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncGetBlockHashByNumber") << LOG_DESC("Error parameters");
        // TODO: to add errorCode
        auto error = std::make_shared<Error>(-1, "wrong block number, callback empty hash");
        _onGetBlock(error, HashType());
        return;
    }
    getStorageGetter()->getBlockHashByNumber(_blockNumber, getMemoryTableFactory(0),
        [_onGetBlock, _blockNumber](Error::Ptr _error, bcos::storage::Entry::Ptr _hashEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBlockHashByNumber")
                                  << LOG_DESC("error happened in open table or get entry")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                // TODO: add error code and msg
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getBlockHashByNumber callback error" + _error->errorMessage());
                _onGetBlock(error, HashType());
                return;
            }
            if (!_hashEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBlockHashByNumber")
                                  << LOG_DESC("error happened in get null entry")
                                  << LOG_KV("blockNumber", _blockNumber);
                _onGetBlock(nullptr, HashType());
                return;
            }
            auto hash = _hashEntry->getField(SYS_VALUE);
            _onGetBlock(nullptr, HashType(hash));
        });
}

void Ledger::asyncGetBlockNumberByHash(const crypto::HashType& _blockHash,
    std::function<void(Error::Ptr, bcos::protocol::BlockNumber)> _onGetBlock)
{
    getStorageGetter()->getBlockNumberByHash(_blockHash.hex(), getMemoryTableFactory(0),
        [_blockHash, _onGetBlock](Error::Ptr _error, bcos::storage::Entry::Ptr _numberEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBlockNumberByHash")
                                  << LOG_DESC("error happened in open table or get entry")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockHash", _blockHash.hex());

                // TODO: add error code
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getBlockNumberByHash callback error" + _error->errorMessage());
                _onGetBlock(error, -1);
                return;
            }
            if (!_numberEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBlockNumberByHash")
                                  << LOG_DESC("error happened in get a null entry")
                                  << LOG_KV("blockHash", _blockHash.hex());
                _onGetBlock(nullptr, -1);
                return;
            }
            auto number = _numberEntry->getField(SYS_VALUE);
            if (number.empty())
            {
                LEDGER_LOG(WARNING) << LOG_BADGE("asyncGetBlockNumberByHash")
                                    << LOG_DESC("get number error, number is null or empty")
                                    << LOG_KV("blockHash", _blockHash.hex());
                // TODO: add error code
                auto error = std::make_shared<Error>(-1, "get number error");
                _onGetBlock(error, -1);
                return;
            }
            _onGetBlock(nullptr, boost::lexical_cast<BlockNumber>(number));
        });
}

void Ledger::asyncGetBatchTxsByHashList(crypto::HashListPtr _txHashList, bool _withProof,
    std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr,
        std::shared_ptr<std::map<std::string, MerkleProofPtr>>)>
        _onGetTx)
{
    if (!_txHashList)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncGetBatchTxsByHashList")
                          << LOG_DESC("Error parameters");
        // TODO: add error code
        auto error = std::make_shared<Error>(-1, "nullptr in parameters");
        _onGetTx(error, nullptr, nullptr);
        return;
    }
    auto txHashStrList = std::make_shared<std::vector<std::string>>();
    for (auto& txHash : *_txHashList)
    {
        txHashStrList->emplace_back(txHash.hex());
    }
    getStorageGetter()->getBatchTxByHashList(txHashStrList, getMemoryTableFactory(0),
        getTransactionFactory(),
        [this, _txHashList, _withProof, _onGetTx](Error::Ptr _error, TransactionsPtr _txList) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetBatchTxsByHashList")
                                  << LOG_DESC("getBatchTxByHashList callback error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getBatchTxByHashList callback error" + _error->errorMessage());
                _onGetTx(error, nullptr, nullptr);
                return;
            }
            if (_withProof)
            {
                auto con_proofMap =
                    std::make_shared<tbb::concurrent_unordered_map<std::string, MerkleProofPtr>>();
                tbb::parallel_for(tbb::blocked_range<size_t>(0, _txHashList->size()),
                    [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i)
                        {
                            std::promise<bool> p;
                            auto future = p.get_future();
                            auto txHash = _txHashList->at(i);
                            getTxProof(txHash, [con_proofMap, txHash, &p](
                                                   Error::Ptr _error, MerkleProofPtr _proof) {
                                if (!_error && _proof)
                                {
                                    con_proofMap->insert(std::make_pair(txHash.hex(), _proof));
                                    p.set_value(true);
                                    return;
                                }
                                p.set_value(false);
                            });
                            if (std::future_status::ready !=
                                future.wait_for(std::chrono::milliseconds(m_timeout)))
                            {
                                LEDGER_LOG(ERROR) << LOG_BADGE("getBatchTxProof")
                                                  << LOG_DESC("getTxProof timeout");
                            }
                            LEDGER_LOG(INFO)
                                << LOG_BADGE("getBatchTxProof") << LOG_DESC("getTxProof complete")
                                << LOG_KV("getResult", future.get());
                        }
                    });
                auto proofMap = std::make_shared<std::map<std::string, MerkleProofPtr>>(
                    con_proofMap->begin(), con_proofMap->end());
                LEDGER_LOG(INFO) << LOG_BADGE("asyncGetBatchTxsByHashList")
                                 << LOG_DESC("get tx list and proofMap complete")
                                 << LOG_KV("txHashListSize", _txHashList->size())
                                 << LOG_KV("proofMapSize", proofMap->size());
                _onGetTx(nullptr, _txList, proofMap);
            }
            else
            {
                LEDGER_LOG(INFO) << LOG_BADGE("asyncGetBatchTxsByHashList")
                                 << LOG_DESC("get tx list complete")
                                 << LOG_KV("txHashListSize", _txHashList->size())
                                 << LOG_KV("withProof", _withProof);
                _onGetTx(nullptr, _txList, nullptr);
            }
        });
}

void Ledger::asyncGetTransactionReceiptByHash(bcos::crypto::HashType const& _txHash,
    bool _withProof,
    std::function<void(Error::Ptr, bcos::protocol::TransactionReceipt::ConstPtr, MerkleProofPtr)>
        _onGetTx)
{
    getStorageGetter()->getReceiptByTxHash(_txHash.hex(), getMemoryTableFactory(0),
        [this, _withProof, _onGetTx, _txHash](
            Error::Ptr _error, bcos::storage::Entry::Ptr _receiptEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                                  << LOG_DESC("getReceiptByTxHash callback error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                // TODO: add error code
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getReceiptByTxHash callback error" + _error->errorMessage());
                _onGetTx(error, nullptr, nullptr);
                return;
            }
            if (!_receiptEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                                  << LOG_DESC("getReceiptByTxHash callback null entry");
                _onGetTx(nullptr, nullptr, nullptr);
                return;
            }
            auto receipt = decodeReceipt(getReceiptFactory(), _receiptEntry->getField(SYS_VALUE));
            if (!receipt)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                                  << LOG_DESC("receipt is null or empty")
                                  << LOG_KV("txHash", _txHash.hex())
                                  << LOG_KV(
                                         "encodedReceiptValue", _receiptEntry->getField(SYS_VALUE));
                // TODO: add error code
                auto error =
                    std::make_shared<Error>(-1, "getReceiptByTxHash callback empty receipt");
                _onGetTx(error, nullptr, nullptr);
                return;
            }
            if (_withProof)
            {
                getReceiptProof(
                    receipt, [receipt, _onGetTx](Error::Ptr _error, MerkleProofPtr _proof) {
                        if (_error && _error->errorCode() != CommonError::SUCCESS)
                        {
                            LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                                              << LOG_DESC("getReceiptProof callback error")
                                              << LOG_KV("errorCode", _error->errorCode())
                                              << LOG_KV("errorMsg", _error->errorMessage());
                            // TODO: add error code
                            auto error = std::make_shared<Error>(_error->errorCode(),
                                "getReceiptProof callback error" + _error->errorMessage());
                            _onGetTx(error, receipt, nullptr);
                            return;
                        }
                        _onGetTx(nullptr, receipt, _proof);
                    });
            }
            else
            {
                LEDGER_LOG(TRACE) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                                  << LOG_DESC("call back receipt");
                _onGetTx(nullptr, receipt, nullptr);
            }
        });
}

void Ledger::asyncGetTotalTransactionCount(
    std::function<void(Error::Ptr, int64_t, int64_t, bcos::protocol::BlockNumber)> _callback)
{
    std::promise<int64_t> countPromise;
    std::promise<int64_t> failedPromise;
    std::promise<BlockNumber> numberPromise;
    auto countFuture = countPromise.get_future();
    auto failedFuture = failedPromise.get_future();
    auto numberFuture = numberPromise.get_future();

    getStorageGetter()->getCurrentState(SYS_KEY_TOTAL_TRANSACTION_COUNT, getMemoryTableFactory(0),
        [&countPromise](Error::Ptr _error, bcos::storage::Entry::Ptr _totalCountEntry) {
            if ((!_error || _error->errorCode() == CommonError::SUCCESS))
            {
                // entry must exist
                auto totalStr = _totalCountEntry->getField(SYS_VALUE);
                int64_t totalCount = totalStr.empty() ? -1 : boost::lexical_cast<int64_t>(totalStr);
                countPromise.set_value(totalCount);
                return;
            }
            LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                              << LOG_DESC("error happened in get SYS_KEY_TOTAL_TRANSACTION_COUNT");
            countPromise.set_value(-1);
        });
    getStorageGetter()->getCurrentState(SYS_KEY_TOTAL_FAILED_TRANSACTION, getMemoryTableFactory(0),
        [&failedPromise](Error::Ptr _error, bcos::storage::Entry::Ptr _totalFailedEntry) {
            if ((!_error || _error->errorCode() == CommonError::SUCCESS))
            {
                // entry must exist
                auto totalFailedStr = _totalFailedEntry->getField(SYS_VALUE);
                auto totalFailed =
                    totalFailedStr.empty() ? -1 : boost::lexical_cast<int64_t>(totalFailedStr);
                failedPromise.set_value(totalFailed);
                return;
            }
            LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                              << LOG_DESC("error happened in get SYS_KEY_TOTAL_FAILED_TRANSACTION");
            failedPromise.set_value(-1);
        });
    getLatestBlockNumber(
        [&numberPromise](BlockNumber _number) { numberPromise.set_value(_number); });
    auto totalCount = countFuture.get();
    auto totalFailed = failedFuture.get();
    auto number = numberFuture.get();
    if (totalCount == -1 || totalFailed == -1 || number == -1)
    {
        LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetTransactionReceiptByHash")
                          << LOG_DESC("error happened in get total tx count");
        // TODO: add error code
        auto error = std::make_shared<Error>(
            -1, "timeout to fetch all data in asyncGetTotalTransactionCount");
        _callback(error, -1, -1, -1);
        return;
    }
    _callback(nullptr, totalCount, totalFailed, number);
}

void Ledger::asyncGetSystemConfigByKey(const std::string& _key,
    std::function<void(Error::Ptr, std::string, bcos::protocol::BlockNumber)> _onGetConfig)
{
    getStorageGetter()->getSysConfig(_key, getMemoryTableFactory(0),
        [_key, _onGetConfig](Error::Ptr _error, bcos::storage::Entry::Ptr _configEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetSystemConfigByKey")
                                  << LOG_DESC("getSysConfig callback error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                // TODO: add error code
                auto error = std::make_shared<Error>(
                    _error->errorCode(), "getSysConfig callback error" + _error->errorMessage());
                _onGetConfig(error, "", -1);
                return;
            }
            if (!_configEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetSystemConfigByKey")
                                  << LOG_DESC("getSysConfig callback null entry")
                                  << LOG_KV("key", _key);
                _onGetConfig(nullptr, "", -1);
                return;
            }
            auto value = _configEntry->getField(SYS_VALUE);
            auto numberStr = _configEntry->getField(SYS_CONFIG_ENABLE_BLOCK_NUMBER);
            BlockNumber number =
                numberStr.empty() ? -1 : boost::lexical_cast<BlockNumber>(numberStr);
            LEDGER_LOG(TRACE) << LOG_BADGE("asyncGetSystemConfigByKey")
                              << LOG_DESC("get config in db") << LOG_KV("key", _key)
                              << LOG_KV("value", value);
            _onGetConfig(nullptr, value, number);
        });
}

void Ledger::asyncGetNonceList(bcos::protocol::BlockNumber _startNumber, int64_t _offset,
    std::function<void(
        Error::Ptr, std::shared_ptr<std::map<protocol::BlockNumber, protocol::NonceListPtr>>)>
        _onGetList)
{
    if (_startNumber < 0 || _offset < 0)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncGetNonceList") << LOG_DESC("Error parameters");
        // TODO: to add errorCode
        auto error = std::make_shared<Error>(-1, "error parameter");
        _onGetList(error, nullptr);
        return;
    }
    getStorageGetter()->getNoncesBatchFromStorage(_startNumber, _startNumber + _offset,
        getMemoryTableFactory(0), m_blockFactory,
        [_onGetList](Error::Ptr _error,
            std::shared_ptr<std::map<protocol::BlockNumber, protocol::NonceListPtr>> _nonceMap) {
            if (!_error || _error->errorCode() == CommonError::SUCCESS)
            {
                _onGetList(nullptr, _nonceMap);
            }
            else
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetNonceList")
                                  << LOG_DESC("error happened in open table or get entry");
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getNoncesBatchFromStorage callback error" + _error->errorMessage());
                _onGetList(error, nullptr);
            }
        });
}

void Ledger::asyncGetNodeListByType(const std::string& _type,
    std::function<void(Error::Ptr, consensus::ConsensusNodeListPtr)> _onGetConfig)
{
    if (_type != CONSENSUS_SEALER && _type != CONSENSUS_OBSERVER)
    {
        LEDGER_LOG(FATAL) << LOG_BADGE("asyncGetNodeListByType") << LOG_DESC("Error parameters");
        // TODO: to add errorCode
        auto error = std::make_shared<Error>(-1, "error type");
        _onGetConfig(error, nullptr);
        return;
    }
    getStorageGetter()->asyncGetConsensusConfig(_type, getMemoryTableFactory(0),
        m_blockFactory->cryptoSuite()->keyFactory(),
        [_onGetConfig](Error::Ptr _error, consensus::ConsensusNodeListPtr _nodeList) {
            if (!_error || _error->errorCode() == CommonError::SUCCESS)
            {
                _onGetConfig(nullptr, _nodeList);
            }
            else
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("asyncGetNodeListByType")
                                  << LOG_DESC("error happened in open table or get entry")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "asyncGetConsensusConfig callback error" + _error->errorMessage());
                _onGetConfig(error, nullptr);
            }
        });
}

void Ledger::getBlock(const BlockNumber& _blockNumber, int32_t _blockFlag,
    std::function<void(Error::Ptr, protocol::Block::Ptr)> _onGetBlock)
{
    auto cachedBlock = m_blockCache.get(_blockNumber);
    /// flag of whether get a single part of block,
    /// if true, then it can callback immediately
    bool singlePartFlag = (_blockFlag & (_blockFlag - 1)) == 0;

    if (bool(cachedBlock.second))
    {
        LEDGER_LOG(TRACE) << LOG_BADGE("getBlock") << LOG_DESC("Cache hit, read from cache")
                          << LOG_KV("blockNumber", _blockNumber);
        _onGetBlock(nullptr, cachedBlock.second);
        return;
    }
    LEDGER_LOG(TRACE) << LOG_BADGE("getBlock") << LOG_DESC("Cache missed, read from storage")
                      << LOG_KV("blockNumber", _blockNumber);
    auto block = m_blockFactory->createBlock();
    int32_t fetchFlag = 0;
    std::shared_ptr<std::promise<bool>> headerPromise = std::make_shared<std::promise<bool>>();
    std::shared_ptr<std::promise<bool>> txsPromise = std::make_shared<std::promise<bool>>();
    std::shared_ptr<std::promise<bool>> receiptsPromise = std::make_shared<std::promise<bool>>();
    auto headerFuture = headerPromise->get_future();
    auto txsFuture = txsPromise->get_future();
    auto receiptsFuture = receiptsPromise->get_future();

    if (_blockFlag & HEADER)
    {
        getBlockHeader(_blockNumber, [headerPromise, _onGetBlock, _blockNumber, singlePartFlag,
                                         block](Error::Ptr _error, BlockHeader::Ptr _header) {
            if ((!_error || _error->errorCode() == CommonError::SUCCESS))
            {
                // should handle nullptr header
                if (singlePartFlag)
                {
                    if (!_header)
                    {
                        // TODO: add errorCode
                        auto error = std::make_shared<Error>(-1, "getBlockHeader callback nullptr");
                        _onGetBlock(error, nullptr);
                        return;
                    }
                    block->setBlockHeader(_header);
                    _onGetBlock(nullptr, block);
                }
                else
                {
                    block->setBlockHeader(_header);
                    headerPromise->set_value((_header != nullptr));
                }
                return;
            }
            LEDGER_LOG(ERROR) << LOG_BADGE("getBlock")
                              << LOG_DESC("Can't find the header, callback error")
                              << LOG_KV("errorCode", _error->errorCode())
                              << LOG_KV("errorMsg", _error->errorMessage())
                              << LOG_KV("blockNumber", _blockNumber);
            if (singlePartFlag)
            {
                auto error = std::make_shared<Error>(
                    _error->errorCode(), "getBlockHeader callback error " + _error->errorMessage());
                _onGetBlock(error, nullptr);
                return;
            }
            headerPromise->set_value(false);
        });
    }
    if (_blockFlag & TRANSACTIONS)
    {
        getTxs(_blockNumber, [_blockNumber, txsPromise, _onGetBlock, block, singlePartFlag](
                                 Error::Ptr _error, bcos::protocol::TransactionsPtr _txs) {
            if ((!_error || _error->errorCode() == CommonError::SUCCESS))
            {
                /// not handle nullptr txs
                auto insertSize = blockTransactionListSetter(block, _txs);
                LEDGER_LOG(TRACE) << LOG_BADGE("getBlock") << LOG_DESC("insert block transactions")
                                  << LOG_KV("insertSize", insertSize)
                                  << LOG_KV("blockNumber", _blockNumber);
                if (singlePartFlag)
                {
                    _onGetBlock(nullptr, block);
                }
                else
                {
                    txsPromise->set_value(true);
                }
                return;
            }
            LEDGER_LOG(ERROR) << LOG_BADGE("getBlock")
                              << LOG_DESC("Can't find the Txs, callback error")
                              << LOG_KV("errorCode", _error->errorCode())
                              << LOG_KV("errorMsg", _error->errorMessage())
                              << LOG_KV("blockNumber", _blockNumber);
            if (singlePartFlag)
            {
                auto error = std::make_shared<Error>(
                    _error->errorCode(), "getBlockTxs callback error " + _error->errorMessage());
                _onGetBlock(error, nullptr);
                return;
            }
            txsPromise->set_value(false);
        });
    }
    if (_blockFlag & RECEIPTS)
    {
        getReceipts(
            _blockNumber, [_blockNumber, receiptsPromise, _onGetBlock, block, singlePartFlag](
                              Error::Ptr _error, protocol::ReceiptsPtr _receipts) {
                if ((!_error || _error->errorCode() == CommonError::SUCCESS))
                {
                    /// not handle nullptr _receipts
                    auto insertSize = blockReceiptListSetter(block, _receipts);
                    LEDGER_LOG(TRACE)
                        << LOG_BADGE("getBlock") << LOG_DESC("insert block receipts")
                        << LOG_KV("insertSize", insertSize) << LOG_KV("blockNumber", _blockNumber);
                    if (singlePartFlag)
                    {
                        _onGetBlock(nullptr, block);
                    }
                    else
                    {
                        receiptsPromise->set_value(true);
                    }
                    return;
                }
                LEDGER_LOG(ERROR) << LOG_BADGE("getBlock") << LOG_DESC("Can't find the Receipts")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                if (singlePartFlag)
                {
                    auto error = std::make_shared<Error>(_error->errorCode(),
                        "getBlockReceipts callback error " + _error->errorMessage());
                    _onGetBlock(error, nullptr);
                    return;
                }
                receiptsPromise->set_value(false);
            });
    }
    // it means _blockFlag has multiple 1 in binary
    // should wait for all datum callback
    if (!singlePartFlag)
    {
        bool headerFlag = true;
        bool txsFlag = true;
        bool receiptFlag = true;
        if (_blockFlag & HEADER)
        {
            headerFlag = std::future_status::ready ==
                                 headerFuture.wait_for(std::chrono::milliseconds(m_timeout)) ?
                             headerFuture.get() :
                             false;
        }
        if (_blockFlag & TRANSACTIONS)
        {
            txsFlag = std::future_status::ready ==
                              txsFuture.wait_for(std::chrono::milliseconds(m_timeout)) ?
                          txsFuture.get() :
                          false;
        }
        if (_blockFlag & RECEIPTS)
        {
            receiptFlag = std::future_status::ready ==
                                  receiptsFuture.wait_for(std::chrono::milliseconds(m_timeout)) ?
                              receiptsFuture.get() :
                              false;
        }
        if (headerFlag && txsFlag && receiptFlag)
        {
            if (!(_blockFlag ^ FULL_BLOCK))
            {
                // get full block data
                LEDGER_LOG(TRACE) << LOG_BADGE("getBlock") << LOG_DESC("Write to cache");
                m_blockCache.add(_blockNumber, block);
            }
            _onGetBlock(nullptr, block);
            return;
        }
        auto error = std::make_shared<Error>(-1, "some data fetch failed");
        _onGetBlock(error, nullptr);
    }
}

void Ledger::getLatestBlockNumber(std::function<void(protocol::BlockNumber)> _onGetNumber)
{
    if (m_blockNumber != -1)
    {
        _onGetNumber(m_blockNumber);
        return;
    }
    else
    {
        auto self = std::weak_ptr<Ledger>(std::dynamic_pointer_cast<Ledger>(shared_from_this()));
        getStorageGetter()->getCurrentState(SYS_KEY_CURRENT_NUMBER, getMemoryTableFactory(0),
            [self, _onGetNumber](Error::Ptr _error, bcos::storage::Entry::Ptr _numberEntry) {
                auto ledger = self.lock();
                if (!ledger || !_numberEntry)
                {
                    _onGetNumber(-1);
                    return;
                }
                if (!_error)
                {
                    // number entry must exist
                    auto numberStr = _numberEntry->getField(SYS_VALUE);
                    BlockNumber number =
                        numberStr.empty() ? -1 : boost::lexical_cast<BlockNumber>(numberStr);
                    _onGetNumber(number);
                    return;
                }
                LEDGER_LOG(ERROR) << LOG_BADGE("getLatestBlockNumber")
                                  << LOG_DESC("Get number from storage error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                _onGetNumber(-1);
            });
    }
}

void Ledger::getBlockHeader(const bcos::protocol::BlockNumber& _blockNumber,
    std::function<void(Error::Ptr, BlockHeader::Ptr)> _onGetHeader)
{
    auto cachedHeader = m_blockHeaderCache.get(_blockNumber);

    if (bool(cachedHeader.second))
    {
        LEDGER_LOG(TRACE) << LOG_BADGE("getBlockHeader")
                          << LOG_DESC("CacheHeader hit, read from cache")
                          << LOG_KV("blockNumber", _blockNumber);
        _onGetHeader(nullptr, cachedHeader.second);
        return;
    }
    LEDGER_LOG(TRACE) << LOG_BADGE("getBlockHeader") << LOG_DESC("Cache missed, read from storage")
                      << LOG_KV("blockNumber", _blockNumber);
    getStorageGetter()->getBlockHeaderFromStorage(_blockNumber, getMemoryTableFactory(0),
        [this, _onGetHeader, _blockNumber](
            Error::Ptr _error, bcos::storage::Entry::Ptr _headerEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getBlockHeader")
                                  << LOG_DESC("Get header from storage error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getBlockHeaderFromStorage callback error" + _error->errorMessage());
                _onGetHeader(error, nullptr);
                return;
            }
            if (!_headerEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getBlockHeader")
                                  << LOG_DESC("Get header from storage callback null entry")
                                  << LOG_KV("blockNumber", _blockNumber);
                _onGetHeader(nullptr, nullptr);
                return;
            }
            auto headerPtr =
                decodeBlockHeader(getBlockHeaderFactory(), _headerEntry->getField(SYS_VALUE));
            LEDGER_LOG(TRACE) << LOG_BADGE("getBlockHeader") << LOG_DESC("Get header from storage")
                              << LOG_KV("blockNumber", _blockNumber);
            if (headerPtr)
            {
                LEDGER_LOG(TRACE) << LOG_BADGE("getBlockHeader") << LOG_DESC("Write to cache");
                m_blockHeaderCache.add(_blockNumber, headerPtr);
            }
            _onGetHeader(nullptr, headerPtr);
        });
}

void Ledger::getTxs(const bcos::protocol::BlockNumber& _blockNumber,
    std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr)> _onGetTxs)
{
    auto cachedTransactions = m_transactionsCache.get(_blockNumber);
    if (bool(cachedTransactions.second))
    {
        LEDGER_LOG(TRACE) << LOG_BADGE("getTxs") << LOG_DESC("CacheTxs hit, read from cache")
                          << LOG_KV("blockNumber", _blockNumber);
        _onGetTxs(nullptr, cachedTransactions.second);
        return;
    }
    LEDGER_LOG(TRACE) << LOG_BADGE("getTxs") << LOG_DESC("Cache missed, read from storage")
                      << LOG_KV("blockNumber", _blockNumber);
    // block with tx hash list
    getStorageGetter()->getTxsFromStorage(_blockNumber, getMemoryTableFactory(0),
        [this, _onGetTxs, _blockNumber](Error::Ptr _error, bcos::storage::Entry::Ptr _blockEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxs")
                                  << LOG_DESC("Get txHashList from storage error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getTxsFromStorage callback error" + _error->errorMessage());
                _onGetTxs(error, nullptr);
                return;
            }
            if (!_blockEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxs")
                                  << LOG_DESC("Get txHashList from storage callback null entry")
                                  << LOG_KV("blockNumber", _blockNumber);
                _onGetTxs(nullptr, nullptr);
                return;
            }
            auto block = decodeBlock(m_blockFactory, _blockEntry->getField(SYS_VALUE));
            if (!block)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxs")
                                  << LOG_DESC("getTxsFromStorage get error block")
                                  << LOG_KV("blockNumber", _blockNumber);
                // TODO: add error code
                auto error = std::make_shared<Error>(-1, "getTxsFromStorage get error block");
                _onGetTxs(error, nullptr);
                return;
            }
            auto txHashList = blockTxHashListGetter(block);
            getStorageGetter()->getBatchTxByHashList(txHashList, getMemoryTableFactory(0),
                getTransactionFactory(),
                [this, _onGetTxs, _blockNumber, txHashList](
                    Error::Ptr _error, protocol::TransactionsPtr _txs) {
                    if (_error && _error->errorCode() != CommonError::SUCCESS)
                    {
                        LEDGER_LOG(ERROR)
                            << LOG_BADGE("getTxs") << LOG_DESC("Get txs from storage error")
                            << LOG_KV("errorCode", _error->errorCode())
                            << LOG_KV("errorMsg", _error->errorMessage())
                            << LOG_KV("txsSize", _txs->size());
                        auto error = std::make_shared<Error>(_error->errorCode(),
                            "getBatchTxByHashList callback error" + _error->errorMessage());
                        _onGetTxs(error, nullptr);
                        return;
                    }
                    LEDGER_LOG(TRACE) << LOG_BADGE("getTxs") << LOG_DESC("Get txs from storage");
                    if (_txs && !_txs->empty())
                    {
                        LEDGER_LOG(TRACE) << LOG_BADGE("getTxs") << LOG_DESC("Write to cache");
                        m_transactionsCache.add(_blockNumber, _txs);
                    }
                    _onGetTxs(nullptr, _txs);
                });
        });
}

void Ledger::getReceipts(const bcos::protocol::BlockNumber& _blockNumber,
    std::function<void(Error::Ptr, bcos::protocol::ReceiptsPtr)> _onGetReceipts)
{
    auto cachedReceipts = m_receiptCache.get(_blockNumber);
    if (bool(cachedReceipts.second))
    {
        LEDGER_LOG(TRACE) << LOG_BADGE("getReceipts")
                          << LOG_DESC("Cache Receipts hit, read from cache")
                          << LOG_KV("blockNumber", _blockNumber);
        _onGetReceipts(nullptr, cachedReceipts.second);
        return;
    }
    LEDGER_LOG(TRACE) << LOG_BADGE("getReceipts") << LOG_DESC("Cache missed, read from storage")
                      << LOG_KV("blockNumber", _blockNumber);
    // block with tx hash list
    getStorageGetter()->getTxsFromStorage(_blockNumber, getMemoryTableFactory(0),
        [this, _onGetReceipts, _blockNumber](
            Error::Ptr _error, bcos::storage::Entry::Ptr _blockEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getReceipts")
                                  << LOG_DESC("Get receipts from storage error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("blockNumber", _blockNumber);
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getTxsFromStorage callback error" + _error->errorMessage());
                _onGetReceipts(error, nullptr);
                return;
            }
            if (!_blockEntry)
            {
                _onGetReceipts(nullptr, nullptr);
                return;
            }
            auto block = decodeBlock(m_blockFactory, _blockEntry->getField(SYS_VALUE));
            if (!block)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getReceipts")
                                  << LOG_DESC("getTxsFromStorage get txHashList error")
                                  << LOG_KV("blockNumber", _blockNumber);
                // TODO: add error code
                auto error = std::make_shared<Error>(-1, "getTxsFromStorage get empty block");
                _onGetReceipts(error, nullptr);
                return;
            }

            auto txHashList = blockTxHashListGetter(block);
            getStorageGetter()->getBatchReceiptsByHashList(txHashList, getMemoryTableFactory(0),
                getReceiptFactory(), [=](Error::Ptr _error, ReceiptsPtr _receipts) {
                    if (_error && _error->errorCode() != CommonError::SUCCESS)
                    {
                        LEDGER_LOG(ERROR) << LOG_BADGE("getReceipts")
                                          << LOG_DESC("Get receipts from storage error")
                                          << LOG_KV("errorCode", _error->errorCode())
                                          << LOG_KV("errorMsg", _error->errorMessage())
                                          << LOG_KV("blockNumber", _blockNumber);
                        auto error = std::make_shared<Error>(_error->errorCode(),
                            "getBatchReceiptsByHashList callback error" + _error->errorMessage());
                        _onGetReceipts(error, nullptr);
                        return;
                    }
                    LEDGER_LOG(TRACE)
                        << LOG_BADGE("getReceipts") << LOG_DESC("Get receipts from storage");
                    if (_receipts && _receipts->size() > 0)
                        m_receiptCache.add(_blockNumber, _receipts);
                    _onGetReceipts(nullptr, _receipts);
                });
        });
}

void Ledger::getTxProof(
    const HashType& _txHash, std::function<void(Error::Ptr, MerkleProofPtr)> _onGetProof)
{
    // get receipt to get block number
    getStorageGetter()->getReceiptByTxHash(_txHash.hex(), getMemoryTableFactory(0),
        [this, _txHash, _onGetProof](Error::Ptr _error, bcos::storage::Entry::Ptr _receiptEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxProof")
                                  << LOG_DESC("getReceiptByTxHash from storage error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage())
                                  << LOG_KV("txHash", _txHash.hex());
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getReceiptByTxHash callback error" + _error->errorMessage());
                _onGetProof(error, nullptr);
                return;
            }
            if (!_receiptEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxProof")
                                  << LOG_DESC("getReceiptByTxHash from storage callback null entry")
                                  << LOG_KV("txHash", _txHash.hex());
                _onGetProof(nullptr, nullptr);
                return;
            }
            auto receipt = decodeReceipt(getReceiptFactory(), _receiptEntry->getField(SYS_VALUE));
            if (!receipt)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getTxProof") << LOG_DESC("receipt is null or empty")
                                  << LOG_KV("txHash", _txHash.hex())
                                  << LOG_KV(
                                         "encodedReceiptValue", _receiptEntry->getField(SYS_VALUE));
                // TODO: add error code
                auto error =
                    std::make_shared<Error>(-1, "getReceiptByTxHash callback empty receipt");
                _onGetProof(error, nullptr);
                return;
            }
            auto blockNumber = receipt->blockNumber();
            getTxs(blockNumber, [this, blockNumber, _onGetProof, _txHash](
                                    Error::Ptr _error, TransactionsPtr _txs) {
                if (_error && _error->errorCode() != CommonError::SUCCESS)
                {
                    // TODO: add error msg
                    LEDGER_LOG(ERROR)
                        << LOG_BADGE("getTxProof") << LOG_DESC("getTxs callback error")
                        << LOG_KV("errorCode", _error->errorCode())
                        << LOG_KV("errorMsg", _error->errorMessage());
                    auto error = std::make_shared<Error>(
                        _error->errorCode(), "getTxs callback error" + _error->errorMessage());
                    _onGetProof(error, nullptr);
                    return;
                }
                if (!_txs)
                {
                    LEDGER_LOG(ERROR)
                        << LOG_BADGE("getTxProof") << LOG_DESC("get txs error")
                        << LOG_KV("blockNumber", blockNumber) << LOG_KV("txHash", _txHash.hex());
                    // TODO: add error code
                    auto error = std::make_shared<Error>(-1, "getTxs callback empty txs");
                    _onGetProof(error, nullptr);
                    return;
                }
                auto merkleProofPtr = std::make_shared<MerkleProof>();
                auto parent2ChildList = m_merkleProofUtility->getParent2ChildListByTxsProofCache(
                    blockNumber, _txs, m_blockFactory->cryptoSuite());
                auto child2Parent = m_merkleProofUtility->getChild2ParentCacheByTransaction(
                    parent2ChildList, blockNumber);
                m_merkleProofUtility->getMerkleProof(
                    _txHash, *parent2ChildList, *child2Parent, *merkleProofPtr);
                LEDGER_LOG(TRACE) << LOG_BADGE("getTxProof") << LOG_DESC("get merkle proof success")
                                  << LOG_KV("blockNumber", blockNumber)
                                  << LOG_KV("txHash", _txHash.hex());
                _onGetProof(nullptr, merkleProofPtr);
            });
        });
}

void Ledger::getReceiptProof(protocol::TransactionReceipt::Ptr _receipt,
    std::function<void(Error::Ptr, MerkleProofPtr)> _onGetProof)
{
    if (!_receipt)
    {
        _onGetProof(nullptr, nullptr);
        return;
    }
    getStorageGetter()->getTxsFromStorage(_receipt->blockNumber(), getMemoryTableFactory(0),
        [this, _onGetProof, _receipt](Error::Ptr _error, bcos::storage::Entry::Ptr _blockEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getReceiptProof")
                                  << LOG_DESC("getTxsFromStorage callback error")
                                  << LOG_KV("errorCode", _error->errorCode())
                                  << LOG_KV("errorMsg", _error->errorMessage());
                // TODO: add error code
                auto error = std::make_shared<Error>(_error->errorCode(),
                    "getTxsFromStorage callback error" + _error->errorMessage());
                _onGetProof(error, nullptr);
                return;
            }
            if (!_blockEntry)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("getReceiptProof")
                                  << LOG_DESC("getTxsFromStorage callback null entry")
                                  << LOG_KV("blockNumber", _receipt->blockNumber());
                _onGetProof(nullptr, nullptr);
                return;
            }
            auto block = decodeBlock(m_blockFactory, _blockEntry->getField(SYS_VALUE));
            if (!block)
            {
                // TODO: add error code
                LEDGER_LOG(ERROR) << LOG_BADGE("getReceiptProof")
                                  << LOG_DESC("getTxsFromStorage callback empty block txs");
                auto error = std::make_shared<Error>(-1, "empty txs");
                _onGetProof(error, nullptr);
                return;
            }
            auto txHashList = blockTxHashListGetter(block);
            getStorageGetter()->getBatchReceiptsByHashList(txHashList, getMemoryTableFactory(0),
                getReceiptFactory(),
                [this, _onGetProof, _receipt](Error::Ptr _error, ReceiptsPtr receipts) {
                    if (_error && _error->errorCode() != CommonError::SUCCESS)
                    {
                        LEDGER_LOG(ERROR) << LOG_BADGE("getReceiptProof")
                                          << LOG_DESC("getBatchReceiptsByHashList callback error")
                                          << LOG_KV("errorCode", _error->errorCode())
                                          << LOG_KV("errorMsg", _error->errorMessage());
                        // TODO: add error code and message
                        auto error = std::make_shared<Error>(_error->errorCode(),
                            "getBatchReceiptsByHashList callback error" + _error->errorMessage());
                        _onGetProof(error, nullptr);
                        return;
                    }
                    if (!receipts || receipts->empty())
                    {
                        // TODO: add error code
                        LEDGER_LOG(ERROR) << LOG_BADGE("getReceiptProof")
                                          << LOG_DESC(
                                                 "getBatchReceiptsByHashList callback empty "
                                                 "receipts");
                        auto error = std::make_shared<Error>(-1, "empty receipts");
                        _onGetProof(error, nullptr);
                        return;
                    }
                    auto merkleProof = std::make_shared<MerkleProof>();
                    auto parent2ChildList =
                        m_merkleProofUtility->getParent2ChildListByReceiptProofCache(
                            _receipt->blockNumber(), receipts, m_blockFactory->cryptoSuite());
                    auto child2Parent = m_merkleProofUtility->getChild2ParentCacheByReceipt(
                        parent2ChildList, _receipt->blockNumber());
                    m_merkleProofUtility->getMerkleProof(
                        _receipt->hash(), *parent2ChildList, *child2Parent, *merkleProof);
                    LEDGER_LOG(INFO)
                        << LOG_BADGE("getReceiptProof") << LOG_DESC("call back receipt and proof");
                    _onGetProof(nullptr, merkleProof);
                });
        });
}

void Ledger::asyncGetLedgerConfig(protocol::BlockNumber _number, const crypto::HashType& _hash,
    std::function<void(Error::Ptr, WrapperLedgerConfig::Ptr)> _onGetLedgerConfig)
{
    auto ledgerConfig = std::make_shared<LedgerConfig>();
    std::atomic_bool asyncRet = {true};
    ledgerConfig->setBlockNumber(_number);
    ledgerConfig->setHash(_hash);

    auto wrapperLedgerConfig = std::make_shared<WrapperLedgerConfig>(ledgerConfig);

    auto storageGetter = getStorageGetter();
    auto tableFactory = getMemoryTableFactory(0);
    auto keys = std::make_shared<std::vector<std::string>>();
    *keys = {SYSTEM_KEY_CONSENSUS_TIMEOUT, SYSTEM_KEY_TX_COUNT_LIMIT,
        SYSTEM_KEY_CONSENSUS_LEADER_PERIOD};
    storageGetter->asyncGetSystemConfigList(keys, tableFactory, false,
        [keys, wrapperLedgerConfig, _onGetLedgerConfig](
            const Error::Ptr& _error, std::map<std::string, Entry::Ptr> const& _entries) {
            if (_error)
            {
                LEDGER_LOG(WARNING)
                    << LOG_DESC("asyncGetLedgerConfig failed")
                    << LOG_KV("code", _error->errorCode()) << LOG_KV("msg", _error->errorMessage());
                _onGetLedgerConfig(_error, nullptr);
                return;
            }
            try
            {
                // parse the configurations
                auto consensusTimeout =
                    (_entries.at(SYSTEM_KEY_CONSENSUS_TIMEOUT))->getField(SYS_VALUE);
                auto ledgerConfig = wrapperLedgerConfig->ledgerConfig();
                ledgerConfig->setConsensusTimeout(boost::lexical_cast<uint64_t>(consensusTimeout));

                auto txCountLimit = (_entries.at(SYSTEM_KEY_TX_COUNT_LIMIT))->getField(SYS_VALUE);
                ledgerConfig->setBlockTxCountLimit(boost::lexical_cast<uint64_t>(txCountLimit));

                auto consensusLeaderPeriod =
                    (_entries.at(SYSTEM_KEY_CONSENSUS_LEADER_PERIOD))->getField(SYS_VALUE);
                ledgerConfig->setLeaderSwitchPeriod(
                    boost::lexical_cast<uint64_t>(consensusLeaderPeriod));
                LEDGER_LOG(INFO) << LOG_DESC(
                                        "asyncGetLedgerConfig: asyncGetSystemConfigList success")
                                 << LOG_KV("consensusTimeout", consensusTimeout)
                                 << LOG_KV("txCountLimit", txCountLimit)
                                 << LOG_KV("consensusLeaderPeriod", consensusLeaderPeriod);
                wrapperLedgerConfig->setSysConfigFetched(true);
                _onGetLedgerConfig(nullptr, wrapperLedgerConfig);
            }
            catch (std::exception const& e)
            {
                auto errorMsg = "asyncGetLedgerConfig:  asyncGetSystemConfigList failed for " +
                                boost::diagnostic_information(e);
                LEDGER_LOG(ERROR) << LOG_DESC(errorMsg);
                _onGetLedgerConfig(std::make_shared<Error>(-1, errorMsg), nullptr);
            }
        });

    // get the consensusNodeInfo and the observerNodeInfo
    std::vector<std::string> nodeTypeList = {CONSENSUS_SEALER, CONSENSUS_OBSERVER};
    storageGetter->asyncGetConsensusConfigList(nodeTypeList, tableFactory,
        m_blockFactory->cryptoSuite()->keyFactory(),
        [wrapperLedgerConfig, _onGetLedgerConfig](
            Error::Ptr _error, std::map<std::string, consensus::ConsensusNodeListPtr> _nodeMap) {
            if (_error)
            {
                LEDGER_LOG(WARNING)
                    << LOG_DESC("asyncGetLedgerConfig: asyncGetConsensusConfig failed")
                    << LOG_KV("code", _error->errorCode()) << LOG_KV("msg", _error->errorMessage());
                _onGetLedgerConfig(_error, nullptr);
                return;
            }
            auto ledgerConfig = wrapperLedgerConfig->ledgerConfig();
            if (_nodeMap.count(CONSENSUS_SEALER) && _nodeMap[CONSENSUS_SEALER])
            {
                auto consensusNodeList = _nodeMap[CONSENSUS_SEALER];
                ledgerConfig->setConsensusNodeList(*consensusNodeList);
            }
            if (_nodeMap.count(CONSENSUS_OBSERVER) && _nodeMap[CONSENSUS_OBSERVER])
            {
                auto observerNodeList = _nodeMap[CONSENSUS_OBSERVER];
                ledgerConfig->setObserverNodeList(*observerNodeList);
            }
            LEDGER_LOG(INFO) << LOG_DESC("asyncGetLedgerConfig: asyncGetConsensusConfig success")
                             << LOG_KV(
                                    "consensusNodeSize", ledgerConfig->consensusNodeList().size())
                             << LOG_KV("observerNodeSize", ledgerConfig->observerNodeList().size());
            wrapperLedgerConfig->setConsensusConfigFetched(true);
            _onGetLedgerConfig(nullptr, wrapperLedgerConfig);
        });
}

bool Ledger::isBlockShouldCommit(const BlockNumber& _blockNumber, const std::string& _parentHash)
{
    std::promise<std::string> hashPromise;
    std::promise<BlockNumber> numberPromise;
    auto hashFuture = hashPromise.get_future();
    auto numberFuture = numberPromise.get_future();
    getLatestBlockNumber(
        [this, &numberPromise, &hashPromise, _blockNumber](protocol::BlockNumber _number) {
            numberPromise.set_value(_number);
            getStorageGetter()->getBlockHashByNumber(_number, getMemoryTableFactory(0),
                [&hashPromise, _number, _blockNumber](
                    Error::Ptr _error, bcos::storage::Entry::Ptr _hashEntry) {
                    if ((!_error || _error->errorCode() == CommonError::SUCCESS))
                    {
                        if (!_hashEntry)
                        {
                            hashPromise.set_value("");
                            return;
                        }
                        hashPromise.set_value(_hashEntry->getField(SYS_VALUE));
                        return;
                    }
                    LEDGER_LOG(ERROR)
                        << LOG_BADGE("isBlockShouldCommit") << LOG_DESC("Get block hash error")
                        << LOG_KV("needNumber", _number + 1)
                        << LOG_KV("committedNumber", _blockNumber);
                    hashPromise.set_value("");
                });
        });
    auto number = numberFuture.get();
    auto hash = hashFuture.get();
    if (_blockNumber == number + 1 && _parentHash == hash)
    {
        return true;
    }
    LEDGER_LOG(WARNING) << LOG_BADGE("isBlockShouldCommit")
                        << LOG_DESC("incorrect block number or incorrect parent hash")
                        << LOG_KV("needNumber", number + 1)
                        << LOG_KV("committedNumber", _blockNumber);
    return false;
}

void Ledger::writeNumber(
    const BlockNumber& blockNumber, const bcos::storage::TableFactoryInterface::Ptr& _tableFactory)
{
    bool ret = getStorageSetter()->setCurrentState(
        _tableFactory, SYS_KEY_CURRENT_NUMBER, boost::lexical_cast<std::string>(blockNumber));
    if (!ret)
    {
        LEDGER_LOG(DEBUG) << LOG_BADGE("writeNumber")
                          << LOG_DESC("Write row in SYS_CURRENT_STATE error")
                          << LOG_KV("blockNumber", blockNumber);
    }
}

void Ledger::writeNumber2Nonces(
    const Block::Ptr& block, const bcos::storage::TableFactoryInterface::Ptr& _tableFactory)
{
    auto blockNumberStr = boost::lexical_cast<std::string>(block->blockHeader()->number());
    auto emptyBlock = m_blockFactory->createBlock();
    emptyBlock->setNonceList(block->nonceList());

    std::shared_ptr<bytes> nonceData = std::make_shared<bytes>();
    emptyBlock->encode(*nonceData);

    auto nonceStr = asString(*nonceData);
    bool ret = getStorageSetter()->setNumber2Nonces(_tableFactory, blockNumberStr, nonceStr);
    if (!ret)
    {
        LEDGER_LOG(DEBUG) << LOG_BADGE("WriteNoncesToBlock")
                          << LOG_DESC("Write row in SYS_BLOCK_NUMBER_2_NONCES error")
                          << LOG_KV("blockNumber", blockNumberStr);
    }
}

void Ledger::writeHash2Number(
    const BlockHeader::Ptr& header, const bcos::storage::TableFactoryInterface::Ptr& _tableFactory)
{
    bool ret = getStorageSetter()->setHash2Number(
        _tableFactory, header->hash().hex(), boost::lexical_cast<std::string>(header->number()));
    ret = ret && getStorageSetter()->setNumber2Hash(_tableFactory,
                     boost::lexical_cast<std::string>(header->number()), header->hash().hex());
    if (!ret)
    {
        LEDGER_LOG(DEBUG) << LOG_BADGE("WriteHash2Number")
                          << LOG_DESC("Write row in SYS_HASH_2_NUMBER error")
                          << LOG_KV("blockHash", header->hash().hex());
    }
}

void Ledger::writeNumber2BlockHeader(
    const BlockHeader::Ptr& _header, const bcos::storage::TableFactoryInterface::Ptr& _tableFactory)
{
    auto encodedBlockHeader = std::make_shared<bytes>();
    auto emptyBlock = m_blockFactory->createBlock();
    emptyBlock->setBlockHeader(_header);
    emptyBlock->blockHeader()->encode(*encodedBlockHeader);

    bool ret = getStorageSetter()->setNumber2Header(_tableFactory,
        boost::lexical_cast<std::string>(_header->number()), asString(*encodedBlockHeader));
    if (!ret)
    {
        LEDGER_LOG(DEBUG) << LOG_BADGE("WriteNumber2Header")
                          << LOG_DESC("Write row in SYS_NUMBER_2_BLOCK_HEADER error")
                          << LOG_KV("blockNumber", _header->number());
    }
}
void Ledger::writeTotalTransactionCount(
    const Block::Ptr& block, const bcos::storage::TableFactoryInterface::Ptr& _tableFactory)
{
    // empty block
    if (block->transactionsSize() == 0 && block->receiptsSize() == 0)
    {
        LEDGER_LOG(ERROR) << LOG_BADGE("writeTotalTransactionCount")
                          << LOG_DESC("Empty block, stop update total tx count")
                          << LOG_KV("blockNumber", block->blockHeader()->number());
        return;
    }
    auto self = std::weak_ptr<Ledger>(std::dynamic_pointer_cast<Ledger>(shared_from_this()));
    getStorageGetter()->getCurrentState(SYS_KEY_TOTAL_TRANSACTION_COUNT, _tableFactory,
        [self, block, _tableFactory](Error::Ptr _error, bcos::storage::Entry::Ptr _totalTxEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("writeTotalTransactionCount")
                                  << LOG_DESC("Get SYS_KEY_TOTAL_TRANSACTION_COUNT error")
                                  << LOG_KV("blockNumber", block->blockHeader()->number());
                return;
            }
            auto ledger = self.lock();
            if (!ledger)
                return;
            int64_t totalTxCount = 0;
            auto totalTxStr = _totalTxEntry->getField(SYS_VALUE);
            if (!totalTxStr.empty())
            {
                totalTxCount += boost::lexical_cast<int64_t>(totalTxStr);
            }
            totalTxCount += block->transactionsSize();
            ledger->getStorageSetter()->setCurrentState(_tableFactory,
                SYS_KEY_TOTAL_TRANSACTION_COUNT, boost::lexical_cast<std::string>(totalTxCount));
        });

    getStorageGetter()->getCurrentState(SYS_KEY_TOTAL_FAILED_TRANSACTION, _tableFactory,
        [self, _tableFactory, block](
            Error::Ptr _error, bcos::storage::Entry::Ptr _totalFailedTxsEntry) {
            if (_error && _error->errorCode() != CommonError::SUCCESS)
            {
                LEDGER_LOG(ERROR) << LOG_BADGE("writeTotalTransactionCount")
                                  << LOG_DESC("Get SYS_KEY_TOTAL_FAILED_TRANSACTION error")
                                  << LOG_KV("blockNumber", block->blockHeader()->number());
                return;
            }
            auto ledger = self.lock();
            if (!ledger)
                return;
            auto receipts = blockReceiptListGetter(block);
            int64_t failedTransactions = 0;
            for (auto& receipt : *receipts)
            {
                // TODO: check receipt status
                if (receipt->status() != 0)
                {
                    ++failedTransactions;
                }
            }
            auto totalFailedTxsStr = _totalFailedTxsEntry->getField(SYS_VALUE);
            if (!totalFailedTxsStr.empty())
            {
                failedTransactions += boost::lexical_cast<int64_t>(totalFailedTxsStr);
            }
            ledger->getStorageSetter()->setCurrentState(_tableFactory,
                SYS_KEY_TOTAL_FAILED_TRANSACTION,
                boost::lexical_cast<std::string>(failedTransactions));
        });
}
void Ledger::writeNumber2Transactions(
    const Block::Ptr& _block, const TableFactoryInterface::Ptr& _tableFactory)
{
    if (_block->transactionsSize() == 0)
    {
        LEDGER_LOG(TRACE) << LOG_BADGE("WriteNumber2Txs") << LOG_DESC("empty txs in block")
                          << LOG_KV("blockNumber", _block->blockHeader()->number());
        return;
    }
    auto encodeBlock = std::make_shared<bytes>();
    auto emptyBlock = m_blockFactory->createBlock();
    auto number = _block->blockHeader()->number();
    for (size_t i = 0; i < _block->transactionsSize(); i++)
    {
        // Note: in some cases(block sync), the transactionHash fields maybe empty
        auto tx = _block->transaction(i);
        emptyBlock->appendTransactionHash(tx->hash());
    }

    emptyBlock->encode(*encodeBlock);
    bool ret = getStorageSetter()->setNumber2Txs(
        _tableFactory, boost::lexical_cast<std::string>(number), asString(*encodeBlock));
    if (!ret)
    {
        LEDGER_LOG(DEBUG) << LOG_BADGE("WriteNumber2Txs")
                          << LOG_DESC("Write row in SYS_NUMBER_2_TXS error")
                          << LOG_KV("blockNumber", _block->blockHeader()->number())
                          << LOG_KV("hash", _block->blockHeader()->hash().abridged());
    }
}
void Ledger::writeHash2Receipt(
    const bcos::protocol::Block::Ptr& _block, const TableFactoryInterface::Ptr& _tableFactory)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, _block->transactionsSize()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i)
            {
                auto tx = _block->transaction(i);
                // Note: in the tars-service, call receipt(_index) will make a new object, must add
                // the receipt here to maintain the lifetime, and in case of encodeReceipt be
                // released
                auto receipt = _block->receipt(i);
                auto encodeReceipt = receipt->encode();
                auto ret = getStorageSetter()->setHashToReceipt(
                    _tableFactory, tx->hash().hex(), asString(encodeReceipt));
                if (!ret)
                {
                    LEDGER_LOG(DEBUG)
                        << LOG_BADGE("writeHash2Receipt")
                        << LOG_DESC("Write row in SYS_HASH_2_RECEIPT error")
                        << LOG_KV("txHash", tx->hash().hex())
                        << LOG_KV("blockNumber", _block->blockHeader()->number())
                        << LOG_KV("blockHash", _block->blockHeader()->hash().abridged());
                }
            }
        });
}

void Ledger::notifyCommittedBlockNumber(protocol::BlockNumber _blockNumber)
{
    if (!m_committedBlockNotifier)
        return;
    m_committedBlockNotifier(_blockNumber, [_blockNumber](Error::Ptr _error) {
        if (!_error)
        {
            return;
        }
        LEDGER_LOG(WARNING) << LOG_BADGE("notifyCommittedBlockNumber")
                            << LOG_DESC("notify the block number failed")
                            << LOG_KV("blockNumber", _blockNumber);
    });
}

bool Ledger::buildGenesisBlock(
    LedgerConfig::Ptr _ledgerConfig, size_t _gasLimit, std::string _genesisData)
{
    LEDGER_LOG(INFO) << LOG_DESC("[#buildGenesisBlock]");
    if (_ledgerConfig->consensusTimeout() > SYSTEM_CONSENSUS_TIMEOUT_MAX ||
        _ledgerConfig->consensusTimeout() < SYSTEM_CONSENSUS_TIMEOUT_MIN)
    {
        LEDGER_LOG(ERROR) << LOG_BADGE("buildGenesisBlock")
                          << LOG_DESC("consensus timeout set error, return false")
                          << LOG_KV("consensusTimeout", _ledgerConfig->consensusTimeout());
        return false;
    }
    if (_gasLimit < TX_GAS_LIMIT_MIN)
    {
        LEDGER_LOG(ERROR) << LOG_BADGE("buildGenesisBlock")
                          << LOG_DESC("gas limit too low, return false")
                          << LOG_KV("gasLimit", _gasLimit)
                          << LOG_KV("gasLimitMin", TX_GAS_LIMIT_MIN);
        return false;
    }
    if (!getStorageGetter()->checkTableExist(SYS_NUMBER_2_BLOCK_HEADER, getMemoryTableFactory(0)))
    {
        LEDGER_LOG(INFO) << LOG_BADGE("buildGenesisBlock")
                         << LOG_DESC(
                                std::string(SYS_NUMBER_2_BLOCK_HEADER) + " table does not exist");
        getStorageSetter()->createTables(getMemoryTableFactory(0));
    };
    Block::Ptr block = nullptr;
    std::promise<Block::Ptr> blockPromise;
    auto blockFuture = blockPromise.get_future();
    getBlock(0, HEADER, [=, &blockPromise](Error::Ptr _error, Block::Ptr _block) {
        if (!_error)
        {
            // block is nullptr means need build a genesis block
            blockPromise.set_value(_block);
            LEDGER_LOG(INFO) << LOG_BADGE("buildGenesisBlock, get the genesis block success")
                             << LOG_KV("hash", _block->blockHeader()->hash().abridged());
            return;
        }
        blockPromise.set_value(nullptr);
        LEDGER_LOG(INFO) << LOG_BADGE("buildGenesisBlock")
                         << LOG_DESC("get genesis block callback error")
                         << LOG_KV("errorCode", _error->errorCode())
                         << LOG_KV("errorMsg", _error->errorMessage());
    });
    if (std::future_status::ready == blockFuture.wait_for(std::chrono::milliseconds(m_timeout)))
    {
        block = blockFuture.get();
    }
    // to build genesis block
    if (block == nullptr)
    {
        auto txLimit = _ledgerConfig->blockTxCountLimit();
        LEDGER_LOG(INFO) << LOG_DESC("Commit the genesis block") << LOG_KV("txLimit", txLimit);
        auto tableFactory = getMemoryTableFactory(0);
        // build a block
        block = m_blockFactory->createBlock();
        auto header = getBlockHeaderFactory()->createBlockHeader();
        header->setNumber(0);
        header->setExtraData(asBytes(_genesisData));
        block->setBlockHeader(header);
        try
        {
            tbb::parallel_invoke(
                [this, tableFactory, header]() {
                    writeHash2Number(header, tableFactory);
                    LEDGER_LOG(INFO) << LOG_DESC("[buildGenesisBlock]writeHash2Number success")
                                     << LOG_KV("hash", header->hash().abridged());
                },
                [this, tableFactory, _ledgerConfig]() {
                    getStorageSetter()->setSysConfig(tableFactory, SYSTEM_KEY_TX_COUNT_LIMIT,
                        boost::lexical_cast<std::string>(_ledgerConfig->blockTxCountLimit()), "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]set blockTxCountLimit success")
                        << LOG_KV("blockTxCountLimit", _ledgerConfig->blockTxCountLimit());
                },
                [this, tableFactory, _gasLimit]() {
                    getStorageSetter()->setSysConfig(tableFactory, SYSTEM_KEY_TX_GAS_LIMIT,
                        boost::lexical_cast<std::string>(_gasLimit), "0");
                    LEDGER_LOG(INFO) << LOG_DESC("[buildGenesisBlock]set gasLimit success")
                                     << LOG_KV("gasLimit", _gasLimit);
                },
                [this, tableFactory, _ledgerConfig]() {
                    getStorageSetter()->setSysConfig(tableFactory,
                        SYSTEM_KEY_CONSENSUS_LEADER_PERIOD,
                        boost::lexical_cast<std::string>(_ledgerConfig->leaderSwitchPeriod()), "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]set leaderSwitchPeriod success")
                        << LOG_KV("leaderSwitchPeriod", _ledgerConfig->leaderSwitchPeriod());
                },
                [this, tableFactory, _ledgerConfig]() {
                    getStorageSetter()->setSysConfig(tableFactory, SYSTEM_KEY_CONSENSUS_TIMEOUT,
                        boost::lexical_cast<std::string>(_ledgerConfig->consensusTimeout()), "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]set consensusTimeout success")
                        << LOG_KV("consensusTimeout", _ledgerConfig->consensusTimeout());
                });
            tbb::parallel_invoke(
                [this, tableFactory, _ledgerConfig]() {
                    getStorageSetter()->setConsensusConfig(
                        tableFactory, CONSENSUS_SEALER, _ledgerConfig->consensusNodeList(), "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]setSealerList success")
                        << LOG_KV("sealerNum", _ledgerConfig->consensusNodeList().size());
                },
                [this, tableFactory, _ledgerConfig]() {
                    getStorageSetter()->setConsensusConfig(
                        tableFactory, CONSENSUS_OBSERVER, _ledgerConfig->observerNodeList(), "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]setObserverList success")
                        << LOG_KV("observers", _ledgerConfig->observerNodeList().size());
                },
                [this, tableFactory, header]() {
                    writeNumber2BlockHeader(header, tableFactory);
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]writeNumber2BlockHeader success");
                },
                [this, tableFactory]() {
                    getStorageSetter()->setCurrentState(tableFactory, SYS_KEY_CURRENT_NUMBER, "0");
                    LEDGER_LOG(INFO)
                        << LOG_DESC("[buildGenesisBlock]set current blockNumber success");
                },
                [this, tableFactory]() {
                    getStorageSetter()->setCurrentState(
                        tableFactory, SYS_KEY_TOTAL_TRANSACTION_COUNT, "0");
                    LEDGER_LOG(INFO) << LOG_DESC(
                        "[buildGenesisBlock]set current total transaction count success");
                },
                [this, tableFactory]() {
                    getStorageSetter()->setCurrentState(
                        tableFactory, SYS_KEY_TOTAL_FAILED_TRANSACTION, "0");
                    LEDGER_LOG(INFO) << LOG_DESC(
                        "[buildGenesisBlock]set current total failed transaction count success");
                });
            // db sync commit
            LEDGER_LOG(INFO) << LOG_DESC("[buildGenesisBlock]commit all the table data");
            auto retPair = tableFactory->commit();
            if ((!retPair.second || retPair.second->errorCode() == CommonError::SUCCESS) &&
                retPair.first > 0)
            {
                LEDGER_LOG(INFO) << LOG_DESC("[buildGenesisBlock]Storage commit success")
                                 << LOG_KV("commitSize", retPair.first);
                return true;
            }
            else
            {
                LEDGER_LOG(ERROR) << LOG_DESC("[#buildGenesisBlock]Storage commit error");
                return false;
            }
        }
        catch (OpenSysTableFailed const& e)
        {
            LEDGER_LOG(FATAL)
                << LOG_DESC(
                       "[#buildGenesisBlock]System meets error when try to write block to storage")
                << LOG_KV("EINFO", boost::diagnostic_information(e));
            raise(SIGTERM);
            BOOST_THROW_EXCEPTION(
                OpenSysTableFailed() << errinfo_comment(" write block to storage failed."));
        }
    }
    else
    {
        LEDGER_LOG(INFO) << LOG_BADGE("buildGenesisBlock") << LOG_DESC("Already have the 0th block")
                         << LOG_KV("hash", block->blockHeader()->hash().abridged())
                         << LOG_KV("number", block->blockHeader()->number());
        auto header = block->blockHeader();
        LEDGER_LOG(INFO) << LOG_BADGE("buildGenesisBlock")
                         << LOG_DESC("Load genesis config from extraData");
        return header->extraData().toString() == _genesisData;
    }
    return true;
}
