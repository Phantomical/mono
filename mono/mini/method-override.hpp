/**
 * \file
 * \brief The table of methods an override replaces.
 *
 * An assembly marked [assembly: MonoOverrideAssembly] holds ordinary managed
 * methods carrying [MonoOverride ("namespace.class:method")]. The runtime reads
 * such an assembly as it loads, and replaces each named method the first time
 * anything asks for it, whichever assembly the target turns out to live in.
 */

#ifndef MONO_MINI_METHOD_OVERRIDE_HPP
#define MONO_MINI_METHOD_OVERRIDE_HPP

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Starts reading overrides out of the assemblies this process loads.
///
/// Call this once the root domain can load an assembly, and before anything an
/// override can name has run: a caller the interpreter has already transformed
/// keeps the body it copied.
void method_overrides_init ();

/// Loads \p path now rather than waiting for something to reference it, and
/// returns whether it loaded.
///
/// This is how an assembly that nothing references gets its overrides read. A
/// missing file is not an error. The assembly it loads stays out of
/// AppDomain.GetAssemblies ().
bool method_overrides_preload (const char *path);

/// Whether any override has been registered.
///
/// Every method both engines ask for goes past this, so the answer has to be
/// free in a process where nothing declares one.
bool method_overrides_registered ();

/// The method registered to replace \p method, or null when none is.
///
/// Returns an instantiation for an instantiation: the override is written
/// against the target's generic definition, and is inflated here with the
/// target's own type arguments.
MonoMethod *registered_override_for (MonoMethod *method);

} // namespace mono

#endif /* MONO_MINI_METHOD_OVERRIDE_HPP */
