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

#include "HostUtils.h"
#include "catapult/extensions/ProcessBootstrapper.h"
#include "catapult/io/IndexFile.h"
#include "catapult/plugins/PluginLoader.h"

namespace catapult { namespace local {

	namespace {
		class BootstrapperPluginLoader {
		public:
			explicit BootstrapperPluginLoader(extensions::ProcessBootstrapper& bootstrapper)
					: m_bootstrapper(bootstrapper)
			{}

		public:
			const std::vector<plugins::PluginModule>& modules() {
				return m_pluginModules;
			}

		public:
			void loadConfiguredPlugins(const model::NetworkConfiguration& config) {
				for (const auto& pair : config.Plugins) {
					bool load = true;
					for(const auto& systemName : m_bootstrapper.extensionManager().systemPluginNames()) {
						if(pair.first == systemName) {
							load = false;
							break;
						}
					}
					if(load) loadOne(pair.first);
				}

			}
			/// Loads plugins required for system booting
			void loadSystemPlugins() {
				for (const auto& pluginName : m_bootstrapper.extensionManager().systemPluginNames())
					loadOne(pluginName);
			}

		private:
			void loadOne(const std::string& pluginName) {
				auto& pluginManager = m_bootstrapper.pluginManager();
				LoadPluginByName(pluginManager, m_pluginModules, pluginManager.configHolder()->Config().User.PluginsDirectory, pluginName);
			}

		private:
			extensions::ProcessBootstrapper& m_bootstrapper;

			std::vector<plugins::PluginModule> m_pluginModules;
		};
	}

	std::vector<plugins::PluginModule> LoadSystemPlugins(extensions::ProcessBootstrapper& bootstrapper) {
		BootstrapperPluginLoader loader(bootstrapper);
		loader.loadSystemPlugins();
		return loader.modules();
	}
	bool IsStatePresent(const config::CatapultDataDirectory& dataDirectory) {
		if (extensions::HasSerializedState(dataDirectory.dir("state")))
			return false;

		auto indexPath = dataDirectory.rootDir().file("index.dat");
		if (boost::filesystem::exists(indexPath)) {
			try {
				io::IndexFile indexFile(indexPath);
				if (indexFile.get() > 1)
					return false;
			} catch (...) {}
		}

		return true;
	}

	std::unique_ptr<io::PrunableBlockStorage> CreateStagingBlockStorage(const config::CatapultDataDirectory& dataDirectory) {
		auto stagingDirectory = dataDirectory.spoolDir("block_sync").str();
		boost::filesystem::create_directory(stagingDirectory);
		return std::make_unique<io::FileBlockStorage>(stagingDirectory, io::FileBlockStorageMode::None);
	}

	std::vector<plugins::PluginModule> LoadConfigurablePlugins(extensions::ProcessBootstrapper& bootstrapper, const model::NetworkConfiguration& config) {
		BootstrapperPluginLoader loader(bootstrapper);
		loader.loadConfiguredPlugins(config);
		return loader.modules();
	}
}}
