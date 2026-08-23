// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

/*============================================================
**
** Purpose: Some single-precision floating-point math operations
**
===========================================================*/

//This class contains only static members and doesn't require serialization.

using System.Runtime;
using System.Runtime.CompilerServices;

namespace System
{
    public static partial class MathF
    {
        public const float E = 2.71828183f;

        public const float PI = 3.14159265f;

        private const int maxRoundingDigits = 6;

        // This table is required for the Round function which can specify the number of digits to round to
        private static float[] roundPower10Single = new float[] {
            1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f
        };

        private static float singleRoundLimit = 1e8f;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static float Abs(float x)
        {
            return Math.Abs(x);
        }

        public static float IEEERemainder(float x, float y)
        {
            if (float.IsNaN(x))
            {
                return x; // IEEE 754-2008: NaN payload must be preserved
            }

            if (float.IsNaN(y))
            {
                return y; // IEEE 754-2008: NaN payload must be preserved
            }

            var regularMod = x % y;

            if (float.IsNaN(regularMod))
            {
                return float.NaN;
            }

            if ((regularMod == 0) && float.IsNegative(x))
            {
                return float.NegativeZero;
            }

            var alternativeResult = (regularMod - (Abs(y) * Sign(x)));

            if (Abs(alternativeResult) == Abs(regularMod))
            {
                var divisionResult = x / y;
                var roundedResult = Round(divisionResult);

                if (Abs(roundedResult) > Abs(divisionResult))
                {
                    return alternativeResult;
                }
                else
                {
                    return regularMod;
                }
            }

            if (Abs(alternativeResult) < Abs(regularMod))
            {
                return alternativeResult;
            }
            else
            {
                return regularMod;
            }
        }

        public static float Log(float x, float y)
        {
            if (float.IsNaN(x))
            {
                return x; // IEEE 754-2008: NaN payload must be preserved
            }

            if (float.IsNaN(y))
            {
                return y; // IEEE 754-2008: NaN payload must be preserved
            }

            if (y == 1)
            {
                return float.NaN;
            }

            if ((x != 1) && ((y == 0) || float.IsPositiveInfinity(y)))
            {
                return float.NaN;
            }

            return Log(x) / Log(y);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static float Max(float x, float y)
        {
            return Math.Max(x, y);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static float Min(float x, float y)
        {
            return Math.Min(x, y);
        }

        [Intrinsic]
        public static float Round(float x)
        {
            // ************************************************************************************
            // IMPORTANT: Do not change this implementation without also updating
            //            mono_round_to_even () in mono/utils/mono-math.h, which is what
            //            Math.Round(double) calls, and the Round row of the JIT's math_table
            //            in mono/llvm/method-to-llvm/math.cpp, which replaces both.
            // ************************************************************************************

            // IEEE 754 roundToIntegralTiesToEven, which is llvm.roundeven.

            float truncated = Truncate(x);
            float fraction = x - truncated;

            // A float of 2^23 or more is already an integer, so its fraction is zero and
            // neither test fires. Under that magnitude truncated fits an int, which is what
            // makes the parity test valid. An infinity and a NaN each give a NaN fraction
            // and fall through the same way.
            //
            // x - Truncate(x) is exact, so the largest value under a half stays under a half
            // here. Adding 0.5f to it instead rounds up to 1.0f and loses the case.

            if ((Abs(fraction) > 0.5f) || ((Abs(fraction) == 0.5f) && ((((int)truncated) & 1) != 0)))
            {
                truncated += CopySign(1.0f, x);
            }

            return truncated;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static float Round(float x, int digits)
        {
            return Round(x, digits, MidpointRounding.ToEven);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static float Round(float x, MidpointRounding mode)
        {
            return Round(x, 0, mode);
        }

        public static unsafe float Round(float x, int digits, MidpointRounding mode)
        {
            if ((digits < 0) || (digits > maxRoundingDigits))
            {
                throw new ArgumentOutOfRangeException(nameof(digits), SR.ArgumentOutOfRange_RoundingDigits);
            }

            if (mode < MidpointRounding.ToEven || mode > MidpointRounding.AwayFromZero)
            {
                throw new ArgumentException(SR.Format(SR.Argument_InvalidEnum, mode, nameof(MidpointRounding)), nameof(mode));
            }

            if (Abs(x) < singleRoundLimit)
            {
                var power10 = roundPower10Single[digits];

                x *= power10;

                if (mode == MidpointRounding.AwayFromZero)
                {
                    var fraction = ModF(x, &x);

                    if (Abs(fraction) >= 0.5f)
                    {
                        x += Sign(fraction);
                    }
                }
                else
                {
                    x = Round(x);
                }

                x /= power10;
            }

            return x;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Sign(float x)
        {
            return Math.Sign(x);
        }

        public static unsafe float Truncate(float x)
        {
            ModF(x, &x);
            return x;
        }

        private static unsafe float CopySign(float x, float y)
        {
            var xbits = BitConverter.SingleToInt32Bits(x);
            var ybits = BitConverter.SingleToInt32Bits(y);

            // If the sign bits of x and y are not the same,
            // flip the sign bit of x and return the new value;
            // otherwise, just return x

            if (((xbits ^ ybits) >> 31) != 0)
            {
                return BitConverter.Int32BitsToSingle(xbits ^ int.MinValue);
            }

            return x;
        }
    }
}
