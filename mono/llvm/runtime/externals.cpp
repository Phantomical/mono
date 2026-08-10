#include "runtime-error.hpp"

#include "externals.hpp"

#include "jit.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

namespace mono {

llvm::Error
resolve_externals (MonoJit &jit, MonoDomain *domain,
                   const std::vector<ExternalSymbol> &externals,
                   llvm::function_ref<llvm::Error (MonoMethod *)> publish_callee)
{
	for (const ExternalSymbol &external : externals) {
		void *address = nullptr;

		switch (external.kind) {
		case ExternalSymbol::Kind::Class:
		case ExternalSymbol::Kind::Method:
		case ExternalSymbol::Kind::Field:
		case ExternalSymbol::Kind::Address:
			address = external.object;
			break;
		case ExternalSymbol::Kind::VTable:
		case ExternalSymbol::Kind::Statics: {
			ERROR_DECL (error);
			MonoVTable *vtable = mono_class_vtable_checked (
				domain, static_cast<MonoClass *> (external.object), error);

			if (vtable == nullptr)
				return runtime_error (error);

			address = external.kind == ExternalSymbol::Kind::VTable
			                  ? static_cast<void *> (vtable)
			                  : mono_vtable_get_static_field_data (vtable);
			break;
		}
		case ExternalSymbol::Kind::Code:
			if (llvm::Error err =
			            publish_callee (static_cast<MonoMethod *> (external.object)))
				return err;
			continue;
		}

		if (llvm::Error err = jit.register_symbol (external.name, address))
			return err;
	}

	return llvm::Error::success ();
}

} // namespace mono
