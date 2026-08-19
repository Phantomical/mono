/**
 * \file
 * \brief The table of methods an override assembly replaces.
 *
 * An override assembly holds ordinary managed methods carrying
 * [MonoOverride ("namespace.class:method")]. The runtime reads it once at
 * startup and replaces each named method the first time anything asks for it,
 * whichever assembly the target turns out to live in.
 */

#ifndef MONO_MINI_METHOD_OVERRIDE_HPP
#define MONO_MINI_METHOD_OVERRIDE_HPP

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Reads the override assembly beside the runtime, where there is one.
///
/// Absent, unreadable or empty are all silently nothing to do. Call this once
/// the root domain can load an assembly, and before any assembly the overrides
/// name could be loaded.
void method_overrides_init ();

/// Reads \p path as an override assembly, and answers whether it was read.
///
/// What it names is matched against every image already loaded and against each
/// one that loads later. Call it before the targets load where you can: a caller
/// the interpreter has already transformed keeps the body it copied.
bool method_overrides_load (const char *path);

/// Whether the override assembly named anything.
///
/// Every method both engines ask for goes past this, so the answer has to be
/// free in a process with no override assembly.
bool method_overrides_registered ();

/// The method registered to replace \p method, or null when none is.
///
/// Answers an instantiation for an instantiation: the override is written
/// against the target's generic definition, and is inflated here with the
/// target's own type arguments.
MonoMethod *registered_override_for (MonoMethod *method);

} // namespace mono

#endif /* MONO_MINI_METHOD_OVERRIDE_HPP */
