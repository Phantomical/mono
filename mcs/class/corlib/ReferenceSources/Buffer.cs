#if BIT64
using nuint = System.UInt64;
#else
using nuint = System.UInt32;
#endif

using System.Runtime.CompilerServices;
using System.Runtime;

#if NETCORE
using Internal.Runtime.CompilerServices;
#endif

namespace System
{
	partial class Buffer
	{
		public static int ByteLength (Array array)
		{
			// note: the other methods in this class also use ByteLength to test for
			// null and non-primitive arguments as a side-effect.

			if (array == null)
				throw new ArgumentNullException ("array");

			int length = _ByteLength (array);
			if (length < 0)
				throw new ArgumentException ("Object must be an array of primitives.");

			return length;
		}

		public static unsafe byte GetByte (Array array, int index)
		{
			if (index < 0 || index >= ByteLength (array))
				throw new ArgumentOutOfRangeException ("index");

			return *(byte*)(Unsafe.AsPointer<byte> (ref Unsafe.Add<byte> (ref array.GetRawSzArrayData (), index)));
		}

		public static unsafe void SetByte (Array array, int index, byte value)
		{
			if (index < 0 || index >= ByteLength (array))
				throw new ArgumentOutOfRangeException ("index");

			*(byte*)(Unsafe.AsPointer<byte> (ref Unsafe.Add<byte> (ref array.GetRawSzArrayData (), index))) = value;
		}

		public static void BlockCopy (Array src, int srcOffset, Array dst, int dstOffset, int count)
		{
			if (src == null)
				throw new ArgumentNullException ("src");

			if (dst == null)
				throw new ArgumentNullException ("dst");

			if (srcOffset < 0)
				throw new ArgumentOutOfRangeException ("srcOffset", "Non-negative number required.");

			if (dstOffset < 0)
				throw new ArgumentOutOfRangeException ("dstOffset", "Non-negative number required.");

			if (count < 0)
				throw new ArgumentOutOfRangeException ("count", "Non-negative number required.");

			// We do the checks in unmanaged code for performance reasons
			bool res = InternalBlockCopy (src, srcOffset, dst, dstOffset, count);
			if (!res) {
				// watch for integer overflow
				if ((srcOffset > ByteLength (src) - count) || (dstOffset > ByteLength (dst) - count))
					throw new ArgumentException (
						"Offset and length were out of bounds for the array or count is greater than " + 
						"the number of elements from index to the end of the source collection.");
			}
		}

		[CLSCompliantAttribute (false)]
		public static unsafe void MemoryCopy (void* source, void* destination, long destinationSizeInBytes, long sourceBytesToCopy)
		{
			if (sourceBytesToCopy > destinationSizeInBytes)
				goto Throw;

			if (sourceBytesToCopy <= 0)
				return;

#if !BIT64
			if (sourceBytesToCopy > uint.MaxValue)
				goto Throw;
#endif

			var src = (byte*)source;
			var dst = (byte*)destination;
			Memmove (dst, src, (nuint) sourceBytesToCopy);
			return;

		Throw:
			ThrowHelper.ThrowArgumentOutOfRangeException(ExceptionArgument.sourceBytesToCopy);
		}

		[CLSCompliantAttribute (false)]
		public static unsafe void MemoryCopy (void* source, void* destination, ulong destinationSizeInBytes, ulong sourceBytesToCopy)
		{
			if (sourceBytesToCopy > destinationSizeInBytes)
				goto Throw;

#if !BIT64
			if (sourceBytesToCopy > uint.MaxValue)
				goto Throw;
#endif

			var src = (byte*)source;
			var dst = (byte*)destination;
			Memmove (dst, src, (nuint) sourceBytesToCopy);
			return;

		Throw:
			ThrowHelper.ThrowArgumentOutOfRangeException(ExceptionArgument.sourceBytesToCopy);
		}

		internal static unsafe void Memcpy (byte *dest, byte *src, int len) {
			if (len <= 0)
				return;

			Memcpy(dest, src, (nuint)len);
		}
		
		internal static unsafe void Memcpy (byte* dest, byte* src, nuint len) {
			if (len <= 0)
				return;

			// RuntimeImports.Memcpy may be directly replaced with a call to memcpy.
			// If memcpy dereferences a null pointer the whole process dies, so
			// we need to explicitly throw the NRE.
			if (dest == null || src == null)
				throw new NullReferenceException();

			RuntimeImports.Memcpy(dest, src, (nuint)len);
		}

#if BIT64
		internal static unsafe void Memmove (byte* dest, byte* src, uint len) =>
			Memmove(dest, src, (nuint)len);
#endif

		internal static unsafe void Memmove (byte *dest, byte *src, nuint len)
		{
			if (len <= 0)
				return;

			// RuntimeImports.Memmove may be directly replaced with a call to memmove.
			// If memmove dereferences a null pointer the whole process dies, so
			// we need to explicitly throw the NRE.
			if (dest == null || src == null)
				throw new NullReferenceException();

			RuntimeImports.Memmove(dest, src, (nuint)len);
		}

		internal static void Memmove<T>(ref T destination, ref T source, nuint elementCount)
		{
			if (!RuntimeHelpers.IsReferenceOrContainsReferences<T>()) {
				unsafe {
					fixed (byte* pDestination = &Unsafe.As<T, byte>(ref destination), pSource = &Unsafe.As<T, byte>(ref source))
						Memmove(pDestination, pSource, (nuint)elementCount * (nuint)Unsafe.SizeOf<T>());
				}
			} else {
				unsafe {
					fixed (byte* pDestination = &Unsafe.As<T, byte>(ref destination), pSource = &Unsafe.As<T, byte>(ref source))
						RuntimeImports.Memmove_wbarrier(pDestination, pSource, (nuint)elementCount, typeof(T).TypeHandle.Value);
				}
			}
		}
	}
}
