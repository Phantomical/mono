#ifndef MONO_LLVM_RUNTIME_MINIMAL_COMPILE_HPP
#define MONO_LLVM_RUNTIME_MINIMAL_COMPILE_HPP

#include "mini.h"

#include "mono/metadata/class-internals.h"

#include <cstring>

namespace mono {

/// What the translator reads about the method it compiles.
struct TranslateInput {
	MonoMethod *method;
	MonoMethodHeader *header;
	MonoDomain *domain;
	guint32 opt;
};

/// A TranslateInput whose header is freed with it.
class MinimalCompile {
public:
	MinimalCompile (MonoMethod *method, MonoDomain *domain, MonoError *error)
	{
		memset (&cfg, 0, sizeof (cfg));
		cfg.method = method;
		/*
		 * domain is the owning linker's, never the compiling thread's current
		 * one. The translator reads it wherever it resolves per-domain state
		 * at translate time (ldstr).
		 */
		cfg.domain = domain;
		cfg.opt = MONO_OPT_SIMD;
		cfg.header = mono_method_get_header_checked (method, error);
	}

	~MinimalCompile () { mono_metadata_free_mh (cfg.header); }

	MinimalCompile (const MinimalCompile &) = delete;
	MinimalCompile &operator= (const MinimalCompile &) = delete;

	TranslateInput *get () { return &cfg; }

private:
	TranslateInput cfg;
};

} // namespace mono

#endif
