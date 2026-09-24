// ==++==
// 
//   Copyright (c) Microsoft Corporation.  All rights reserved.
// 
// ==--==
// <OWNER>Microsoft</OWNER>
// 

using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics.Contracts;
//using System.Globalization;
using System.Runtime.CompilerServices;

namespace System.Collections.Generic
{    
    [Serializable]
    [TypeDependencyAttribute("System.Collections.Generic.ObjectComparer`1")] 
    public abstract class Comparer<T> : IComparer, IComparer<T>
    {
        static readonly Comparer<T> defaultComparer = CreateComparer();

        public static Comparer<T> Default {
            get {
                Contract.Ensures(Contract.Result<Comparer<T>>() != null);

                return defaultComparer;
            }
        }

        public static Comparer<T> Create(Comparison<T> comparison)
        {
            Contract.Ensures(Contract.Result<Comparer<T>>() != null);

            if (comparison == null)
                throw new ArgumentNullException("comparison");

            return new ComparisonComparer<T>(comparison);
        }

        //
        // Note that logic in this method is replicated in vm\compile.cpp to ensure that NGen
        // saves the right instantiations
        //
        [System.Security.SecuritySafeCritical]  // auto-generated
        private static Comparer<T> CreateComparer() {
            RuntimeType t = (RuntimeType)typeof(T);

            // If T implements IComparable<T> return a GenericComparer<T>
#if FEATURE_LEGACYNETCF
            // Pre-Apollo Windows Phone call the overload that sorts the keys, not values this achieves the same result
            if (CompatibilitySwitches.IsAppEarlierThanWindowsPhone8) {
                if (t.ImplementInterface(typeof(IComparable<T>))) {
                    return (Comparer<T>)RuntimeTypeHandle.CreateInstanceForAnotherGenericParameter((RuntimeType)typeof(GenericComparer<int>), t);
                }
            }
            else
#endif
                if (typeof(IComparable<T>).IsAssignableFrom(t)) {
#if MONO
                    return (Comparer<T>)RuntimeType.CreateInstanceForAnotherGenericParameter (typeof(GenericComparer<>), t);
#else
                    return (Comparer<T>)RuntimeTypeHandle.CreateInstanceForAnotherGenericParameter((RuntimeType)typeof(GenericComparer<int>), t);
#endif
                }

            // If T is a Nullable<U> where U implements IComparable<U> return a NullableComparer<U>
            if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(Nullable<>)) {
                RuntimeType u = (RuntimeType)t.GetGenericArguments()[0];
                if (typeof(IComparable<>).MakeGenericType(u).IsAssignableFrom(u)) {
#if MONO
                    return (Comparer<T>)RuntimeType.CreateInstanceForAnotherGenericParameter (typeof(NullableComparer<>), u);
#else
                    return (Comparer<T>)RuntimeTypeHandle.CreateInstanceForAnotherGenericParameter((RuntimeType)typeof(NullableComparer<int>), u);
#endif
                }
            }
#if MONO
            // An enum implements only the non-generic IComparable, so the
            // ObjectComparer<T> below would box both operands of every compare.
            if (t.IsEnum) {
                Type comparer = EnumComparerFor (Type.GetTypeCode (Enum.GetUnderlyingType (t)));
                if (comparer != null)
                    return (Comparer<T>)RuntimeType.CreateInstanceForAnotherGenericParameter (comparer, t);
            }
#endif
            // Otherwise return an ObjectComparer<T>
          return new ObjectComparer<T>();
        }

#if MONO
        // The EnumComparer<> for an enum with underlying typeCode, or null if
        // none matches.
        static Type EnumComparerFor (TypeCode typeCode)
        {
            switch (typeCode) {
            case TypeCode.SByte: return typeof (SByteEnumComparer<>);
            case TypeCode.Byte:
            case TypeCode.Boolean: return typeof (ByteEnumComparer<>);
            case TypeCode.Int16: return typeof (Int16EnumComparer<>);
            case TypeCode.UInt16:
            case TypeCode.Char: return typeof (UInt16EnumComparer<>);
            case TypeCode.Int32: return typeof (Int32EnumComparer<>);
            case TypeCode.UInt32: return typeof (UInt32EnumComparer<>);
            case TypeCode.Int64: return typeof (Int64EnumComparer<>);
            case TypeCode.UInt64: return typeof (UInt64EnumComparer<>);
            default: return null;
            }
        }
#endif

        public abstract int Compare(T x, T y);

        int IComparer.Compare(object x, object y) {
            if (x == null) return y == null ? 0 : -1;
            if (y == null) return 1;
            if (x is T && y is T) return Compare((T)x, (T)y);
            ThrowHelper.ThrowArgumentException(ExceptionResource.Argument_InvalidArgumentForComparison);
            return 0;
        }
    }

    [Serializable]
    internal class GenericComparer<T> : Comparer<T> where T: IComparable<T>
    {    
        public override int Compare(T x, T y) {
            if (x != null) {
                if (y != null) return x.CompareTo(y);
                return 1;
            }
            if (y != null) return -1;
            return 0;
        }

        // Equals method for the comparer itself. 
        public override bool Equals(Object obj){
            GenericComparer<T> comparer = obj as GenericComparer<T>;
            return comparer != null;
        }        

        public override int GetHashCode() {
            return this.GetType().Name.GetHashCode();
        }
    }

    [Serializable]
    internal class NullableComparer<T> : Comparer<Nullable<T>> where T : struct, IComparable<T>
    {
        public override int Compare(Nullable<T> x, Nullable<T> y) {
            if (x.HasValue) {
                if (y.HasValue) return x.value.CompareTo(y.value);
                return 1;
            }
            if (y.HasValue) return -1;
            return 0;
        }

        // Equals method for the comparer itself. 
        public override bool Equals(Object obj){
            NullableComparer<T> comparer = obj as NullableComparer<T>;
            return comparer != null;
        }        


        public override int GetHashCode() {
            return this.GetType().Name.GetHashCode();
        }
    }

    [Serializable]
    internal class ObjectComparer<T> : Comparer<T>
    {
        public override int Compare(T x, T y) {
            return System.Collections.Comparer.Default.Compare(x, y);
        }

        // Equals method for the comparer itself. 
        public override bool Equals(Object obj){
            ObjectComparer<T> comparer = obj as ObjectComparer<T>;
            return comparer != null;
        }        

        public override int GetHashCode() {
            return this.GetType().Name.GetHashCode();
        }
    }

#if MONO
    // Each subclass reads the value back at its underlying type's own width
    // and signedness, whatever UnsafeEnumCast () left in the bits above it,
    // and returns -1, 0 or 1 the way Enum.CompareTo () does.
    [Serializable]
    internal abstract class EnumComparer<T> : Comparer<T>, System.Runtime.Serialization.ISerializable where T : struct
    {
        protected static int Order (long x, long y) => x < y ? -1 : x > y ? 1 : 0;
        protected static int Order (ulong x, ulong y) => x < y ? -1 : x > y ? 1 : 0;

        // Serialized as the ObjectComparer<T> .NET Framework hands out for an
        // enum, so a stream round-trips with it.
        [System.Security.SecurityCritical]
        public void GetObjectData (System.Runtime.Serialization.SerializationInfo info, System.Runtime.Serialization.StreamingContext context)
        {
            info.SetType (typeof (ObjectComparer<T>));
        }

        public override bool Equals (Object obj) => obj != null && obj.GetType () == GetType ();

        public override int GetHashCode () => GetType ().Name.GetHashCode ();
    }

    [Serializable]
    internal sealed class SByteEnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((sbyte) JitHelpers.UnsafeEnumCast (x), (sbyte) JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class ByteEnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((byte) JitHelpers.UnsafeEnumCast (x), (byte) JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class Int16EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((short) JitHelpers.UnsafeEnumCast (x), (short) JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class UInt16EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((ushort) JitHelpers.UnsafeEnumCast (x), (ushort) JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class Int32EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order (JitHelpers.UnsafeEnumCast (x), JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class UInt32EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((uint) JitHelpers.UnsafeEnumCast (x), (uint) JitHelpers.UnsafeEnumCast (y));
    }

    [Serializable]
    internal sealed class Int64EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order (JitHelpers.UnsafeEnumCastLong (x), JitHelpers.UnsafeEnumCastLong (y));
    }

    [Serializable]
    internal sealed class UInt64EnumComparer<T> : EnumComparer<T> where T : struct
    {
        public override int Compare (T x, T y) => Order ((ulong) JitHelpers.UnsafeEnumCastLong (x), (ulong) JitHelpers.UnsafeEnumCastLong (y));
    }
#endif

    [Serializable]
    internal class ComparisonComparer<T> : Comparer<T>
    {
        private readonly Comparison<T> _comparison;

        public ComparisonComparer(Comparison<T> comparison) {
            _comparison = comparison;
        }

        public override int Compare(T x, T y) {
            return _comparison(x, y);
        }
    }
}
