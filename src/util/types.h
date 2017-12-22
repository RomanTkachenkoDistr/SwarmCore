#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include <algorithm>
#include <locale>
#include "overlay/StellarXDR.h"
#include "xdrpp/message.h"
#include <locale>
#include <algorithm>

namespace stellar
{

const std::locale cLocale("C");

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

int32_t getManagerType(AccountType accountType);

// Returns true, if flag value is valid. (Does not contains any values which do not belongs to FlagType)
template <typename FlagType>
bool isValidXDRFlag(int32 value)
{
	for (auto flagElem : xdr::xdr_traits<FlagType>::enum_values()) {
		value &= ~flagElem;
	}

	return value == 0;
}

// Returns true, if enum value valid
template <typename EnumType>
bool isValidEnumValue(EnumType value)
{
	auto enums = xdr::xdr_traits<EnumType>::enum_values();
	return std::find(std::begin(enums), std::end(enums), static_cast<int32_t>(value)) != std::end(enums);
}

template <typename EnumType>
bool isSetFlag(uint32 value, EnumType flag)
{
    uint32 flagValue = static_cast<uint32>(flag);
    return (flagValue & value) == flagValue;
}

// returns true if result is valid (no overflow)
bool safeSum(uint64_t a, uint64_t b, uint64_t& result);
bool safeSum(uint64_t& result, std::vector<uint64_t> params);

// calculates A*B/C when A*B overflows 64bits
int64_t bigDivide(int64_t A, int64_t B, int64_t C, Rounding rounding);
// no throw version, returns true if result is valid
bool bigDivide(int64_t& result, int64_t A, int64_t B, int64_t C, Rounding rounding);

// no throw version, returns true if result is valid
bool bigDivide(uint64_t& result, uint64_t A, uint64_t B, uint64_t C, Rounding rounding);

bool iequals(std::string const& a, std::string const& b);
}

template<typename R>
std::string getNameCode(R code) {
    if (!stellar::isValidEnumValue<R>(code)) {
        throw std::runtime_error("Invalid result code");
    }
    const char * rawResultCode = xdr::xdr_traits<R>::enum_name(code);
    if (!rawResultCode) {
        throw std::runtime_error("name result code is nullptr");
    }
    std::string strResultCode = std::string(rawResultCode);
    return strResultCode;
}

namespace std {

    template <>
    struct hash<stellar::ExternalSystemIDGeneratorType>
    {
        size_t operator()(const stellar::ExternalSystemIDGeneratorType& k) const
        {
            using std::size_t;
            using std::hash;
            const auto rawKey = static_cast<int32_t>(k);
            return hash<int32_t>()(rawKey);
        }
    };
}
