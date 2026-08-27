#include "dump-ir.hpp"

#include "../dump.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <string>

using namespace llvm;

namespace mono {

PreservedAnalyses
DumpIRPass::run (Module &m, ModuleAnalysisManager &)
{
	for (const Function &f : m) {
		if (!is_published_body (f))
			continue;

		std::string name = dump_name_of (f);

		if (dumping (point_, name.c_str ()))
			cantFail (dump_body_module (point_, m, f.getName (), name));
	}

	return PreservedAnalyses::all ();
}

} // namespace mono
