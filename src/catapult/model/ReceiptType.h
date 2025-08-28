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

#pragma once
#include "FacilityCode.h"
#include "catapult/utils/Casting.h"
#include <iosfwd>
#include <stdint.h>

namespace catapult { namespace model {

	/// Enumeration of basic receipt types.
	/// \note BasicReceiptType is used as highest nibble of receipt type.
	enum class BasicReceiptType : uint8_t {
		/// Some other receipt type.
		Other = 0x0,

		/// Balance transfer.
		BalanceTransfer = 0x1,

		/// Balance credit.
		BalanceCredit = 0x2,

		/// Balance debit.
		BalanceDebit = 0x3,

		/// Artifact expiry receipt.
		ArtifactExpiry = 0x4,

		/// Inflation.
		Inflation = 0x5,

		/// Aggregate receipt.
		Aggregate = 0xE,

		/// Alias resolution.
		AliasResolution = 0xF,

		/// Drive receipt.
		Drive = 0x6,

		/// Operation receipt.
		Operation = 0x7,

		/// Offer creation.
		OfferCreation = 0xA,

		/// Offer exchange.
		OfferExchange = 0xB,

		/// Offer removal.
		OfferRemoval = 0xC,

		/// Data modification approval.
		Data_Modification_Approval = 0xD,

		/// Data modification cancel.
		Data_Modification_Cancel = 0xE,

		/// Download approval.
		Download_Approval = 0xF,

		/// Download channel refund.
		Download_Channel_Refund = 0x10,

		/// Drive closure.
		Drive_Closure = 0x11,

		/// End drive verification.
		End_Drive_Verification = 0x12,

		/// Periodic payment.
		Periodic_Payment = 0x13,

		/// Replicator deposit.
		Replicator_Deposit = 0x14,

		/// Liquidity provider.
		Liquidity_Provider = 0x15,
	};

	/// Enumeration of receipt types.
	enum class ReceiptType : uint16_t {};

	/// Makes receipt type given \a basicReceiptType, \a facilityCode and \a code.
	constexpr ReceiptType MakeReceiptType(BasicReceiptType basicReceiptType, FacilityCode facilityCode, uint8_t code) {
		return static_cast<ReceiptType>(
				((utils::to_underlying_type(basicReceiptType) & 0x0F) << 12) | // 01..04: basic type
				((code & 0xF) << 8) | //                                          05..08: code
				(static_cast<uint8_t>(facilityCode) & 0xFF)); //                  09..16: facility
	}

/// Defines receipt type given \a BASIC_TYPE, \a FACILITY, \a DESCRIPTION and \a CODE.
#define DEFINE_RECEIPT_TYPE(BASIC_TYPE, FACILITY, DESCRIPTION, CODE) \
	constexpr auto Receipt_Type_##DESCRIPTION = model::MakeReceiptType( \
			(model::BasicReceiptType::BASIC_TYPE), \
			(model::FacilityCode::FACILITY), \
			CODE)

	/// Harvest fee credit.
	DEFINE_RECEIPT_TYPE(BalanceCredit, Core, Harvest_Fee, 1);

	/// Inflation.
	DEFINE_RECEIPT_TYPE(Inflation, Core, Inflation, 1);

	/// Transaction group.
	DEFINE_RECEIPT_TYPE(Aggregate, Core, Transaction_Group, 1);

	/// Public key group.
	DEFINE_RECEIPT_TYPE(Aggregate, Core, Public_Key_Group, 2);

	/// Address alias resolution.
	DEFINE_RECEIPT_TYPE(AliasResolution, Core, Address_Alias_Resolution, 1);

	/// Mosaic alias resolution.
	DEFINE_RECEIPT_TYPE(AliasResolution, Core, Mosaic_Alias_Resolution, 2);

	/// Insertion operator for outputting \a receiptType to \a out.
	std::ostream& operator<<(std::ostream& out, ReceiptType receiptType);
}}
