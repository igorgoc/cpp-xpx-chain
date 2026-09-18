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
#include <cctype>
#include <inttypes.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

		struct RollbackJournalHeader {
			uint32_t magic;           // 0x5349524A ('SIRJ')
			uint32_t version;         // 1
			uint64_t targetHeight;    // Target height
			uint64_t previousHeight;  // Height before rollback
			uint64_t targetBlockEnd;  // Target file size for blocks.dat
			uint64_t targetStmtEnd;   // Target file size for statements.dat
			uint32_t targetIndex;     // Index from which blocks.idx should be zeroed
			uint32_t padding;
		};
#pragma pack(pop)
		static_assert(sizeof(BlockChunkIndexEntry) == 16, "BlockChunkIndexEntry must be exactly 16 bytes");
		static_assert(sizeof(RollbackJournalHeader) == 48, "RollbackJournalHeader must be exactly 48 bytes");
		static constexpr uint32_t Rollback_Journal_Magic = 0x5349524Au;
		static constexpr uint32_t Rollback_Journal_Version = 1u;

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

		bool TryGetChunkId(const boost::filesystem::path& path, uint64_t& chunkId) {
			auto filename = path.filename().string();
			if (filename.length() != 5)
				return false;

			for (char c : filename) {
				if (!std::isdigit(static_cast<unsigned char>(c)))
					return false;
			}

			try {
				chunkId = std::stoull(filename);
				return true;
			} catch (...) {
				return false;
			}
		}

		void SyncDirectory(const std::string& directory) {
#ifndef _WIN32
			int dirFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
			if (dirFd == -1)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to open directory for fsync: " + directory).c_str());

			if (::fsync(dirFd) != 0) {
				::close(dirFd);
				CATAPULT_THROW_FILE_IO_ERROR(("failed to fsync directory: " + directory).c_str());
			}

			if (::close(dirFd) != 0)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to close directory after fsync: " + directory).c_str());
#else
			(void)directory;
#endif
		}

		void SyncFile(const boost::filesystem::path& path) {
			if (!boost::filesystem::is_regular_file(path))
				return;

#ifdef _WIN32
			int fd = _open(path.generic_string().c_str(), _O_RDWR | _O_BINARY);
			if (fd == -1)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to open file for flush: " + path.string()).c_str());
			HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
			if (h == INVALID_HANDLE_VALUE || !FlushFileBuffers(h)) {
				_close(fd);
				CATAPULT_THROW_FILE_IO_ERROR(("failed to flush file buffers: " + path.string()).c_str());
			}
			if (_close(fd) != 0)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to close file after flush: " + path.string()).c_str());
#else
			int fd = ::open(path.string().c_str(), O_RDWR | O_CLOEXEC);
			if (fd == -1)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to open file for fsync: " + path.string()).c_str());
			if (::fsync(fd) != 0) {
				::close(fd);
				CATAPULT_THROW_FILE_IO_ERROR(("failed to fsync file: " + path.string()).c_str());
			}
			if (::close(fd) != 0)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to close file after fsync: " + path.string()).c_str());
#endif
		}

		void ValidateRollbackJournal(const std::string& dataDirectory, const RollbackJournalHeader& header, const IndexFile& indexFile) {
			if (header.magic != Rollback_Journal_Magic || header.version != Rollback_Journal_Version)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: invalid magic or version");

			if (header.targetHeight > header.previousHeight)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetHeight is greater than previousHeight");

			auto expectedTargetIndex = (header.targetHeight == 0) ? 0 : static_cast<uint32_t>((header.targetHeight % Files_Per_Directory) + 1);
			if (header.targetIndex != expectedTargetIndex)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetIndex does not match targetHeight");

			auto currentHeight = indexFile.exists() ? indexFile.get() : 0;
			if (currentHeight != header.previousHeight && currentHeight != header.targetHeight)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: index.dat height does not match journal state");

			if (header.targetHeight == 0) {
				if (header.targetBlockEnd != 0 || header.targetStmtEnd != 0)
					CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetBlockEnd or targetStmtEnd must be 0 for targetHeight 0");
				return;
			}

			auto height = Height(header.targetHeight);
			auto retainedIndex = height.unwrap() % Files_Per_Directory;
			auto retainedChunkId = height.unwrap() / Files_Per_Directory;

			auto blocksDatPath = GetBlocksDatPath(dataDirectory, height);
			if (!boost::filesystem::is_regular_file(blocksDatPath))
				CATAPULT_THROW_FILE_IO_ERROR("missing blocks.dat during rollback");

			auto idxPath = GetBlocksIdxPath(dataDirectory, height);
			if (!boost::filesystem::is_regular_file(idxPath))
				CATAPULT_THROW_FILE_IO_ERROR("missing blocks.idx during rollback");

			BlockChunkIndexEntry retainedEntry;
			if (!HasChunkIndexEntry(dataDirectory, height, retainedEntry))
				CATAPULT_THROW_FILE_IO_ERROR("missing retained block index entry during rollback");

			if (retainedIndex == Files_Per_Directory - 1) {
				if (header.targetBlockEnd != 0 || header.targetStmtEnd != 0)
					CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: boundary targetBlockEnd or targetStmtEnd must be 0");
				return;
			}

			auto currentBlocksSize = boost::filesystem::file_size(blocksDatPath);
			if (header.targetBlockEnd > currentBlocksSize)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetBlockEnd exceeds actual blocks.dat size");

			auto expectedBlockEnd = static_cast<uint64_t>(retainedEntry.blockOffset) + static_cast<uint64_t>(retainedEntry.blockSize);
			if (header.targetBlockEnd != expectedBlockEnd)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetBlockEnd does not match index entry");

			uint64_t expectedStmtEnd = 0;
			for (auto h = height; h > Height(0) && (h.unwrap() / Files_Per_Directory == retainedChunkId); h = h - Height(1)) {
				BlockChunkIndexEntry stmtEntry;
				if (HasChunkIndexEntry(dataDirectory, h, stmtEntry) && stmtEntry.stmtSize > 0) {
					expectedStmtEnd = static_cast<uint64_t>(stmtEntry.stmtOffset) + static_cast<uint64_t>(stmtEntry.stmtSize);
					break;
				}
			}
			if (header.targetStmtEnd != expectedStmtEnd)
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetStmtEnd does not match index entries");

			auto stmtDatPath = GetStatementsDatPath(dataDirectory, height);
			if (header.targetStmtEnd > 0) {
				if (!boost::filesystem::is_regular_file(stmtDatPath))
					CATAPULT_THROW_FILE_IO_ERROR("missing statements.dat during rollback when targetStmtEnd > 0");

				auto currentStmtSize = boost::filesystem::file_size(stmtDatPath);
				if (header.targetStmtEnd > currentStmtSize)
					CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: targetStmtEnd exceeds actual statements.dat size");
			}
		}

		void ApplyRollbackOperations(const std::string& dataDirectory, const RollbackJournalHeader& header, IndexFile& indexFile) {
			ValidateRollbackJournal(dataDirectory, header, indexFile);

			auto height = Height(header.targetHeight);
			auto retainedChunkId = (Height(0) == height) ? 0 : (height.unwrap() / Files_Per_Directory);
			auto retainedIndex = (Height(0) == height) ? 0 : (height.unwrap() % Files_Per_Directory);

			boost::system::error_code ec;

			// 1. Truncate and flush files inside retained chunk
			if (Height(0) == height) {
				auto blocksDatPath = GetBlocksDatPath(dataDirectory, Height(0));
				if (boost::filesystem::is_regular_file(blocksDatPath)) {
					boost::filesystem::resize_file(blocksDatPath, 0, ec);
					if (ec)
						CATAPULT_THROW_FILE_IO_ERROR(("failed to resize blocks.dat at height 0: " + ec.message()).c_str());
					SyncFile(blocksDatPath);
				}

				auto stmtDatPath = GetStatementsDatPath(dataDirectory, Height(0));
				if (boost::filesystem::is_regular_file(stmtDatPath)) {
					boost::filesystem::resize_file(stmtDatPath, 0, ec);
					if (ec)
						CATAPULT_THROW_FILE_IO_ERROR(("failed to resize statements.dat at height 0: " + ec.message()).c_str());
					SyncFile(stmtDatPath);
				}

				auto idxPath = GetBlocksIdxPath(dataDirectory, Height(0));
				if (boost::filesystem::is_regular_file(idxPath)) {
					boost::filesystem::resize_file(idxPath, 0, ec);
					if (ec)
						CATAPULT_THROW_FILE_IO_ERROR(("failed to resize blocks.idx at height 0: " + ec.message()).c_str());
					SyncFile(idxPath);
				}
			} else if (retainedIndex < Files_Per_Directory - 1) {
				auto blocksDatPath = GetBlocksDatPath(dataDirectory, height);
				boost::filesystem::resize_file(blocksDatPath, header.targetBlockEnd, ec);
				if (ec)
					CATAPULT_THROW_FILE_IO_ERROR(("failed to resize blocks.dat: " + ec.message()).c_str());
				SyncFile(blocksDatPath);

				auto stmtDatPath = GetStatementsDatPath(dataDirectory, height);
				if (header.targetStmtEnd > 0) {
					boost::filesystem::resize_file(stmtDatPath, header.targetStmtEnd, ec);
					if (ec)
						CATAPULT_THROW_FILE_IO_ERROR(("failed to resize statements.dat: " + ec.message()).c_str());
					SyncFile(stmtDatPath);
				} else if (boost::filesystem::is_regular_file(stmtDatPath)) {
					boost::filesystem::resize_file(stmtDatPath, 0, ec);
					if (ec)
						CATAPULT_THROW_FILE_IO_ERROR(("failed to resize statements.dat: " + ec.message()).c_str());
					SyncFile(stmtDatPath);
				}

				auto idxPath = GetBlocksIdxPath(dataDirectory, height);
				auto targetOffset = header.targetIndex * sizeof(BlockChunkIndexEntry);
				try {
					RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);
					if (idxFile.size() > targetOffset) {
						std::vector<uint8_t> zeros(idxFile.size() - targetOffset, 0);
						idxFile.seek(targetOffset);
						idxFile.write(zeros);
					}
				} catch (const std::exception& e) {
					CATAPULT_THROW_FILE_IO_ERROR(("failed to zero blocks.idx: " + std::string(e.what())).c_str());
				}
				SyncFile(idxPath);
			}

			// 2. Commit logical chain height and flush index.dat
			indexFile.set(height.unwrap());
			auto indexDatPath = boost::filesystem::path(dataDirectory) / "index.dat";
			SyncFile(indexDatPath);

			// 3. Purge future chunk directories beyond the retained chunk
			if (boost::filesystem::exists(dataDirectory) && boost::filesystem::is_directory(dataDirectory)) {
				std::vector<boost::filesystem::path> futureChunkPaths;
				boost::filesystem::directory_iterator endIt;
				for (boost::filesystem::directory_iterator it(dataDirectory); it != endIt; ++it) {
					if (boost::filesystem::is_directory(it->path())) {
						uint64_t chunkId = 0;
						if (TryGetChunkId(it->path(), chunkId)) {
							if (chunkId > retainedChunkId)
								futureChunkPaths.push_back(it->path());
						}
					}
				}

				if (!futureChunkPaths.empty()) {
					for (const auto& path : futureChunkPaths) {
						boost::filesystem::remove_all(path, ec);
						if (ec)
							CATAPULT_THROW_FILE_IO_ERROR(("failed to remove future chunk directory: " + path.string() + ": " + ec.message()).c_str());
					}
					SyncDirectory(dataDirectory);
				}
			}
		}

		void WriteRollbackJournal(const std::string& dataDirectory, const RollbackJournalHeader& header) {
			auto journalTmpPath = boost::filesystem::path(dataDirectory) / "rollback.journal.tmp";
			auto journalPath = boost::filesystem::path(dataDirectory) / "rollback.journal";

			if (boost::filesystem::exists(journalPath))
				CATAPULT_THROW_FILE_IO_ERROR("rollback already in progress; recover rollback.journal first");

			if (boost::filesystem::exists(journalTmpPath)) {
				boost::system::error_code ec;
				boost::filesystem::remove(journalTmpPath, ec);
				if (ec)
					CATAPULT_THROW_FILE_IO_ERROR(("failed to remove existing rollback.journal.tmp before write: " + ec.message()).c_str());
			}

#ifdef _WIN32
			int fd = _open(journalTmpPath.generic_string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
			if (fd == -1)
				CATAPULT_THROW_FILE_IO_ERROR("failed to open rollback.journal.tmp for writing");
			auto written = _write(fd, &header, sizeof(RollbackJournalHeader));
			if (written != sizeof(RollbackJournalHeader)) {
				_close(fd);
				CATAPULT_THROW_FILE_IO_ERROR("failed to write complete rollback.journal.tmp");
			}
			HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
			if (h == INVALID_HANDLE_VALUE || !FlushFileBuffers(h)) {
				_close(fd);
				CATAPULT_THROW_FILE_IO_ERROR("failed to flush rollback.journal.tmp to disk");
			}
			if (_close(fd) != 0)
				CATAPULT_THROW_FILE_IO_ERROR("failed to close rollback.journal.tmp");
#else
			int fd = ::open(journalTmpPath.generic_string().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
			if (fd == -1)
				CATAPULT_THROW_FILE_IO_ERROR("failed to open rollback.journal.tmp for writing");
			auto written = ::write(fd, &header, sizeof(RollbackJournalHeader));
			if (written != sizeof(RollbackJournalHeader)) {
				::close(fd);
				CATAPULT_THROW_FILE_IO_ERROR("failed to write complete rollback.journal.tmp");
			}
			if (::fsync(fd) != 0) {
				::close(fd);
				CATAPULT_THROW_FILE_IO_ERROR("failed to fsync rollback.journal.tmp to disk");
			}
			if (::close(fd) != 0)
				CATAPULT_THROW_FILE_IO_ERROR("failed to close rollback.journal.tmp");
#endif

			if (boost::filesystem::exists(journalPath))
				CATAPULT_THROW_FILE_IO_ERROR("rollback already in progress; recover rollback.journal first");

			boost::system::error_code ec;
			boost::filesystem::rename(journalTmpPath, journalPath, ec);
			if (ec)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to atomically rename rollback.journal: " + ec.message()).c_str());

			SyncDirectory(dataDirectory);
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
			if (openMode != OpenMode::Read_Only) {
				auto dirPath = GetDirectoryPath(baseDirectory, height);
				if (!boost::filesystem::exists(dirPath))
					boost::filesystem::create_directories(dirPath);
			}

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
			auto dirPath = GetDirectoryPath(m_dataDirectory, height);
			if (!boost::filesystem::exists(dirPath))
				boost::filesystem::create_directories(dirPath);

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
		auto stmtOffset = m_pCachedStmtFile->size();
		if (stmtOffset > std::numeric_limits<uint32_t>::max())
			CATAPULT_THROW_RUNTIME_ERROR_1("statements.dat exceeded 4GB for directory at height", height);

		entry.stmtOffset = static_cast<uint32_t>(stmtOffset);

		if (blockElement.OptionalStatement) {
			m_pCachedStmtFile->seek(stmtOffset);
			RawFileOutputStreamAdapter streamAdapter(*m_pCachedStmtFile);
			WriteBlockStatement(streamAdapter, *blockElement.OptionalStatement);

			auto stmtSize = m_pCachedStmtFile->size() - stmtOffset;
			if (stmtSize > std::numeric_limits<uint32_t>::max())
				CATAPULT_THROW_RUNTIME_ERROR_1("statement size exceeded 4GB at height", height);

			entry.stmtSize = static_cast<uint32_t>(stmtSize);
		} else {
			entry.stmtSize = 0;
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
			, m_indexFile((boost::filesystem::path(m_dataDirectory) / "index.dat").generic_string()) {
		recoverUnfinishedRollback();
	}

	void FileBlockStorage::recoverUnfinishedRollback() {
		auto journalTmpPath = boost::filesystem::path(m_dataDirectory) / "rollback.journal.tmp";
		if (boost::filesystem::exists(journalTmpPath)) {
			boost::system::error_code ec;
			boost::filesystem::remove(journalTmpPath, ec);
			if (ec)
				CATAPULT_THROW_FILE_IO_ERROR(("failed to remove stale rollback.journal.tmp on startup: " + ec.message()).c_str());
		}

		auto journalPath = boost::filesystem::path(m_dataDirectory) / "rollback.journal";
		if (!boost::filesystem::is_regular_file(journalPath))
			return;

		RollbackJournalHeader header;
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			if (journalFile.size() != sizeof(RollbackJournalHeader))
				CATAPULT_THROW_FILE_IO_ERROR("corrupted rollback.journal: invalid header size");

			journalFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&header), sizeof(RollbackJournalHeader)));
		}

		// Apply rollback operations (which validates the journal against physical files and index entries).
		// If an error occurs, it throws and preserves rollback.journal.
		ApplyRollbackOperations(m_dataDirectory, header, m_indexFile);

		boost::system::error_code ec;
		boost::filesystem::remove(journalPath, ec);
		if (ec)
			CATAPULT_THROW_FILE_IO_ERROR(("failed to remove rollback.journal after recovery: " + ec.message()).c_str());

		SyncDirectory(m_dataDirectory);
	}

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

		auto journalPath = boost::filesystem::path(m_dataDirectory) / "rollback.journal";
		if (boost::filesystem::exists(journalPath))
			CATAPULT_THROW_FILE_IO_ERROR("rollback already in progress; recover rollback.journal first");

		auto currentHeight = chainHeight();
		if (height >= currentHeight && Height(0) != height)
			return;

		auto retainedChunkId = (Height(0) == height) ? 0 : (height.unwrap() / Files_Per_Directory);
		auto retainedIndex = (Height(0) == height) ? 0 : (height.unwrap() % Files_Per_Directory);

		RollbackJournalHeader header;
		header.magic = Rollback_Journal_Magic;
		header.version = Rollback_Journal_Version;
		header.targetHeight = height.unwrap();
		header.previousHeight = currentHeight.unwrap();
		header.targetBlockEnd = 0;
		header.targetStmtEnd = 0;
		header.targetIndex = static_cast<uint32_t>((Height(0) == height) ? 0 : (retainedIndex + 1));
		header.padding = 0;

		if (Height(0) != height && retainedIndex < Files_Per_Directory - 1) {
			BlockChunkIndexEntry retainedEntry;
			if (!HasChunkIndexEntry(m_dataDirectory, height, retainedEntry)) {
				std::ostringstream out;
				out << "cannot rollback: retained block index entry missing at height " << height;
				CATAPULT_THROW_INVALID_ARGUMENT(out.str().c_str());
			}

			header.targetBlockEnd = static_cast<uint64_t>(retainedEntry.blockOffset) + static_cast<uint64_t>(retainedEntry.blockSize);

			uint64_t lastRetainedStmtEnd = 0;
			for (auto h = height; h > Height(0) && (h.unwrap() / Files_Per_Directory == retainedChunkId); h = h - Height(1)) {
				BlockChunkIndexEntry stmtEntry;
				if (HasChunkIndexEntry(m_dataDirectory, h, stmtEntry) && stmtEntry.stmtSize > 0) {
					lastRetainedStmtEnd = static_cast<uint64_t>(stmtEntry.stmtOffset) + static_cast<uint64_t>(stmtEntry.stmtSize);
					break;
				}
			}
			header.targetStmtEnd = lastRetainedStmtEnd;
		}

		// 1. Durably write rollback journal header before modifying any files
		WriteRollbackJournal(m_dataDirectory, header);

		// 2. Apply all file truncations, height commit, and directory purges
		ApplyRollbackOperations(m_dataDirectory, header, m_indexFile);

		// 3. Remove rollback journal after successful completion
		boost::system::error_code ec;
		boost::filesystem::remove(journalPath, ec);
		if (ec)
			CATAPULT_THROW_FILE_IO_ERROR(("failed to remove rollback.journal: " + ec.message()).c_str());

		SyncDirectory(m_dataDirectory);
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
