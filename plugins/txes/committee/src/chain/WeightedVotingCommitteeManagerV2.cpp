/**
*** Copyright 2024 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#include "WeightedVotingCommitteeManagerV2.h"
#include "src/config/CommitteeConfiguration.h"
#include "catapult/utils/NetworkTime.h"

namespace catapult { namespace chain {

	void WeightedVotingCommitteeManagerV2::logAccountData(const cache::AccountMap& accounts, const config::CommitteeConfiguration& config) {
		CATAPULT_LOG(trace) << "Harvester account data:";
		for (const auto& pair : accounts) {
			const auto& data = pair.second;
			CATAPULT_LOG(trace) << "committee account " << pair.first << " data: "
				<< calculateWeight(data, config) << "|" << data.Activity << "|" << data.FeeInterest << "|" << data.FeeInterestDenominator << "|" << data.LastSigningBlockHeight << "|"
				<< data.CanHarvest << "|" << data.EffectiveBalance;
		}
	}

	void WeightedVotingCommitteeManagerV2::decreaseActivity(const Key& key, cache::AccountMap& accounts, const config::CommitteeConfiguration& config) {
		auto iter = accounts.find(key);
		if (iter == accounts.end())
			CATAPULT_THROW_RUNTIME_ERROR_1("account not found", key)
		auto& data = iter->second;
		data.decreaseActivity(config.ActivityCommitteeNotCosignedDeltaInt);

		CATAPULT_LOG(trace) << "committee account " << key << ": activity " << data.Activity << ", weight " << calculateWeight(data, config);
	}

	Hash256& WeightedVotingCommitteeManagerV2::Hasher::calculateHash(Hash256& hash, const GenerationHash& generationHash, const Key& key) {
		crypto::Sha3_256_Builder sha3;
		sha3.update({ generationHash, key });
		sha3.final(hash);
		return hash;
	}

	Hash256& WeightedVotingCommitteeManagerV2::Hasher::calculateHash(Hash256& hash) {
		crypto::Sha3_256_Builder sha3;
		sha3.update(hash);
		sha3.final(hash);
		return hash;
	}

	Hash256 WeightedVotingCommitteeManagerV2::Hasher::calculateHash(int64_t rate, const Key& key) {
		Hash256 hash;
		crypto::Sha3_256_Builder sha3;
		sha3.update({ RawBuffer{reinterpret_cast<const uint8_t*>(&rate), sizeof(rate)}, key });
		sha3.final(hash);
		return hash;
	}

	void WeightedVotingCommitteeManagerV2::logProcess(const char* prefix, const Key& process, const Timestamp& timestamp, std::ostringstream& out) {
		auto time = std::chrono::system_clock::to_time_t(utils::ToTimePoint(timestamp));
		char buffer[40];
		std::strftime(buffer, 40 ,"%F %T", std::localtime(&time));
		out << std::endl << prefix << process << " expires at: " << buffer;
	}

	void WeightedVotingCommitteeManagerV2::logHarvesters() const {
		std::ostringstream out;
		out << std::endl << "Harvesters (" << m_accounts.size() << "):";
		for (const auto& [ key, data ] : m_accounts)
			logProcess("harvester ", key, data.ExpirationTime, out);

		CATAPULT_LOG(trace) << out.str();
	}

	void WeightedVotingCommitteeManagerV2::logCommittee() const {
		std::lock_guard<std::mutex> guard(m_mutex);

		logHarvesters();

		std::ostringstream out;
		out << std::endl << "Committee (" << m_committee.Cosigners.size() + 1u << "):";
		auto iter = m_accounts.find(m_committee.BlockProposer);
		if (iter == m_accounts.end())
			CATAPULT_THROW_RUNTIME_ERROR_1("account not found", m_committee.BlockProposer)
		logProcess("block proposer ", m_committee.BlockProposer, iter->second.ExpirationTime, out);
		for (const auto& key : m_committee.Cosigners) {
			iter = m_accounts.find(key);
			if (iter == m_accounts.end())
				CATAPULT_THROW_RUNTIME_ERROR_1("account not found", key)
			logProcess("committee member ", key, iter->second.ExpirationTime, out);
		}

		CATAPULT_LOG(debug) << out.str();
	}

	WeightedVotingCommitteeManagerV2::WeightedVotingCommitteeManagerV2(std::shared_ptr<cache::CommitteeAccountCollector> pAccountCollector)
			: m_pAccountCollector(std::move(pAccountCollector))
			, m_accounts(m_pAccountCollector->accounts())
			, m_phaseTime(0u) {
		setFilter([](const Key& key, const config::CommitteeConfiguration& config) { return true; });
		setIneligibleHarvesterHandler([](const Key& key) {});
	}

	void WeightedVotingCommitteeManagerV2::reset() {
		std::lock_guard<std::mutex> guard(m_mutex);

		m_hashes.clear();
		m_committee = Committee();
		m_accounts = m_pAccountCollector->accounts();
	}

	Committee WeightedVotingCommitteeManagerV2::committee() const {
		std::lock_guard<std::mutex> guard(m_mutex);

		return m_committee;
	}

	cache::AccountMap WeightedVotingCommitteeManagerV2::accounts() {
		std::lock_guard<std::mutex> guard(m_mutex);

		return m_accounts;
	}

	int64_t WeightedVotingCommitteeManagerV2::calculateWeight(const state::AccountData& accountData, const config::CommitteeConfiguration& config) {
		if (config.EnableEqualWeights)
			return static_cast<int64_t>(config.WeightScaleFactor);

		auto weight = static_cast<int64_t>(config.WeightScaleFactor / (1.0 + std::exp(static_cast<double>(-accountData.Activity) / config.ActivityScaleFactor)));
		if (weight == 0)
			weight = 1;

		return weight;
	}

	HarvesterWeight WeightedVotingCommitteeManagerV2::weight(const Key& accountKey, const model::NetworkConfiguration& networkConfig) const {
		const auto& config = networkConfig.GetPluginConfiguration<config::CommitteeConfiguration>();
		if (config.EnableEqualWeights)
			return HarvesterWeight{ .n = static_cast<int64_t>(config.WeightScaleFactor) };

		std::lock_guard<std::mutex> guard(m_mutex);

		auto iter = m_accounts.find(accountKey);
		HarvesterWeight weight{};
		weight.n = (m_accounts.end() != iter) ? calculateWeight(iter->second, config) : 0;

		return weight;
	}

	HarvesterWeight WeightedVotingCommitteeManagerV2::zeroWeight() const {
		return HarvesterWeight{ .n = 0 };
	}

	void WeightedVotingCommitteeManagerV2::add(HarvesterWeight& weight, const chain::HarvesterWeight& delta) const {
		weight.n += delta.n;
	}

	void WeightedVotingCommitteeManagerV2::mul(HarvesterWeight& weight, double multiplier) const {
		weight.n = static_cast<int64_t>(static_cast<double>(weight.n) * multiplier);
	}

	bool WeightedVotingCommitteeManagerV2::ge(const HarvesterWeight& weight1, const chain::HarvesterWeight& weight2) const {
		return weight1.n >= weight2.n
		   || std::abs(weight1.n - weight2.n) <= static_cast<int64_t>(std::numeric_limits<double>::epsilon() * std::abs(static_cast<double>(weight1.n) + static_cast<double>(weight2.n)));
	}

	bool WeightedVotingCommitteeManagerV2::eq(const HarvesterWeight& weight1, const chain::HarvesterWeight& weight2) const {
		return weight1.n == weight1.n;
	}

	std::string WeightedVotingCommitteeManagerV2::str(const HarvesterWeight& weight) const {
		std::ostringstream out;
		out << weight.n;

		return out.str();
	}

	void WeightedVotingCommitteeManagerV2::decreaseActivities(const config::CommitteeConfiguration& config) {
		CATAPULT_LOG(trace) << "decreasing activities of previous committee accounts";
		if (m_committee.BlockProposers.empty()) {
			decreaseActivity(m_committee.BlockProposer, m_accounts, config);
		} else {
			for (const auto& blockProposer : m_committee.BlockProposers)
				decreaseActivity(blockProposer, m_accounts, config);
		}
		for (const auto& cosigner : m_committee.Cosigners) {
			decreaseActivity(cosigner, m_accounts, config);
		}
	}

	Key WeightedVotingCommitteeManagerV2::getBootKey(const Key& harvestKey, const model::NetworkConfiguration& config) const {
		auto accountIter = m_accounts.find(harvestKey);
		if (accountIter != m_accounts.cend() && accountIter->second.BootKey != Key()) {
			CATAPULT_LOG(trace) << "Boot key found: " << harvestKey << " - " << accountIter->second.BootKey;
			return accountIter->second.BootKey;
		}

		auto bootstrapIter = config.BootstrapHarvesters.find(harvestKey);
		if (bootstrapIter != config.BootstrapHarvesters.cend()) {
			CATAPULT_LOG(trace) << "Boot key of bootstrap harvester found: " << harvestKey << " - " << bootstrapIter->second;
			return bootstrapIter->second;
		}

		CATAPULT_LOG(warning) << "Boot key not found for " << harvestKey;
		return {};
	}

	std::multimap<int64_t, Key, std::greater<>> WeightedVotingCommitteeManagerV2::getCandidates(
			const model::NetworkConfiguration& networkConfig,
			const config::CommitteeConfiguration& config,
			const BlockchainVersion& blockchainVersion) {
		auto previousRound = m_committee.Round;
		logAccountData(m_accounts, config);
		auto pLastBlockElement = lastBlockElementSupplier()();
		if (previousRound < 0) {
			m_phaseTime = pLastBlockElement->Block.committeePhaseTime();
			if (!m_phaseTime)
				m_phaseTime = networkConfig.CommitteePhaseTime.millis();
			m_timestamp = pLastBlockElement->Block.Timestamp;
		} else {
			if (previousRound > 0) {
				if (networkConfig.BlockTimeUpdateStrategy == model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient ||
					networkConfig.BlockTimeUpdateStrategy == model::BlockTimeUpdateStrategy::Increase_Coefficient ||
					!networkConfig.EnableCommitteeManagerImprovement) {
					IncreasePhaseTime(m_phaseTime, networkConfig);
				}
			} else {
				if (networkConfig.BlockTimeUpdateStrategy == model::BlockTimeUpdateStrategy::IncreaseDecrease_Coefficient || !networkConfig.EnableCommitteeManagerImprovement) {
					DecreasePhaseTime(m_phaseTime, networkConfig);
				} else if (networkConfig.BlockTimeUpdateStrategy == model::BlockTimeUpdateStrategy::Increase_Coefficient) {
					m_phaseTime = networkConfig.MinCommitteePhaseTime.millis();
				}
			}
			decreaseActivities(config);
		}
		m_timestamp = m_timestamp + Timestamp(CommitteePhaseCount * m_phaseTime);

		m_committee = Committee(previousRound + 1);
		m_committee.RoundStart = m_timestamp;

		// Compute account rates and sort them in descending order.
		std::multimap<int64_t, Key, std::greater<>> rates;
		for (const auto& pair : m_accounts) {
			const auto& key = pair.first;
			const auto& accountData = pair.second;

			if (!m_filter(key, config))
				continue;

			if (!accountData.CanHarvest) {
				m_ineligibleHarvesterHandler(key);
				continue;
			}

			bool notBootstrapHarvester = (networkConfig.BootstrapHarvesters.find(key) == networkConfig.BootstrapHarvesters.cend());
			bool notEmergencyHarvester = (networkConfig.EmergencyHarvesters.find(key) == networkConfig.EmergencyHarvesters.cend());

			if (config.EnableBlockchainVersionValidation && accountData.BlockchainVersion < blockchainVersion && notBootstrapHarvester && notEmergencyHarvester) {
				CATAPULT_LOG(debug) << "rejecting outdated harvester " << key << " (" << std::hex << accountData.BlockchainVersion << " < " << std::hex << blockchainVersion << ")";
				m_ineligibleHarvesterHandler(key);
				continue;
			}

			if (accountData.BanPeriod > BlockDuration(0) && notBootstrapHarvester && notEmergencyHarvester) {
				CATAPULT_LOG(debug) << "rejecting harvester " << key << " (banned for " << accountData.BanPeriod << " blocks)";
				m_ineligibleHarvesterHandler(key);
				continue;
			}

			if (networkConfig.BootstrapHarvesters.empty()) {
				if (networkConfig.EnableHarvesterExpiration && accountData.ExpirationTime <= m_timestamp && notEmergencyHarvester) {
					CATAPULT_LOG(debug) << "ineligible harvester " << key << " (expired at " << accountData.ExpirationTime << ")";
					m_ineligibleHarvesterHandler(key);
					continue;
				}
			} else {
				if (notBootstrapHarvester) {
					if (networkConfig.EnableHarvesterExpiration && accountData.ExpirationTime <= m_timestamp) {
						CATAPULT_LOG(debug) << "ineligible harvester " << key << " (expired at " << accountData.ExpirationTime << ")";
						m_ineligibleHarvesterHandler(key);
						continue;
					}

					if (accountData.BootKey == Key()) {
						CATAPULT_LOG(debug) << "ineligible harvester " << key << " (boot key not registered)";
						m_ineligibleHarvesterHandler(key);
						continue;
					}
				}
			}

			auto weight = static_cast<double>(calculateWeight(accountData, config));
			const auto& hash = previousRound < 0 ?
				Hasher::calculateHash(m_hashes[key], pLastBlockElement->GenerationHash, key) :
				Hasher::calculateHash(m_hashes[key]);
			auto hit = *reinterpret_cast<const uint64_t*>(hash.data());
			if (hit == 0u)
				hit = 1u;
			if (config.EnableEqualWeights) {
				auto nextHeight = pLastBlockElement->Block.Height + Height(1);
				auto heightDelta = nextHeight > accountData.LastSigningBlockHeight ? (nextHeight - accountData.LastSigningBlockHeight).unwrap() : 1ULL;
				weight *= static_cast<double>(heightDelta);
			}
			auto stake = static_cast<double>(accountData.EffectiveBalance.unwrap()) / static_cast<double>(hit);
			auto fRate = stake * weight;
			auto nRate = std::numeric_limits<int64_t>::max();
			if (fRate < static_cast<double>(std::numeric_limits<int64_t>::max())) {
				nRate = static_cast<int64_t>(fRate);
				if (nRate == 0)
					nRate = 1;
			}

			CATAPULT_LOG(trace) << "rate of " << key << ": " << nRate;

			rates.emplace(nRate, key);
		}

		return rates;
	}

	void WeightedVotingCommitteeManagerV2::selectCommittee(const model::NetworkConfiguration& networkConfig, const BlockchainVersion& blockchainVersion) {
		std::lock_guard<std::mutex> guard(m_mutex);

		const auto& config = networkConfig.GetPluginConfiguration<config::CommitteeConfiguration>();
		auto rates = getCandidates(networkConfig, config, blockchainVersion);

		// The 21st account may be followed by the accounts with the same rate, select them all as candidates for
		// the committee.
		auto endRateIter = rates.begin();
		for (auto i = 0u; i < networkConfig.CommitteeSize && endRateIter != rates.end(); ++i, ++endRateIter);
		if (endRateIter != rates.end())
			endRateIter = rates.equal_range(endRateIter->first).second;
		std::map<Rate, Key, RateGreater> committeeCandidates;
		for (auto rateIter = rates.begin(); rateIter != endRateIter; ++rateIter)
			committeeCandidates.emplace(Rate(rateIter->first, rateIter->second), rateIter->second);

		if (committeeCandidates.empty())
			CATAPULT_THROW_RUNTIME_ERROR_1("committee empty", m_committee.Round);

		// Select the first 21 candidates to the committee and select block proposer.
		std::map<Rate, Key, RateLess> blockProposerCandidates;
		auto candidateIter = committeeCandidates.begin();
		for (auto i = 0u; i < networkConfig.CommitteeSize && candidateIter != committeeCandidates.end(); ++i, ++candidateIter) {
			const auto& key = candidateIter->second;
			m_committee.Cosigners.insert(key);

			const auto& data = m_accounts.at(key);
			auto greed = static_cast<double>(data.FeeInterest) / static_cast<double>(data.FeeInterestDenominator);
			auto minGreed = static_cast<double>(config.MinGreedFeeInterest) / static_cast<double>(config.MinGreedFeeInterestDenominator);
			greed = std::max(greed, minGreed);
			auto hit = *reinterpret_cast<const uint64_t*>(m_hashes[key].data());
			blockProposerCandidates.emplace(Rate(static_cast<int64_t>(greed * static_cast<double>(hit)), key), key);
		}

		if (m_committee.Cosigners.size() < networkConfig.CommitteeSize)
			CATAPULT_THROW_RUNTIME_ERROR_2("committee not full", m_committee.Cosigners.size(), networkConfig.CommitteeSize);

		m_committee.BlockProposer = blockProposerCandidates.begin()->second;
		m_committee.Cosigners.erase(m_committee.BlockProposer);

		CATAPULT_LOG(trace) << "committee: block proposer " << m_committee.BlockProposer;
		for (const auto& cosigner : m_committee.Cosigners)
			CATAPULT_LOG(trace) << "committee: cosigner " << cosigner;
	}
}}
