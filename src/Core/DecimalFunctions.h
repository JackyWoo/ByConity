/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <Core/Types.h>
#include <Common/Exception.h>
#include <Common/intExp.h>
#include <common/arithmeticOverflow.h>

#include <limits>
#include <type_traits>


namespace DB
{

template <typename T>
class DataTypeNumber;

namespace ErrorCodes
{
    extern const int DECIMAL_OVERFLOW;
    extern const int ARGUMENT_OUT_OF_BOUND;
}

namespace DecimalUtils
{

inline constexpr size_t min_precision = 1;
template <typename T> inline constexpr size_t max_precision = 0;
template <> inline constexpr size_t max_precision<Decimal32> = 9;
template <> inline constexpr size_t max_precision<Decimal64> = 18;
template <> inline constexpr size_t max_precision<DateTime64> = 18;
template <> inline constexpr size_t max_precision<Decimal128> = 38;
template <> inline constexpr size_t max_precision<Decimal256> = 76;

template <typename T0> struct NextDecimal { using Result = void; };
template <> struct NextDecimal<Decimal32> { using Result = Decimal64; };
template <> struct NextDecimal<Decimal64> { using Result = Decimal128; };
template <> struct NextDecimal<Decimal128> { using Result = Decimal256; };

template <typename T>
inline auto scaleMultiplier(UInt32 scale)
{
    if constexpr (std::is_same_v<T, Int32> || std::is_same_v<T, Decimal32>)
        return common::exp10_i32(scale);
    else if constexpr (std::is_same_v<T, Int64> || std::is_same_v<T, Decimal64> || std::is_same_v<T, DateTime64>)
        return common::exp10_i64(scale);
    else if constexpr (std::is_same_v<T, Int128> || std::is_same_v<T, Decimal128>)
        return common::exp10_i128(scale);
    else if constexpr (std::is_same_v<T, Int256> || std::is_same_v<T, Decimal256>)
        return common::exp10_i256(scale);
}


/** Components of DecimalX value:
 * whole - represents whole part of decimal, can be negative or positive.
 * fractional - for fractional part of decimal, always positive.
 */
template <typename DecimalType>
struct DecimalComponents
{
    using T = typename DecimalType::NativeType;
    T whole;
    T fractional;
};

/// Traits used for determining final Type/Precision/Scale for certain math operations on decimals.
template <typename T>
struct DataTypeDecimalTrait
{
    using FieldType = T;
    const UInt32 precision;
    const UInt32 scale;

    DataTypeDecimalTrait(UInt32 precision_, UInt32 scale_)
        : precision(precision_),
          scale(scale_)
    {}

    /// @returns multiplier for U to become T with correct scale
    template <typename U>
    T scaleFactorFor(const DataTypeDecimalTrait<U> & x, bool) const
    {
        if (scale < x.scale)
            throw Exception("Decimal result's scale is less than argument's one", ErrorCodes::ARGUMENT_OUT_OF_BOUND);
        const UInt32 scale_delta = scale - x.scale; /// scale_delta >= 0
        return DecimalUtils::scaleMultiplier<typename T::NativeType>(scale_delta);
    }
};

/** Make a decimal value from whole and fractional components with given scale multiplier.
  * where scale_multiplier = scaleMultiplier<T>(scale)
  * this is to reduce number of calls to scaleMultiplier when scale is known.
  *
  * Sign of `whole` controls sign of result: negative whole => negative result, positive whole => positive result.
  * Sign of `fractional` is expected to be positive, otherwise result is undefined.
  * If `scale` is to big (scale > max_precision<DecimalType::NativeType>), result is undefined.
  */
template <typename DecimalType, bool throw_on_error>
inline bool decimalFromComponentsWithMultiplierImpl(
        const typename DecimalType::NativeType & whole,
        const typename DecimalType::NativeType & fractional,
        typename DecimalType::NativeType scale_multiplier,
        DecimalType & result)
{
    using T = typename DecimalType::NativeType;
    const auto fractional_sign = whole < 0 ? -1 : 1;

    T whole_scaled = 0;
    if (common::mulOverflow(whole, scale_multiplier, whole_scaled))
    {
        if constexpr (throw_on_error)
            throw Exception("Decimal math overflow", ErrorCodes::DECIMAL_OVERFLOW);
        return false;
    }

    T value;
    if (common::addOverflow(whole_scaled, fractional_sign * (fractional % scale_multiplier), value))
    {
        if constexpr (throw_on_error)
            throw Exception("Decimal math overflow", ErrorCodes::DECIMAL_OVERFLOW);
        return false;
    }

    result = DecimalType(value);
    return true;
}

template <typename DecimalType>
inline DecimalType decimalFromComponentsWithMultiplier(
        const typename DecimalType::NativeType & whole,
        const typename DecimalType::NativeType & fractional,
        typename DecimalType::NativeType scale_multiplier)
{
    DecimalType result;
    decimalFromComponentsWithMultiplierImpl<DecimalType, true>(whole, fractional, scale_multiplier, result);
    return result;
}

template <typename DecimalType>
inline bool tryGetDecimalFromComponentsWithMultiplier(
        const typename DecimalType::NativeType & whole,
        const typename DecimalType::NativeType & fractional,
        typename DecimalType::NativeType scale_multiplier,
        DecimalType & result)
{
    return decimalFromComponentsWithMultiplierImpl<DecimalType, false>(whole, fractional, scale_multiplier, result);
}

template <typename DecimalType>
inline DecimalType decimalFromComponentsWithMultiplier(
        const DecimalComponents<DecimalType> & components,
        typename DecimalType::NativeType scale_multiplier)
{
    return decimalFromComponentsWithMultiplier<DecimalType>(components.whole, components.fractional, scale_multiplier);
}

template <typename DecimalType>
inline bool tryGetDecimalFromComponents(
        const typename DecimalType::NativeType & whole,
        const typename DecimalType::NativeType & fractional,
        UInt32 scale,
        DecimalType & result)
{
    using T = typename DecimalType::NativeType;

    return tryGetDecimalFromComponentsWithMultiplier<DecimalType>(whole, fractional, scaleMultiplier<T>(scale), result);
}

/** Make a decimal value from whole and fractional components with given scale.
 *
 * @see `decimalFromComponentsWithMultiplier` for details.
 */
template <typename DecimalType>
inline DecimalType decimalFromComponents(
        const typename DecimalType::NativeType & whole,
        const typename DecimalType::NativeType & fractional,
        UInt32 scale)
{
    using T = typename DecimalType::NativeType;

    return decimalFromComponentsWithMultiplier<DecimalType>(whole, fractional, scaleMultiplier<T>(scale));
}

/** Make a decimal value from whole and fractional components with given scale.
 * @see `decimalFromComponentsWithMultiplier` for details.
 */
template <typename DecimalType>
inline DecimalType decimalFromComponents(
        const DecimalComponents<DecimalType> & components,
        UInt32 scale)
{
    return decimalFromComponents<DecimalType>(components.whole, components.fractional, scale);
}

template <typename DecimalType>
inline bool tryGetDecimalFromComponents(
        const DecimalComponents<DecimalType> & components,
        UInt32 scale,
        DecimalType & result)
{
    return tryGetDecimalFromComponents<DecimalType>(components.whole, components.fractional, scale, result);
}

/** Split decimal into whole and fractional parts with given scale_multiplier.
 * This is an optimization to reduce number of calls to scaleMultiplier on known scale.
 */
template <typename DecimalType>
inline DecimalComponents<DecimalType> splitWithScaleMultiplier(
        const DecimalType & decimal,
        typename DecimalType::NativeType scale_multiplier)
{
    using T = typename DecimalType::NativeType;
    const auto whole = decimal.value / scale_multiplier;
    auto fractional = decimal.value % scale_multiplier;
    if (whole && fractional < T(0))
        fractional *= T(-1);

    return {whole, fractional};
}

/// Split decimal into components: whole and fractional part, @see `DecimalComponents` for details.
template <typename DecimalType>
inline DecimalComponents<DecimalType> split(const DecimalType & decimal, UInt32 scale)
{
    if (scale == 0)
    {
        return {decimal.value, 0};
    }
    return splitWithScaleMultiplier(decimal, scaleMultiplier<typename DecimalType::NativeType>(scale));
}

/** Get whole part from decimal.
 *
 * Sign of result follows sign of `decimal` value.
 * If scale is to big, result is undefined.
 */
template <typename DecimalType>
inline typename DecimalType::NativeType getWholePart(const DecimalType & decimal, size_t scale)
{
    if (scale == 0)
        return decimal.value;

    return decimal.value / scaleMultiplier<typename DecimalType::NativeType>(scale);
}


template <typename DecimalType, bool keep_sign = false>
inline typename DecimalType::NativeType getFractionalPartWithScaleMultiplier(
        const DecimalType & decimal,
        typename DecimalType::NativeType scale_multiplier)
{
    using T = typename DecimalType::NativeType;

    /// There's UB with min integer value here. But it does not matter for Decimals cause they use not full integer ranges.
    /// Anycase we make modulo before compare to make scale_multiplier > 1 unaffected.
    T result = decimal.value % scale_multiplier;
    if constexpr (!keep_sign)
        if (decimal.value / scale_multiplier && result < T(0))
            result = -result;

    return result;
}

/** Get fractional part from decimal
 *
 * Result is always positive.
 * If scale is to big, result is undefined.
 */
template <typename DecimalType>
inline typename DecimalType::NativeType getFractionalPart(const DecimalType & decimal, size_t scale)
{
    if (scale == 0)
        return 0;

    return getFractionalPartWithScaleMultiplier(decimal, scaleMultiplier<typename DecimalType::NativeType>(scale));
}

/// Decimal to integer/float conversion
template <typename To, typename DecimalType, typename ReturnType>
ReturnType convertToImpl(const DecimalType & decimal, size_t scale, To & result)
{
    using NativeT = typename DecimalType::NativeType;
    static constexpr bool throw_exception = std::is_same_v<ReturnType, void>;

    if constexpr (std::is_floating_point_v<To>)
    {
        result = static_cast<To>(decimal.value) / static_cast<To>(scaleMultiplier<NativeT>(scale));
    }
    else if constexpr (is_integer_v<To> && (sizeof(To) >= sizeof(NativeT)))
    {
        NativeT whole = getWholePart(decimal, scale);

        if constexpr (is_unsigned_v<To>)
        {
            if (whole < 0)
            {
                if constexpr (throw_exception)
                    throw Exception("Convert overflow", ErrorCodes::DECIMAL_OVERFLOW);
                else
                    return ReturnType(true);
            }
        }

        result = static_cast<To>(whole);
    }
    else if constexpr (is_integer_v<To>)
    {
        using ToNativeT = typename NativeType<To>::Type;
        using CastTo = std::conditional_t<(is_big_int_v<NativeT> && std::is_same_v<ToNativeT, UInt8>), uint8_t, ToNativeT>;

        const NativeT whole = getWholePart(decimal, scale);

        static const constexpr CastTo min_to = std::numeric_limits<ToNativeT>::min();
        static const constexpr CastTo max_to = std::numeric_limits<ToNativeT>::max();

        if (whole < min_to || whole > max_to)
        {
            if constexpr (throw_exception)
                throw Exception("Convert overflow", ErrorCodes::DECIMAL_OVERFLOW);
            else
                return ReturnType(true);
        }

        result = static_cast<CastTo>(whole);
    }

    return ReturnType(true);
}


template <typename To, typename DecimalType>
To convertTo(const DecimalType & decimal, size_t scale)
{
    To result;
    convertToImpl<To, DecimalType, void>(decimal, scale, result);
    return result;
}

template <typename To, typename DecimalType>
bool tryConvertTo(const DecimalType & decimal, size_t scale, To & result)
{
    return convertToImpl<To, DecimalType, bool>(decimal, scale, result);
}

constexpr UInt32 DECIMAL_DIVISION_EXTENDED_SCALE = 6;

template <bool is_multiply, bool is_division, bool allow_promote_storage, typename T, typename U, template <typename> typename DecimalType>
inline auto binaryOpResult(const DecimalType<T> & tx, const DecimalType<U> & ty, bool use_extended_scale)
{
    UInt32 scale{};
    if constexpr (is_multiply)
        scale = tx.getScale() + ty.getScale();
    else if constexpr (is_division)
    {
        if (use_extended_scale)
            scale = std::max(tx.getScale(), DECIMAL_DIVISION_EXTENDED_SCALE);
        else
            scale = tx.getScale();
    }
    else
        scale = (tx.getScale() > ty.getScale() ? tx.getScale() : ty.getScale());

    if constexpr ((is_multiply || is_division) && allow_promote_storage && sizeof(T) == sizeof(U) && !std::is_same_v<typename NextDecimal<T>::Result, void>)
    {
        using ResultType = typename NextDecimal<T>::Result;
        return DataTypeDecimalTrait<ResultType>(DecimalUtils::max_precision<ResultType>, scale);
    }
    else if constexpr (sizeof(T) < sizeof(U))
        return DataTypeDecimalTrait<U>(DecimalUtils::max_precision<U>, scale);
    else
        return DataTypeDecimalTrait<T>(DecimalUtils::max_precision<T>, scale);
}

template <bool is_multiply, bool is_division, bool allow_promote_storage, typename T, typename U, template <typename> typename DecimalType>
inline auto binaryOpResult(const DecimalType<T> & tx, const DataTypeNumber<U> &, bool use_extended_scale)
{
    UInt32 scale = tx.getScale();

    if constexpr (is_division)
    {
        if (use_extended_scale)
            scale = std::max(scale, DECIMAL_DIVISION_EXTENDED_SCALE);
    }

    if constexpr (is_division && allow_promote_storage && !std::is_same_v<typename NextDecimal<T>::Result, void>)
    {
        using ResultType = typename NextDecimal<T>::Result;
        return DataTypeDecimalTrait<ResultType>(DecimalUtils::max_precision<ResultType>, scale);
    }
    else
        return DataTypeDecimalTrait<T>(DecimalUtils::max_precision<T>, scale);
}

template <bool is_multiply, bool is_division, bool allow_promote_storage, typename T, typename U, template <typename> typename DecimalType>
inline auto binaryOpResult(const DataTypeNumber<T> &, const DecimalType<U> & ty, bool use_extended_scale)
{
    UInt32 scale = ty.getScale();

    if constexpr (is_division)
    {
        if (use_extended_scale)
            scale = std::max(scale, DECIMAL_DIVISION_EXTENDED_SCALE);
    }

    if constexpr (is_division && allow_promote_storage && !std::is_same_v<typename NextDecimal<U>::Result, void>)
    {
        using ResultType = typename NextDecimal<U>::Result;
        return DataTypeDecimalTrait<ResultType>(DecimalUtils::max_precision<ResultType>, scale);
    }
    else
        return DataTypeDecimalTrait<U>(DecimalUtils::max_precision<U>, scale);
}

}

}
