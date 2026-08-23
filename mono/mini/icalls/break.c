/**
 * \file
 * Where a debugger breaks on the break opcode.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

/*
 * On some architectures, gdb doesn't like encountering the cpu breakpoint instructions
 * in generated code. So instead we emit a call to this function and place a gdb
 * breakpoint here.
 */
void
mono_break (void)
{
}
