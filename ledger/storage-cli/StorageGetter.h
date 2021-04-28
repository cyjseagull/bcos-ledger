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
 * @brief storage getter interfaces call layer
 * @file StorageGetter.h
 * @author: kyonRay
 * @date 2021-04-23
 */

#pragma once
#include "interfaces/protocol/Block.h"
#include "interfaces/protocol/BlockFactory.h"
#include "interfaces/protocol/Transaction.h"
#include "interfaces/protocol/TransactionReceipt.h"
#include "interfaces/protocol/BlockHeader.h"
#include "interfaces/protocol/BlockHeaderFactory.h"
#include "interfaces/storage/TableInterface.h"

namespace bcos::ledger
{
using stringsPair = std::pair<std::string, std::string>;
class StorageGetter final{
public:
    using Ptr = std::shared_ptr<StorageGetter>;
    StorageGetter() = default;

    inline static StorageGetter::Ptr storageGetterFactory(){
        return std::make_shared<StorageGetter>();
    }

    /**
     * @brief get transactions in SYS_NUMBER_2_TXS table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @return encoded block data, where txs contains
     */
    std::string getTxsFromStorage(const bcos::protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory);

    /**
     * @brief get receipts in SYS_NUMBER_2_RECEIPTS table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @return encoded block data, where receipts contains
     */
    std::string getReceiptsFromStorage(const bcos::protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory);

    /**
     * @brief get block header in SYS_NUMBER_2_BLOCK_HEADER table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @return encoded block header data, which can be decoded by BlockHeader.decode()
     */
    std::string getBlockHeaderFromStorage(const bcos::protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory);

    /**
     * @brief get encoded block in SYS_NUMBER_2_BLOCK table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @return encoded block data, which can be decoded by Block.decode()
     */
    std::string getFullBlockFromStorage(const bcos::protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory);

    /**
     * @brief get nonce list in SYS_BLOCK_NUMBER_2_NONCES table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @return encoded nonce list
     */
    std::string getNoncesFromStorage(const bcos::protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory);

    /**
     * @brief get a encode data by block hash in _tableName table
     * @param _blockHash the hash of block
     * @param _tableFactory
     * @param _tableName the table name of the table to get data
     * @return return encoded data
     */
    std::string getterByBlockHash(const std::string& _blockHash,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory,
        const std::string& _tableName);

    /**
     * @brief get a encode data by block number in _tableName table
     * @param _blockNumber the number of block
     * @param _tableFactory
     * @param _tableName the table name of the table to get data
     * @return return encoded data
     */
    std::string getterByBlockNumber(const protocol::BlockNumber& _blockNumber,
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory,
        const std::string& _tableName);

    /**
     * @brief get block number by blockHash
     * @param _tableFactory
     * @param _tableName the table name of the table to get data
     * @param _hash hash string, it can be blockHash
     * @return return string data of block number
     */
    std::string getBlockNumberByHash(const bcos::storage::TableFactoryInterface::Ptr& _tableFactory,
        const std::string& _tableName, const std::string& _hash);

    /**
     * @brief get current state in row
     * @param _tableFactory
     * @param _row
     * @return
     */
    std::string getCurrentState(const bcos::storage::TableFactoryInterface::Ptr & _tableFactory,
        const std::string& _row);

    /**
     * @brief get sys config in table SYS_CONFIG
     * @param _tableFactory
     * @param _key row key in table
     * @return return a string pair <value, enableBlockNumber>
     */
    std::shared_ptr<stringsPair> getSysConfig(
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory, const std::string& _key);

    /**
     * @brief get block number and index by tx hash in table SYS_TX_HASH_2_BLOCK_NUMBER
     * @param _tableFactory
     * @param _hash transaction hash
     * @return return a string pair <number, transaction index>
     */
    std::shared_ptr<stringsPair> getBlockNumberAndIndexByHash(
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory, const std::string& _hash);

    /**
     * @brief select field from tableName where row=_row
     * @param _tableFactory
     * @param _tableName
     * @param _row
     * @param _field
     * @return string of field
     */
    std::string tableGetterByRowAndField(const bcos::storage::TableFactoryInterface::Ptr & _tableFactory,
        const std::string& _tableName, const std::string& _row, const std::string& _field);

    /**
     * @brief select field1, field2 from tableName where row=_row
     * @param _tableFactory
     * @param _tableName
     * @param _row
     * @param _field1
     * @param _filed2
     * @return
     */
    std::shared_ptr<stringsPair> stringsPairGetterByRowAndFields(
        const bcos::storage::TableFactoryInterface::Ptr& _tableFactory,
        const std::string& _tableName, const std::string& _row, const std::string& _field1,
        const std::string& _filed2);

};
}