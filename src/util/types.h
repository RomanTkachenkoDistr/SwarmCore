#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"

namespace stellar
{

const int64 ONE = 10000LL;

enum Rounding
{
	ROUND_DOWN,
	ROUND_UP
};

typedef std::vector<unsigned char> Blob;

bool isZero(uint256 const& b);

Hash& operator^=(Hash& l, Hash const& r);

// returns true if ( l ^ x ) < ( r ^ x)
bool lessThanXored(Hash const& l, Hash const& r, Hash const& x);

uint256 makePublicKey(uint256 const& b);

// returns true if the passed string32 is valid
bool isString32Valid(std::string const& str);

// returns all signer types
int32_t getAnySignerType();

int32 getAnyAssetPolicy();

bool isValidManageAssetPairAction(ManageAssetPairAction action);
bool isValidManageAssetAction(ManageAssetAction action);

int32 getAnyAssetPairPolicy();

uint32_t getAnyBlockReason();

// returns vector of all account types
std::vector<AccountType> getAllAccountTypes();

std::vector<AccountType> getSystemAccountTypes();

bool isSystemAccountType(AccountType accountType);

std::vector<FeeType> getAllFeeTypes();
// returns true, if passed opType is valid operation type

bool isFeeValid(FeeData const& fee);
bool isFeeTypeValid(FeeType feeType);
bool isAssetValid(AssetCode asset);
// returns true if source of the op must be master account
bool isAdminOp(OperationType op);
// returns true if percent fee can be charged
bool isTransferOp(OperationType op);

int32_t getManagerType(AccountType accountType);

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C, Rounding rounding);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C, Rounding rounding);

bool iequals(std::string const& a, std::string const& b);
}