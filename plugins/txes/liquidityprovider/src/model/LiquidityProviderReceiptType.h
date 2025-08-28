/**
*** Copyright 2025 ProximaX Limited. All rights reserved.
*** Use of this source code is governed by the Apache 2.0
*** license that can be found in the LICENSE file.
**/

#pragma once
#ifndef CUSTOM_RECEIPT_TYPE_DEFINITION
#include "catapult/model/ReceiptType.h"

namespace catapult { namespace model {

#endif

	/// Currency debit.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Currency_Debit, 1);

	/// Currency credit.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Currency_Credit, 2);

	/// Mosaic debit.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Mosaic_Debit, 3);

	/// Mosaic credit.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Mosaic_Credit, 4);

	/// Debtor currency balance is insufficient.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Debtor_Currency_Balance_Insufficient, 5);

	/// Debtor mosaic balance is insufficient.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Debtor_Mosaic_Balance_Insufficient, 6);

	/// Liquidity provider currency balance is insufficient.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Liquidity_Provider_Currency_Balance_Insufficient, 7);

	/// Additionally minted mosaic by the liquidity provider.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Additionally_Minted, 8);

	/// Turnover by the liquidity provider.
	DEFINE_RECEIPT_TYPE(Liquidity_Provider, LiquidityProvider, Turnover, 9);

#ifndef CUSTOM_RECEIPT_TYPE_DEFINITION
}}
#endif
