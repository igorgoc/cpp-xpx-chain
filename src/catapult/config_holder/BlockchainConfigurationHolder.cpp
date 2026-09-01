/**
*** Copyright 2019 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#include "BlockchainConfigurationHolder.h"
#include "catapult/cache/CatapultCache.h"

namespace catapult { namespace config {

	BlockchainConfigurationHolder::BlockchainConfigurationHolder(cache::CatapultCache* pCache)
			: m_pCache(pCache)
			, m_pluginInitializer([](auto&) {}) {
		auto config = BlockchainConfiguration{
			ImmutableConfiguration::Uninitialized(),
			model::NetworkConfiguration::Uninitialized(),
			NodeConfiguration::Uninitialized(),
			LoggingConfiguration::Uninitialized(),
			UserConfiguration::Uninitialized(),
			ExtensionsConfiguration::Uninitialized(),
			InflationConfiguration::Uninitialized(),
			SupportedEntityVersions()
		};
		m_InflationCalculator = model::InflationCalculator();
		m_configs.insert({ Height(0), config });
	}

	BlockchainConfigurationHolder::BlockchainConfigurationHolder(const BlockchainConfiguration& config)
			:  m_pCache(nullptr)
			, m_pluginInitializer([](auto&) {}){

		m_configs.insert({ Height(0), config });
		m_InflationCalculator = model::InflationCalculator();
	}

	BlockchainConfigurationHolder::BlockchainConfigurationHolder(const BlockchainConfiguration& config, cache::CatapultCache* pCache, const Height& height)
			:  m_pCache(pCache)
			, m_pluginInitializer([](auto&) {}){


		m_configs.insert({ height, config });
		m_InflationCalculator = model::InflationCalculator();
		if(height != Height())
			m_InflationCalculator.add(height, config.Network.Inflation);
	}

	boost::filesystem::path BlockchainConfigurationHolder::GetResourcesPath(int argc, const char** argv) {
		return boost::filesystem::path(argc > 1 ? argv[1] : "..") / "resources";
	}

	const BlockchainConfiguration& BlockchainConfigurationHolder::Config(const Height& height) const {
		std::shared_lock lock(m_mutex);

		auto iter = m_configs.lower_bound(height);

		if (iter != m_configs.end() && iter->first != height) {
			iter = (iter == m_configs.begin()) ? m_configs.end() : --iter;
		} else if (iter == m_configs.end()) {
			if (!m_configs.empty()) {
				--iter;
			}
		}

		if (iter == m_configs.end())
			CATAPULT_THROW_INVALID_ARGUMENT_1("config not found at height", height);

		return iter->second;
	}

	const BlockchainConfiguration& BlockchainConfigurationHolder::Config() const {
		return Config(m_pCache != nullptr ? m_pCache->configHeight() : Height(0));
	}

	const BlockchainConfiguration& BlockchainConfigurationHolder::ConfigAtHeightOrLatest(const Height& height) const {
		if (height == HEIGHT_OF_LATEST_CONFIG)
			return Config();
		return Config(height);
	}

	const BlockchainConfiguration* BlockchainConfigurationHolder::LastConfigOrNull(const Height& height) const {
		auto iter = m_configs.lower_bound(height);

		if (iter != m_configs.end() && iter->first != height) {
			iter = (iter == m_configs.begin()) ? m_configs.end() : --iter;
		} else if (iter == m_configs.end()) {
			if (!m_configs.empty()) {
				--iter;
			}
		}

		if (iter == m_configs.end())
			return nullptr;

		return &iter->second;

	}

	void BlockchainConfigurationHolder::SetPluginInitializer(PluginInitializer&& initializer) {
		std::unique_lock lock(m_mutex);
		m_pluginInitializer = std::move(initializer);
		if (m_pluginInitializer) {
			for (auto& [_, config] : m_configs) {
				m_pluginInitializer(const_cast<model::NetworkConfiguration&>(config.Network));
			}
		}
	}

	void BlockchainConfigurationHolder::InitializeNetworkConfiguration(const model::NetworkConfiguration& networkConfiguration) {
		if(m_configs.empty() || m_configs.size() > 1)
			CATAPULT_THROW_RUNTIME_ERROR("Attempting to initialize configuration holder base config, but it's empty or has been initialized already.")
		std::unique_lock lock(m_mutex);
		const auto& baseConfig = m_configs.at(Height(0));
		auto config = BlockchainConfiguration(
			baseConfig.Immutable,
			networkConfiguration,
			baseConfig.Node,
			baseConfig.Logging,
			baseConfig.User,
			baseConfig.Extensions,
			baseConfig.Inflation,
			baseConfig.SupportedEntityVersions,
			Height(0),
			nullptr
		);
		if (m_pluginInitializer)
			m_pluginInitializer(const_cast<model::NetworkConfiguration&>(config.Network));

		m_configs.erase(Height(0));
		m_configs.insert({ Height(0), config });
	}

	void BlockchainConfigurationHolder::InsertConfig(const Height& height, const std::string& strConfig, const std::string& supportedVersion) {
		std::unique_lock lock(m_mutex);

		try {
			std::istringstream inputBlock(strConfig);
			auto networkConfig = model::NetworkConfiguration::LoadFromBag(utils::ConfigurationBag::FromStream(inputBlock));
			if (m_pluginInitializer)
				m_pluginInitializer(networkConfig);

			std::istringstream inputVersions(supportedVersion);
			config::SupportedEntityVersions supportedEntityVersions;
			supportedEntityVersions = LoadSupportedEntityVersions(inputVersions);

			InsertConfig(height, networkConfig, supportedEntityVersions);

		} catch (...) {
			CATAPULT_THROW_INVALID_ARGUMENT_1("unable to insert to config holder at height", height);
		}
	}

	void BlockchainConfigurationHolder::InsertConfig(const Height& height, const model::NetworkConfiguration& networkConfig, const config::SupportedEntityVersions& supportedEntities) {
		if (m_configs.empty() || m_configs.find(Height(0)) == m_configs.end()) {
			CATAPULT_LOG(warning) << "Cannot insert config at height " << height << " because base config Height(0) is not initialized";
			return;
		}
		const auto& baseConfig = m_configs.at(Height(0));
		auto config = BlockchainConfiguration(
				baseConfig.Immutable,
				networkConfig,
				baseConfig.Node,
				baseConfig.Logging,
				baseConfig.User,
				baseConfig.Extensions,
				baseConfig.Inflation,
				supportedEntities,
				height,
				LastConfigOrNull(height-Height(1))
		);
		if (m_pluginInitializer)
			m_pluginInitializer(const_cast<model::NetworkConfiguration&>(config.Network));

		m_configs.erase(height);
		m_configs.insert({ height, config });
		if(height != Height() && config.Network.Inflation != m_InflationCalculator.getSpotAmount(height-Height(1)))
			m_InflationCalculator.add(height, config.Network.Inflation);
	}
	const model::InflationCalculator& BlockchainConfigurationHolder::InflationCalculator() {
		return m_InflationCalculator;
	}

	void BlockchainConfigurationHolder::RemoveConfig(const Height& height) {
		std::unique_lock lock(m_mutex);
		m_InflationCalculator.remove(height);
		m_configs.erase(height);
	}

	void BlockchainConfigurationHolder::ClearPluginConfigurations() const {
		for (const auto& [_, config] : m_configs)
			config.Network.ClearPluginConfigurations();
	}

	void BlockchainConfigurationHolder::InsertBlockchainVersion(const Height& height, const BlockchainVersion& version) {
		std::unique_lock lock(m_versionMutex);
		m_versions.emplace(height, version);
	}

	void BlockchainConfigurationHolder::RemoveBlockchainVersion(const Height& height) {
		std::unique_lock lock(m_versionMutex);
		m_versions.erase(height);
	}

	BlockchainVersion BlockchainConfigurationHolder::Version(const Height& height) {
		std::unique_lock lock(m_versionMutex);

		auto iter = m_versions.lower_bound(height);

		if (iter != m_versions.end() && iter->first != height) {
			iter = (iter == m_versions.begin()) ? m_versions.end() : --iter;
		} else if (iter == m_versions.end()) {
			if (!m_versions.empty()) {
				--iter;
			}
		}

		if (iter == m_versions.end())
			return BlockchainVersion(0);

		return iter->second;
	}
}}
