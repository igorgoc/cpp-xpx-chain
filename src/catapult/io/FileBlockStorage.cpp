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

#include "FileBlockStorage.h"
#include "BlockElementSerializer.h"
#include "BlockStatementSerializer.h"
#include "BufferInputStreamAdapter.h"
#include "BufferedFileStream.h"
#include "FilesystemUtils.h"
#include "PodIoUtils.h"
#include <inttypes.h>

namespace catapult { namespace io {

	namespace {
		static constexpr uint64_t Unset_Directory_Id = std::numeric_limits<uint64_t>::max();
		static constexpr uint32_t Files_Per_Directory = 65536u;
		static constexpr auto Block_File_Extension = ".dat";
		static constexpr auto Block_Statement_File_Extension = ".stmt";

#pragma pack(push, 1)
		struct BlockChunkIndexEntry {
			uint32_t blockOffset;  // byte offset inside blocks.dat
			uint32_t blockSize;    // byte length of block element
			uint32_t stmtOffset;   // byte offset inside statements.dat
			uint32_t stmtSize;     // byte length of statement (0 if none)
		};
#pragma pack(pop)
		static_assert(sizeof(BlockChunkIndexEntry) == 16, "BlockChunkIndexEntry must be exactly 16 bytes");

		// region path utils

#ifdef _MSC_VER
#define SPRINTF sprintf_s
#else
#define SPRINTF sprintf
#endif

		boost::filesystem::path GetDirectoryPath(const std::string& baseDirectory, Height height) {
			char subDirectory[16];
			SPRINTF(subDirectory, "%05" PRId64, height.unwrap() / Files_Per_Directory);
			boost::filesystem::path path = baseDirectory;
			path /= subDirectory;
			if (!boost::filesystem::exists(path))
				boost::filesystem::create_directory(path);

			return path;
		}

		boost::filesystem::path GetBlocksDatPath(const std::string& baseDirectory, Height height) {
			auto path = GetDirectoryPath(baseDirectory, height);
			path /= "blocks.dat";
			return path;
		}

		boost::filesystem::path GetStatementsDatPath(const std::string& baseDirectory, Height height) {
			auto path = GetDirectoryPath(baseDirectory, height);
			path /= "statements.dat";
			return path;
		}

		boost::filesystem::path GetBlocksIdxPath(const std::string& baseDirectory, Height height) {
			auto path = GetDirectoryPath(baseDirectory, height);
			path /= "blocks.idx";
			return path;
		}

		boost::filesystem::path GetBlockPath(const std::string& baseDirectory, Height height, const char* extension) {
			auto path = GetDirectoryPath(baseDirectory, height);
			char filename[16];
			SPRINTF(filename, "%05" PRId64, height.unwrap() % Files_Per_Directory);
			path /= filename;
			path += extension;
			return path;
		}

		boost::filesystem::path GetHashFilePath(const std::string& baseDirectory, Height height) {
			auto path = GetDirectoryPath(baseDirectory, height);
			path /= "hashes.dat";
			return path;
		}

		boost::filesystem::path GetBlockStatementPath(const std::string& baseDirectory, Height height) {
			return GetBlockPath(baseDirectory, height, Block_Statement_File_Extension);
		}

		// endregion

		// region file utils

		bool IsRegularFile(const boost::filesystem::path& path) {
			return boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path);
		}

		auto OpenBlockFile(const std::string& baseDirectory, Height height, OpenMode mode = OpenMode::Read_Only) {
			auto blockPath = GetBlockPath(baseDirectory, height, Block_File_Extension);
			return std::make_unique<RawFile>(blockPath.generic_string().c_str(), mode);
		}

		auto OpenBlockStatementFile(const std::string& baseDirectory, Height height, OpenMode mode = OpenMode::Read_Only) {
			auto blockStatementPath = GetBlockStatementPath(baseDirectory, height);
			return RawFile(blockStatementPath.generic_string().c_str(), mode);
		}

		bool HasChunkIndexEntry(const std::string& baseDirectory, Height height, BlockChunkIndexEntry& entry) {
			auto idxPath = GetBlocksIdxPath(baseDirectory, height);
			if (!IsRegularFile(idxPath))
				return false;

			auto index = height.unwrap() % Files_Per_Directory;
			auto requiredSize = (index + 1) * sizeof(BlockChunkIndexEntry);

			try {
				RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
				if (idxFile.size() < requiredSize)
					return false;

				idxFile.seek(index * sizeof(BlockChunkIndexEntry));
				idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry), sizeof(BlockChunkIndexEntry)));
				return entry.blockSize > 0;
			} catch (...) {
				return false;
			}
		}

		// endregion
	}

	// region FileBlockStorage::HashFile

	FileBlockStorage::HashFile::HashFile(const std::string& dataDirectory)
			: m_dataDirectory(dataDirectory)
			, m_cachedDirectoryId(Unset_Directory_Id)
	{}

	namespace {
		std::unique_ptr<RawFile> OpenHashFile(const std::string& baseDirectory, Height height, OpenMode openMode) {
			auto hashFilePath = GetHashFilePath(baseDirectory, height);
			auto pHashFile = std::make_unique<RawFile>(hashFilePath.generic_string().c_str(), openMode, LockMode::None);
			// check that first hash file has at least two hashes inside.
			if (height.unwrap() < Files_Per_Directory && Hash256_Size * 2 > pHashFile->size())
				CATAPULT_THROW_RUNTIME_ERROR_1("hashes.dat has invalid size", pHashFile->size());

			return pHashFile;
		}

		void SeekHashFile(RawFile& hashFile, Height height) {
			auto index = height.unwrap() % Files_Per_Directory;
			hashFile.seek(index * Hash256_Size);
		}
	}

	model::HashRange FileBlockStorage::HashFile::loadHashesFrom(Height height, size_t numHashes) const {
		uint8_t* pData = nullptr;
		auto range = model::HashRange::PrepareFixed(numHashes, &pData);

		while (numHashes) {
			auto pHashFile = OpenHashFile(m_dataDirectory, height, OpenMode::Read_Only);
			SeekHashFile(*pHashFile, height);

			auto count = Files_Per_Directory - (height.unwrap() % Files_Per_Directory);
			count = std::min<size_t>(numHashes, count);

			pHashFile->read(MutableRawBuffer(pData, count * Hash256_Size));

			pData += count * Hash256_Size;
			numHashes -= count;
			height = height + Height(count);
		}

		return range;
	}

	void FileBlockStorage::HashFile::save(Height height, const Hash256& hash) {
		auto currentId = height.unwrap() / Files_Per_Directory;
		if (m_cachedDirectoryId != currentId) {
			m_pCachedHashFile = OpenHashFile(m_dataDirectory, height, OpenMode::Read_Append);
			m_cachedDirectoryId = currentId;
		}

		SeekHashFile(*m_pCachedHashFile, height);
		m_pCachedHashFile->write(hash);
	}

	void FileBlockStorage::HashFile::reset() {
		m_cachedDirectoryId = Unset_Directory_Id;
		m_pCachedHashFile.reset();
	}

	// endregion

	// region FileBlockStorage::ChunkWriter

	FileBlockStorage::ChunkWriter::ChunkWriter(const std::string& dataDirectory)
			: m_dataDirectory(dataDirectory)
			, m_cachedDirectoryId(Unset_Directory_Id)
	{}

	namespace {
		class RawFileOutputStreamAdapter : public OutputStream {
		public:
			explicit RawFileOutputStreamAdapter(RawFile& rawFile) : m_rawFile(rawFile)
			{}

		public:
			void write(const RawBuffer& buffer) override {
				m_rawFile.write(buffer);
			}

			void flush() override {
				CATAPULT_THROW_INVALID_ARGUMENT("flush not supported");
			}

		private:
			RawFile& m_rawFile;
		};
	}

	void FileBlockStorage::ChunkWriter::save(Height height, const model::BlockElement& blockElement) {
		auto currentId = height.unwrap() / Files_Per_Directory;
		if (m_cachedDirectoryId != currentId || !m_pCachedBlocksFile) {
			reset();
			auto blocksDatPath = GetBlocksDatPath(m_dataDirectory, height);
			m_pCachedBlocksFile = std::make_unique<RawFile>(blocksDatPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);

			auto stmtDatPath = GetStatementsDatPath(m_dataDirectory, height);
			m_pCachedStmtFile = std::make_unique<RawFile>(stmtDatPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);

			auto idxPath = GetBlocksIdxPath(m_dataDirectory, height);
			m_pCachedIdxFile = std::make_unique<RawFile>(idxPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);

			m_cachedDirectoryId = currentId;
		}

		BlockChunkIndexEntry entry{ 0, 0, 0, 0 };

		// 1. Append block element payload into chunked blocks.dat
		{
			auto blockOffset = m_pCachedBlocksFile->size();
			if (blockOffset > std::numeric_limits<uint32_t>::max())
				CATAPULT_THROW_RUNTIME_ERROR_1("blocks.dat exceeded 4GB for directory at height", height);

			entry.blockOffset = static_cast<uint32_t>(blockOffset);

			m_pCachedBlocksFile->seek(blockOffset);
			RawFileOutputStreamAdapter streamAdapter(*m_pCachedBlocksFile);
			WriteBlockElement(streamAdapter, blockElement);

			auto blockSize = m_pCachedBlocksFile->size() - blockOffset;
			if (blockSize > std::numeric_limits<uint32_t>::max())
				CATAPULT_THROW_RUNTIME_ERROR_1("block size exceeded 4GB at height", height);

			entry.blockSize = static_cast<uint32_t>(blockSize);
		}

		// 2. Append optional statement payload into chunked statements.dat
		if (blockElement.OptionalStatement) {
			auto stmtOffset = m_pCachedStmtFile->size();
			if (stmtOffset > std::numeric_limits<uint32_t>::max())
				CATAPULT_THROW_RUNTIME_ERROR_1("statements.dat exceeded 4GB for directory at height", height);

			entry.stmtOffset = static_cast<uint32_t>(stmtOffset);

			m_pCachedStmtFile->seek(stmtOffset);
			RawFileOutputStreamAdapter streamAdapter(*m_pCachedStmtFile);
			WriteBlockStatement(streamAdapter, *blockElement.OptionalStatement);

			auto stmtSize = m_pCachedStmtFile->size() - stmtOffset;
			if (stmtSize > std::numeric_limits<uint32_t>::max())
				CATAPULT_THROW_RUNTIME_ERROR_1("statement size exceeded 4GB at height", height);

			entry.stmtSize = static_cast<uint32_t>(stmtSize);
		}

		// 3. Write index entry into blocks.idx
		{
			auto index = height.unwrap() % Files_Per_Directory;
			auto targetOffset = index * sizeof(BlockChunkIndexEntry);
			if (m_pCachedIdxFile->size() < targetOffset) {
				std::vector<uint8_t> zeros(targetOffset - m_pCachedIdxFile->size(), 0);
				m_pCachedIdxFile->seek(m_pCachedIdxFile->size());
				m_pCachedIdxFile->write(zeros);
			}
			m_pCachedIdxFile->seek(targetOffset);
			m_pCachedIdxFile->write(RawBuffer(reinterpret_cast<const uint8_t*>(&entry), sizeof(BlockChunkIndexEntry)));
		}
	}

	void FileBlockStorage::ChunkWriter::reset() {
		m_cachedDirectoryId = Unset_Directory_Id;
		m_pCachedBlocksFile.reset();
		m_pCachedStmtFile.reset();
		m_pCachedIdxFile.reset();
	}

	// endregion

	// region ctor

	FileBlockStorage::FileBlockStorage(const std::string& dataDirectory, FileBlockStorageMode mode)
			: m_dataDirectory(dataDirectory)
			, m_mode(mode)
			, m_hashFile(m_dataDirectory)
			, m_chunkWriter(m_dataDirectory)
			, m_indexFile((boost::filesystem::path(m_dataDirectory) / "index.dat").generic_string())
	{}

	// endregion

	// region LightBlockStorage

	Height FileBlockStorage::chainHeight() const {
		return m_indexFile.exists() ? Height(m_indexFile.get()) : Height(0);
	}

	model::HashRange FileBlockStorage::loadHashesFrom(Height height, size_t maxHashes) const {
		if (FileBlockStorageMode::Hash_Index != m_mode)
			CATAPULT_THROW_INVALID_ARGUMENT("loadHashesFrom is not supported when Hash_Index mode is disabled");

		auto currentHeight = chainHeight();
		if (Height(0) == height || currentHeight < height)
			return model::HashRange();

		auto numAvailableHashes = static_cast<size_t>((currentHeight - height).unwrap() + 1);
		auto numHashes = std::min(maxHashes, numAvailableHashes);
		return m_hashFile.loadHashesFrom(height, numHashes);
	}

	void FileBlockStorage::saveBlock(const model::BlockElement& blockElement) {
		auto currentHeight = chainHeight();
		auto height = blockElement.Block.Height;

		if (height != currentHeight + Height(1)) {
			std::ostringstream out;
			out << "cannot save block with height " << height << " when storage height is " << currentHeight;
			CATAPULT_THROW_INVALID_ARGUMENT(out.str().c_str());
		}

		m_chunkWriter.save(height, blockElement);

		if (FileBlockStorageMode::Hash_Index == m_mode)
			m_hashFile.save(height, blockElement.EntityHash);

		if (height > currentHeight)
			m_indexFile.set(height.unwrap());
	}

	void FileBlockStorage::dropBlocksAfter(Height height) {
		m_hashFile.reset();
		m_chunkWriter.reset();
		m_indexFile.set(height.unwrap());

		if (Height(0) == height)
			return;

		auto nextHeight = height + Height(1);
		BlockChunkIndexEntry nextEntry;
		if (HasChunkIndexEntry(m_dataDirectory, nextHeight, nextEntry)) {
			boost::system::error_code ec;

			auto blocksDatPath = GetBlocksDatPath(m_dataDirectory, nextHeight);
			if (boost::filesystem::is_regular_file(blocksDatPath))
				boost::filesystem::resize_file(blocksDatPath, nextEntry.blockOffset, ec);

			auto stmtDatPath = GetStatementsDatPath(m_dataDirectory, nextHeight);
			if (boost::filesystem::is_regular_file(stmtDatPath))
				boost::filesystem::resize_file(stmtDatPath, nextEntry.stmtOffset, ec);

			auto idxPath = GetBlocksIdxPath(m_dataDirectory, nextHeight);
			if (boost::filesystem::is_regular_file(idxPath)) {
				auto nextIndex = nextHeight.unwrap() % Files_Per_Directory;
				auto targetOffset = nextIndex * sizeof(BlockChunkIndexEntry);
				try {
					RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);
					if (idxFile.size() > targetOffset) {
						std::vector<uint8_t> zeros(idxFile.size() - targetOffset, 0);
						idxFile.seek(targetOffset);
						idxFile.write(zeros);
					}
				} catch (...) {}
			}
		}
	}

	// endregion

	// region BlockStorage

	namespace {
		class RawFileInputStreamAdapter : public InputStream {
		public:
			explicit RawFileInputStreamAdapter(RawFile& rawFile) : m_rawFile(rawFile)
			{}

		public:
			bool eof() const override {
				CATAPULT_THROW_INVALID_ARGUMENT("eof not supported");
			}

			void read(const MutableRawBuffer& buffer) override {
				m_rawFile.read(buffer);
			}

		private:
			RawFile& m_rawFile;
		};

		std::shared_ptr<model::Block> ReadBlock(RawFile& blockFile) {
			auto size = Read32(blockFile);
			blockFile.seek(0);

			auto pBlock = utils::MakeSharedWithSize<model::Block>(size);
			blockFile.read({ reinterpret_cast<uint8_t*>(pBlock.get()), size });
			return pBlock;
		}
	}

	std::shared_ptr<const model::Block> FileBlockStorage::loadBlock(Height height) const {
		requireHeight(height, "block");

		BlockChunkIndexEntry entry;
		if (HasChunkIndexEntry(m_dataDirectory, height, entry)) {
			auto blocksDatPath = GetBlocksDatPath(m_dataDirectory, height);
			RawFile blocksFile(blocksDatPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			blocksFile.seek(entry.blockOffset);

			auto size = Read32(blocksFile);
			blocksFile.seek(entry.blockOffset);

			auto pBlock = utils::MakeSharedWithSize<model::Block>(size);
			blocksFile.read({ reinterpret_cast<uint8_t*>(pBlock.get()), size });
			return pBlock;
		}

		// Fallback for legacy single-file storage (e.g. genesis / seed nemesis)
		auto blockPath = GetBlockPath(m_dataDirectory, height, Block_File_Extension);
		if (IsRegularFile(blockPath)) {
			auto pBlockFile = OpenBlockFile(m_dataDirectory, height);
			return ReadBlock(*pBlockFile);
		}

		CATAPULT_THROW_RUNTIME_ERROR_1("block not found at height", height);
	}

	std::shared_ptr<const model::BlockElement> FileBlockStorage::loadBlockElement(Height height) const {
		requireHeight(height, "block element");

		BlockChunkIndexEntry entry;
		if (HasChunkIndexEntry(m_dataDirectory, height, entry)) {
			auto blocksDatPath = GetBlocksDatPath(m_dataDirectory, height);
			RawFile blocksFile(blocksDatPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			blocksFile.seek(entry.blockOffset);

			std::vector<uint8_t> blockBuffer(entry.blockSize);
			blocksFile.read(blockBuffer);

			BufferInputStreamAdapter streamAdapter(blockBuffer);
			auto pBlockElement = ReadBlockElement(streamAdapter);

			if (entry.stmtSize > 0) {
				auto stmtDatPath = GetStatementsDatPath(m_dataDirectory, height);
				RawFile stmtFile(stmtDatPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
				stmtFile.seek(entry.stmtOffset);

				std::vector<uint8_t> stmtBuffer(entry.stmtSize);
				stmtFile.read(stmtBuffer);

				BufferInputStreamAdapter stmtStream(stmtBuffer);
				auto pBlockStatement = std::make_shared<model::BlockStatement>();
				ReadBlockStatement(stmtStream, *pBlockStatement);
				const_cast<model::BlockElement&>(*pBlockElement).OptionalStatement = std::move(pBlockStatement);
			}

			return std::move(pBlockElement);
		}

		// Fallback for legacy single-file storage (e.g. genesis / seed nemesis)
		auto blockPath = GetBlockPath(m_dataDirectory, height, Block_File_Extension);
		if (IsRegularFile(blockPath)) {
			auto pBlockFile = OpenBlockFile(m_dataDirectory, height);
			RawFileInputStreamAdapter streamAdapter(*pBlockFile);
			auto pBlockElement = ReadBlockElement(streamAdapter);

			if (pBlockFile->position() != pBlockFile->size())
				CATAPULT_THROW_RUNTIME_ERROR_1("additional data after block at height", height);

			return std::move(pBlockElement);
		}

		CATAPULT_THROW_RUNTIME_ERROR_1("block element not found at height", height);
	}

	std::pair<std::vector<uint8_t>, bool> FileBlockStorage::loadBlockStatementData(Height height) const {
		requireHeight(height, "block statement data");

		BlockChunkIndexEntry entry;
		if (HasChunkIndexEntry(m_dataDirectory, height, entry)) {
			if (entry.stmtSize == 0)
				return std::make_pair(std::vector<uint8_t>(), false);

			auto stmtDatPath = GetStatementsDatPath(m_dataDirectory, height);
			RawFile stmtFile(stmtDatPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			stmtFile.seek(entry.stmtOffset);

			std::vector<uint8_t> blockStatement(entry.stmtSize);
			stmtFile.read(blockStatement);
			return std::make_pair(std::move(blockStatement), true);
		}

		// Fallback for legacy single-file storage
		auto path = GetBlockStatementPath(m_dataDirectory, height);
		if (!IsRegularFile(path))
			return std::make_pair(std::vector<uint8_t>(), false);

		auto blockStatementFile = OpenBlockStatementFile(m_dataDirectory, height);
		std::vector<uint8_t> blockStatement;
		blockStatement.resize(blockStatementFile.size());
		blockStatementFile.read(blockStatement);
		return std::make_pair(std::move(blockStatement), true);
	}

	// endregion

	// region PrunableBlockStorage

	void FileBlockStorage::purge() {
		// remove everything under the directory
		m_hashFile.reset();
		m_chunkWriter.reset();
		PurgeDirectory(m_dataDirectory);
	}

	// endregion

	// region requireHeight

	void FileBlockStorage::requireHeight(Height height, const char* description) const {
		auto chainHeight = this->chainHeight();
		if (height <= chainHeight)
			return;

		std::ostringstream out;
		out << "cannot load " << description << " at height (" << height << ") greater than chain height (" << chainHeight << ")";
		CATAPULT_THROW_INVALID_ARGUMENT(out.str().c_str());
	}

	// endregion
}}
