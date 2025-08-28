/**
*** Copyright 2022 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#include "catapult/cache_core/AccountStateCache.h"
#include "src/utils/TransferUtils.h"
#include "src/cache/LiquidityProviderCache.h"
#include "src/model/LiquidityProviderReceiptType.h"
#include "LiquidityProviderExchangeObserverImpl.h"

namespace catapult::observers {

	void LiquidityProviderExchangeObserverImpl::creditMosaics(
			ObserverContext& context,
			const Key& currencyDebtor,
			const Key& mosaicCreditor,
			const UnresolvedMosaicId& unresolvedMosaicId,
			const UnresolvedAmount& unresolvedMosaicAmount) const {
		auto resolvedAmount = context.Resolvers.resolve(unresolvedMosaicAmount);
		creditMosaics(context, currencyDebtor, mosaicCreditor, unresolvedMosaicId, resolvedAmount);
	}

	void LiquidityProviderExchangeObserverImpl::debitMosaics(
			ObserverContext& context,
			const Key& mosaicDebtor,
			const Key& currencyCreditor,
			const UnresolvedMosaicId& unresolvedMosaicId,
			const UnresolvedAmount& unresolvedMosaicAmount) const {
		auto resolvedAmount = context.Resolvers.resolve(unresolvedMosaicAmount);
		debitMosaics(context, mosaicDebtor, currencyCreditor, unresolvedMosaicId, resolvedAmount);
	}

	void LiquidityProviderExchangeObserverImpl::creditMosaics(
			ObserverContext& context,
			const Key& currencyDebtor,
			const Key& mosaicCreditor,
			const UnresolvedMosaicId& mosaicId,
			const Amount& mosaicAmount) const {
		auto& lpCache = context.Cache.sub<cache::LiquidityProviderCache>();

		auto lpEntryIter = lpCache.find(mosaicId);
		auto& lpEntry = lpEntryIter.get();

		auto& accountStateCache = context.Cache.sub<cache::AccountStateCache>();

		auto lpAccountIter = accountStateCache.find(lpEntry.providerKey());
		auto& lpAccount = lpAccountIter.get();

		const auto& pluginConfig =
				context.Config.Network.template GetPluginConfiguration<config::LiquidityProviderConfiguration>();

		const auto& currencyMosaicId = context.Config.Immutable.CurrencyMosaicId;

		auto resolvedMosaicId = context.Resolvers.resolve(mosaicId);

		// In the observer the optional always has the value
		auto currencyAmount = *utils::computeCreditCurrencyAmount(
				lpEntry,
				lpAccount.Balances.get(currencyMosaicId),
				lpAccount.Balances.get(resolvedMosaicId),
				mosaicAmount,
				pluginConfig.PercentsDigitsAfterDot);

		auto debtorAccountIter = accountStateCache.find(currencyDebtor);
		auto& debtorAccount = debtorAccountIter.get();

		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Currency_Debit, currencyDebtor, currencyMosaicId, currencyAmount));

		auto debtorCurrencyBalance = debtorAccount.Balances.get(currencyMosaicId);
		if (debtorCurrencyBalance < currencyAmount) {
			context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
				model::Receipt_Type_Debtor_Currency_Balance_Insufficient, currencyDebtor, currencyMosaicId, debtorCurrencyBalance));
			CATAPULT_LOG( error ) << "Debtor Not Enough Currency " << debtorCurrencyBalance << " " << currencyAmount;
			return;
		}

		debtorAccount.Balances.debit(currencyMosaicId, currencyAmount);

		lpAccount.Balances.credit(currencyMosaicId, currencyAmount);

		auto creditorAccountIter = accountStateCache.find(mosaicCreditor);
		auto& creditorAccount = creditorAccountIter.get();
		creditorAccount.Balances.credit(resolvedMosaicId, mosaicAmount);
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Mosaic_Credit, mosaicCreditor, resolvedMosaicId, mosaicAmount));

		lpEntry.setAdditionallyMinted(lpEntry.additionallyMinted() + mosaicAmount);
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Additionally_Minted, lpEntry.providerKey(), resolvedMosaicId, lpEntry.additionallyMinted()));

		lpEntry.recentTurnover().m_turnover = lpEntry.recentTurnover().m_turnover + currencyAmount;
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Turnover, lpEntry.providerKey(), currencyMosaicId, lpEntry.recentTurnover().m_turnover));
	}

	void LiquidityProviderExchangeObserverImpl::debitMosaics(
			ObserverContext& context,
			const Key& mosaicDebtor,
			const Key& currencyCreditor,
			const UnresolvedMosaicId& mosaicId,
			const Amount& mosaicAmount) const {
		auto& lpCache = context.Cache.sub<cache::LiquidityProviderCache>();

		auto lpEntryIter = lpCache.find(mosaicId);
		auto& lpEntry = lpEntryIter.get();

		auto& accountStateCache = context.Cache.sub<cache::AccountStateCache>();
		auto lpAccountIter = accountStateCache.find(lpEntry.providerKey());
		auto& lpAccount = lpAccountIter.get();

		const auto& pluginConfig =
				context.Config.Network.template GetPluginConfiguration<config::LiquidityProviderConfiguration>();

		const auto& currencyMosaicId = context.Config.Immutable.CurrencyMosaicId;

		auto resolvedMosaicId = context.Resolvers.resolve(mosaicId);
		Amount currencyAmount = utils::computeDebitCurrencyAmount(
				lpEntry,
				lpAccount.Balances.get(currencyMosaicId),
				lpAccount.Balances.get(resolvedMosaicId),
				mosaicAmount,
				pluginConfig.PercentsDigitsAfterDot);

		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Mosaic_Debit, mosaicDebtor, resolvedMosaicId, mosaicAmount));

		auto lpCurrencyBalance = lpAccount.Balances.get(currencyMosaicId);
		if (lpCurrencyBalance < currencyAmount) {
			context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
				model::Receipt_Type_Liquidity_Provider_Currency_Balance_Insufficient, lpEntry.providerKey(), currencyMosaicId, lpCurrencyBalance));
			CATAPULT_LOG( error ) << "LP Not Enough Currency " << lpCurrencyBalance << " " << currencyAmount;
			return;
		}

		auto debtorAccountIter = accountStateCache.find(mosaicDebtor);
		auto& debtorAccount = debtorAccountIter.get();

		auto debtorMosaicBalance = debtorAccount.Balances.get(resolvedMosaicId);
		if (debtorMosaicBalance < mosaicAmount) {
			context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
				model::Receipt_Type_Debtor_Mosaic_Balance_Insufficient, mosaicDebtor, resolvedMosaicId, debtorMosaicBalance));
			CATAPULT_LOG( error ) << "Debtor Not Enough Mosaics " << mosaicDebtor << " " << resolvedMosaicId << " " << debtorMosaicBalance << " " << mosaicAmount;
			return;
		}

		auto creditorAccountIter = accountStateCache.find(currencyCreditor);
		auto& creditorAccount = creditorAccountIter.get();
		creditorAccount.Balances.credit(currencyMosaicId, currencyAmount);
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Currency_Credit, currencyCreditor, currencyMosaicId, currencyAmount));

		lpAccount.Balances.debit(currencyMosaicId, currencyAmount);

		debtorAccount.Balances.debit(resolvedMosaicId, mosaicAmount);

		lpEntry.setAdditionallyMinted(lpEntry.additionallyMinted() - mosaicAmount);
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Additionally_Minted, lpEntry.providerKey(), resolvedMosaicId, lpEntry.additionallyMinted()));

		lpEntry.recentTurnover().m_turnover = lpEntry.recentTurnover().m_turnover + currencyAmount;
		context.StatementBuilder().addTransactionReceipt(model::BalanceChangeReceipt(
			model::Receipt_Type_Turnover, lpEntry.providerKey(), currencyMosaicId, lpEntry.recentTurnover().m_turnover));
	}
}