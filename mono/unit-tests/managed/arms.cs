// The attributes that put a class into one of the opt-in arms runner.cpp
// knows how to filter by, through --arm. No CMakeLists.txt registration
// currently passes ARM for either one; both predate the interpreter's
// removal, which took the two arms these attributes selected along with it.
// A class written in IL is marked the same way, through `.module extern`. An
// assembly of its own carries its own copy of the attribute, since the runner
// matches it by name.

using System;

/// Marks a class for an arm that varies whether the engine's own optimizer runs.
[AttributeUsage (AttributeTargets.Class)]
public sealed class NoOptAttribute : Attribute { }

/// Marks a class for an arm that varies whether the engine runs instrumented.
[AttributeUsage (AttributeTargets.Class)]
public sealed class InstrumentedAttribute : Attribute { }
