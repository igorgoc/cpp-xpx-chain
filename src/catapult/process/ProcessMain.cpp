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

#include "ProcessMain.h"
#include "Signals.h"
#include "catapult/config_holder/BlockchainConfigurationHolder.h"
#include "catapult/config/ValidateConfiguration.h"
#include "catapult/crypto/KeyPair.h"
#include "catapult/crypto/KeyUtils.h"
#include "catapult/io/FileLock.h"
#include "catapult/thread/ThreadInfo.h"
#include "catapult/utils/ExceptionLogging.h"
#include "catapult/version/version.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace catapult { namespace process {

	namespace {
		// region initialization utils

		std::unique_ptr<utils::LogFilter> CreateLogFilter(const config::BasicLoggerConfiguration& config) {
			auto pFilter = std::make_unique<utils::LogFilter>(config.Level);
			for (const auto& pair : config.ComponentLevels)
				pFilter->setLevel(pair.first.c_str(), pair.second);

			return pFilter;
		}

		std::shared_ptr<void> SetupLogging(const config::LoggingConfiguration& config) {
			auto pBootstrapper = std::make_shared<utils::LoggingBootstrapper>();
			pBootstrapper->addConsoleLogger(config::GetConsoleLoggerOptions(config.Console), *CreateLogFilter(config.Console));
			pBootstrapper->addFileLogger(config::GetFileLoggerOptions(config.File), *CreateLogFilter(config.File));
			return std::move(pBootstrapper);
		}

		[[noreturn]]
		void TerminateHandler() noexcept {
			// 1. if termination is caused by an exception, log it
			if (std::current_exception()) {
				CATAPULT_LOG(fatal)
						<< std::endl << "thread: " << thread::GetThreadName()
						<< std::endl << UNHANDLED_EXCEPTION_MESSAGE("running local node");
			}

			// 2. flush the log and abort
			utils::CatapultLogFlush();
			std::abort();
		}

		// endregion

		void Run(const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder, ProcessOptions processOptions, const CreateProcessHost& createProcessHost) {
			auto keyPair = crypto::KeyPair::FromString(pConfigHolder->Config().User.BootKey);

			CATAPULT_LOG(info) << "booting process with public key " << crypto::FormatKey(keyPair.publicKey());
			auto pProcessHost = createProcessHost(pConfigHolder, keyPair);

			if (ProcessOptions::Exit_After_Termination_Signal == processOptions)
				WaitForTerminationSignal();

			CATAPULT_LOG(info) << "shutting down process";
			pProcessHost.reset();
		}
	}

	int ProcessMain(int argc, const char** argv, const std::string& host, const CreateProcessHostConfig& createProcessConfig,const CreateProcessHost& createProcessHost) {
		return ProcessMain(argc, argv, host, ProcessOptions::Exit_After_Termination_Signal, createProcessConfig, createProcessHost);
	}

	int ProcessMain(
			int argc,
			const char** argv,
			const std::string& host,
			ProcessOptions processOptions,
			const CreateProcessHostConfig& createProcessConfig,
			const CreateProcessHost& createProcessHost) {
#ifndef _WIN32
		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
			rl.rlim_cur = 65536;
			if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
				rl.rlim_cur = 10240;
				setrlimit(RLIMIT_NOFILE, &rl);
			}
		}
#endif
		std::set_terminate(&TerminateHandler);
		thread::SetThreadName("Process Main (" + host + ")");
		version::WriteVersionInformation(std::cout);

		// 1. load and validate the configuration

		auto resourcesPath = config::BlockchainConfigurationHolder::GetResourcesPath(argc, argv);
		auto pConfigHolder = createProcessConfig(resourcesPath, host);

		// 2. initialize logging
		auto pLoggingGuard = SetupLogging(pConfigHolder->Config().Logging);

		// 3. check instance
		boost::filesystem::path lockFilePath = pConfigHolder->Config().User.DataDirectory;
		lockFilePath /= host + ".lock";
		if ( boost::filesystem::exists(boost::filesystem::path( pConfigHolder->Config().User.DataDirectory ) / "../../../../../autoremove-server-lock") ) {
			if ( boost::filesystem::exists(lockFilePath) ) {
				boost::filesystem::remove(lockFilePath);
			}
		}
		io::FileLock instanceLock(lockFilePath.generic_string());
		if (!instanceLock.try_lock()) {
			CATAPULT_LOG(fatal) << "could not acquire instance lock " << lockFilePath;
			return -3;
		}

		// 4. run the server
		Run(pConfigHolder, processOptions, createProcessHost);
		return 0;
	}
}}
