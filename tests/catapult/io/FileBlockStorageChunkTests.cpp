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
#include "tests/test/core/BlockTestUtils.h"
#include "tests/test/core/StorageTestUtils.h"
#include "tests/test/nodeps/Filesystem.h"
#include "tests/TestHarness.h"
#include <boost/filesystem.hpp>
#ifndef _WIN32
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
#pragma pack(pop)
	static_assert(sizeof(BlockChunkIndexEntry) == 16, "BlockChunkIndexEntry must be exactly 16 bytes");

	struct TestBlockElementContext {
		model::UniqueEntityPtr<model::Block> pBlock;
		std::unique_ptr<model::BlockElement> pElement;

		explicit TestBlockElementContext(Height height)
			: pBlock(test::GenerateBlockWithTransactions(0, height))
			, pElement(std::make_unique<model::BlockElement>(*pBlock)) {
			pElement->EntityHash = test::GenerateRandomByteArray<Hash256_Size>();
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
	test::FakeHeight(tempDir.name(), 65534);

	FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

	// Act: Write block at height 65535 (last in chunk 0)
	TestBlockElementContext block65535(Height(65535));
	storage.saveBlock(block65535.get());

	// Act: Write block at height 65536 (first in chunk 1)
	TestBlockElementContext block65536(Height(65536));
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

	// Act: Write blocks sequentially from height 1 to 5
	for (uint32_t i = 1; i <= 5; ++i) {
		TestBlockElementContext blockCtx(Height(i));
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

	constexpr uint32_t NumBlocks = 20;
	for (uint32_t i = 1; i <= NumBlocks; ++i) {
		TestBlockElementContext blockCtx(Height(i));
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

		// Block 1 with Statement
		TestBlockElementContext block1(Height(1));
		auto pStatement1 = std::make_shared<model::BlockStatement>();
		pStatement1->Receipts.emplace(model::ReceiptSource(), std::make_unique<model::Receipt>(model::ReceiptType::Mosaic_Levy, model::ReceiptVersion(1)));
		const_cast<model::BlockElement&>(block1.get()).OptionalStatement = pStatement1;
		storage.saveBlock(block1.get());

		// Block 2 WITHOUT Statement
		TestBlockElementContext block2(Height(2));
		storage.saveBlock(block2.get());

		// Block 3 WITHOUT Statement
		TestBlockElementContext block3(Height(3));
		storage.saveBlock(block3.get());

		// Act: Rollback to block 1
		storage.dropBlocksAfter(Height(1));

		// Assert: Statement data for block 1 must be preserved and non-empty
		auto [stmtData, hasStmt] = storage.loadBlockStatementData(Height(1));
		EXPECT_TRUE(hasStmt);
		EXPECT_GT(stmtData.size(), 0u);

		auto pLoadedBlock1 = storage.loadBlockElement(Height(1));
		EXPECT_TRUE(pLoadedBlock1->OptionalStatement != nullptr);
		EXPECT_EQ(1u, pLoadedBlock1->OptionalStatement->Receipts.size());
	}

	TEST(TEST_CLASS, DropBlocksAfterTruncatesStatementsToZeroWhenAllRetainedBlocksHaveNoStatement) {
		// Arrange: prepare storage
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Block 1 WITHOUT Statement
		TestBlockElementContext block1(Height(1));
		storage.saveBlock(block1.get());

		// Block 2 WITH Statement
		TestBlockElementContext block2(Height(2));
		auto pStatement2 = std::make_shared<model::BlockStatement>();
		pStatement2->Receipts.emplace(model::ReceiptSource(), std::make_unique<model::Receipt>(model::ReceiptType::Mosaic_Levy, model::ReceiptVersion(1)));
		const_cast<model::BlockElement&>(block2.get()).OptionalStatement = pStatement2;
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
		test::FakeHeight(tempDir.name(), 65534);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		// Block 65535 in chunk 00000 with Statement
		TestBlockElementContext block65535(Height(65535));
		auto pStatement65535 = std::make_shared<model::BlockStatement>();
		pStatement65535->Receipts.emplace(model::ReceiptSource(), std::make_unique<model::Receipt>(model::ReceiptType::Mosaic_Levy, model::ReceiptVersion(1)));
		const_cast<model::BlockElement&>(block65535.get()).OptionalStatement = pStatement65535;
		storage.saveBlock(block65535.get());

		// Block 65536 in chunk 00001 with Statement
		TestBlockElementContext block65536(Height(65536));
		auto pStatement65536 = std::make_shared<model::BlockStatement>();
		pStatement65536->Receipts.emplace(model::ReceiptSource(), std::make_unique<model::Receipt>(model::ReceiptType::Mosaic_Levy, model::ReceiptVersion(1)));
		const_cast<model::BlockElement&>(block65536.get()).OptionalStatement = pStatement65536;
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
		EXPECT_EQ(1u, pLoadedBlock->OptionalStatement->Receipts.size());

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
		for (uint32_t i = 1; i <= 10; ++i) {
			TestBlockElementContext blockCtx(Height(i));
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
		test::FakeHeight(tempDir.name(), 65534);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		TestBlockElementContext block65535(Height(65535));
		storage.saveBlock(block65535.get());

		TestBlockElementContext block65536(Height(65536));
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

		// Arrange: prepare storage with blocks in chunk 0 and chunk 1
		test::TempDirectoryGuard tempDir;
		test::PrepareStorage(tempDir.name());
		test::FakeHeight(tempDir.name(), 65534);

		FileBlockStorage storage(tempDir.name(), FileBlockStorageMode::Hash_Index);

		TestBlockElementContext block65535(Height(65535));
		storage.saveBlock(block65535.get());

		TestBlockElementContext block65536(Height(65536));
		storage.saveBlock(block65536.get());

		EXPECT_EQ(Height(65536), storage.chainHeight());
		AssertDirectoryExists(tempDir.name(), 0);
		AssertDirectoryExists(tempDir.name(), 1);

		// Make blocks.idx in chunk 00000 read-only so modifying it during rollback throws
		boost::filesystem::path idxPath = boost::filesystem::path(tempDir.name()) / "00000" / "blocks.idx";
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_read);

		// Act & Assert: dropBlocksAfter to height 65534 throws because blocks.idx cannot be modified
		EXPECT_THROW(storage.dropBlocksAfter(Height(65534)), catapult_file_io_error);

		// Restore permissions for assertion and clean tempDir teardown
		boost::filesystem::permissions(idxPath, boost::filesystem::perms::owner_all);

		// Assert: Chunk 00001 is NOT destroyed because rollback failed before committing height
		boost::filesystem::path chunk1Path = boost::filesystem::path(tempDir.name()) / "00001";
		EXPECT_TRUE(boost::filesystem::exists(chunk1Path));

		// Assert: Logical chain height remains unchanged
		EXPECT_EQ(Height(65536), storage.chainHeight());
	}
#endif

}}
