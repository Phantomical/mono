// The attributes that put a class into one of the opt-in arms.
//
// Every test runs interpreted and compiled. The two arms that change what the
// transform emits -- its optimizations off, or instrumentation spliced into the
// stream -- ask one question of a whole suite: does the program still answer
// the same. Most suites cannot answer it differently, so those arms are opt in
// rather than another copy of everything.
//
// Mark a class where a defect in what the arm varies would change an answer.
// CMakeLists.txt in the directory above says what each arm turns on.
//
// A class written in IL is marked the same way, through `.module extern`. An
// assembly of its own carries its own copy of the attribute, since the runner
// matches it by name.

using System;

/// Runs this class again with the transform's optimizations off.
[AttributeUsage (AttributeTargets.Class)]
public sealed class NoOptAttribute : Attribute { }

/// Runs this class again with the interpreter instrumented.
[AttributeUsage (AttributeTargets.Class)]
public sealed class InstrumentedAttribute : Attribute { }
