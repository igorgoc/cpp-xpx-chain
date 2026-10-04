/**
*** Copyright 2022 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#include "FastFinalityUtils.h"
#include "catapult/crypto/KeyPair.h"
#include "catapult/extensions/ServiceState.h"
#include "catapult/extensions/ExecutionConfigurationFactory.h"
#include "catapult/handlers/ChainHandlers.h"
#include "catapult/harvesting_core/HarvestingConfiguration.h"
#include "catapult/harvesting_core/HarvestingUtFacadeFactory.h"
#include "catapult/harvesting_core/UnlockedAccounts.h"

#include "FastFinalityUtils.h"
#include "catapult/crypto/KeyPair.h"
#include "catapult/crypto/KeyUtils.h"
#include "catapult/extensions/ServiceState.h"
#include "catapult/extensions/ExecutionConfigurationFactory.h"
#include "catapult/handlers/ChainHandlers.h"
#include "catapult/harvesting_core/HarvestingConfiguration.h"
#include "catapult/harvesting_core/HarvestingUtFacadeFactory.h"
#include "catapult/harvesting_core/UnlockedAccounts.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cctype>
#include <algorithm>

namespace catapult { namespace fastfinality {

	boost::filesystem::path GetDelegatedKeysDirectory(const harvesting::HarvestingConfiguration& config) {
		if (!config.DelegatedKeysDirectory.empty()) {
			boost::filesystem::path p(config.DelegatedKeysDirectory);
			return p.is_relative() ? config.ResourcesPath / p : p;
		}
		return config.ResourcesPath / "delegated_keys";
	}

	void SyncDelegatedHarvesterKeys(harvesting::UnlockedAccounts& unlockedAccounts, const boost::filesystem::path& keysDir) {
		static std::time_t lastCheckTime = 0;
		static uint32_t tickCounter = 0;

		boost::system::error_code ec;
		if (!boost::filesystem::is_directory(keysDir, ec) || ec)
			return;

		auto currentWriteTime = boost::filesystem::last_write_time(keysDir, ec);
		if (ec)
			return;

		++tickCounter;
		if (currentWriteTime == lastCheckTime && tickCounter % 10 != 0)
			return;

		lastCheckTime = currentWriteTime;

		for (boost::filesystem::directory_iterator it(keysDir, ec), end; it != end && !ec; it.increment(ec)) {
			if (!boost::filesystem::is_regular_file(it->status()) || it->path().extension() != ".key")
				continue;

			try {
				std::ifstream file(it->path().string());
				if (!file.is_open())
					continue;

				std::string line;
				while (std::getline(file, line)) {
					line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) { return !std::isspace(ch); }));
					line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), line.end());

					if (line.empty() || line[0] == '#')
						continue;

					if (!crypto::IsValidKeyString(line)) {
						CATAPULT_LOG(warning) << "Ignoring malformed key in " << it->path().filename() << ": " << line;
						continue;
					}

					auto keyPair = crypto::KeyPair::FromString(line);
					auto publicKey = keyPair.publicKey();

					auto result = unlockedAccounts.modifier().add(std::move(keyPair));
					if (harvesting::UnlockedAccountsAddResult::Success == result) {
						CATAPULT_LOG(info) << "Hot-loaded delegated harvesting account " << publicKey
										   << " from " << it->path().filename();
					} else if (harvesting::UnlockedAccountsAddResult::Failure_Server_Limit == result) {
						CATAPULT_LOG(warning) << "Failed to unlock delegated account " << publicKey << ": MaxUnlockedAccounts limit reached";
					}
				}
			} catch (const std::exception& e) {
				CATAPULT_LOG(warning) << "Error reading key file " << it->path() << ": " << e.what();
			}
		}
	}

	std::shared_ptr<harvesting::UnlockedAccounts> CreateUnlockedAccounts(const harvesting::HarvestingConfiguration& config) {
		auto pUnlockedAccounts = std::make_shared<harvesting::UnlockedAccounts>(config.MaxUnlockedAccounts);
		if (config.IsAutoHarvestingEnabled) {
			auto keyPair = crypto::KeyPair::FromString(config.HarvestKey);
			auto publicKey = keyPair.publicKey();

			auto unlockResult = pUnlockedAccounts->modifier().add(std::move(keyPair));
			CATAPULT_LOG(info) << "Added account " << publicKey << " for harvesting with result " << unlockResult;
		}

		// Pre-load existing delegated keys from directory on boot
		auto keysDir = GetDelegatedKeysDirectory(config);
		boost::system::error_code ec;
		if (boost::filesystem::is_directory(keysDir, ec) && !ec) {
			for (boost::filesystem::directory_iterator it(keysDir, ec), end; it != end && !ec; it.increment(ec)) {
				if (!boost::filesystem::is_regular_file(it->status()) || it->path().extension() != ".key")
					continue;

				try {
					std::ifstream file(it->path().string());
					std::string line;
					while (std::getline(file, line)) {
						line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) { return !std::isspace(ch); }));
						line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), line.end());

						if (line.empty() || line[0] == '#')
							continue;

						if (crypto::IsValidKeyString(line)) {
							auto keyPair = crypto::KeyPair::FromString(line);
							auto publicKey = keyPair.publicKey();
							auto unlockResult = pUnlockedAccounts->modifier().add(std::move(keyPair));
							CATAPULT_LOG(info)
									<< "Pre-loaded delegated harvesting account " << publicKey
									<< " from " << it->path().filename()
									<< " with result " << unlockResult;
						}
					}
				} catch (const std::exception& e) {
					CATAPULT_LOG(warning) << "Error pre-loading key file " << it->path() << ": " << e.what();
				}
			}
		}

		return pUnlockedAccounts;
	}

	harvesting::BlockGenerator CreateHarvesterBlockGenerator(extensions::ServiceState& state) {
		auto strategy = state.config().Node.TransactionSelectionStrategy;
		auto executionConfig = extensions::CreateExecutionConfiguration(state.pluginManager());
		harvesting::HarvestingUtFacadeFactory utFacadeFactory(state.cache(), executionConfig);

		return harvesting::CreateHarvesterBlockGenerator(strategy, utFacadeFactory, state.utCache());
	}

	handlers::PullBlocksHandlerConfiguration CreatePullBlocksHandlerConfiguration(const config::NodeConfiguration& nodeConfig) {
		handlers::PullBlocksHandlerConfiguration config {
			nodeConfig.MaxBlocksPerSyncAttempt,
			nodeConfig.MaxChainBytesPerSyncAttempt.bytes32(),
		};

		return config;
	}
}}