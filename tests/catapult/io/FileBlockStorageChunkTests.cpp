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
#include "RawFile.h"
#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <vector>
#include <cstring>
#include <cstdio>

namespace catapult { namespace io { namespace test {

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

	std::string CreateTempDirectory() {
		boost::filesystem::path tempDir = boost::filesystem::temp_directory_path();
		tempDir /= "FileBlockStorageChunkTest_";
		tempDir /= boost::filesystem::unique_path().string();
		boost::filesystem::create_directories(tempDir);
		return tempDir.string();
	}

	model::BlockElement CreateTestBlockElement(Height height) {
		model::BlockElement blockElement;
		blockElement.Block.Height = height;
		// Initialize EntityHash to satisfy Hash_Index mode requirements.
		// In a full test suite, this would be populated with a valid cryptographic hash.
		blockElement.EntityHash = {};
		return blockElement;
	}

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

TEST(FileBlockStorageChunkTests, CanWriteBlocksAcrossChunkBoundary) {
	// Arrange:
	auto dataDir = CreateTempDirectory();
	FileBlockStorage storage(dataDir, FileBlockStorageMode::Hash_Index);

	// Act: Write block at height 65535 (last in chunk 0)
	storage.saveBlock(CreateTestBlockElement(Height(65535)));
	
	// Act: Write block at height 65536 (first in chunk 1)
	storage.saveBlock(CreateTestBlockElement(Height(65536)));

	// Assert: Verify directories and files exist for both chunks
	AssertDirectoryExists(dataDir, 0);
	AssertDirectoryExists(dataDir, 1);

	AssertFileExists(dataDir, 0, "blocks.dat");
	AssertFileExists(dataDir, 0, "blocks.idx");
	AssertFileExists(dataDir, 1, "blocks.dat");
	AssertFileExists(dataDir, 1, "blocks.idx");

	// Assert: Verify chain height is updated correctly
	EXPECT_EQ(Height(65536), storage.chainHeight());
}

TEST(FileBlockStorageChunkTests, BlockChunkIndexEntryMatchesBinaryLayout) {
	// Arrange:
	auto dataDir = CreateTempDirectory();
	FileBlockStorage storage(dataDir, FileBlockStorageMode::Hash_Index);

	// Act: Write a block at height 10
	auto height = Height(10);
	storage.saveBlock(CreateTestBlockElement(height));

	// Assert: Open blocks.idx and verify binary layout
	char dirName[16];
	std::snprintf(dirName, sizeof(dirName), "%05lu", height.unwrap() / Files_Per_Directory);
	boost::filesystem::path idxPath = dataDir;
	idxPath /= dirName;
	idxPath /= "blocks.idx";

	RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
	EXPECT_GT(idxFile.size(), sizeof(BlockChunkIndexEntry));

	auto index = height.unwrap() % Files_Per_Directory;
	idxFile.seek(index * sizeof(BlockChunkIndexEntry));

	BlockChunkIndexEntry entry;
	idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry), sizeof(BlockChunkIndexEntry)));

	// Assert: Verify entry contains valid offsets and sizes
	EXPECT_GT(entry.blockOffset, 0u);
	EXPECT_GT(entry.blockSize, 0u);
	EXPECT_EQ(entry.stmtOffset, 0u); // No statement provided
	EXPECT_EQ(entry.stmtSize, 0u);

	// Assert: Verify we can read the block using the offset and it matches the recorded size
	boost::filesystem::path blocksDatPath = dataDir;
	blocksDatPath /= dirName;
	blocksDatPath /= "blocks.dat";
	
	RawFile blocksFile(blocksDatPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
	blocksFile.seek(entry.blockOffset);
	
	uint32_t blockSizeFromHeader = 0;
	blocksFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&blockSizeFromHeader), sizeof(uint32_t)));
	EXPECT_EQ(entry.blockSize, blockSizeFromHeader);
}

TEST(FileBlockStorageChunkTests, DropBlocksAfterTruncatesFilesAndZerosIndex) {
	// Arrange:
	auto dataDir = CreateTempDirectory();
	FileBlockStorage storage(dataDir, FileBlockStorageMode::Hash_Index);

	constexpr uint32_t NumBlocks = 20;
	for (uint32_t i = 1; i <= NumBlocks; ++i) {
		storage.saveBlock(CreateTestBlockElement(Height(i)));
	}

	// Act: Drop blocks after height 10
	storage.dropBlocksAfter(Height(10));

	// Assert: Chain height should be 10
	EXPECT_EQ(Height(10), storage.chainHeight());

	// Assert: Verify blocks.dat is truncated to the offset of block 11
	char dirName[16];
	std::snprintf(dirName, sizeof(dirName), "%05lu", 0u);
	boost::filesystem::path blocksDatPath = dataDir;
	blocksDatPath /= dirName;
	blocksDatPath /= "blocks.dat";
	
	boost::filesystem::path idxPath = dataDir;
	idxPath /= dirName;
	idxPath /= "blocks.idx";

	RawFile idxFile(idxPath.generic_string().c_str(), OpenMode::Read_Only, LockMode::None);
	
	// Read entry for height 11 (index 10) to get truncation offset
	idxFile.seek(10 * sizeof(BlockChunkIndexEntry));
	BlockChunkIndexEntry entry11;
	idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&entry11), sizeof(BlockChunkIndexEntry)));
	
	EXPECT_EQ(boost::filesystem::file_size(blocksDatPath), entry11.blockOffset);

	// Assert: Verify statements.dat is also truncated (to 0 since no statements were saved)
	boost::filesystem::path stmtDatPath = dataDir;
	stmtDatPath /= dirName;
	stmtDatPath /= "statements.dat";
	if (boost::filesystem::exists(stmtDatPath)) {
		EXPECT_EQ(boost::filesystem::file_size(stmtDatPath), entry11.stmtOffset);
	}

	// Assert: Verify index entries from height 11 onwards are zeroed
	for (uint32_t i = 10; i < NumBlocks; ++i) {
		idxFile.seek(i * sizeof(BlockChunkIndexEntry));
		BlockChunkIndexEntry zeroedEntry;
		idxFile.read(MutableRawBuffer(reinterpret_cast<uint8_t*>(&zeroedEntry), sizeof(BlockChunkIndexEntry)));
		EXPECT_EQ(0u, zeroedEntry.blockOffset);
		EXPECT_EQ(0u, zeroedEntry.blockSize);
		EXPECT_EQ(0u, zeroedEntry.stmtOffset);
		EXPECT_EQ(0u, zeroedEntry.stmtSize);
	}
}

}}}
