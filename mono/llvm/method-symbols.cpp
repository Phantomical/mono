#include "method-symbols.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>

using namespace llvm;

namespace mono {

namespace {

/*
 * A pointer travels on an attribute as bare hexadecimal, and the radix is
 * spelled out at both ends.  %p is what a host's C library makes of it -- glibc
 * writes a 0x prefix and MSVC writes zero-padded digits with none -- and a
 * reader that lets the string say which radix it is reads the second as octal
 * and stops at the first letter.
 */
constexpr const char *pointer_format = "%" PRIxPTR;
constexpr unsigned pointer_radix = 16;

/// The attribute holding a marked declaration's MonoMethod.
constexpr StringRef method_attribute = "mono-method";

/// The same for the MonoClass a per-class symbol stands for.
constexpr StringRef class_attribute = "mono-class";

/// The same for the MonoClass whose static fields a block holds.
constexpr StringRef statics_class_attribute = "mono-statics-class";

/// The same for the MonoString an interned string literal's symbol stands for.
constexpr StringRef ldstr_attribute = "mono-ldstr";

/// The same for a symbol holding the MonoMethod itself. Kept apart from
/// method_attribute, which is what the renaming below reads.
constexpr StringRef method_pointer_attribute = "mono-method-pointer";

/// The pointer \p name carries on \p value, or nothing where it carries none.
std::optional<uintptr_t>
pointer_marker (const GlobalValue &value, StringRef name)
{
	StringRef printed;

	if (auto *fn = dyn_cast<Function> (&value)) {
		if (!fn->hasFnAttribute (name))
			return std::nullopt;
		printed = fn->getFnAttribute (name).getValueAsString ();
	} else if (auto *global = dyn_cast<GlobalVariable> (&value)) {
		if (!global->hasAttribute (name))
			return std::nullopt;
		printed = global->getAttribute (name).getValueAsString ();
	} else {
		return std::nullopt;
	}

	uintptr_t address = 0;

	if (printed.getAsInteger (pointer_radix, address) || address == 0)
		return std::nullopt;
	return address;
}

void
mark_pointer (GlobalValue &value, StringRef name, const void *pointer)
{
	char printed[32];

	snprintf (printed, sizeof (printed), pointer_format, (uintptr_t) pointer);

	if (auto *fn = dyn_cast<Function> (&value))
		fn->addFnAttr (name, printed);
	else if (auto *global = dyn_cast<GlobalVariable> (&value))
		global->addAttribute (name, printed);
}

std::optional<MonoMethod *>
marker_of (const GlobalValue &value)
{
	std::optional<uintptr_t> address = pointer_marker (value, method_attribute);

	if (!address)
		return std::nullopt;
	return reinterpret_cast<MonoMethod *> (*address);
}

} // namespace

void
mark_method_reference (GlobalValue &value, MonoMethod *method)
{
	mark_pointer (value, method_attribute, method);
}

MonoMethod *
marked_method (const GlobalValue &value)
{
	return marker_of (value).value_or (nullptr);
}

void
mark_class_reference (GlobalValue &value, MonoClass *klass)
{
	mark_pointer (value, class_attribute, klass);
}

MonoClass *
marked_class (const GlobalValue &value)
{
	std::optional<uintptr_t> address = pointer_marker (value, class_attribute);

	return address ? reinterpret_cast<MonoClass *> (*address) : nullptr;
}

void
mark_statics_reference (GlobalValue &value, MonoClass *klass)
{
	mark_pointer (value, statics_class_attribute, klass);
}

MonoClass *
marked_statics_class (const GlobalValue &value)
{
	std::optional<uintptr_t> address = pointer_marker (value, statics_class_attribute);

	return address ? reinterpret_cast<MonoClass *> (*address) : nullptr;
}

void
mark_ldstr_reference (GlobalValue &value, MonoString *interned)
{
	mark_pointer (value, ldstr_attribute, interned);
}

MonoString *
marked_ldstr (const GlobalValue &value)
{
	std::optional<uintptr_t> address = pointer_marker (value, ldstr_attribute);

	return address ? reinterpret_cast<MonoString *> (*address) : nullptr;
}

void
mark_method_pointer (GlobalValue &value, MonoMethod *method)
{
	mark_pointer (value, method_pointer_attribute, method);
}

MonoMethod *
marked_method_pointer (const GlobalValue &value)
{
	std::optional<uintptr_t> address = pointer_marker (value, method_pointer_attribute);

	return address ? reinterpret_cast<MonoMethod *> (*address) : nullptr;
}

/// Carries a rename through the attributes that hold a symbol name rather
/// than a reference to it. `mono-builtin-target` is one, and it is how a
/// builtin declaration says which method it stands for.
///
/// An IR use moves with a rename on its own. A name written into a string does
/// not, and without this the pass that reads it back would look for a function
/// that no longer answers to that name.
void
follow_renames (Module &m, const StringMap<std::string> &renames)
{
	if (renames.empty ())
		return;

	for (Function &fn : m.functions ()) {
		AttributeSet attrs = fn.getAttributes ().getFnAttrs ();

		for (Attribute attr : attrs) {
			if (!attr.isStringAttribute ())
				continue;

			auto renamed = renames.find (attr.getValueAsString ());

			if (renamed == renames.end ())
				continue;

			StringRef kind = attr.getKindAsString ();

			fn.removeFnAttr (kind);
			fn.addFnAttr (kind, renamed->second);
		}
	}
}

Error
bind_method_symbols (Module &m,
                     function_ref<Expected<std::string> (MonoMethod *)> name_of)
{
	SmallVector<GlobalValue *, 16> marked;

	for (Function &fn : m.functions ())
		marked.push_back (&fn);
	for (GlobalVariable &global : m.globals ())
		marked.push_back (&global);

	StringMap<std::string> renames;

	// Functions are pushed into marked before globals, so a Function's
	// collision below always erases a GlobalVariable this loop has not
	// reached yet. Skip it here instead of dereferencing freed memory.
	SmallPtrSet<GlobalValue *, 16> erased;

	for (GlobalValue *value : marked) {
		if (erased.contains (value))
			continue;

		/*
		 * Only a declaration stands for something the engine publishes. A
		 * definition is the thing itself and keeps whatever it was called.
		 * That matters here rather than being a nicety: a thunk built to a
		 * declaration's shape is given its attributes, marker and all.
		 * Without this, the dispatcher would be renamed to the method it
		 * dispatches for, and the name it was compiled under would not exist.
		 */
		if (!value->isDeclaration ())
			continue;

		std::optional<MonoMethod *> marker = marker_of (*value);

		if (!marker)
			continue;

		Expected<std::string> name = name_of (*marker);

		if (!name)
			return name.takeError ();
		if (value->getName () == *name)
			continue;

		renames[value->getName ()] = *name;

		/*
		 * setName () uniques against whatever already answers to the name
		 * rather than failing, which would leave a reference to a symbol
		 * nothing defines. Two references to one method have to become the
		 * same value instead.
		 */
		if (GlobalValue *existing = m.getNamedValue (*name)) {
			/*
			 * A method gets one symbol, and where the two spellings meet it has
			 * to be the callable one. An address use reads a Function as
			 * readily as it reads a plain global, and a call cannot be made on
			 * the global. So a Function arriving at a name an address symbol
			 * already holds takes the name over instead of being folded away.
			 *
			 * The translator reaches this the other way round, because it binds
			 * the functions before the globals. A pass that mints a callee for a
			 * method some ldftn already took the address of does not.
			 */
			if (isa<Function> (value) && !isa<Function> (existing)) {
				existing->replaceAllUsesWith (value);
				erased.insert (existing);
				existing->eraseFromParent ();
				value->setName (*name);
				continue;
			}

			value->replaceAllUsesWith (existing);
			erased.insert (value);
			value->eraseFromParent ();
			continue;
		}

		value->setName (*name);
	}

	follow_renames (m, renames);
	return Error::success ();
}

} // namespace mono
