// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace System.Runtime.Intrinsics
{
	[Intrinsic]
	[StructLayout (LayoutKind.Sequential, Size = 32)]
	public readonly struct Vector256<T> where T : struct
	{
		private readonly ulong _00;
		private readonly ulong _01;
		private readonly ulong _02;
		private readonly ulong _03;
	}
}
