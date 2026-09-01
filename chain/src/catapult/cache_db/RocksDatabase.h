#ifndef CATAPULT_CACHE_DB_ROCKSDATABASE_H
#define CATAPULT_CACHE_DB_ROCKSDATABASE_H

#include "RocksInclude.h"
#include "catapult/utils/FileSize.h"
#include <boost/filesystem.hpp>
#include <memory>
#include <vector>
#include <mutex>

namespace catapult { namespace cache {

	class RdbDataIterator {
	private:
		struct Impl;
		std::shared_ptr<Impl> m_pImpl;
		bool m_isFound;

	public:
		enum class StorageStrategy { Allocate, Do_Not_Allocate };

		RdbDataIterator(StorageStrategy storageStrategy);
		RdbDataIterator();
		~RdbDataIterator();
		RdbDataIterator(RdbDataIterator&&) = default;
		RdbDataIterator& operator=(RdbDataIterator&&) = default;

		static RdbDataIterator End();

		bool operator==(const RdbDataIterator& rhs) const;
		bool operator!=(const RdbDataIterator& rhs) const;

		rocksdb::PinnableSlice& storage() const;
		void setFound(bool found);
		RawBuffer buffer() const;
	};

	struct RocksDatabaseSettings {
		std::string DatabaseDirectory;
		std::vector<std::string> ColumnFamilyNames;
		utils::FileSize MaxDatabaseWriteBatchSize;
		FilterPruningMode PruningMode;

		RocksDatabaseSettings();
		RocksDatabaseSettings(
				const std::string& databaseDirectory,
				const std::vector<std::string>& columnFamilyNames,
				utils::FileSize maxDatabaseWriteBatchSize,
				FilterPruningMode pruningMode);
	};

	class RocksDatabase {
	private:
		RocksDatabaseSettings m_settings;
		std::unique_ptr<rocksdb::DB> m_pDb;
		std::vector<rocksdb::ColumnFamilyHandle*> m_handles;
		std::unique_ptr<rocksdb::WriteBatch> m_pWriteBatch;
		PruningFilter m_pruningFilter;
		std::mutex m_writeMutex;

	public:
		RocksDatabase() = default;
		RocksDatabase(const RocksDatabaseSettings& settings);
		~RocksDatabase();

		const std::vector<std::string>& columnFamilyNames() const;
		bool canPrune() const;

		void get(size_t columnId, const rocksdb::Slice& key, RdbDataIterator& result);
		void getLowerOrEqual(size_t columnId, const rocksdb::Slice& key, RdbDataIterator& result);
		void getAll(size_t columnId, std::vector<std::string>& result);
		void put(size_t columnId, const rocksdb::Slice& key, const std::string& value);
		void del(size_t columnId, const rocksdb::Slice& key);
		size_t prune(size_t columnId, uint64_t boundary);
		void flush();

	private:
		void saveIfBatchFull();
	};

}}

#endif
