#include "internal-loads.hpp"

#include "runtime/options.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>

namespace mono {

namespace {

llvm::MDNode *
internal_tag (llvm::LLVMContext &c, llvm::StringRef leaf)
{
	llvm::MDBuilder md (c);
	llvm::MDNode *node =
		md.createTBAANode (leaf, md.createTBAARoot (managed_memory_tbaa_root));

	return md.createTBAAStructTagNode (node, node, 0);
}

} // namespace

llvm::LoadInst *
mark_internal_load (llvm::LoadInst *load, llvm::StringRef leaf, InternalLife life)
{
	llvm::LLVMContext &c = load->getContext ();

	if (internal_tbaa ())
		load->setMetadata (llvm::LLVMContext::MD_tbaa, internal_tag (c, leaf));

	switch (life) {
	case InternalLife::varies:
		break;

	case InternalLife::per_object:
		if (load->getType ()->isPointerTy () || tag_non_pointer_invariant_group ())
			load->setMetadata (llvm::LLVMContext::MD_invariant_group,
			                   llvm::MDNode::get (c, {}));
		break;

	case InternalLife::fixed:
		load->setMetadata (llvm::LLVMContext::MD_invariant_load,
		                   llvm::MDNode::get (c, {}));
		break;
	}

	return load;
}

llvm::StoreInst *
mark_internal_store (llvm::StoreInst *store, llvm::StringRef leaf)
{
	if (internal_tbaa ())
		store->setMetadata (llvm::LLVMContext::MD_tbaa,
		                    internal_tag (store->getContext (), leaf));

	return store;
}

} // namespace mono
