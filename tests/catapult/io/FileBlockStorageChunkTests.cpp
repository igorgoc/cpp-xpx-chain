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

#include "catapult/io/FileBlockStorage.h"
#include "catapult/io/RawFile.h"
#include "tests/test/core/BlockStatementTestUtils.h"
#include "tests/test/core/BlockTestUtils.h"
#include "tests/test/core/StorageTestUtils.h"
#include "tests/test/nodeps/Filesystem.h"
#include "tests/TestHarness.h"
#include <atomic>
#include <boost/filesystem.hpp>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace catapult { namespace io {

#define TEST_CLASS FileBlockStorageChunkTests

namespace {
	constexpr uint32_t Files_Per_Directory = 65536u;

#pragma pack(push, 1)
	struct BlockChunkIndexEntry {
		uint32_t blockOffset;
		uint32_t blockSize;
		uint32_t stmtOffset;
		uint32_t stmtSize;
	};

	struct RollbackJournalHeader {
		uint32_t magic;
		uint32_t version;
		uint64_t targetHeight;
		uint64_t previousHeight;
		uint64_t targetBlockEnd;
		uint64_t targetStmtEnd;
		uint32_t targetIndex;
		uint32_t padding;
	};
#pragma pack(pop)
	static_assert(sizeof(BlockChunkIndexEntry) == 16, "BlockChunkIndexEntry must be exactly 16 bytes");
	static_assert(sizeof(RollbackJournalHeader) == 48, "RollbackJournalHeader must be exactly 48 bytes");
	static constexpr uint32_t Rollback_Journal_Magic = 0x5349524Au;
	static constexpr uint32_t Rollback_Journal_Version = 1u;

	struct TestBlockElementContext {
		model::UniqueEntityPtr<model::Block> pBlock;
		std::unique_ptr<model::BlockElement> pElement;

		explicit TestBlockElementContext(Height height)
			: pBlock(test::GenerateBlockWithTransactions(0, height))
			, pElement(std::make_unique<model::BlockElement>(*pBlock)) {
			pElement->EntityHash = test::GenerateRandomByteArray<Hash256>();
		}

		const model::BlockElement& get() const {
			return *pElement;
		}
	};

	void AssertDirectoryExists(const std::string& baseDir, uint64_t chunkId) {
		char dirName[16];
		std::snprintf(dirName, sizeof(dirName), "%05lu", chunkId);
		boost::filesystem::path dirPath = baseDir;
		dirPath /= dirName;
		EXPECT_TRUE(boost::filesystem::exists(dirPath)) << "Directory " << dirPath << " should exist";
	}

	void AssertFileExists(const std::string& baseDir, uint64_t chunkId, const std::string& filename) {
		char dirName[16];
		std::snprintf(dirName, sizeof(dirName), "%05lu", chunkId);
		boost::filesystem::path filePath = baseDir;
		filePath /= dirName;
		filePath /= filename;
		EXPECT_TRUE(boost::filesystem::exists(filePath)) << "File " << filePath << " should exist";
	}
}

TEST(TEST_CLASS, CanWriteBlocksAcrossChunkBoundary) {
	// Arrange: prepare storage seeded at height 65534
	test::TempDirectoryGuard tempDir;
	test::PrepareStorage(tempDir.name());
	test::FakeHeight(tempDir.name(), 65535);

	FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

	// Act: Write block at height 65535 (last in chunk 0)
	TestBlockElementContext block65535((Height(65535)));
	storage.saveBlock(block65535.get());

	// Act: Write block at height 65536 (first in chunk 1)
	TestBlockElementContext block65536((Height(65536)));
	storage.saveBlock(block65536.get());

	// Assert: Verify directories and files exist for both chunks (00000 and 00001)
	AssertDirectoryExists(tempDir.name(), 0);
	AssertDirectoryExists(tempDir.name(), 1);

	AssertFileExists(tempDir.name(), 0, "blocks.dat");
	AssertFileExists(tempDir.name(), 0, "blocks.idx");
	AssertFileExists(tempDir.name(), 1, "blocks.dat");
	AssertFileExists(tempDir.name(), 1, "blocks.idx");

	// Assert: Verify chain height is updated correctly
	EXPECT_EQ(Height(65536), storage.chainHeight());
}

TEST(TEST_CLASS, BlockChunkIndexEntryMatchesBinaryLayout) {
	// Arrange: prepare storage with seed
	test::TempDirectoryGuard tempDir;
	test::PrepareStorage(tempDir.name());

	FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
	storage.dropBlocksAfter(Height(0));

	// Act: Write blocks sequentially from height 1 to 5
	for (uint32_t i = 1; i <= 5; ++i) {
		TestBlockElementContext blockCtx((Height(i)));
		storage.saveBlock(blockCtx.get());
	}

	// Assert: Open blocks.idx and verify binary layout of block 5
	auto height = Height(5);
	char dirName[16];
	std::snprintf(dirName, sizeof(dirName), "%05lu", height.unwrap() / Files_Per_Directory);
	boost::filesystem::path idxPath = tempDir.name();
	idxPath /= dirName;
	idxPath /= "blocks.idx";

	RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
	EXPECT_GT(idxFile.size(), 5 * sizeof(BlockChunkIndexEntry));

	auto index = height.unwrap() % Files_Per_Directory;
	idxFile.seek(index * sizeof(BlockChunkIndexEntry));

	BlockChunkIndexEntry entry;
	idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry), sizeof(BlockChunkIndexEntry)));

	// Assert: Verify entry contains valid non-zero block offset and size
	EXPECT_GT(entry.blockOffset, 0u);
	EXPECT_GT(entry.blockSize, 0u);

	// Assert: Verify loadBlockElement reads the exact block matching binary storage
	auto pLoadedBlock = storage.loadBlockElement(height);
	EXPECT_EQ(height, pLoadedBlock->Block.Height);
}

TEST(TEST_CLASS, DropBlocksAfterTruncatesFilesAndZerosIndex) {
	// Arrange: prepare storage with 20 blocks
	test::TempDirectoryGuard tempDir;
	test::PrepareStorage(tempDir.name());

	FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
	storage.dropBlocksAfter(Height(0));

	constexpr uint32_t NumBlocks = 20;
	for (uint32_t i = 1; i <= NumBlocks; ++i) {
		TestBlockElementContext blockCtx((Height(i)));
		storage.saveBlock(blockCtx.get());
	}

	// Read block 11 index entry BEFORE dropping to obtain expected truncation offset
	char dirName[16];
	std::snprintf(dirName, sizeof(dirName), "%05lu", 0u);
	boost::filesystem::path idxPath = tempDir.name();
	idxPath /= dirName;
	idxPath /= "blocks.idx";

	BlockChunkIndexEntry entry11;
	{
		RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
		idxFile.seek(11 * sizeof(BlockChunkIndexEntry));
		idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry11), sizeof(BlockChunkIndexEntry)));
	}

	// Act: Drop blocks after height 10
	storage.dropBlocksAfter(Height(10));

	// Assert: Chain height should be 10
	EXPECT_EQ(Height(10), storage.chainHeight());

	// Assert: Verify blocks.dat is truncated to block 11's start offset
	boost::filesystem::path blocksDatPath = tempDir.name();
	blocksDatPath /= dirName;
	blocksDatPath /= "blocks.dat";
	EXPECT_EQ(boost::filesystem::file_size(blocksDatPath), entry11.blockOffset);

	// Assert: Verify index entries from height 11 onwards are zeroed
	RawFile idxFilePost(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
	for (uint32_t i = 11; i <= NumBlocks; ++i) {
		idxFilePost.seek(i * sizeof(BlockChunkIndexEntry));
		BlockChunkIndexEntry zeroedEntry;
		idxFilePost.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&zeroedEntry), sizeof(BlockChunkIndexEntry)));
		EXPECT_EQ(0u, zeroedEntry.blockOffset);
		EXPECT_EQ(0u, zeroedEntry.blockSize);
		EXPECT_EQ(0u, zeroedEntry.stmtOffset);
		EXPECT_EQ(0u, zeroedEntry.stmtSize);
	}
}

TEST(TEST_CLASS, DropBlocksAfterPreservesStatementsOfRetainedBlocksWhenNextBlockHasNoStatement) {
		// Arrange: prepare storage
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
		storage.dropBlocksAfter(Height(0));

		// Block 1 with Statement
		TestBlockElementContext block1((Height(1)));
		auto pStatement1 = test::GenerateRandomStatements({ 1, 0, 0, 0 });
		const_cast<model::BlockElement&>(block1.get()).OptionalStatement = std::move(pStatement1);
		storage.saveBlock(block1.get());

		// Block 2 WITHOUT Statement
		TestBlockElementContext block2((Height(2)));
		storage.saveBlock(block2.get());

		// Block 3 WITHOUT Statement
		TestBlockElementContext block3((Height(3)));
		storage.saveBlock(block3.get());

		// Act: Rollback to block 1
		storage.dropBlocksAfter(Height(1));

		// Assert: Statement data for block 1 must be preserved and non-empty
		auto [stmtData, hasStmt] = storage.loadBlockStatementData(Height(1));
		EXPECT_TRUE(hasStmt);
		EXPECT_GT(stmtData.size(), 0u);

		auto pLoadedBlock1 = storage.loadBlockElement(Height(1));
		EXPECT_TRUE(pLoadedBlock1->OptionalStatement != nullptr);
		EXPECT_EQ(1u, pLoadedBlock1->OptionalStatement->TransactionStatements.size());
	}

	TEST(TEST_CLASS, DropBlocksAfterTruncatesStatementsToZeroWhenAllRetainedBlocksHaveNoStatement) {
		// Arrange: prepare storage
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
		storage.dropBlocksAfter(Height(0));

		// Block 1 WITHOUT Statement
		TestBlockElementContext block1((Height(1)));
		storage.saveBlock(block1.get());

		// Block 2 WITH Statement
		TestBlockElementContext block2((Height(2)));
		auto pStatement2 = test::GenerateRandomStatements({ 1, 0, 0, 0 });
		const_cast<model::BlockElement&>(block2.get()).OptionalStatement = std::move(pStatement2);
		storage.saveBlock(block2.get());

		// Act: Rollback to block 1
		storage.dropBlocksAfter(Height(1));

		// Assert: Block 1 has no statement and statements.dat is 0 bytes
		auto [stmtData, hasStmt] = storage.loadBlockStatementData(Height(1));
		EXPECT_FALSE(hasStmt);
		EXPECT_EQ(0u, stmtData.size());

		char dirName[16];
		std::snprintf(dirName, sizeof(dirName), "%05lu", 0u);
		boost::filesystem::path stmtDatPath = tempDir.name();
		stmtDatPath /= dirName;
		stmtDatPath /= "statements.dat";
		EXPECT_EQ(0u, boost::filesystem::file_size(stmtDatPath));
	}

	TEST(TEST_CLASS, DropBlocksAfterAcrossChunkBoundaryPurgesFutureChunkAndPreservesCurrentChunk) {
		// Arrange: prepare storage seeded at height 65534
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());
		test::FakeHeight(tempDir.name(), 65535);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Block 65535 in chunk 00000 with Statement
		TestBlockElementContext block65535((Height(65535)));
		auto pStatement65535 = test::GenerateRandomStatements({ 1, 0, 0, 0 });
		const_cast<model::BlockElement&>(block65535.get()).OptionalStatement = std::move(pStatement65535);
		storage.saveBlock(block65535.get());

		// Block 65536 in chunk 00001 with Statement
		TestBlockElementContext block65536((Height(65536)));
		auto pStatement65536 = test::GenerateRandomStatements({ 1, 0, 0, 0 });
		const_cast<model::BlockElement&>(block65536.get()).OptionalStatement = std::move(pStatement65536);
		storage.saveBlock(block65536.get());

		// Assert both chunks exist before rollback
		AssertDirectoryExists(tempDir.name(), 0);
		AssertDirectoryExists(tempDir.name(), 1);

		// Act: Rollback to block 65535 (boundary of chunk 00000)
		storage.dropBlocksAfter(Height(65535));

		// Assert: Chain height should be 65535
		EXPECT_EQ(Height(65535), storage.chainHeight());

		// Assert: Chunk 00000 exists and has its statement intact
		AssertDirectoryExists(tempDir.name(), 0);
		auto [stmtData, hasStmt] = storage.loadBlockStatementData(Height(65535));
		EXPECT_TRUE(hasStmt);
		EXPECT_GT(stmtData.size(), 0u);

		auto pLoadedBlock = storage.loadBlockElement(Height(65535));
		EXPECT_TRUE(pLoadedBlock->OptionalStatement != nullptr);
		EXPECT_EQ(1u, pLoadedBlock->OptionalStatement->TransactionStatements.size());

		// Assert: Chunk 00001 is completely removed
		boost::filesystem::path chunk1Path = tempDir.name();
		chunk1Path /= "00001";
		EXPECT_FALSE(boost::filesystem::exists(chunk1Path)) << "Chunk directory 00001 should have been purged";
	}

	TEST(TEST_CLASS, DropBlocksAfterRemovesSparseFutureChunkDirectories) {
		// Arrange: prepare storage seeded at height 10
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
		storage.dropBlocksAfter(Height(0));
		for (uint32_t i = 1; i <= 11; ++i) {
			TestBlockElementContext blockCtx((Height(i)));
			storage.saveBlock(blockCtx.get());
		}

		// Create sparse future chunk directories 00001 and 00003
		boost::filesystem::path chunk1Path = boost::filesystem::path(tempDir.name()) / "00001";
		boost::filesystem::path chunk3Path = boost::filesystem::path(tempDir.name()) / "00003";
		boost::filesystem::create_directories(chunk1Path);
		boost::filesystem::create_directories(chunk3Path);

		EXPECT_TRUE(boost::filesystem::exists(chunk1Path));
		EXPECT_TRUE(boost::filesystem::exists(chunk3Path));

		// Act: drop blocks after height 10 (in chunk 00000)
		storage.dropBlocksAfter(Height(10));

		// Assert: Both 00001 and 00003 are removed despite the gap (00002 was absent)
		EXPECT_EQ(Height(10), storage.chainHeight());
		AssertDirectoryExists(tempDir.name(), 0);
		EXPECT_FALSE(boost::filesystem::exists(chunk1Path));
		EXPECT_FALSE(boost::filesystem::exists(chunk3Path));
	}

	TEST(TEST_CLASS, DropBlocksAfterHeightZeroPurgesFutureChunksAndTruncatesChunkZero) {
		// Arrange: prepare storage with blocks in chunk 0 and chunk 1
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());
		test::FakeHeight(tempDir.name(), 65535);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		TestBlockElementContext block65535((Height(65535)));
		storage.saveBlock(block65535.get());

		TestBlockElementContext block65536((Height(65536)));
		storage.saveBlock(block65536.get());

		EXPECT_EQ(Height(65536), storage.chainHeight());
		AssertDirectoryExists(tempDir.name(), 0);
		AssertDirectoryExists(tempDir.name(), 1);

		// Act: Rollback to height 0 (logical purge)
		storage.dropBlocksAfter(Height(0));

		// Assert: Chain height is 0
		EXPECT_EQ(Height(0), storage.chainHeight());

		// Assert: Chunk 00001 is removed
		boost::filesystem::path chunk1Path = boost::filesystem::path(tempDir.name()) / "00001";
		EXPECT_FALSE(boost::filesystem::exists(chunk1Path));

		// Assert: Chunk 00000 files exist and are 0 bytes
		boost::filesystem::path blocksDatPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.dat";
		boost::filesystem::path stmtDatPath = boost::filesystem::path(tempDir.name()) / "00000" / "statements.dat";
		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";

		EXPECT_EQ(0u, boost::filesystem::file_size(blocksDatPath));
		EXPECT_EQ(0u, boost::filesystem::file_size(stmtDatPath));
		EXPECT_EQ(0u, boost::filesystem::file_size(idxPath));
	}

#ifndef _WIN32
	TEST(TEST_CLASS, DropBlocksAfterFailurePreservesFutureChunksAndChainHeight) {
		if (geteuid() == 0)
			return; // Skip if running as root where filesystem permissions checks are bypassed

		// Arrange: prepare storage with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		// Create dummy future chunk 00001
		boost::filesystem::path chunk1Path = boost::filesystem::path(tempDir.name()) / "00001";
		boost::filesystem::create_directories(chunk1Path);
		EXPECT_TRUE(boost::filesystem::exists(chunk1Path));

		// Make blocks.idx in chunk 00000 read-only so modifying it during rollback throws
		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_read);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Act & Assert: dropBlocksAfter to height 5 throws because blocks.idx cannot be modified
		EXPECT_THROW(storage.dropBlocksAfter(Height(5)), catapult_file_io_error);

		// Restore permissions for assertion and clean tempDir teardown
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_all);

		// Assert: Chunk 00001 is NOT destroyed because rollback failed before committing height
		EXPECT_TRUE(boost::filesystem::exists(chunk1Path));

		// Assert: Logical chain height remains unchanged
		EXPECT_EQ(Height(10), storage.chainHeight());
	}
#endif

	TEST(TEST_CLASS, RecoverUnfinishedRollbackReconcilesInterruptedRollback) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		// Read index entry for block 6 to get expected blockOffset at height 5 rollback
		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry6;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(6 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry6), sizeof(BlockChunkIndexEntry)));
		}

		// Create dummy future chunk 00001 (simulating crash before future chunk purge)
		boost::filesystem::path chunk1Path = boost::filesystem::path(tempDir.name()) / "00001";
		boost::filesystem::create_directories(chunk1Path);
		EXPECT_TRUE(boost::filesystem::exists(chunk1Path));

		// Write mock rollback.journal simulating a crash mid-rollback to height 5
		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		journalHeader.targetBlockEnd = entry6.blockOffset;
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}
		EXPECT_TRUE(boost::filesystem::exists(journalPath));

		// Act: Instantiate FileBlockStorage (triggers recoverUnfinishedRollback on startup)
		FileBlockStorage recoveredStorage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Assert:
		// 1. Chain height is recovered to 5
		EXPECT_EQ(Height(5), recoveredStorage.chainHeight());

		// 2. Future chunk 00001 was purged during startup recovery
		EXPECT_FALSE(boost::filesystem::exists(chunk1Path));

		// 3. Rollback journal was removed
		EXPECT_FALSE(boost::filesystem::exists(journalPath));

		// 4. Retained block 5 is loadable and valid
		auto pBlock5 = recoveredStorage.loadBlockElement(Height(5));
		EXPECT_EQ(Height(5), pBlock5->Block.Height);
	}

#ifndef _WIN32
	TEST(TEST_CLASS, RecoverUnfinishedRollbackPreservesJournalOnFailure) {
		if (geteuid() == 0)
			return; // Skip if running as root where filesystem permissions checks are bypassed

		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry5;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(5 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry5), sizeof(BlockChunkIndexEntry)));
		}

		// Write mock rollback.journal targeting height 5 with valid index-derived offsets
		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		journalHeader.targetBlockEnd = static_cast<uint64_t>(entry5.blockOffset) + static_cast<uint64_t>(entry5.blockSize);
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}
		EXPECT_TRUE(boost::filesystem::exists(journalPath));

		// Make blocks.idx in chunk 00000 read-only to force ApplyRollbackOperations to fail during zeroing
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_read);

		// Act & Assert: Opening storage fails during recovery
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);

		// Restore permissions for assertions and clean teardown
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_all);

		// Assert: rollback.journal was NOT deleted on failed recovery
		EXPECT_TRUE(boost::filesystem::exists(journalPath)) << "rollback.journal must be preserved on recovery failure";
	}
#endif

	TEST(TEST_CLASS, RecoverUnfinishedRollbackRejectsOversizedJournalOffsets) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		boost::filesystem::path blocksDatPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.dat";
		auto originalBlocksSize = boost::filesystem::file_size(blocksDatPath);

		// Write mock rollback.journal targeting height 5 with oversized targetBlockEnd
		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		journalHeader.targetBlockEnd = originalBlocksSize + 100000; // Oversized offset!
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}

		// Act & Assert: Opening storage throws catapult_file_io_error without extending blocks.dat with zeros
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);
		EXPECT_EQ(originalBlocksSize, boost::filesystem::file_size(blocksDatPath));
	}

	TEST(TEST_CLASS, RecoverUnfinishedRollbackRejectsMissingBlocksDat) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
		}

		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry5;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(5 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry5), sizeof(BlockChunkIndexEntry)));
		}

		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		journalHeader.targetBlockEnd = static_cast<uint64_t>(entry5.blockOffset) + static_cast<uint64_t>(entry5.blockSize);
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}

		// Delete 00000/blocks.dat to simulate missing required chunk file
		boost::filesystem::path blocksDatPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.dat";
		boost::filesystem::remove(blocksDatPath);

		// Act & Assert: Opening storage throws file IO error because required blocks.dat is missing
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);
	}

	TEST(TEST_CLASS, RecoverUnfinishedRollbackRejectsMissingBlocksIdx) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
		}

		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry5;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(5 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry5), sizeof(BlockChunkIndexEntry)));
		}

		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		journalHeader.targetBlockEnd = static_cast<uint64_t>(entry5.blockOffset) + static_cast<uint64_t>(entry5.blockSize);
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}

		// Delete 00000/blocks.idx to simulate missing required chunk index file
		boost::filesystem::remove(idxPath);

		// Act & Assert: Opening storage throws file IO error because required blocks.idx is missing
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);
	}

	TEST(TEST_CLASS, RecoverUnfinishedRollbackRejectsMissingStatementsDatWhenTargetStmtEndNonZero) {
		// Arrange: prepare storage seeded with 5 blocks with statements
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 5; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				auto pStatement = test::GenerateRandomStatements({ 1, 0, 0, 0 });
				const_cast<model::BlockElement&>(blockCtx.get()).OptionalStatement = std::move(pStatement);
				storage.saveBlock(blockCtx.get());
			}
		}

		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry3;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(3 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry3), sizeof(BlockChunkIndexEntry)));
		}

		EXPECT_GT(entry3.stmtSize, 0u);
		auto expectedStmtEnd = static_cast<uint64_t>(entry3.stmtOffset) + static_cast<uint64_t>(entry3.stmtSize);

		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 3;
		journalHeader.previousHeight = 5;
		journalHeader.targetBlockEnd = static_cast<uint64_t>(entry3.blockOffset) + static_cast<uint64_t>(entry3.blockSize);
		journalHeader.targetStmtEnd = expectedStmtEnd;
		journalHeader.targetIndex = 4;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}

		// Delete 00000/statements.dat to simulate missing statements.dat when targetStmtEnd > 0
		boost::filesystem::path stmtPath = boost::filesystem::path(tempDir.name()) / "00000" / "statements.dat";
		boost::filesystem::remove(stmtPath);

		// Act & Assert: Opening storage throws file IO error because required statements.dat is missing
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);
	}

	TEST(TEST_CLASS, RecoverUnfinishedRollbackRejectsInconsistentTargetBlockEnd) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
		}

		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		BlockChunkIndexEntry entry5;
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			idxFile.seek(5 * sizeof(BlockChunkIndexEntry));
			idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry5), sizeof(BlockChunkIndexEntry)));
		}

		RollbackJournalHeader journalHeader;
		journalHeader.magic = Rollback_Journal_Magic;
		journalHeader.version = Rollback_Journal_Version;
		journalHeader.targetHeight = 5;
		journalHeader.previousHeight = 10;
		// Inconsistent targetBlockEnd (smaller than entry5.blockOffset + entry5.blockSize)
		journalHeader.targetBlockEnd = static_cast<uint64_t>(entry5.blockOffset) + 1;
		journalHeader.targetStmtEnd = 0;
		journalHeader.targetIndex = 6;
		journalHeader.padding = 0;

		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(RawBuffer(reinterpret_cast<const uint8_t*>(&journalHeader), sizeof(RollbackJournalHeader)));
		}

		// Act & Assert: Opening storage throws file IO error because targetBlockEnd does not match index entry
		EXPECT_THROW(FileBlockStorage(tempDir.name(), FileBlockStorageMode::Hash_Index), catapult_file_io_error);
	}

	TEST(TEST_CLASS, RecoverUnfinishedRollbackCleansUpStaleJournalTmp) {
		// Arrange: prepare storage seeded with 5 blocks
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 5; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
		}

		// Create a stale rollback.journal.tmp (simulating crash before rename)
		boost::filesystem::path tmpPath = boost::filesystem::path(tempDir.name()) / "rollback.journal.tmp";
		{
			RawFile tmpFile(tmpPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			std::vector<uint8_t> staleData(32, 0xFF);
			tmpFile.write(staleData);
		}
		EXPECT_TRUE(boost::filesystem::exists(tmpPath));

		// Act: Instantiate FileBlockStorage
		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Assert: stale .tmp file was removed
		EXPECT_FALSE(boost::filesystem::exists(tmpPath));
		EXPECT_EQ(Height(5), storage.chainHeight());
	}

	TEST(TEST_CLASS, DropBlocksAfterThrowsIfRetainedBlockIndexEntryIsMissing) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		// Manually zero out index entry for block 5 to simulate missing index entry
		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		{
			RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Append, LockMode::None);
			idxFile.seek(5 * sizeof(BlockChunkIndexEntry));
			std::vector<uint8_t> zeros(sizeof(BlockChunkIndexEntry), 0);
			idxFile.write(zeros);
		}

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Act & Assert: dropBlocksAfter(Height(5)) must throw invalid argument and not create zero-offset journal
		EXPECT_THROW(storage.dropBlocksAfter(Height(5)), catapult_invalid_argument);

		// Assert: chain height remains 10
		EXPECT_EQ(Height(10), storage.chainHeight());

		// Assert: no uncommitted rollback.journal was created
		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		EXPECT_FALSE(boost::filesystem::exists(journalPath));
	}

	TEST(TEST_CLASS, DropBlocksAfterThrowsIfRollbackJournalAlreadyExists) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Create mock rollback.journal with distinctive payload
		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		std::vector<uint8_t> originalJournalPayload(sizeof(RollbackJournalHeader), 0xAB);
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			journalFile.write(originalJournalPayload);
		}
		EXPECT_TRUE(boost::filesystem::exists(journalPath));

		// Act & Assert: dropBlocksAfter must reject operation and throw file IO error
		EXPECT_THROW(storage.dropBlocksAfter(Height(5)), catapult_file_io_error);

		// Assert: chain height remains 10
		EXPECT_EQ(Height(10), storage.chainHeight());

		// Assert: existing rollback.journal was NOT overwritten or deleted
		EXPECT_TRUE(boost::filesystem::exists(journalPath));
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
			std::vector<uint8_t> readPayload(sizeof(RollbackJournalHeader), 0);
			journalFile.read(readPayload);
			EXPECT_EQ(originalJournalPayload, readPayload);
		}
	}

	TEST(TEST_CLASS, DropBlocksAfterThrowsIfRollbackJournalAlreadyExistsEvenIfHeightMatches) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Create mock rollback.journal
		boost::filesystem::path journalPath = boost::filesystem::path(tempDir.name()) / "rollback.journal";
		{
			RawFile journalFile(journalPath.generic_string().c_str(), OpenMode::Read_Write, LockMode::None);
			std::vector<uint8_t> payload(sizeof(RollbackJournalHeader), 0xCD);
			journalFile.write(payload);
		}

		// Act & Assert: dropBlocksAfter(Height(10)) or greater must still throw instead of early returning
		EXPECT_THROW(storage.dropBlocksAfter(Height(10)), catapult_file_io_error);
		EXPECT_THROW(storage.dropBlocksAfter(Height(15)), catapult_file_io_error);
	}

	TEST(TEST_CLASS, DropBlocksAfterThrowsIfRollbackLockIsHeld) {
		// Arrange: prepare storage seeded with 10 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 10; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(10), storage.chainHeight());
		}

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Manually acquire exclusive lock on rollback.lock (simulating concurrent rollback process)
		boost::filesystem::path lockPath = boost::filesystem::path(tempDir.name()) / "rollback.lock";
#ifdef _WIN32
		int fd = -1;
		auto result = _sopen_s(&fd, lockPath.generic_string().c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
		EXPECT_EQ(0, result);
#else
		int fd = ::open(lockPath.string().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		EXPECT_NE(-1, fd);
		EXPECT_EQ(0, ::flock(fd, LOCK_EX | LOCK_NB));
#endif

		// Act & Assert: dropBlocksAfter must reject operation because lock is held by another process
		EXPECT_THROW(storage.dropBlocksAfter(Height(5)), catapult_file_io_error);

		// Release lock
#ifdef _WIN32
		_close(fd);
#else
		::flock(fd, LOCK_UN);
		::close(fd);
#endif

		// Assert: chain height remains 10
		EXPECT_EQ(Height(10), storage.chainHeight());

		// Now dropBlocksAfter succeeds after lock is released
		EXPECT_NO_THROW(storage.dropBlocksAfter(Height(5)));
		EXPECT_EQ(Height(5), storage.chainHeight());
	}

	TEST(TEST_CLASS, DropBlocksAfterConcurrentInProcessCallsAreSerialized) {
		// Arrange: prepare storage seeded with 20 blocks in chunk 0
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		{
			FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);
			storage.dropBlocksAfter(Height(0));
			for (uint32_t i = 1; i <= 20; ++i) {
				TestBlockElementContext blockCtx((Height(i)));
				storage.saveBlock(blockCtx.get());
			}
			EXPECT_EQ(Height(20), storage.chainHeight());
		}

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Act: 4 concurrent threads calling dropBlocksAfter on same storage instance
		std::vector<std::thread> threads;
		std::atomic<uint32_t> successCount{0};
		std::atomic<uint32_t> errorCount{0};

		for (uint32_t i = 0; i < 4; ++i) {
			threads.emplace_back([&storage, &successCount, &errorCount]() {
				try {
					storage.dropBlocksAfter(Height(5));
					++successCount;
				} catch (...) {
					++errorCount;
				}
			});
		}

		for (auto& t : threads)
			t.join();

		// Assert: All threads completed without error (first executed rollback, subsequent were serialized no-ops)
		EXPECT_EQ(4u, successCount);
		EXPECT_EQ(0u, errorCount);
		EXPECT_EQ(Height(5), storage.chainHeight());

		// Retained block 5 is loadable and valid
		auto pBlock5 = storage.loadBlockElement(Height(5));
		EXPECT_EQ(Height(5), pBlock5->Block.Height);
	}

}}
