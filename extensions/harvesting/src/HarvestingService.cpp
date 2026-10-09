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

#include "HarvestingService.h"
#include "catapult/harvesting_core/HarvestingUtFacadeFactory.h"
#include "ScheduledHarvesterTask.h"
#include "catapult/cache_core/ImportanceView.h"
#include "catapult/crypto/KeyUtils.h"
#include "catapult/extensions/ExecutionConfigurationFactory.h"
#include "catapult/extensions/ServiceLocator.h"
#include "catapult/extensions/ServiceState.h"
#include "catapult/io/BlockStorageCache.h"
#include "catapult/utils/ArraySet.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cctype>
#include <algorithm>

namespace catapult { namespace harvesting {

	namespace {
		boost::filesystem::path GetDelegatedKeysDirectory(const HarvestingConfiguration& config) {
			if (!config.DelegatedKeysDirectory.empty()) {
				boost::filesystem::path p(config.DelegatedKeysDirectory);
				return p.is_relative() ? config.ResourcesPath / p : p;
			}
			return config.ResourcesPath / "delegated_keys";
		}

		std::shared_ptr<UnlockedAccounts> CreateUnlockedAccounts(const HarvestingConfiguration& config) {
			auto pUnlockedAccounts = std::make_shared<UnlockedAccounts>(config.MaxUnlockedAccounts);
			if (config.IsAutoHarvestingEnabled) {
				// unlock configured account if it's eligible to harvest the next block
				auto keyPair = crypto::KeyPair::FromString(config.HarvestKey);
				auto publicKey = keyPair.publicKey();

				auto unlockResult = pUnlockedAccounts->modifier().add(std::move(keyPair));
				CATAPULT_LOG(info)
						<< "Unlocked harvesting account " << publicKey
						<< " for harvesting with result " << unlockResult;
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

		ScheduledHarvesterTaskOptions CreateHarvesterTaskOptions(extensions::ServiceState& state) {
			ScheduledHarvesterTaskOptions options;
			options.HarvestingAllowed = state.hooks().chainSyncedPredicate();
			options.LastBlockElementSupplier = [&storage = state.storage()]() {
				auto storageView = storage.view();
				return storageView.loadBlockElement(storageView.chainHeight());
			};
			options.TimeSupplier = state.timeSupplier();
			options.RangeConsumer = state.hooks().completionAwareBlockRangeConsumerFactory()(disruptor::InputSource::Local);
			return options;
		}

		void PruneUnlockedAccounts(
				UnlockedAccounts& unlockedAccounts,
				const cache::CatapultCache& cache,
				const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder,
				const Key& primaryHarvestKey) {
			auto cacheView = cache.createView();
			auto height = cacheView.height() + Height(1);
			auto readOnlyAccountStateCache = cache::ReadOnlyAccountStateCache(cacheView.sub<cache::AccountStateCache>());
			auto minHarvesterBalance = pConfigHolder->Config(height).Network.MinHarvesterBalance;
			unlockedAccounts.modifier().removeIf([height, minHarvesterBalance, &readOnlyAccountStateCache, &primaryHarvestKey](const auto& key) {
				if (key == primaryHarvestKey)
					return false;
				cache::ImportanceView view(readOnlyAccountStateCache);
				return !view.canHarvest(key, height, minHarvesterBalance);
			});
		}

		void SyncDelegatedHarvesterKeys(
				UnlockedAccounts& unlockedAccounts,
				const boost::filesystem::path& keysDir,
				const Key& primaryHarvestKey,
				std::time_t& lastCheckTime,
				uint32_t& tickCounter,
				const cache::CatapultCache& cache,
				const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder) {
			boost::system::error_code ec;
			if (!boost::filesystem::is_directory(keysDir, ec) || ec)
				return;

			// Check directory last write time
			auto currentWriteTime = boost::filesystem::last_write_time(keysDir, ec);
			if (ec)
				return;

			++tickCounter;
			// Re-scan if directory modified OR every 30 seconds (30 ticks)
			if (currentWriteTime == lastCheckTime && tickCounter % 30 != 0)
				return;

			lastCheckTime = currentWriteTime;

			auto cacheView = cache.createView();
			auto height = cacheView.height() + Height(1);
			auto readOnlyAccountStateCache = cache::ReadOnlyAccountStateCache(cacheView.sub<cache::AccountStateCache>());
			auto minHarvesterBalance = pConfigHolder->Config(height).Network.MinHarvesterBalance;
			cache::ImportanceView view(readOnlyAccountStateCache);

			utils::KeySet keysOnDisk;

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
						keysOnDisk.insert(publicKey);

						// Skip if account is already unlocked to eliminate periodic log spam
						if (unlockedAccounts.view().contains(publicKey))
							continue;

						// Check if account can harvest before unlocking
						if (!view.canHarvest(publicKey, height, minHarvesterBalance)) {
							CATAPULT_LOG(debug) << "Account " << publicKey << " in " << it->path().filename()
												<< " is not yet eligible to harvest (balance < min or not linked)";
							continue;
						}

						auto result = unlockedAccounts.modifier().add(std::move(keyPair));
						if (UnlockedAccountsAddResult::Success == result) {
							CATAPULT_LOG(info) << "Hot-loaded delegated harvesting account " << publicKey
											   << " from " << it->path().filename();
						} else if (UnlockedAccountsAddResult::Failure_Server_Limit == result) {
							CATAPULT_LOG(warning) << "Failed to unlock delegated account " << publicKey << ": MaxUnlockedAccounts limit reached";
						}
					}
				} catch (const std::exception& e) {
					CATAPULT_LOG(warning) << "Error reading key file " << it->path() << ": " << e.what();
				}
			}

			// Prune any accounts in unlockedAccounts that were deleted from disk (except the primary harvest key)
			unlockedAccounts.modifier().removeIf([&keysOnDisk, &primaryHarvestKey](const auto& key) {
				if (key == primaryHarvestKey)
					return false;
				return keysOnDisk.find(key) == keysOnDisk.end();
			});
		}

		thread::Task CreateHarvestingTask(
				extensions::ServiceState& state,
				UnlockedAccounts& unlockedAccounts,
				const Key& beneficiary,
				const boost::filesystem::path& keysDir,
				const Key& primaryHarvestKey) {
			const auto& cache = state.cache();
			const auto& pConfigHolder = state.pluginManager().configHolder();
			const auto& utCache = state.utCache();
			auto strategy = state.config().Node.TransactionSelectionStrategy;
			auto executionConfig = extensions::CreateExecutionConfiguration(state.pluginManager());
			HarvestingUtFacadeFactory utFacadeFactory(cache, executionConfig);

			auto blockGenerator = CreateHarvesterBlockGenerator(strategy, utFacadeFactory, utCache);
			auto pHarvesterTask = std::make_shared<ScheduledHarvesterTask>(
					CreateHarvesterTaskOptions(state),
					std::make_unique<Harvester>(cache, pConfigHolder, beneficiary, unlockedAccounts, blockGenerator));

			auto pState = std::make_shared<std::pair<std::time_t, uint32_t>>(0, 0);

			auto isChainSynced = state.hooks().chainSyncedPredicate();
			return thread::CreateNamedTask("harvesting task", [&cache, &unlockedAccounts, pHarvesterTask, pConfigHolder, keysDir, primaryHarvestKey, pState, isChainSynced]() {
				// prune accounts that are not eligible to harvest the next block only when chain is synced
				if (isChainSynced())
					PruneUnlockedAccounts(unlockedAccounts, cache, pConfigHolder, primaryHarvestKey);

				// hot-reload new delegated accounts from keys directory and prune deleted files
				SyncDelegatedHarvesterKeys(unlockedAccounts, keysDir, primaryHarvestKey, pState->first, pState->second, cache, pConfigHolder);

				// harvest the next block
				pHarvesterTask->harvest();
				return thread::make_ready_future(thread::TaskResult::Continue);
			});
		}

		class HarvestingServiceRegistrar : public extensions::ServiceRegistrar {
		public:
			explicit HarvestingServiceRegistrar(const HarvestingConfiguration& config) : m_config(config)
			{}

			extensions::ServiceRegistrarInfo info() const override {
				return { "Harvesting", extensions::ServiceRegistrarPhase::Post_Range_Consumers };
			}

			void registerServiceCounters(extensions::ServiceLocator& locator) override {
				locator.registerServiceCounter<UnlockedAccounts>(
						"unlockedAccounts",
						"UNLKED ACCTS",
						[](const auto& accounts) { return accounts.view().size(); });
			}

			void registerServices(extensions::ServiceLocator& locator, extensions::ServiceState& state) override {
				auto pUnlockedAccounts = CreateUnlockedAccounts(m_config);
				locator.registerRootedService("unlockedAccounts", pUnlockedAccounts);

				// add tasks
				auto keysDir = GetDelegatedKeysDirectory(m_config);
				auto beneficiary = crypto::ParseKey(m_config.Beneficiary);
				Key primaryHarvestKey{};
				if (m_config.IsAutoHarvestingEnabled && crypto::IsValidKeyString(m_config.HarvestKey)) {
					primaryHarvestKey = crypto::KeyPair::FromString(m_config.HarvestKey).publicKey();
				}

				state.tasks().push_back(CreateHarvestingTask(state, *pUnlockedAccounts, beneficiary, keysDir, primaryHarvestKey));
			}

		private:
			HarvestingConfiguration m_config;
		};
	}

	DECLARE_SERVICE_REGISTRAR(Harvesting)(const HarvestingConfiguration& config) {
		return std::make_unique<HarvestingServiceRegistrar>(config);
	}
}}

