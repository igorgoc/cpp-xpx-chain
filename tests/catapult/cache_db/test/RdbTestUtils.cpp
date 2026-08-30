/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "RdbTestUtils.h"
#include "SliceTestUtils.h"
#include "tests/TestHarness.h"

namespace catapult { namespace test {

	namespace {
		void PutValue(rocksdb::DB& db, rocksdb::ColumnFamilyHandle* column, size_t key) {
			db.Put(rocksdb::WriteOptions(), column, test::ToSlice(key), EvenKeyToValue(key));
		}
	}

	std::string EvenKeyToValue(size_t key) {
		return std::to_string(123400 + key);
	}

	DbSeeder CreateEvenDbSeeder(size_t numKeys) {
		return [numKeys] (auto& db, const auto& columns) {
			for (auto i = 0u; i < numKeys; ++i)
				PutValue(db, columns[0], i * 2);
		};
	}

	DbInitializer::DbInitializer(const ColumnNames& columns, const DbSeeder& seeder) : DbInitializer(columns, seeder, nullptr)
	{}

	DbInitializer::DbInitializer(const ColumnNames& columns, const DbSeeder& seeder, const rocksdb::CompactionFilter* compactionFilter) {
		seedDb(m_dbDirGuard.name(), columns, seeder, compactionFilter);
	}

	namespace {
		// Overload for RocksDB 9.x+ (takes std::unique_ptr<rocksdb::DB>*)
		template<typename TDb = rocksdb::DB>
		auto OpenDbCompatible(
				const rocksdb::Options& options,
				const std::string& name,
				const std::vector<rocksdb::ColumnFamilyDescriptor>& columnFamilies,
				std::vector<rocksdb::ColumnFamilyHandle*>* pHandles,
				std::unique_ptr<rocksdb::DB>& pDb,
				int)
			-> decltype(TDb::Open(options, name, columnFamilies, pHandles, &pDb)) {
			return TDb::Open(options, name, columnFamilies, pHandles, &pDb);
		}

		// Overload for RocksDB <= 8.x (takes rocksdb::DB**)
		template<typename TDb = rocksdb::DB>
		auto OpenDbCompatible(
				const rocksdb::Options& options,
				const std::string& name,
				const std::vector<rocksdb::ColumnFamilyDescriptor>& columnFamilies,
				std::vector<rocksdb::ColumnFamilyHandle*>* pHandles,
				std::unique_ptr<rocksdb::DB>& pDb,
				long) {
			rocksdb::DB* rawDb = nullptr;
			auto status = TDb::Open(options, name, columnFamilies, pHandles, &rawDb);
			pDb.reset(rawDb);
			return status;
		}
	}

	bool DbInitializer::seedDb(
			const std::string& dbDir,
			const std::vector<std::string>& columns,
			const DbSeeder& seeder,
			const rocksdb::CompactionFilter* compactionFilter) {
		if (!seeder)
			return false;

		std::unique_ptr<rocksdb::DB> pDb;
		rocksdb::Options dbOptions;
		dbOptions.create_if_missing = true;
		dbOptions.create_missing_column_families = true;
		dbOptions.max_open_files = 512;

		rocksdb::ColumnFamilyOptions defaultColumnOptions;
		defaultColumnOptions.compaction_filter = compactionFilter;

		std::vector<rocksdb::ColumnFamilyDescriptor> columnFamilies;
		for (const auto& name : columns)
			columnFamilies.push_back(rocksdb::ColumnFamilyDescriptor(name, defaultColumnOptions));

		std::vector<rocksdb::ColumnFamilyHandle*> handles;
		auto status = OpenDbCompatible(dbOptions, dbDir, columnFamilies, &handles, pDb, 0);
		std::vector<std::shared_ptr<rocksdb::ColumnFamilyHandle>> handleGuards;
		for (auto* pHandle : handles) {
			handleGuards.emplace_back(pHandle, [&db = *pDb](auto* pColumnHandle) {
				db.DestroyColumnFamilyHandle(pColumnHandle);
			});
		}

		seeder(*pDb, handles);
		return true;
	}

	RdbTestContext::RdbTestContext(const cache::RocksDatabaseSettings& settings, const DbSeeder& seeder)
			: DbInitializer(settings.ColumnFamilyNames, seeder)
			, m_database(settings)
	{}

	cache::RocksDatabase& RdbTestContext::database() {
		return m_database;
	}

	void AssertIteratorValue(const std::string& value, const cache::RdbDataIterator& iter) {
		ASSERT_NE(cache::RdbDataIterator::End(), iter);
		EXPECT_EQ(value.size(), iter.storage().size());
		EXPECT_EQ(value, std::string(iter.storage().data(), iter.storage().size()));
	}
}}
