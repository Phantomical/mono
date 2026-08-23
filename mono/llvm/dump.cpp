#include "dump.hpp"

#include <cstdio>

#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {

namespace {

constexpr const char *dump_name_key = "mono.dump.name";

} // namespace

Error
with_dump_stream (DumpPoint point, StringRef name,
                  function_ref<Error (raw_pwrite_stream &)> body)
{
	DumpDestination destination (point, name.str ().c_str ());

	if (destination.stream () == nullptr)
		return Error::success ();

	// Anything already buffered on this stream was printed first, so it has to
	// reach the file first. Only this stream writes past here.
	fflush (destination.stream ());

	raw_fd_ostream out (fileno (destination.stream ()), /*shouldClose=*/false);
	Error result = body (out);

	out.flush ();
	return result;
}

void
set_dump_name (Function &function, StringRef name)
{
	LLVMContext &context = function.getContext ();

	function.setMetadata (dump_name_key,
	                      MDNode::get (context, MDString::get (context, name)));
}

std::string
dump_name_of (const Function &function)
{
	if (MDNode *node = function.getMetadata (dump_name_key))
		if (node->getNumOperands () == 1)
			if (auto *name = dyn_cast<MDString> (node->getOperand (0)))
				return name->getString ().str ();

	return function.getName ().str ();
}

bool
is_published_body (const Function &function)
{
	return !function.isDeclaration () && !function.hasLocalLinkage ();
}

} // namespace mono
