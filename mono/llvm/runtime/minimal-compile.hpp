/**
 * \file
 * \brief The compile context the translator is handed.
 */

#ifndef MONO_LLVM_RUNTIME_MINIMAL_COMPILE_HPP
#define MONO_LLVM_RUNTIME_MINIMAL_COMPILE_HPP

#include "mini.h"

#include "mono/metadata/class-internals.h"

#include <cstring>

namespace mono {

/// The parts of a MonoCompile the translator reads. The rest belongs to the mini
/// pipeline, which is not running here.
class MinimalCompile {
public:
	MinimalCompile (MonoMethod *method, MonoDomain *domain, MonoError *error)
	{
		memset (&cfg, 0, sizeof (cfg));
		cfg.method = method;
		/*
		 * The domain the code is being compiled for - the owning linker's, not
		 * the compiling thread's current one. The translator reads it wherever
		 * it resolves per-domain state at translate time (ldstr).
		 */
		cfg.domain = domain;
		cfg.opt = MONO_OPT_SIMD;
		cfg.header = mono_method_get_header_checked (method, error);
	}

	~MinimalCompile () { mono_metadata_free_mh (cfg.header); }

	MinimalCompile (const MinimalCompile &) = delete;
	MinimalCompile &operator= (const MinimalCompile &) = delete;

	MonoCompile *get () { return &cfg; }

private:
	MonoCompile cfg;
};

} // namespace mono

#endif
