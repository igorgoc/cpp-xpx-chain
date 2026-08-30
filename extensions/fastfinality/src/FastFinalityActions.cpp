/**
*** Copyright 2024 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#include "FastFinalityFsm.h"
#include "fastfinality/src/utils/FastFinalityUtils.h"
#include "catapult/api/ChainPackets.h"
#include "catapult/api/RemoteChainApi.h"
#include "catapult/chain/BlockDifficultyScorer.h"
#include "catapult/crypto/Signer.h"
#include "catapult/extensions/LocalNodeChainScore.h"
#include "catapult/extensions/PluginUtils.h"
#include "catapult/harvesting_core/UnlockedAccounts.h"
#include "catapult/ionet/PacketEntityUtils.h"
#include "catapult/model/BlockUtils.h"
#include "catapult/utils/StackLogger.h"
#include "catapult/validators/AggregateEntityValidator.h"

namespace catapult { namespace fastfinality {

	namespace {
		constexpr VersionType Block_Version = 7;
		constexpr uint Max_Failed_Node_State_Retrieval_Attempt = 100;

		bool ApprovalRatingSufficient(
				const double approvalRating,
				const double totalRating,
				const model::NetworkConfiguration& config) {
			return approvalRating / totalRating >= config.CommitteeEndSyncApproval;
		}

		void DelayAction(
				const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
				boost::asio::system_timer& timer,
				utils::TimePoint expirationTime,
				const action& callback,
				const action& cancelledCallback = [](){}) {
			timer.expires_at(expirationTime);
			timer.async_wait([pFsmWeak, callback, cancelledCallback](const boost::system::error_code& ec) {
				TRY_GET_FSM()

				if (ec) {
					if (ec == boost::asio::error::operation_aborted) {
						if (!pFsmShared->stopped())
							cancelledCallback();
						return;
					}

					CATAPULT_THROW_EXCEPTION(boost::system::system_error(ec));
				}

				callback();
			});
		}

		void DelayAction(
				const std::shared_ptr<FastFinalityFsm>& pFsmShared,
				boost::asio::system_timer& timer,
				uint64_t delay,
				const action& callback,
				const action& cancelledCallback = [](){}) {
			auto expirationTime = pFsmShared->fastFinalityData().round().RoundStart + std::chrono::milliseconds(delay);
			DelayAction(pFsmShared, timer, expirationTime, callback, cancelledCallback);
		}
	}

	action CreateFastFinalityCheckLocalChainAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			const extensions::ServiceState& state,
			const RemoteNodeStateRetriever& retriever,
			const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder,
			const model::BlockElementSupplier& lastBlockElementSupplier,
			const std::function<uint64_t (const Key&)>& importanceGetter,
			const dbrb::DbrbConfiguration& dbrbConfig) {
		return [pFsmWeak, &state, retriever, pConfigHolder, lastBlockElementSupplier, importanceGetter, dbrbConfig]() {
			TRY_GET_FSM()

			const auto& maxChainHeight = state.maxChainHeight();
			auto localHeight = lastBlockElementSupplier()->Block.Height;
			if (maxChainHeight > Height(0) && localHeight >= maxChainHeight) {
				pFsmShared->processEvent(Hold{});
				return;
			}

			pFsmShared->setNodeWorkState(NodeWorkState::Synchronizing);
			pFsmShared->resetChainSyncData();
			pFsmShared->resetFastFinalityData();
			auto& fastFinalityData = pFsmShared->fastFinalityData();
			fastFinalityData.setIsBlockBroadcastEnabled(false);

			auto dbrbProcess = pFsmShared->dbrbProcess();
			bool isInDbrbSystem = dbrbProcess.updateView(pConfigHolder, utils::NetworkTime(), localHeight + Height(1));

			std::vector<RemoteNodeState> remoteNodeStates = retriever();

			if (pFsmShared->stopped())
				return;

		  	const auto& config = pConfigHolder->Config().Network;
		  	if (remoteNodeStates.empty()) {
				fastFinalityData.incrementFailedNodeStateRetrievalCount();
				if (fastFinalityData.failedNodeStateRetrievalCount() >= Max_Failed_Node_State_Retrieval_Attempt) {
					fastFinalityData.resetFailedNodeStateRetrievalCount();
					dbrbProcess.messageSender()->closeAllConnections();
				}

				DelayAction(pFsmShared, pFsmShared->timer(), config.CommitteeChainHeightRequestInterval.millis(), [pFsmWeak] {
					TRY_GET_FSM()

					CATAPULT_LOG(debug) << "got no remote node states";
					pFsmShared->processEvent(NetworkHeightDetectionFailure{});
				});
				return;
			}

			std::sort(remoteNodeStates.begin(), remoteNodeStates.end(), [](auto a, auto b) {
				return (a.Height == b.Height ? a.BlockHash > b.BlockHash : a.Height > b.Height);
			});

			auto& chainSyncData = pFsmShared->chainSyncData();
			chainSyncData.NetworkHeight = remoteNodeStates.begin()->Height;
			chainSyncData.LocalHeight = localHeight;

			if (chainSyncData.NetworkHeight < chainSyncData.LocalHeight) {

				DelayAction(pFsmShared, pFsmShared->timer(), chain::CommitteePhaseCount * config.MinCommitteePhaseTime.millis(), [pFsmWeak] {
					TRY_GET_FSM()

					pFsmShared->processEvent(NetworkHeightLessThanLocal{});
				});

			} else if (chainSyncData.NetworkHeight > chainSyncData.LocalHeight) {

				for (const auto& state : remoteNodeStates) {
					if (state.Height < chainSyncData.NetworkHeight)
						break;

					chainSyncData.NodeIdentityKeys.push_back(state.NodeKey);
				}

				pFsmShared->processEvent(NetworkHeightGreaterThanLocal{});

			} else if (!dbrbConfig.IsDbrbProcess) {

				DelayAction(pFsmShared, pFsmShared->timer(), chain::CommitteePhaseCount * config.MinCommitteePhaseTime.millis(), [pFsmWeak] {
					TRY_GET_FSM()

					pFsmShared->processEvent(StartLocalChainCheck{});
				});

			} else {

				double approvalRating = 0;
				double totalRating = 0;
				const auto& localBlockHash = lastBlockElementSupplier()->EntityHash;

				for (const auto& state : remoteNodeStates) {
					uint64_t importance = 0;
					for (const auto& key : state.HarvesterKeys)
						importance += importanceGetter(key);
					double alpha;
					if (state.BlockHash != localBlockHash) {
						alpha = 0;
					} else if (state.NodeWorkState != NodeWorkState::Running) {
						alpha = config.CommitteeNotRunningContribution;
					} else {
						alpha = 1;
					}
					double importance_log = std::log10((double) (importance + config.CommitteeBaseTotalImportance));
					approvalRating += alpha * importance_log;
					totalRating += importance_log;
				}

				if (ApprovalRatingSufficient(approvalRating, totalRating, config)) {
					if (isInDbrbSystem) {
						pFsmShared->processEvent(NetworkHeightEqualToLocal{});
					} else {
						auto banned = (state.pluginManager().dbrbViewFetcher().getBanPeriod(dbrbProcess.id()) > BlockDuration(0));
						if (!banned)
							dbrbProcess.registerDbrbProcess(pConfigHolder, utils::NetworkTime(), localHeight + Height(1));
						DelayAction(pFsmShared, pFsmShared->timer(), config.CommitteeChainHeightRequestInterval.millis(), [pFsmWeak, banned] {
							TRY_GET_FSM()

							if (banned) {
								pFsmShared->processEvent(DbrbProcessBanned{});
							} else {
								pFsmShared->processEvent(NotRegisteredInDbrbSystem{});
							}
						});
					}
				} else {
					DelayAction(pFsmShared, pFsmShared->timer(), config.CommitteeChainHeightRequestInterval.millis(), [pFsmWeak] {
						TRY_GET_FSM()

						CATAPULT_LOG(debug) << "approval rating not sufficient";
						pFsmShared->processEvent(NetworkHeightDetectionFailure{});
					});
				}
			}
		};
	}

	action CreateFastFinalityResetLocalChainAction() {
		return []() {
			CATAPULT_THROW_RUNTIME_ERROR("local chain is invalid and needs to be reset")
		};
	}

	namespace {
		bool ValidateBlockCosignatures(const std::shared_ptr<model::Block>& pBlock, const chain::CommitteeManager& committeeManager, const model::NetworkConfiguration& config) {
			auto committee = committeeManager.committee();
			if (!committee.validateBlockProposer(pBlock->Signer)) {
				CATAPULT_LOG(warning) << "rejecting block, signer " << pBlock->Signer << " invalid";
				return false;
			}

			if (pBlock->EntityVersion() >= 6)
				return true;

			auto numCosignatures = pBlock->CosignaturesCount();
			if (committee.Cosigners.size() + 1u < numCosignatures) {
				CATAPULT_LOG(warning) << "rejecting block, number of cosignatures exceeded committee number";
				return false;
			}

			auto blockProposerWeight = committeeManager.weight(pBlock->Signer, config);
			auto actualSumOfVotes = blockProposerWeight;
			auto pCosignature = pBlock->CosignaturesPtr();
			for (auto i = 0u; i < numCosignatures; ++i, ++pCosignature) {
				if (committee.Cosigners.find(pCosignature->Signer) == committee.Cosigners.end()) {
					CATAPULT_LOG(warning) << "rejecting block, invalid cosigner " << pCosignature->Signer;
					return false;
				}

				if (!model::VerifyBlockHeaderCosignature(*pBlock, *pCosignature)) {
					CATAPULT_LOG(warning) << "rejecting block, cosignature invalid";
					return false;
				}

				committeeManager.add(actualSumOfVotes, committeeManager.weight(pCosignature->Signer, config));
			}

			chain::HarvesterWeight totalSumOfVotes = committeeManager.zeroWeight();
			for (const auto& cosigner : committee.Cosigners)
				committeeManager.add(totalSumOfVotes, committeeManager.weight(cosigner, config));

			auto minSumOfVotes = totalSumOfVotes;
			committeeManager.mul(minSumOfVotes, config.CommitteeApproval);
			if (!committeeManager.ge(actualSumOfVotes, minSumOfVotes)) {
				CATAPULT_LOG(warning) << "rejecting block, sum of votes insufficient: " << committeeManager.str(actualSumOfVotes) << " < " << committeeManager.str(minSumOfVotes);
				return false;
			}

			return true;
		}
	}

	action CreateFastFinalityDownloadBlocksAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state,
			const consumer<model::BlockRange&&, const disruptor::ProcessingCompleteFunc&>& rangeConsumer) {
		return [pFsmWeak, &state, rangeConsumer]() {
			TRY_GET_FSM()

			const auto& maxChainHeight = state.maxChainHeight();
			const auto& nodeConfig = state.config().Node;
			auto maxBlocksPerSyncAttempt = nodeConfig.MaxBlocksPerSyncAttempt;
			const auto& chainSyncData = pFsmShared->chainSyncData();
			auto startHeight = chainSyncData.LocalHeight + Height(1);
			auto targetHeight = std::min(chainSyncData.NetworkHeight, chainSyncData.LocalHeight + Height(maxBlocksPerSyncAttempt));
			api::BlocksFromOptions blocksFromOptions{
				utils::checked_cast<uint64_t, uint32_t>(targetHeight.unwrap() - chainSyncData.LocalHeight.unwrap()),
				nodeConfig.MaxChainBytesPerSyncAttempt.bytes32()
			};

			auto pMessageSender = pFsmShared->dbrbProcess().messageSender();
			pMessageSender->clearQueue();
			for (const auto& identityKey : chainSyncData.NodeIdentityKeys) {
				auto pPromise = std::make_shared<std::promise<std::vector<std::shared_ptr<model::Block>>>>();
				pFsmShared->packetHandlers().registerRemovableHandler(ionet::PacketType::Pull_Blocks_Response, [pPromise, identityKey, &transactionRegistry = state.pluginManager().transactionRegistry()](
						const auto& packet, auto& context) {
					auto blockRange = ionet::ExtractEntitiesFromPacket<model::Block>(packet, [&transactionRegistry](const model::Block& block) {
						return IsSizeValid(block, transactionRegistry);
					});
					if (!blockRange.empty() || sizeof(ionet::PacketHeader) == packet.Size) {
						pPromise->set_value(model::EntityRange<model::Block>::ExtractEntitiesFromRange(std::move(blockRange)));
					} else {
						std::ostringstream message;
						message << identityKey << " returned malformed packet for blocks from request";
						pPromise->set_exception(std::make_exception_ptr(catapult_runtime_error(message.str().data())));
					}
				});

				CATAPULT_LOG(debug) << "requesting blocks from " << identityKey << " starting at height " << startHeight << " to " << targetHeight;

				bool pullBlocksFailure = false;
				std::vector<std::shared_ptr<model::Block>> blocks;
				try {
					auto pPacket = ionet::CreateSharedPacket<api::PullBlocksRequest>();
					pPacket->Height = startHeight;
					pPacket->NumBlocks = blocksFromOptions.NumBlocks;
					pPacket->NumResponseBytes = blocksFromOptions.NumBytes;
					pMessageSender->enqueue(pPacket, true, { identityKey });
					auto future = pPromise->get_future();
					auto status = future.wait_for(std::chrono::seconds(15));
					if (std::future_status::ready != status) {
						CATAPULT_LOG(warning) << "pull blocks request timed out";
						pullBlocksFailure = true;
					} else {
						blocks = future.get();
					}
				} catch (std::exception const& error) {
					CATAPULT_LOG(warning) << "error downloading blocks: " << error.what();
					pullBlocksFailure = true;
				} catch (...) {
					CATAPULT_LOG(warning) << "error downloading blocks: unknown error";
					pullBlocksFailure = true;
				}

				pFsmShared->packetHandlers().removeHandler(ionet::PacketType::Pull_Blocks_Response);

				if (pFsmShared->stopped())
					return;

				if (pullBlocksFailure)
					continue;

				bool success = false;
				for (const auto& pBlock : blocks) {
					const auto& config = state.config(pBlock->Height).Network;
					auto blockchainVersion = state.pluginManager().configHolder()->Version(pBlock->Height);
					auto& committeeManager = state.pluginManager().getCommitteeManager(pBlock->EntityVersion());
					committeeManager.reset();
					while (committeeManager.committee().Round < pBlock->round())
						committeeManager.selectCommittee(config, blockchainVersion);
					CATAPULT_LOG(debug) << "block " << pBlock->Height << ": selected block producer for round " << pBlock->round();
					committeeManager.logCommittee();

					if (ValidateBlockCosignatures(pBlock, committeeManager, config)) {
						std::lock_guard<std::mutex> guard(pFsmShared->mutex());

						auto pPromise = std::make_shared<thread::promise<bool>>();
						rangeConsumer(model::BlockRange::FromEntity(pBlock), [pPromise, pBlock](auto, const auto& result) {
							bool success = (disruptor::CompletionStatus::Aborted != result.CompletionStatus);
							if (success) {
								CATAPULT_LOG(info) << "successfully committed block (height " << pBlock->Height << ", signer " << pBlock->Signer << ")";
							} else {
								auto validationResult = static_cast<validators::ValidationResult>(result.CompletionCode);
								CATAPULT_LOG(warning) << "block (height " << pBlock->Height << ") commit failed due to " << validationResult;
							}

							pPromise->set_value(std::move(success));
						});

						success = pPromise->get_future().get();
					} else {
						success = false;
					}

					if (!success) {
						startHeight = pBlock->Height;
						blocksFromOptions.NumBlocks = utils::checked_cast<uint64_t, uint32_t>(targetHeight.unwrap() - startHeight.unwrap()) + 1u;
						break;
					}

					if (maxChainHeight > Height(0) && pBlock->Height >= maxChainHeight) {
						pFsmShared->processEvent(Hold{});
						return;
					}
				}

				if (success) {
					pFsmShared->processEvent(DownloadBlocksSucceeded{});
					return;
				}
			}

			DelayAction(pFsmShared, pFsmShared->timer(), state.config().Network.CommitteeChainHeightRequestInterval.millis(), [pFsmWeak] {
				TRY_GET_FSM()

				pFsmShared->processEvent(DownloadBlocksFailed{});
			});
		};
	}

	namespace {
		std::string GetTimeString(const utils::TimePoint& timestamp) {
			auto time = std::chrono::system_clock::to_time_t(timestamp);
			char buffer[40];
			std::strftime(buffer, 40 ,"%F %T", std::localtime(&time));
			return buffer;
		}

		auto GetCurrentView(const std::shared_ptr<FastFinalityFsm>& pFsmShared, extensions::ServiceState& state) {
			const auto& fastFinalityData = pFsmShared->fastFinalityData();
			auto roundStart = utils::FromTimePoint(fastFinalityData.round().RoundStart);
			auto view = dbrb::View{ state.pluginManager().dbrbViewFetcher().getView(roundStart) };
			auto bootstrapView = dbrb::View{ state.config(fastFinalityData.currentBlockHeight()).Network.DbrbBootstrapProcesses };
			view.merge(bootstrapView);

			return view;
		}
	}

	action CreateFastFinalityDetectRoundAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			const model::BlockElementSupplier& lastBlockElementSupplier,
			extensions::ServiceState& state) {
		return [pFsmWeak, lastBlockElementSupplier, &state]() {
			TRY_GET_FSM()

		  	pFsmShared->resetChainSyncData();
			pFsmShared->setNodeWorkState(NodeWorkState::Running);
			auto& committeeManager = state.pluginManager().getCommitteeManager(Block_Version);
			committeeManager.reset();

			auto pLastBlockElement = lastBlockElementSupplier();
			const auto& block = pLastBlockElement->Block;
			auto currentHeight = block.Height + Height(1);
			const auto& config = state.pluginManager().config(currentHeight);
			auto blockchainVersion = state.pluginManager().configHolder()->Version(currentHeight);

			auto phaseTimeMillis = block.committeePhaseTime() ? block.committeePhaseTime() : config.CommitteePhaseTime.millis();
			auto roundStart = block.Timestamp + Timestamp(chain::CommitteePhaseCount * phaseTimeMillis);
			auto timeSupplier = state.timeSupplier();
			auto currentTime = timeSupplier();
			if (block.Timestamp > currentTime)
				CATAPULT_THROW_RUNTIME_ERROR_2("invalid current time", currentTime, block.Timestamp)

			switch (config.BlockTimeUpdateStrategy) {
				case model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient: {
					chain::DecreasePhaseTime(phaseTimeMillis, config);
					break;
				}
				case model::BlockTimeUpdateStrategy::Increase_Coefficient: {
					phaseTimeMillis = config.MinCommitteePhaseTime.millis();
					break;
				}
				case model::BlockTimeUpdateStrategy::None: {
					break;
				}
				default: {
					CATAPULT_THROW_INVALID_ARGUMENT_1("invalid block time update strategy value", utils::to_underlying_type(config.BlockTimeUpdateStrategy))
				}
			}

			auto nextRoundStart = roundStart + Timestamp(chain::CommitteePhaseCount * phaseTimeMillis);
			committeeManager.selectCommittee(config, blockchainVersion);

			while (nextRoundStart <= timeSupplier()) {
				roundStart = nextRoundStart;
				switch (config.BlockTimeUpdateStrategy) {
					case model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient: {
						[[fallthrough]];
					}
					case model::BlockTimeUpdateStrategy::Increase_Coefficient: {
						chain::IncreasePhaseTime(phaseTimeMillis, config);
						break;
					}
					case model::BlockTimeUpdateStrategy::None: {
						break;
					}
					default: {
						CATAPULT_THROW_INVALID_ARGUMENT_1("invalid block time update strategy value", utils::to_underlying_type(config.BlockTimeUpdateStrategy))
					}
				}
				nextRoundStart = nextRoundStart + Timestamp(chain::CommitteePhaseCount * phaseTimeMillis);

				committeeManager.selectCommittee(config, blockchainVersion);
			}

			auto committee = committeeManager.committee();
			auto timeSliceCount = committee.BlockProposers.size();
			auto roundTimeMillis = chain::CommitteePhaseCount * phaseTimeMillis;
			auto timeSliceMillis = roundTimeMillis / timeSliceCount;
			auto now = timeSupplier();
			uint64_t timeSliceIndex = 0u;
			if (now > roundStart)
				timeSliceIndex = (now.unwrap() - roundStart.unwrap()) / timeSliceMillis;
			auto roundStartTime = utils::ToTimePoint(roundStart);
			auto timeSliceStart = roundStartTime + std::chrono::milliseconds(timeSliceIndex * timeSliceMillis);

			FastFinalityRound round{
				committee.Round,
				roundStartTime,
				roundTimeMillis,
				timeSliceStart,
				timeSliceMillis,
				timeSliceIndex,
				timeSliceCount,
			};

			CATAPULT_LOG(debug) << "detected round: block " << currentHeight << ", start time " << GetTimeString(round.RoundStart) << ", round time " << round.RoundTimeMillis << "ms, round " << round.Round;
			auto& fastFinalityData = pFsmShared->fastFinalityData();
			fastFinalityData.setRound(round);
			fastFinalityData.setCurrentBlockHeight(currentHeight);

			DelayAction(pFsmShared, pFsmShared->timer(), 0, [pFsmWeak] {
				TRY_GET_FSM()

				pFsmShared->processEvent(RoundDetectionCompleted{});
			});
		};
	}

	action CreateFastFinalityCheckConnectionsAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state) {
		return [pFsmWeak, &state]() {
			TRY_GET_FSM()

			const auto& dbrbProcess = pFsmShared->dbrbProcess();
			auto view = GetCurrentView(pFsmShared, state);
			auto maxUnreachableNodeCount = dbrb::View::maxInvalidProcesses(view.Data.size());
			view.Data.erase(dbrbProcess.id());
			auto pMessageSender = dbrbProcess.messageSender();
			auto unreachableNodeCount = pMessageSender->getUnreachableNodeCount(view.Data);
			CATAPULT_LOG(warning) << "unreachable node count " << unreachableNodeCount << " / limit " << maxUnreachableNodeCount << " (view size " << (view.Data.size() + 1) << ")";
			if (unreachableNodeCount > maxUnreachableNodeCount) {
				CATAPULT_LOG(warning) << "unreachable node count exceeds the limit";
				const auto& config = state.config(pFsmShared->fastFinalityData().currentBlockHeight()).Network;
				DelayAction(pFsmShared, pFsmShared->timer(), config.CommitteeChainHeightRequestInterval.millis(), [pFsmWeak] {
					TRY_GET_FSM()

					pFsmShared->processEvent(ConnectionNumberInsufficient{});
				});
			} else {
				pFsmShared->processEvent(ConnectionNumberSufficient{});
			}
		};
	}

	action CreateFastFinalityStartRoundAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state) {
		return [pFsmWeak, &state]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			fastFinalityData.setUnexpectedBlockHeight(false);
			auto round = fastFinalityData.round();
			const auto& pluginManager = state.pluginManager();
			const auto& pConfigHolder = pluginManager.configHolder();
			auto roundStart = utils::FromTimePoint(round.RoundStart);
			auto& dbrbProcess = pFsmShared->dbrbProcess();
			bool isInDbrbSystem = dbrbProcess.updateView(pConfigHolder, roundStart, fastFinalityData.currentBlockHeight());
			dbrbProcess.registerDbrbProcess(pConfigHolder, roundStart, fastFinalityData.currentBlockHeight());
			if (!isInDbrbSystem) {
				auto banned = (pluginManager.dbrbViewFetcher().getBanPeriod(dbrbProcess.id()) > BlockDuration(0));
				DelayAction(pFsmShared, pFsmShared->timer(), pluginManager.config().CommitteeChainHeightRequestInterval.millis(), [pFsmWeak, banned] {
					TRY_GET_FSM()

					if (banned) {
						pFsmShared->processEvent(DbrbProcessBanned{});
					} else {
						pFsmShared->processEvent(NotRegisteredInDbrbSystem{});
					}
				});
				return;
			}

			fastFinalityData.setIsBlockBroadcastEnabled(true);
			auto& committeeManager = pluginManager.getCommitteeManager(Block_Version);
			auto committee = committeeManager.committee();
			if (committee.Round != round.Round)
				CATAPULT_THROW_RUNTIME_ERROR_2("invalid round", committee.Round, round.Round)

			pFsmShared->processEvent(RoundStarted{});
		};
	}

	action CreateFastFinalitySelectBlockProducerAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state) {
		return [pFsmWeak, &state]() {
			TRY_GET_FSM()

			const auto& pluginManager = state.pluginManager();
			const auto& pConfigHolder = pluginManager.configHolder();
			auto& fastFinalityData = pFsmShared->fastFinalityData();
			const auto& config = pConfigHolder->Config(fastFinalityData.currentBlockHeight()).Network;

			auto committee = pluginManager.getCommitteeManager(Block_Version).committee();
			auto round = fastFinalityData.round();
			const auto& blockProducer = committee.BlockProposers[round.TimeSliceIndex];

			auto accounts = fastFinalityData.unlockedAccounts()->view();
			auto blockProducerIter = std::find_if(accounts.begin(), accounts.end(), [&committee, &blockProducer](const auto& keyPair) {
				return (blockProducer == keyPair.publicKey());
			});
			bool isBlockProducer = (blockProducerIter != accounts.end());
			fastFinalityData.setBlockProducer(isBlockProducer ? &(*blockProducerIter) : nullptr);

			CATAPULT_LOG(debug) << "block producer selection result: block " << fastFinalityData.currentBlockHeight() << ", is block producer = "
				<< isBlockProducer << ", round start " << GetTimeString(round.RoundStart) << ", round time = " << round.RoundTimeMillis << "ms"
				<< ", time slice index " << round.TimeSliceIndex << ", time slice start " << GetTimeString(round.TimeSliceStart) << ", time slice = " << round.TimeSliceMillis << "ms";

			bool skipBlockProducing = ((state.timeSupplier()().unwrap() - utils::FromTimePoint(round.TimeSliceStart).unwrap()) > round.TimeSliceMillis / 2);
			if (isBlockProducer && !skipBlockProducing) {
				pFsmShared->processEvent(GenerateBlock{});
			} else {
				if (isBlockProducer)
					CATAPULT_LOG(debug) << "skipping block producing, current time is too far in the time slice";
				pFsmShared->processEvent(WaitForBlock{});
			}
		};
	}

	namespace {
		struct NextBlockContext {
		public:
			explicit NextBlockContext(const model::BlockElement& parentBlockElement, Timestamp nextTimestamp)
					: ParentBlock(parentBlockElement.Block)
					, ParentContext(parentBlockElement)
					, Timestamp(std::move(nextTimestamp))
					, Height(ParentBlock.Height + catapult::Height(1))
			{}

		public:
			const model::Block& ParentBlock;
			model::PreviousBlockContext ParentContext;
			catapult::Timestamp Timestamp;
			catapult::Height Height;
			catapult::Difficulty Difficulty;

		public:
			bool tryCalculateDifficulty(const cache::BlockDifficultyCache& cache, const model::NetworkConfiguration& config) {
				return chain::TryCalculateDifficulty(cache, state::BlockDifficultyInfo(Height, Timestamp, Difficulty), config, Difficulty);
			}
		};
	}

	action CreateFastFinalityGenerateBlockAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state,
			const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder,
			const harvesting::BlockGenerator& blockGenerator,
			const model::BlockElementSupplier& lastBlockElementSupplier) {
		return [pFsmWeak, &state, pConfigHolder, blockGenerator, lastBlockElementSupplier]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			fastFinalityData.setBlock(nullptr);
			auto round = fastFinalityData.round();
			auto pParentBlockElement = lastBlockElementSupplier();
			NextBlockContext context(*pParentBlockElement, utils::FromTimePoint(round.RoundStart));
			const auto& config = pConfigHolder->Config(context.Height);

			if (!config.Network.EnableDbrbFastFinality) {
				CATAPULT_LOG(warning) << "skipping block propose attempt due to DBRB fast finality is disabled";
				pFsmShared->processEvent(BlockGenerationFailed{});
				return;
			}

			if (!context.tryCalculateDifficulty(state.cache().sub<cache::BlockDifficultyCache>(), config.Network)) {
				CATAPULT_LOG(debug) << "skipping block propose attempt due to error calculating difficulty";
				pFsmShared->processEvent(BlockGenerationFailed{});
				return;
			}

			utils::StackLogger stackLogger("generating block", utils::LogLevel::Debug);
			auto pBlockHeader = model::CreateBlock(context.ParentContext, config.Immutable.NetworkIdentifier, fastFinalityData.blockProducer()->publicKey(), {});
			pBlockHeader->Difficulty = context.Difficulty;
			pBlockHeader->Timestamp = context.Timestamp;
			pBlockHeader->Beneficiary = fastFinalityData.beneficiary();
			pBlockHeader->setRound(round.Round);
			pBlockHeader->setCommitteePhaseTime(round.RoundTimeMillis / chain::CommitteePhaseCount);

			std::atomic_bool stopTransactionFetching = false;
			DelayAction(pFsmShared, pFsmShared->timer(), round.TimeSliceStart + std::chrono::milliseconds(round.TimeSliceMillis / 3), [&stopTransactionFetching] { stopTransactionFetching = true; });
			auto pBlock = utils::UniqueToShared(blockGenerator(*pBlockHeader, config.Network.MaxTransactionsPerBlock, [&stopTransactionFetching] { return stopTransactionFetching.load(); }));
			pFsmShared->timer().cancel();

			if (pBlock) {
				model::SignBlockHeader(*fastFinalityData.blockProducer(), *pBlock);
				auto pPacket = ionet::CreateSharedPacket<ionet::Packet>(pBlock->Size);
				pPacket->Type = ionet::PacketType::Push_Block;
				std::memcpy(static_cast<void*>(pPacket->Data()), pBlock.get(), pBlock->Size);

				DelayAction(pFsmShared, pFsmShared->timer(), round.TimeSliceStart + std::chrono::milliseconds(config.Network.CommitteeSilenceInterval.millis()), [pFsmWeak, pPacket, &state] {
					TRY_GET_FSM()

					auto view = GetCurrentView(pFsmShared, state);
					pFsmShared->dbrbProcess().broadcast(pPacket, view.Data);
				});

				pFsmShared->processEvent(BlockGenerationSucceeded{});
			} else {
				pFsmShared->processEvent(BlockGenerationFailed{});
			}
		};
	}

	action CreateFastFinalityWaitForBlockAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			const std::shared_ptr<config::BlockchainConfigurationHolder>& pConfigHolder) {
		return [pFsmWeak, pConfigHolder]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			if (fastFinalityData.block()) {
				pFsmShared->processEvent(BlockReceived{});
				return;
			}

			auto& dbrbProcess = pFsmShared->dbrbProcess();
			dbrbProcess.maybeDeliver();

			auto future = fastFinalityData.startWaitForBlock();
			auto round = fastFinalityData.round();
			auto now = std::chrono::system_clock::now();
			auto timeout = round.TimeSliceStart + std::chrono::milliseconds(round.TimeSliceMillis);
			if (timeout < now) {
				// If timeslice has already elapsed, give at least a small delay to avoid tight CPU spin
				timeout = now + std::chrono::milliseconds(50);
			}

			try {
				auto status = future.wait_until(timeout);
				if (std::future_status::ready == status) {
					if (future.get()) {
						pFsmShared->processEvent(BlockReceived{});
						return;
					} else if (pFsmShared->stopped()) {
						return;
					}
				}
			} catch (std::exception const& error) {
				CATAPULT_LOG(warning) << "error waiting for block: " << error.what();
			} catch (...) {
				CATAPULT_LOG(warning) << "error waiting for block: unknown error";
			}

			dbrbProcess.clearData();

			if (fastFinalityData.unexpectedBlockHeight()) {
				pFsmShared->processEvent(UnexpectedBlockHeight{});
			} else if (round.TimeSliceIndex + 1 == round.TimeSliceCount) {
				DelayAction(pFsmShared, pFsmShared->timer(), fastFinalityData.round().RoundTimeMillis, [pFsmWeak, pConfigHolder] {
					TRY_GET_FSM()

					auto& fastFinalityData = pFsmShared->fastFinalityData();
					auto round = fastFinalityData.round();
					const auto& config = pConfigHolder->Config(fastFinalityData.currentBlockHeight());
					bool syncWithNetwork = (fastFinalityData.proposedBlockHash() != Hash256()) || (round.Round % config.Network.CheckNetworkHeightInterval == 0);
					pFsmShared->processEvent(BlockNotReceived{ syncWithNetwork });
				});
			} else {
				round.TimeSliceIndex++;
				CATAPULT_LOG(debug) << "time slice index: " << round.TimeSliceIndex;
				round.TimeSliceStart += std::chrono::milliseconds(round.TimeSliceMillis);
				if (round.TimeSliceStart < std::chrono::system_clock::now()) {
					round.TimeSliceStart = std::chrono::system_clock::now();
				}
				fastFinalityData.setRound(round);
				fastFinalityData.setProposedBlockHash(Hash256());
				pFsmShared->processEvent(SelectNextBlockProducer{});
			}
		};
	}

	action CreateFastFinalityCommitBlockAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			const consumer<model::BlockRange&&, const disruptor::ProcessingCompleteFunc&>& rangeConsumer,
			extensions::ServiceState& state) {
		return [pFsmWeak, rangeConsumer, &state]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			auto pBlock = fastFinalityData.block();
			const auto& committeeManager = state.pluginManager().getCommitteeManager(Block_Version);
			auto committee = committeeManager.committee();

			if (pBlock->round() != committee.Round || pBlock->Signer != committee.BlockProposers[fastFinalityData.round().TimeSliceIndex] || pBlock->Height != fastFinalityData.currentBlockHeight()) {
				pFsmShared->processEvent(UnexpectedBlock{});
				return;
			}

			bool success;
			{
				// Commit block.
				std::lock_guard<std::mutex> guard(pFsmShared->mutex());

				auto pPromise = std::make_shared<thread::promise<bool>>();
				rangeConsumer(model::BlockRange::FromEntity(pBlock), [pPromise, pBlock](auto, const auto& result) {
					bool success = (disruptor::CompletionStatus::Aborted != result.CompletionStatus);
					if (success) {
						CATAPULT_LOG(info) << "successfully committed block " << pBlock->Height << " produced by " << pBlock->Signer;
					} else {
						auto validationResult = static_cast<validators::ValidationResult>(result.CompletionCode);
						CATAPULT_LOG(warning) << "commit of block " << pBlock->Height << " produced by " << pBlock->Signer << " failed due to " << validationResult;
					}

					pPromise->set_value(std::move(success));
				});

				success = pPromise->get_future().get();
			}

			DelayAction(pFsmShared, pFsmShared->timer(), fastFinalityData.round().RoundTimeMillis, [pFsmWeak, success, &state] {
				TRY_GET_FSM()

				pFsmShared->dbrbProcess().clearData();

				const auto& maxChainHeight = state.maxChainHeight();
				if (success && (maxChainHeight > Height(0)) && (pFsmShared->fastFinalityData().block()->Height >= maxChainHeight)) {
					pFsmShared->processEvent(Hold{});
				} else if (success) {
					pFsmShared->processEvent(CommitBlockSucceeded{});
				} else {
					pFsmShared->processEvent(CommitBlockFailed{});
				}
			});
		};
	}

	action CreateFastFinalityIncrementRoundAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state) {
		return [pFsmWeak, &state]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			auto currentRound = fastFinalityData.round();
			auto pConfigHolder = state.pluginManager().configHolder();
			auto height = fastFinalityData.currentBlockHeight() + Height(1);
			const auto& config = pConfigHolder->Config(height).Network;
			auto blockchainVersion = pConfigHolder->Version(height);
			auto& committeeManager = state.pluginManager().getCommitteeManager(Block_Version);
			committeeManager.selectCommittee(config, blockchainVersion);
			int64_t nextRound = currentRound.Round + 1;
			CATAPULT_LOG(debug) << "block " << height << ": selected committee for round " << nextRound;
			committeeManager.logCommittee();
			pFsmShared->resetFastFinalityData();

			CATAPULT_LOG(debug) << "incremented round " << nextRound;
			auto nextRoundStart = currentRound.RoundStart + std::chrono::milliseconds(currentRound.RoundTimeMillis);
			uint64_t roundTimeMillis = 0;
			switch (config.BlockTimeUpdateStrategy) {
				case model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient: {
					[[fallthrough]];
				}
				case model::BlockTimeUpdateStrategy::Increase_Coefficient: {
					uint64_t nextPhaseTimeMillis = currentRound.RoundTimeMillis / chain::CommitteePhaseCount;
					chain::IncreasePhaseTime(nextPhaseTimeMillis, config);
					roundTimeMillis = chain::CommitteePhaseCount * nextPhaseTimeMillis;
					break;
				}
				case model::BlockTimeUpdateStrategy::None: {
					roundTimeMillis = currentRound.RoundTimeMillis;
					break;
				}
				default: {
					CATAPULT_THROW_INVALID_ARGUMENT_1("invalid block time update strategy value", utils::to_underlying_type(config.BlockTimeUpdateStrategy))
				}
			}
			uint64_t nextPhaseTimeMillis = currentRound.RoundTimeMillis / chain::CommitteePhaseCount;
			chain::IncreasePhaseTime(nextPhaseTimeMillis, config);
			auto committee = committeeManager.committee();
			auto timeSliceCount = committee.BlockProposers.size();
			fastFinalityData.setRound(FastFinalityRound{
				nextRound,
				nextRoundStart,
				roundTimeMillis,
				nextRoundStart,
				roundTimeMillis / timeSliceCount,
				0,
				timeSliceCount,
			});
		};
	}

	action CreateFastFinalityResetRoundAction(
			const std::weak_ptr<FastFinalityFsm>& pFsmWeak,
			extensions::ServiceState& state) {
		return [pFsmWeak, &state]() {
			TRY_GET_FSM()

			auto& fastFinalityData = pFsmShared->fastFinalityData();
			auto currentRound = fastFinalityData.round();
			auto pConfigHolder = state.pluginManager().configHolder();
			auto height = fastFinalityData.currentBlockHeight() + Height(1);
			const auto& config = pConfigHolder->Config(height).Network;
			auto blockchainVersion = pConfigHolder->Version(height);
			auto& committeeManager = state.pluginManager().getCommitteeManager(Block_Version);
			committeeManager.reset();
			committeeManager.selectCommittee(config, blockchainVersion);
			CATAPULT_LOG(debug) << "block " << height << ": selected committee for round 0";
			committeeManager.logCommittee();
			pFsmShared->resetFastFinalityData();

			auto nextRoundStart = currentRound.RoundStart + std::chrono::milliseconds(currentRound.RoundTimeMillis);
			uint64_t roundTimeMillis = 0;
			switch (config.BlockTimeUpdateStrategy) {
				case model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient: {
					uint64_t nextPhaseTimeMillis = currentRound.RoundTimeMillis / chain::CommitteePhaseCount;
					chain::DecreasePhaseTime(nextPhaseTimeMillis, config);
					roundTimeMillis = chain::CommitteePhaseCount * nextPhaseTimeMillis;
					break;
				}
				case model::BlockTimeUpdateStrategy::Increase_Coefficient: {
					roundTimeMillis = chain::CommitteePhaseCount * config.MinCommitteePhaseTime.millis();
					break;
				}
				case model::BlockTimeUpdateStrategy::None: {
					roundTimeMillis = currentRound.RoundTimeMillis;
					break;
				}
				default: {
					CATAPULT_THROW_INVALID_ARGUMENT_1("invalid block time update strategy value", utils::to_underlying_type(config.BlockTimeUpdateStrategy))
				}
			}
			fastFinalityData.incrementCurrentBlockHeight();
			auto committee = committeeManager.committee();
			auto timeSliceCount = committee.BlockProposers.size();
			fastFinalityData.setRound(FastFinalityRound{
				0u,
				nextRoundStart,
				roundTimeMillis,
				nextRoundStart,
				roundTimeMillis / timeSliceCount,
				0,
				timeSliceCount,
			});
		};
	}
}}